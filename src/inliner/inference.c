#include "inliner/inference.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ========================================================================== */
/* Default model: 30 trees, depth 3                                            */
/* ========================================================================== */

/**
 * The default model encodes inlining heuristics as 30 GBDT trees of depth 3.
 *
 * Design principles:
 *   - Each tree tests 1-3 features and produces a small leaf contribution
 *   - Positive leaf values favor inlining; negative values oppose it
 *   - Leaf values are calibrated so the sigmoid of the sum gives
 *     values above VTX_INLINE_THRESHOLD (0.6) for good inline candidates
 *     and below for bad ones
 *   - A good inline (small, hot, monomorphic) sums to ~+1.5 → sigmoid ≈ 0.82
 *   - A bad inline (large, cold, deep, deopting) sums to ~-1.5 → sigmoid ≈ 0.18
 *
 * Tree groups:
 *   Trees  0-5:  Callee size features (small = good, large = bad)
 *   Trees  6-11: Call frequency features (hot = good, cold = bad)
 *   Trees 12-17: Receiver type certainty (monomorphic = good, mega = bad)
 *   Trees 18-23: Penalty features (deep, high deopt, many inlines = bad)
 *   Trees 24-29: Combination and secondary features
 *
 * Feature indices (after normalization):
 *   0  callee_size             5  callee_is_hot            10 receiver_type_certainty
 *   1  callee_instruction_count 6  callee_has_loops         11 constant_arg_ratio
 *   2  call_site_frequency     7  callee_has_try_catch     12 estimated_register_pressure
 *   3  caller_size             8  callee_allocates         13 callee_deopt_rate
 *   4  call_depth              9  callee_calls_virtual     14 inline_history
 */

/* A depth-3 tree has at most 15 nodes (7 internal + 8 leaves).
 * We use a compact tree builder to construct the default model. */

/* Helper: add a leaf node, return its index within the tree-local array */
static uint32_t build_leaf(vtx_gbdt_node_t *nodes, uint32_t *pos, float value)
{
    uint32_t idx = (*pos)++;
    nodes[idx].feature_index = VTX_GBDT_LEAF_MARKER;
    nodes[idx].threshold     = 0.0f;
    nodes[idx].left_child    = -1;
    nodes[idx].right_child   = -1;
    nodes[idx].leaf_value    = value;
    return idx;
}

/* Helper: add a split node, return its index.
 * Children must be set after they are created. */
static uint32_t build_split(vtx_gbdt_node_t *nodes, uint32_t *pos,
                             uint16_t feature, float threshold)
{
    uint32_t idx = (*pos)++;
    nodes[idx].feature_index = feature;
    nodes[idx].threshold     = threshold;
    nodes[idx].left_child    = -1;  /* set later */
    nodes[idx].right_child   = -1;  /* set later */
    nodes[idx].leaf_value    = 0.0f;
    return idx;
}

/**
 * Build a depth-3 tree with the given structure.
 *
 * Layout (depth 3):
 *            N0 (root: f0/t0)
 *           /              \
 *     N1 (f1/t1)        N2 (f2/t2)
 *     /        \         /        \
 *   N3(L0)  N4(L1)   N5(L2)   N6(L3)
 *   / \     / \      / \      / \
 * L0  L1  L2  L3  L4  L5  L6  L7
 *
 * For depth < 3 trees, some internal nodes become leaves.
 * Depth 1: root has two leaf children (3 nodes)
 * Depth 2: root's children each have two leaf children (7 nodes)
 * Depth 3: full tree with 15 nodes (7 internal + 8 leaves)
 */
typedef struct {
    uint16_t f0; float t0;          /* root split */
    uint16_t f1; float t1;          /* left child split */
    uint16_t f2; float t2;          /* right child split */
    uint16_t f3; float t3;          /* left-left child split */
    uint16_t f4; float t4;          /* left-right child split */
    uint16_t f5; float t5;          /* right-left child split */
    uint16_t f6; float t6;          /* right-right child split */
    float leaves[8];                /* leaf values */
    uint32_t depth;                 /* 1, 2, or 3 */
} vtx_tree_spec_t;

