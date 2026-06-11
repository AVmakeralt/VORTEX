#ifndef VORTEX_INLINER_INFERENCE_H
#define VORTEX_INLINER_INFERENCE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>
#include "vortex_config.h"
#include "inliner/features.h"

/**
 * VORTEX ML Inliner — GBDT Inference Engine
 *
 * Gradient-Boosted Decision Tree inference for inlining decisions.
 * The default embedded model uses 30 trees of max depth 3.
 * The model struct supports up to VTX_GBDT_MAX_TREES (100) trees
 * with max depth VTX_GBDT_MAX_DEPTH (5) for trained models.
 *
 * Each tree node is a flat struct containing:
 *   - feature_index: which feature to test (VTX_GBDT_LEAF_MARKER for leaves)
 *   - threshold: decision boundary for the feature
 *   - left_child: index of left child node in the flat array
 *   - right_child: index of right child node in the flat array
 *   - leaf_value: prediction contribution if this is a leaf
 *
 * Inference: traverse each tree from root to leaf, sum leaf values.
 * Output is a score in [0, 1]; above VTX_INLINE_THRESHOLD (0.6) means inline.
 *
 * A default conservative model is embedded with 30 trees of depth 3.
 * This model uses simple heuristics: high frequency + small callee +
 * monomorphic receiver → inline. A trained model can be loaded to
 * replace the default.
 */

/* ========================================================================== */
/* Constants                                                                   */
/* ========================================================================== */

/* Maximum model capacity (for trained models) */
#define VTX_GBDT_MAX_TREES     100
#define VTX_GBDT_MAX_DEPTH     5

/* Default embedded model parameters */
#define VTX_GBDT_DEFAULT_TREE_COUNT 30
#define VTX_GBDT_DEFAULT_DEPTH      3

/* Leaf marker: stored in feature_index to indicate a leaf node */
#define VTX_GBDT_LEAF_MARKER   0xFFFFu

/* Maximum number of nodes per tree at max depth:
 * A complete binary tree of depth D has 2^(D+1) - 1 nodes.
 * For max depth 5: 2^6 - 1 = 63 nodes. Add 1 for safety. */
#define VTX_GBDT_MAX_NODES_PER_TREE 64

/* Total maximum nodes across all trees */
#define VTX_GBDT_MAX_TOTAL_NODES (VTX_GBDT_MAX_TREES * VTX_GBDT_MAX_NODES_PER_TREE)

/* ========================================================================== */
/* Tree node                                                                   */
/* ========================================================================== */

/**
 * A single node in a GBDT tree. Internal nodes test a feature against
 * a threshold; leaf nodes contribute a value to the prediction sum.
 *
 * Types are chosen for compactness:
 *   uint16_t feature_index: 15 features, 0xFFFF = leaf marker
 *   float    threshold:     sufficient precision for normalized [0,1] features
 *   int16_t  left_child:    indices within a tree (< 32768 nodes per tree)
 *   int16_t  right_child:   same
 *   float    leaf_value:    small values summed across trees
 */
typedef struct {
    uint16_t feature_index;  /* feature to test, or VTX_GBDT_LEAF_MARKER for leaf */
    float    threshold;      /* decision threshold for split */
    int16_t  left_child;     /* index of left child (feature < threshold) */
    int16_t  right_child;    /* index of right child (feature >= threshold) */
    float    leaf_value;     /* value if this is a leaf node */
} vtx_gbdt_node_t;

/* ========================================================================== */
/* Tree descriptor                                                             */
/* ========================================================================== */

typedef struct {
    uint32_t root_index;     /* index of root node in the flat node array */
    uint32_t node_count;     /* number of nodes in this tree */
} vtx_gbdt_tree_desc_t;

/* ========================================================================== */
/* GBDT model                                                                  */
/* ========================================================================== */

typedef struct {
    vtx_gbdt_node_t     *nodes;         /* flat array of all tree nodes */
    uint32_t             node_count;    /* total number of nodes */
    uint32_t             node_capacity; /* allocated capacity */

    vtx_gbdt_tree_desc_t trees[VTX_GBDT_MAX_TREES]; /* tree descriptors */
    uint32_t             tree_count;    /* number of trees (<= VTX_GBDT_MAX_TREES) */
    uint32_t             max_depth;     /* maximum depth across all trees */

    double               init_score;    /* initial prediction (bias term) */

    bool                 owns_nodes;    /* true if nodes array was allocated by us */
} vtx_gbdt_model_t;

/* ========================================================================== */
/* Model lifecycle                                                             */
/* ========================================================================== */

/**
 * Initialize an empty GBDT model.
 * Returns 0 on success, -1 on failure.
 */
int vtx_gbdt_model_init(vtx_gbdt_model_t *model);

/**
 * Destroy a GBDT model and free owned memory.
 */
void vtx_gbdt_model_destroy(vtx_gbdt_model_t *model);

/**
 * Load a GBDT model from a flat data array.
 *
 * The data array layout:
 *   [0]     : init_score (double)
 *   [1]     : tree_count (uint32_t cast to double)
 *   [2]     : max_depth (uint32_t cast to double)
 *   [3..]   : for each tree:
 *               node_count (uint32_t as double)
 *               then node_count * 5 doubles:
 *                 feature_index, threshold, left_child, right_child, leaf_value
 *
 * @param model  Model to load into
 * @param data   Flat array of doubles
 * @param count  Number of doubles in the data array
 * @return       0 on success, -1 on failure
 */
int vtx_gbdt_load_model(vtx_gbdt_model_t *model, const double *data, uint32_t count);

/**
 * Load the default conservative model.
 * This model uses 30 trees of depth 3 with simple heuristics:
 *   - Inline if callee is small (normalized size < 0.15)
 *   - Inline if receiver type is certain (> 0.8)
 *   - Inline if call frequency is high
 *   - Don't inline if call depth is too deep
 *   - Don't inline if callee deopt rate is high
 *   - Positive for constant arguments
 *   - Positive for hot callees
 *   - Negative for try/catch and virtual calls in some contexts
 *
 * The default model is always available and can be used until
 * a real model is trained from production data.
 *
 * @param model  Model to load into
 * @return       0 on success, -1 on failure
 */
int vtx_gbdt_load_default_model(vtx_gbdt_model_t *model);

/* ========================================================================== */
/* Inference                                                                   */
/* ========================================================================== */

/**
 * Run GBDT inference on a feature vector.
 *
 * Traverses each tree from root to leaf, summing leaf values.
 * Applies sigmoid transformation to produce output in [0, 1].
 *
 * @param model    Trained GBDT model
 * @param features Feature vector for a call site
 * @return         Score in [0, 1]. Above VTX_INLINE_THRESHOLD → inline.
 */
double vtx_gbdt_infer(const vtx_gbdt_model_t *model,
                       const vtx_inline_features_t *features);

/**
 * Check if the inference score indicates an inline decision.
 *
 * @param score  Output of vtx_gbdt_infer
 * @return       true if score >= VTX_INLINE_THRESHOLD
 */
static inline bool vtx_gbdt_should_inline(double score)
{
    return score >= VTX_INLINE_THRESHOLD;
}

/* ========================================================================== */
/* Sigmoid helper                                                              */
/* ========================================================================== */

/**
 * Sigmoid function: 1 / (1 + exp(-x))
 * Maps raw GBDT sum to [0, 1].
 */
static inline double vtx_sigmoid(double x)
{
    /* Guard against overflow */
    if (x > 500.0) return 1.0;
    if (x < -500.0) return 0.0;
    return 1.0 / (1.0 + exp(-x));
}

#endif /* VORTEX_INLINER_INFERENCE_H */
