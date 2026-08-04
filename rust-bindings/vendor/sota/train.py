#!/usr/bin/env python3
"""
VORTEX GBDT Model Training Script

Trains a Gradient-Boosted Decision Tree model for inlining decisions.
Reads CSV of features + label, trains using scikit-learn's
GradientBoostingClassifier, and outputs the model in the flat array
format consumed by inference.c.

Input CSV format:
    callee_size,callee_instruction_count,call_site_frequency,...,label
    0.123,0.456,...,1
    0.789,...,0

The label column is 1 for profitable inlines, 0 for unprofitable.

Output format:
    Flat array of doubles that can be loaded by vtx_gbdt_load_model():
    [0]     : init_score
    [1]     : tree_count
    [2..]   : for each tree:
                node_count
                then node_count * 5 doubles:
                  feature_index, threshold, left_child, right_child, leaf_value

Usage:
    python train.py --input features.csv --output model.bin [--trees 100] [--depth 5]
"""

import argparse
import csv
import struct
import sys
import os

try:
    import numpy as np
    from sklearn.ensemble import GradientBoostingClassifier
    from sklearn.model_selection import cross_val_score
except ImportError:
    print("Error: scikit-learn and numpy are required.", file=sys.stderr)
    print("Install with: pip install scikit-learn numpy", file=sys.stderr)
    sys.exit(1)


# Feature names (must match features.h/c)
FEATURE_NAMES = [
    "callee_size",
    "callee_instruction_count",
    "call_site_frequency",
    "caller_size",
    "call_depth",
    "callee_is_hot",
    "callee_has_loops",
    "callee_has_try_catch",
    "callee_allocates",
    "callee_calls_virtual",
    "receiver_type_certainty",
    "constant_arg_ratio",
    "estimated_register_pressure",
    "callee_deopt_rate",
    "inline_history",
]

NUM_FEATURES = len(FEATURE_NAMES)

# Leaf marker must match VTX_GBDT_LEAF_MARKER in inference.h
LEAF_MARKER = 0xFFFF


def load_csv(filepath):
    """Load feature vectors and labels from a CSV file.

    Returns:
        X: numpy array of shape (n_samples, n_features)
        y: numpy array of shape (n_samples,) with binary labels
    """
    rows = []
    labels = []

    with open(filepath, 'r') as f:
        reader = csv.reader(f)
        header = next(reader)  # skip header

        for row in reader:
            if len(row) < NUM_FEATURES + 1:
                print(f"Warning: skipping short row with {len(row)} columns",
                      file=sys.stderr)
                continue

            try:
                features = [float(x) for x in row[:NUM_FEATURES]]
                label = int(float(row[NUM_FEATURES]))
                if label not in (0, 1):
                    print(f"Warning: skipping row with invalid label {label}",
                          file=sys.stderr)
                    continue
                rows.append(features)
                labels.append(label)
            except ValueError as e:
                print(f"Warning: skipping row with parse error: {e}",
                      file=sys.stderr)
                continue

    X = np.array(rows, dtype=np.float64)
    y = np.array(labels, dtype=np.int32)

    return X, y


def train_model(X, y, n_trees=100, max_depth=5):
    """Train a GradientBoostingClassifier.

    Returns:
        model: trained sklearn model
    """
    model = GradientBoostingClassifier(
        n_estimators=n_trees,
        max_depth=max_depth,
        learning_rate=0.1,
        subsample=0.8,       # stochastic gradient boosting
        min_samples_split=5,
        min_samples_leaf=2,
        max_features='sqrt', # feature subsampling for robustness
        random_state=42,
    )

    model.fit(X, y)
    return model