static uint32_t build_tree_from_spec(vtx_gbdt_node_t *base,
                                      uint32_t base_offset,
                                      const vtx_tree_spec_t *spec)
{
    uint32_t pos = base_offset;
    uint32_t node_count = 0;

    if (spec->depth == 1) {
        /* Root + 2 leaves */
        uint32_t root = build_split(base, &pos, spec->f0, spec->t0);
        uint32_t left = build_leaf(base, &pos, spec->leaves[0]);
        uint32_t right = build_leaf(base, &pos, spec->leaves[1]);
        base[root].left_child  = (int16_t)left;
        base[root].right_child = (int16_t)right;
        node_count = 3;
    } else if (spec->depth == 2) {
        /* Root + 2 internal + 4 leaves */
        uint32_t root = build_split(base, &pos, spec->f0, spec->t0);
        uint32_t n1   = build_split(base, &pos, spec->f1, spec->t1);
        uint32_t n2   = build_split(base, &pos, spec->f2, spec->t2);
        uint32_t l0   = build_leaf(base, &pos, spec->leaves[0]);
        uint32_t l1   = build_leaf(base, &pos, spec->leaves[1]);
        uint32_t l2   = build_leaf(base, &pos, spec->leaves[2]);
        uint32_t l3   = build_leaf(base, &pos, spec->leaves[3]);
        base[root].left_child  = (int16_t)n1;
        base[root].right_child = (int16_t)n2;
        base[n1].left_child    = (int16_t)l0;
        base[n1].right_child   = (int16_t)l1;
        base[n2].left_child    = (int16_t)l2;
        base[n2].right_child   = (int16_t)l3;
        node_count = 7;
    } else {
        /* Full depth 3: root + 2 internal + 4 internal + 8 leaves = 15 nodes */
        uint32_t root = build_split(base, &pos, spec->f0, spec->t0);
        uint32_t n1   = build_split(base, &pos, spec->f1, spec->t1);
        uint32_t n2   = build_split(base, &pos, spec->f2, spec->t2);
        uint32_t n3   = build_split(base, &pos, spec->f3, spec->t3);
        uint32_t n4   = build_split(base, &pos, spec->f4, spec->t4);
        uint32_t n5   = build_split(base, &pos, spec->f5, spec->t5);
        uint32_t n6   = build_split(base, &pos, spec->f6, spec->t6);
        uint32_t l0   = build_leaf(base, &pos, spec->leaves[0]);
        uint32_t l1   = build_leaf(base, &pos, spec->leaves[1]);
        uint32_t l2   = build_leaf(base, &pos, spec->leaves[2]);
        uint32_t l3   = build_leaf(base, &pos, spec->leaves[3]);
        uint32_t l4   = build_leaf(base, &pos, spec->leaves[4]);
        uint32_t l5   = build_leaf(base, &pos, spec->leaves[5]);
        uint32_t l6   = build_leaf(base, &pos, spec->leaves[6]);
        uint32_t l7   = build_leaf(base, &pos, spec->leaves[7]);
        base[root].left_child  = (int16_t)n1;
        base[root].right_child = (int16_t)n2;
        base[n1].left_child    = (int16_t)n3;
        base[n1].right_child   = (int16_t)n4;
        base[n2].left_child    = (int16_t)n5;
        base[n2].right_child   = (int16_t)n6;
        base[n3].left_child    = (int16_t)l0;
        base[n3].right_child   = (int16_t)l1;
        base[n4].left_child    = (int16_t)l2;
        base[n4].right_child   = (int16_t)l3;
        base[n5].left_child    = (int16_t)l4;
        base[n5].right_child   = (int16_t)l5;
        base[n6].left_child    = (int16_t)l6;
        base[n6].right_child   = (int16_t)l7;
        node_count = 15;
    }

    return node_count;
}

/* ========================================================================== */
/* Default model tree specifications                                           */
/* ========================================================================== */

/* Feature index shorthand */
#define F_CALLEE_SIZE    0
#define F_CALLEE_NCOUNT  1
#define F_FREQUENCY      2
#define F_CALLER_SIZE    3
#define F_CALL_DEPTH     4
#define F_IS_HOT         5
#define F_HAS_LOOPS      6
#define F_HAS_TRY_CATCH  7
#define F_ALLOCATES      8
#define F_CALLS_VIRTUAL  9
#define F_TYPE_CERTAINTY 10
#define F_CONST_ARG_RATIO 11
#define F_REG_PRESSURE   12
#define F_DEOPT_RATE     13
#define F_INLINE_HISTORY 14

/* Leaf marker for unused depth-3 nodes (depth < 3 uses fewer nodes) */
#define F_NONE 0xFFFF

