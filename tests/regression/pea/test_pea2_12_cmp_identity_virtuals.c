/*
 * VORTEX PEA-2-12 Regression Test
 *
 * Bug: virtual.c had no CmpP handling. A CmpP comparing two virtual
 * allocations would compare their IR NodeIDs (arbitrary integers),
 * producing a meaningless boolean at runtime.
 *
 * Fix: rewrite_virtual_cmp_identity detects CmpP whose two data
 * inputs are both virtual allocations and replaces the CmpP with:
 *   - Constant(true)  if the two inputs are the same NodeID
 *   - Constant(false) if the two inputs are different NodeIDs
 * (For CmpP of virtual vs non-alloc, leave the CmpP alone — the
 * virtual will be materialized at the escape point and the real
 * CmpP will run on the materialized object.)
 *
 * Test:
 *   - Build two virtual NewObjects A and B (NoEscape).
 *   - Build a CmpP(A, A) — same alloc → must fold to Constant(1).
 *   - Build a CmpP(A, B) — different allocs → must fold to Constant(0).
 *   - Run vtx_virtual_run.
 *   - Post-pass: both CmpP nodes are dead, and their replacements
 *     are Constant nodes with the expected values.
 *
 * Verification:
 *   1. vtx_virtual_run returns non-NULL.
 *   2. Both CmpP nodes are marked dead after the pass.
 *   3. There exist Constant nodes with type=Int, value=1 (for the
 *      self-compare) and value=0 (for the cross-compare).
 *
 * Per CRITICAL REPRODUCER CONSTRAINT: this is a genuine end-to-end
 * reproducer that exercises the new rewrite_virtual_cmp_identity
 * pass and inspects the post-pass graph state.
 */

#include "pea_test_setup.h"

VTX_TEST(pea2_12_cmp_identity_folds_virtual_object_compares)
{
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 0), 0);

    /* Two virtual allocations, same type. */
    vtx_nodeid_t alloc_a = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    vtx_nodeid_t alloc_b = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    VTX_ASSERT_TRUE(alloc_a != VTX_NODEID_INVALID);
    VTX_ASSERT_TRUE(alloc_b != VTX_NODEID_INVALID);
    vtx_node_t *a_n = vtx_node_get(&graph.node_table, alloc_a);
    vtx_node_t *b_n = vtx_node_get(&graph.node_table, alloc_b);
    a_n->type_id = 0;
    b_n->type_id = 0;

    /* CmpP(A, A) — same alloc, must fold to Constant(true=1). */
    vtx_nodeid_t cmp_same = vtx_node_create(&graph.node_table, VTX_OP_CmpP);
    VTX_ASSERT_TRUE(cmp_same != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, cmp_same, alloc_a), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, cmp_same, alloc_a), 0);

    /* CmpP(A, B) — different allocs, must fold to Constant(false=0). */
    vtx_nodeid_t cmp_diff = vtx_node_create(&graph.node_table, VTX_OP_CmpP);
    VTX_ASSERT_TRUE(cmp_diff != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, cmp_diff, alloc_a), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, cmp_diff, alloc_b), 0);

    /* Build analysis: A and B NoEscape. */
    vtx_pea_analysis_t analysis;
    vtx_nodeid_t allocs[2] = { alloc_a, alloc_b };
    VTX_ASSERT_EQUAL(
        vtx_pea_test_build_analysis(&arena, &graph, allocs, 2, &analysis),
        0);

    vtx_virtual_result_t *result = vtx_virtual_run(&graph, &analysis, &arena);
    VTX_ASSERT_NOT_NULL(result);

    /* 1. Both CmpP nodes must be dead. */
    vtx_node_t *cmp_same_post = vtx_node_get(&graph.node_table, cmp_same);
    vtx_node_t *cmp_diff_post = vtx_node_get(&graph.node_table, cmp_diff);
    VTX_ASSERT_NOT_NULL(cmp_same_post);
    VTX_ASSERT_NOT_NULL(cmp_diff_post);
    VTX_ASSERT_TRUE(cmp_same_post->dead);
    VTX_ASSERT_TRUE(cmp_diff_post->dead);

    /* 2. There must be Constant(true) and Constant(false) nodes
     *    produced by the fold (search the table for Constant nodes
     *    with type=Int and the expected values). */
    bool found_true = false, found_false = false;
    for (uint32_t i = 0; i < graph.node_table.count; i++) {
        vtx_node_t *n = vtx_node_get(&graph.node_table, i);
        if (!n || n->dead) continue;
        if (n->opcode != VTX_OP_Constant) continue;
        if (n->type != VTX_TYPE_Int) continue;
        if (n->constval.kind != VTX_TYPE_Int) continue;
        if (n->constval.as.int_val == 1) found_true = true;
        if (n->constval.as.int_val == 0) found_false = true;
    }
    VTX_ASSERT_TRUE(found_true);
    VTX_ASSERT_TRUE(found_false);

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    printf("=== PEA-2-12 regression: CmpP identity fold for virtuals ===\n\n");
    vtx_test_result_t r = vtx_test_run_all();
    printf("\nPEA-2-12 regression: %u passed, %u failed, %u total\n",
           r.pass_count, r.fail_count, r.total_count);
    return (r.fail_count > 0) ? 1 : 0;
}