def evaluate_model(model, X, y):
    """Print model evaluation metrics."""
    # Training accuracy
    train_score = model.score(X, y)
    print(f"Training accuracy: {train_score:.4f}")

    # Cross-validation score
    if len(y) >= 10:  # need enough samples for CV
        cv_scores = cross_val_score(model, X, y, cv=min(5, len(y) // 2))
        print(f"CV accuracy: {cv_scores.mean():.4f} (+/- {cv_scores.std():.4f})")

    # Feature importances
    importances = model.feature_importances_
    print("\nFeature importances:")
    sorted_idx = np.argsort(importances)[::-1]
    for i in sorted_idx[:10]:  # top 10 features
        if importances[i] > 0.001:
            print(f"  {FEATURE_NAMES[i]:30s}: {importances[i]:.4f}")


def extract_tree_nodes(tree):
    """Extract nodes from a sklearn decision tree in the VORTEX format.

    Each tree in sklearn has:
        tree_.feature[i]    : feature index (-2 for leaf)
        tree_.threshold[i]  : threshold
        tree_.children_left[i]  : left child index
        tree_.children_right[i] : right child index
        tree_.value[i]      : value at node

    For leaf nodes, feature == -2 (TREE_LEAF in sklearn).
    We remap to use VTX_GBDT_LEAF_MARKER (0xFFFF) for feature_index.

    Returns:
        nodes: list of (feature_index, threshold, left_child, right_child, leaf_value)
    """
    TREE_LEAF = -1  # sklearn's leaf marker

    n_nodes = tree.node_count
    nodes = []

    for i in range(n_nodes):
        if tree.children_left[i] == TREE_LEAF:
            # Leaf node
            # The value is the log-odds ratio for the positive class
            # For binary classification, value[i][0][1] is the positive class count
            # We convert to a leaf value that contributes to the raw score
            leaf_value = tree.value[i][0][1]
            # Normalize by the number of samples at this leaf
            if tree.n_node_samples[i] > 0:
                leaf_value = leaf_value / tree.n_node_samples[i]
            else:
                leaf_value = 0.0

            nodes.append((LEAF_MARKER, 0.0, 0, 0, leaf_value))
        else:
            # Split node
            feature_index = tree.feature[i]
            threshold = tree.threshold[i]
            left_child = tree.children_left[i]
            right_child = tree.children_right[i]

            nodes.append((feature_index, threshold, left_child, right_child, 0.0))

    return nodes


def serialize_model(model, output_path):
    """Serialize the trained model to the flat array format for inference.c.

    Format:
        [0]     : init_score (double)
        [1]     : tree_count (double, cast from uint32_t)
        [2..]   : for each tree:
                    node_count (double, cast from uint32_t)
                    then node_count * 5 doubles:
                      feature_index, threshold, left_child, right_child, leaf_value
    """
    data = []

    # Init score: for GradientBoostingClassifier, the init prediction is
    # the log-odds of the positive class in the training data.
    # sklearn stores this in model.init_.prior_proba (or similar).
    try:
        # For binary classification, the initial score is log(p / (1 - p))
        # where p is the proportion of positive samples
        classes = model.classes_
        if len(classes) == 2:
            n_pos = np.sum(model.estimators_[0][0].train_samples if hasattr(model.estimators_[0][0], 'train_samples') else 0)
            # Fallback: compute from the model's init estimator
            init_pred = model.init_.predict_proba(np.array([[0.0] * NUM_FEATURES]))
            if len(init_pred.shape) == 2 and init_pred.shape[1] == 2:
                p = init_pred[0][1]
                if p > 0 and p < 1:
                    init_score = np.log(p / (1 - p))
                else:
                    init_score = 0.0
            else:
                init_score = 0.0
        else:
            init_score = 0.0
    except Exception:
        init_score = 0.0

    data.append(float(init_score))

    # Tree count
    n_trees = len(model.estimators_)
    data.append(float(n_trees))

    for i in range(n_trees):
        # Each estimator is a list of trees (one per class for classification)
        # For binary classification, each estimator has one tree
        sklearn_tree = model.estimators_[i][0].tree_
        nodes = extract_tree_nodes(sklearn_tree)

        # Node count
        data.append(float(len(nodes)))

        # Node data
        for node in nodes:
            feature_index, threshold, left_child, right_child, leaf_value = node
            data.append(float(feature_index))
            data.append(float(threshold))
            data.append(float(left_child))
            data.append(float(right_child))
            data.append(float(leaf_value))

    # Write to binary file
    with open(output_path, 'wb') as f:
        f.write(struct.pack(f'<{len(data)}d', *data))

    print(f"\nModel serialized to {output_path}")
    print(f"  Total doubles: {len(data)}")
    print(f"  Trees: {n_trees}")
    print(f"  File size: {len(data) * 8} bytes")


def generate_c_header(model, output_path):
    """Generate a C header file with the model embedded as a static array.

    This is useful for embedding the default model directly in inference.c.
    """
    data = []

    # Same serialization as serialize_model but output as C source
    try:
        init_pred = model.init_.predict_proba(np.array([[0.0] * NUM_FEATURES]))
        if len(init_pred.shape) == 2 and init_pred.shape[1] == 2:
            p = init_pred[0][1]
            init_score = np.log(p / (1 - p)) if 0 < p < 1 else 0.0
        else:
            init_score = 0.0
    except Exception:
        init_score = 0.0

    n_trees = len(model.estimators_)
    all_nodes = []

    for i in range(n_trees):
        sklearn_tree = model.estimators_[i][0].tree_
        nodes = extract_tree_nodes(sklearn_tree)
        all_nodes.append(nodes)

    with open(output_path, 'w') as f:
        f.write("/* Auto-generated GBDT model — DO NOT EDIT */\n")
        f.write("/* Generated by train.py */\n\n")
        f.write("#include \"inliner/inference.h\"\n\n")

        f.write(f"static const double vtx_trained_model_data[] = {{\n")
        f.write(f"    {init_score:.15g},  /* init_score */\n")
        f.write(f"    {n_trees}.0,       /* tree_count */\n")

        for i, nodes in enumerate(all_nodes):
            f.write(f"    /* Tree {i} */\n")
            f.write(f"    {len(nodes)}.0,  /* node_count */\n")
            for j, node in enumerate(nodes):
                feature_index, threshold, left_child, right_child, leaf_value = node
                f.write(f"    {feature_index}.0, {threshold:.15g}, "
                        f"{left_child}.0, {right_child}.0, "
                        f"{leaf_value:.15g},  /* node {j} */\n")

        f.write("};\n\n")

        f.write(f"static const uint32_t vtx_trained_model_data_count = {2 + sum(len(n) * 5 + 1 for n in all_nodes)};\n")

    print(f"C header generated at {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Train GBDT model for VORTEX inlining decisions")
    parser.add_argument("--input", "-i", required=True,
                        help="Input CSV file with features and labels")
    parser.add_argument("--output", "-o", required=True,
                        help="Output binary model file")
    parser.add_argument("--header", "-H", default=None,
                        help="Optional: output C header file with embedded model")
    parser.add_argument("--trees", "-t", type=int, default=100,
                        help="Number of trees (default: 100)")
    parser.add_argument("--depth", "-d", type=int, default=5,
                        help="Maximum tree depth (default: 5)")
    parser.add_argument("--evaluate", "-e", action="store_true",
                        help="Print model evaluation metrics")
    args = parser.parse_args()

    # Load data
    print(f"Loading data from {args.input}...")
    X, y = load_csv(args.input)

    if len(X) == 0:
        print("Error: no valid data found in input file", file=sys.stderr)
        sys.exit(1)

    print(f"  Samples: {len(X)}")
    print(f"  Features: {X.shape[1]}")
    print(f"  Positive labels: {np.sum(y == 1)}")
    print(f"  Negative labels: {np.sum(y == 0)}")

    # Train model
    print(f"\nTraining GBDT model ({args.trees} trees, max depth {args.depth})...")
    model = train_model(X, y, n_trees=args.trees, max_depth=args.depth)

    # Evaluate
    if args.evaluate:
        print("\nEvaluating model...")
        evaluate_model(model, X, y)

    # Serialize
    serialize_model(model, args.output)

    # Generate C header if requested
    if args.header:
        generate_c_header(model, args.header)

    print("\nDone!")


if __name__ == "__main__":
    main()