static const vtx_tree_spec_t default_tree_specs[VTX_GBDT_DEFAULT_TREE_COUNT] = {
    /* ---- Group 1: Callee size features (trees 0-5) ---- */

    /* Tree 0: Very small callee → strong positive */
    { F_CALLEE_SIZE, 0.10f,
      F_IS_HOT, 0.5f,   F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.18f, 0.08f, -0.02f, -0.08f}, 2 },

    /* Tree 1: Small callee + monomorphic → positive */
    { F_CALLEE_SIZE, 0.20f,
      F_TYPE_CERTAINTY, 0.7f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.15f, 0.03f, -0.04f, -0.10f}, 2 },

    /* Tree 2: Large callee → negative */
    { F_CALLEE_SIZE, 0.50f,
      F_CALL_DEPTH, 0.3f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.02f, -0.05f, -0.10f, -0.18f}, 2 },

    /* Tree 3: Small callee + high frequency → positive (depth 3) */
    { F_CALLEE_SIZE, 0.15f,
      F_FREQUENCY, 0.3f, F_FREQUENCY, 0.1f,
      F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.05f, 0.12f, -0.02f, 0.02f, -0.05f, -0.08f, -0.02f, -0.12f}, 3 },

    /* Tree 4: Medium callee + constant args → slight positive */
    { F_CALLEE_SIZE, 0.40f,
      F_CONST_ARG_RATIO, 0.5f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.08f, 0.02f, -0.03f, -0.08f}, 2 },

    /* Tree 5: Large caller + small callee → beneficial to inline */
    { F_CALLER_SIZE, 0.50f,
      F_CALLEE_SIZE, 0.20f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.03f, 0.10f, -0.03f, -0.06f}, 2 },

    /* ---- Group 2: Call frequency features (trees 6-11) ---- */

    /* Tree 6: Hot + monomorphic → strong positive */
    { F_IS_HOT, 0.5f,
      F_TYPE_CERTAINTY, 0.8f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.15f, 0.05f, -0.02f, -0.08f}, 2 },

    /* Tree 7: Hot + small → positive */
    { F_IS_HOT, 0.5f,
      F_CALLEE_SIZE, 0.30f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.12f, 0.04f, -0.04f, -0.10f}, 2 },

    /* Tree 8: Not hot → negative */
    { F_IS_HOT, 0.5f,
      F_CALLEE_SIZE, 0.15f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {-0.02f, -0.08f, 0.03f, -0.03f}, 2 },

    /* Tree 9: Very high frequency (depth 3) → positive with refinement */
    { F_FREQUENCY, 0.50f,
      F_CALLEE_SIZE, 0.30f, F_CALLEE_SIZE, 0.15f,
      F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.03f, 0.10f, 0.02f, 0.06f, -0.04f, -0.02f, -0.06f, -0.10f}, 3 },

    /* Tree 10: Moderate frequency + monomorphic → slight positive */
    { F_FREQUENCY, 0.15f,
      F_TYPE_CERTAINTY, 0.6f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.08f, 0.02f, -0.02f, -0.06f}, 2 },

    /* Tree 11: Low frequency → negative */
    { F_FREQUENCY, 0.05f,
      F_CALLEE_SIZE, 0.30f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.02f, -0.04f, -0.08f, -0.12f}, 2 },

    /* ---- Group 3: Receiver type certainty (trees 12-17) ---- */

    /* Tree 12: Monomorphic + small → strong positive */
    { F_TYPE_CERTAINTY, 0.8f,
      F_CALLEE_SIZE, 0.20f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.18f, 0.06f, 0.03f, -0.04f}, 2 },

    /* Tree 13: Monomorphic + hot → positive */
    { F_TYPE_CERTAINTY, 0.8f,
      F_IS_HOT, 0.5f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.14f, 0.04f, 0.02f, -0.05f}, 2 },

    /* Tree 14: Megamorphic → negative */
    { F_TYPE_CERTAINTY, 0.2f,
      F_CALLEE_SIZE, 0.40f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {-0.04f, -0.10f, 0.02f, -0.02f}, 2 },

    /* Tree 15: Polymorphic + small → slight positive (depth 3) */
    { F_TYPE_CERTAINTY, 0.5f,
      F_CALLEE_SIZE, 0.25f, F_IS_HOT, 0.5f,
      F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.06f, 0.02f, 0.01f, -0.02f, -0.04f, -0.06f, -0.02f, -0.08f}, 3 },

    /* Tree 16: Monomorphic + constant args → positive */
    { F_TYPE_CERTAINTY, 0.7f,
      F_CONST_ARG_RATIO, 0.4f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.10f, 0.03f, 0.02f, -0.04f}, 2 },

    /* Tree 17: Megamorphic + virtual calls → negative (depth 3) */
    { F_TYPE_CERTAINTY, 0.3f,
      F_CALLS_VIRTUAL, 0.5f, F_CALLEE_SIZE, 0.40f,
      F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {-0.02f, -0.06f, -0.04f, -0.08f, 0.01f, -0.02f, 0.02f, -0.03f}, 3 },

    /* ---- Group 4: Penalty features (trees 18-23) ---- */

    /* Tree 18: Deep call depth → negative */
    { F_CALL_DEPTH, 0.40f,
      F_CALLEE_SIZE, 0.30f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.02f, -0.06f, -0.10f, -0.18f}, 2 },

    /* Tree 19: High deopt rate → strong negative */
    { F_DEOPT_RATE, 0.03f,
      F_CALLEE_SIZE, 0.30f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.02f, -0.06f, -0.10f, -0.20f}, 2 },

    /* Tree 20: Deep + has loops → negative (depth 3) */
    { F_CALL_DEPTH, 0.50f,
      F_HAS_LOOPS, 0.5f, F_CALLEE_SIZE, 0.30f,
      F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.01f, -0.03f, -0.05f, -0.08f, -0.03f, -0.06f, -0.08f, -0.14f}, 3 },

    /* Tree 21: High deopt + allocates → negative */
    { F_DEOPT_RATE, 0.05f,
      F_ALLOCATES, 0.5f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {-0.02f, -0.08f, -0.04f, -0.14f}, 2 },

    /* Tree 22: Too many previous inlines → negative */
    { F_INLINE_HISTORY, 0.30f,
      F_CALL_DEPTH, 0.40f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.02f, -0.04f, -0.08f, -0.15f}, 2 },

    /* Tree 23: Deep + large callee → very negative (depth 3) */
    { F_CALL_DEPTH, 0.50f,
      F_CALLEE_SIZE, 0.40f, F_DEOPT_RATE, 0.03f,
      F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.01f, -0.04f, -0.06f, -0.10f, -0.05f, -0.08f, -0.12f, -0.18f}, 3 },

    /* ---- Group 5: Combinations and secondary features (trees 24-29) ---- */

    /* Tree 24: Hot + small + monomorphic → very positive (depth 3) */
    { F_IS_HOT, 0.5f,
      F_CALLEE_SIZE, 0.20f, F_TYPE_CERTAINTY, 0.7f,
      F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.08f, 0.14f, 0.04f, 0.08f, -0.04f, -0.02f, -0.06f, -0.04f}, 3 },

    /* Tree 25: Cold + large → very negative (depth 3) */
    { F_IS_HOT, 0.5f,
      F_CALLEE_SIZE, 0.50f, F_TYPE_CERTAINTY, 0.3f,
      F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {-0.02f, -0.06f, -0.08f, -0.12f, -0.04f, -0.06f, -0.08f, -0.14f}, 3 },

    /* Tree 26: Try/catch + virtual → slight negative */
    { F_HAS_TRY_CATCH, 0.5f,
      F_CALLS_VIRTUAL, 0.5f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {-0.02f, -0.06f, 0.02f, -0.02f}, 2 },

    /* Tree 27: Allocates + hot → neutral/slight positive */
    { F_ALLOCATES, 0.5f,
      F_IS_HOT, 0.5f, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {-0.04f, 0.04f, -0.06f, 0.02f}, 2 },

    /* Tree 28: Has loops + hot → neutral (inlining loop bodies can help) */
    { F_HAS_LOOPS, 0.5f,
      F_IS_HOT, 0.5f, F_CALLEE_SIZE, 0.30f,
      F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {-0.03f, 0.03f, -0.05f, 0.02f, -0.06f, -0.02f, -0.08f, -0.03f}, 3 },

    /* Tree 29: Constant args + no virtual → positive (specialization opportunity) */
    { F_CONST_ARG_RATIO, 0.5f,
      F_CALLS_VIRTUAL, 0.5f, F_CALLEE_SIZE, 0.30f,
      F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0, F_NONE, 0,
      {0.08f, 0.04f, 0.06f, 0.02f, -0.02f, -0.04f, -0.04f, -0.08f}, 3 },
};

#undef F_CALLEE_SIZE
#undef F_CALLEE_NCOUNT
#undef F_FREQUENCY
#undef F_CALLER_SIZE
#undef F_CALL_DEPTH
#undef F_IS_HOT
#undef F_HAS_LOOPS
#undef F_HAS_TRY_CATCH
#undef F_ALLOCATES
#undef F_CALLS_VIRTUAL
#undef F_TYPE_CERTAINTY
#undef F_CONST_ARG_RATIO
#undef F_REG_PRESSURE
#undef F_DEOPT_RATE
#undef F_INLINE_HISTORY
#undef F_NONE

/* Maximum nodes needed for the default model:
 * Worst case: 30 trees * 15 nodes = 450.
 * With mixed depths (some trees have 3, 7, or 15 nodes),
 * the actual count is computed during building. */
#define DEFAULT_MODEL_MAX_NODES 450

/* ========================================================================== */
/* Model lifecycle                                                             */
/* ========================================================================== */

int vtx_gbdt_model_init(vtx_gbdt_model_t *model)
{
    if (model == NULL) return -1;

    memset(model, 0, sizeof(*model));
    model->nodes = NULL;
    model->node_count = 0;
    model->node_capacity = 0;
    model->tree_count = 0;
    model->max_depth = 0;
    model->init_score = 0.0;
    model->owns_nodes = false;

    return 0;
}

void vtx_gbdt_model_destroy(vtx_gbdt_model_t *model)
{
    if (model == NULL) return;

    if (model->owns_nodes && model->nodes != NULL) {
        free(model->nodes);
        model->nodes = NULL;
    }

    model->node_count = 0;
    model->node_capacity = 0;
    model->tree_count = 0;
}

/* ========================================================================== */
/* Load model from flat data array                                             */
/* ========================================================================== */

int vtx_gbdt_load_model(vtx_gbdt_model_t *model, const double *data, uint32_t count)
{
    if (model == NULL || data == NULL || count < 3) return -1;

    /* Parse header:
     * [0] = init_score
     * [1] = tree_count
     * [2] = max_depth */
    double init_score = data[0];
    uint32_t tree_count = (uint32_t)data[1];
    uint32_t max_depth = (uint32_t)data[2];

    if (tree_count > VTX_GBDT_MAX_TREES) return -1;
    if (max_depth > VTX_GBDT_MAX_DEPTH) return -1;

    /* First pass: count total nodes */
    uint32_t total_nodes = 0;
    uint32_t pos = 3;
    for (uint32_t t = 0; t < tree_count; t++) {
        if (pos >= count) return -1;
        uint32_t nc = (uint32_t)data[pos];
        if (nc > VTX_GBDT_MAX_NODES_PER_TREE) return -1;
        total_nodes += nc;
        pos += 1 + nc * 5;
    }

    if (pos > count) return -1;

    /* Allocate node array */
    vtx_gbdt_node_t *nodes = (vtx_gbdt_node_t *)malloc(
        total_nodes * sizeof(vtx_gbdt_node_t));
    if (nodes == NULL) return -1;

    /* Free previous allocation if owned */
    if (model->owns_nodes && model->nodes != NULL) {
        free(model->nodes);
    }

    model->nodes = nodes;
    model->node_capacity = total_nodes;
    model->owns_nodes = true;
    model->init_score = init_score;
    model->tree_count = tree_count;
    model->max_depth = max_depth;

    /* Second pass: parse nodes */
    pos = 3;
    uint32_t node_offset = 0;
    for (uint32_t t = 0; t < tree_count; t++) {
        uint32_t nc = (uint32_t)data[pos];
        pos++;

        model->trees[t].root_index = node_offset;
        model->trees[t].node_count = nc;

        for (uint32_t n = 0; n < nc; n++) {
            if (pos + 4 >= count) {
                free(nodes);
                model->nodes = NULL;
                model->tree_count = 0;
                return -1;
            }

            vtx_gbdt_node_t *node = &nodes[node_offset + n];
            node->feature_index = (uint16_t)data[pos++];
            node->threshold     = (float)data[pos++];
            node->left_child    = (int16_t)data[pos++];
            node->right_child   = (int16_t)data[pos++];
            node->leaf_value    = (float)data[pos++];

            /* Validate: feature index must be in range or LEAF_MARKER */
            if (node->feature_index != VTX_GBDT_LEAF_MARKER &&
                node->feature_index >= VTX_INLINE_FEATURE_COUNT) {
                free(nodes);
                model->nodes = NULL;
                model->tree_count = 0;
                return -1;
            }

            /* Validate: child indices must be non-negative for internal nodes */
            if (node->feature_index != VTX_GBDT_LEAF_MARKER) {
                if (node->left_child < 0 || node->right_child < 0 ||
                    (uint32_t)node->left_child >= nc ||
                    (uint32_t)node->right_child >= nc) {
                    free(nodes);
                    model->nodes = NULL;
                    model->tree_count = 0;
                    return -1;
                }
            }
        }

        node_offset += nc;
    }

    model->node_count = node_offset;
    return 0;
}

/* ========================================================================== */
/* Load default model                                                          */
/* ========================================================================== */

int vtx_gbdt_load_default_model(vtx_gbdt_model_t *model)
{
    if (model == NULL) return -1;

    /* Free previous allocation if owned */
    if (model->owns_nodes && model->nodes != NULL) {
        free(model->nodes);
    }

    /* Allocate node array for the default model.
     * Worst case: 30 trees * 15 nodes = 450. */
    vtx_gbdt_node_t *nodes = (vtx_gbdt_node_t *)malloc(
        DEFAULT_MODEL_MAX_NODES * sizeof(vtx_gbdt_node_t));
    if (nodes == NULL) return -1;

    model->nodes = nodes;
    model->node_count = 0;
    model->node_capacity = DEFAULT_MODEL_MAX_NODES;
    model->owns_nodes = true;
    model->init_score = 0.0;
    model->tree_count = VTX_GBDT_DEFAULT_TREE_COUNT;
    model->max_depth = VTX_GBDT_DEFAULT_DEPTH;

    /* Build each tree from its specification */
    uint32_t node_offset = 0;

    for (uint32_t t = 0; t < VTX_GBDT_DEFAULT_TREE_COUNT; t++) {
        const vtx_tree_spec_t *spec = &default_tree_specs[t];

        model->trees[t].root_index = node_offset;

        uint32_t nc = build_tree_from_spec(nodes, node_offset, spec);
        model->trees[t].node_count = nc;
        node_offset += nc;
    }

    model->node_count = node_offset;

    /* Clear remaining tree slots */
    for (uint32_t t = VTX_GBDT_DEFAULT_TREE_COUNT; t < VTX_GBDT_MAX_TREES; t++) {
        model->trees[t].root_index = 0;
        model->trees[t].node_count = 0;
    }

    return 0;
}

/* ========================================================================== */
/* Inference                                                                   */
/* ========================================================================== */

/**
 * Traverse a single tree from root to leaf.
 * Returns the leaf value.
 */
static double traverse_tree(const vtx_gbdt_node_t *nodes,
                             uint32_t root_index,
                             const double *features)
{
    uint32_t idx = root_index;

    for (;;) {
        const vtx_gbdt_node_t *node = &nodes[idx];

        /* If this is a leaf, return its value */
        if (node->feature_index == VTX_GBDT_LEAF_MARKER) {
            return (double)node->leaf_value;
        }

        /* Decision: go left if feature < threshold, right otherwise */
        double feature_val = features[node->feature_index];
        if (feature_val < (double)node->threshold) {
            idx = (uint32_t)node->left_child;
        } else {
            idx = (uint32_t)node->right_child;
        }

        /* Safety: detect cycles (should never happen with valid model) */
        VTX_ASSERT(idx != root_index || node->feature_index == VTX_GBDT_LEAF_MARKER,
                   "GBDT tree contains a cycle");
    }
}

double vtx_gbdt_infer(const vtx_gbdt_model_t *model,
                       const vtx_inline_features_t *features)
{
    if (model == NULL || features == NULL || model->nodes == NULL) {
        /* No model or features: don't inline (score = 0) */
        return 0.0;
    }

    if (model->tree_count == 0) {
        return vtx_sigmoid(model->init_score);
    }

    /* Sum leaf values across all trees */
    double raw_score = model->init_score;

    for (uint32_t t = 0; t < model->tree_count; t++) {
        const vtx_gbdt_tree_desc_t *tree = &model->trees[t];
        if (tree->node_count == 0) continue;

        double leaf_val = traverse_tree(model->nodes, tree->root_index,
                                         features->features);
        raw_score += leaf_val;
    }

    /* Apply sigmoid to get [0, 1] output */
    return vtx_sigmoid(raw_score);
}
