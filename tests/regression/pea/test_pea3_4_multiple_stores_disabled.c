/*
 * VORTEX PEA-3-4 Regression Test
 *
 * Bug: cross_object_sr.c's LoadField value resolution does a reverse
 * linear scan and returns the LAST-inserted mapping for
 * (alloc_id, field_offset). With multiple reaching StoreFields
 * (across branches, or across loop iterations), this picks an
 * arbitrary one — silently miscompiling code that uses the field
 * after a merge.
 *
 * Surgical fix (per audit): if more than one StoreField reaches a
 * LoadField's (alloc_id, field_offset), DISABLE the rewrite for
 * that LoadField — leave it in place so it falls back to the
 * original heap load (correct, unoptimized). A proper fix requires
 * synthesizing Phi nodes at merge points — deferred.
 *
 * Test:
 *   - Build one NewObject A (NoEscape).
 *   - Build two StoreField nodes to A.f0 with DIFFERENT values
 *     (simulating branch-conditional stores).
 *   - Build a LoadField on A.f0.
 *   - Run vtx_cross_object_sr_run.
 *   - Verify the LoadField is NOT marked dead (rewrite was skipped).
 *   - Verify the allocation is NOT marked dead (LoadField keeps it alive).
 *
 * Verification:
 *   1. vtx_cross_object_sr_run returns non-NULL.
 *   2. The LoadField node is NOT marked dead (PEA-3-4: rewrite skipped).
 *   3. The NewObject alloc is NOT marked dead (PEA-3-4: LoadField
 *      still uses it).
 *
 * Per CRITICAL REPRODUCER CONSTRAINT: this is a genuine end-to-end
 * reproducer that exercises the new "multiple reaching stores →
 * skip rewrite" path.
 */

#include "pea_test_setup.h"

VTX_TEST(pea3_4_multiple_stores_disables_loadfield_rewrite)
{
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 0), 0);

    /* NewObject A — virtual / NoEscape. */
    vtx_nodeid_t alloc_a = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    VTX_ASSERT_TRUE(alloc_a != VTX_NODEID_INVALID);
    vtx_node_t *a_n = vtx_node_get(&graph.node_table, alloc_a);
    a_n->type_id = 0;

    /* Two distinct value Constants. */
    vtx_nodeid_t val1 = vtx_node_create(&graph.node_table, VTX_OP_Constant);
    vtx_nodeid_t val2 = vtx_node_create(&graph.node_table, VTX_OP_Constant);
    VTX_ASSERT_TRUE(val1 != VTX_NODEID_INVALID);
    VTX_ASSERT_TRUE(val2 != VTX_NODEID_INVALID);

    /* Two StoreField nodes to A.f0 — simulates branch-conditional
     * stores reaching a merge point. */
    vtx_nodeid_t sf1 = vtx_node_create(&graph.node_table, VTX_OP_StoreField);
    vtx_nodeid_t sf2 = vtx_node_create(&graph.node_table, VTX_OP_StoreField);
    VTX_ASSERT_TRUE(sf1 != VTX_NODEID_INVALID);
    VTX_ASSERT_TRUE(sf2 != VTX_NODEID_INVALID);
    vtx_node_t *sf1_n = vtx_node_get(&graph.node_table, sf1);
    vtx_node_t *sf2_n = vtx_node_get(&graph.node_table, sf2);
    sf1_n->field_offset = 0;
    sf2_n->field_offset = 0;
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, sf1, alloc_a), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, sf1, alloc_a), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, sf1, val1), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, sf2, alloc_a), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, sf2, alloc_a), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, sf2, val2), 0);

    /* LoadField on A.f0 — reached by BOTH stores. */
    vtx_nodeid_t load_id = vtx_node_create(&graph.node_table, VTX_OP_LoadField);
    VTX_ASSERT_TRUE(load_id != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, load_id, alloc_a), 0);
    vtx_node_t *load_n = vtx_node_get(&graph.node_table, load_id);
    load_n->field_offset = 0;
    load_n->type = VTX_TYPE_Int;

    /* Build analysis: A is NoEscape. */
    vtx_pea_analysis_t analysis;
    vtx_nodeid_t allocs[1] = { alloc_a };
    VTX_ASSERT_EQUAL(
        vtx_pea_test_build_analysis(&arena, &graph, allocs, 1, &analysis),
        0);

    vtx_cross_sr_result_t *result = vtx_cross_object_sr_run(
        &graph, &analysis, &arena);
    VTX_ASSERT_NOT_NULL(result);

    /* 1. PEA-3-4: LoadField must NOT be rewritten (multiple reaching
     *    stores detected → rewrite skipped → LoadField left in place
     *    for correctness). Pre-fix, it was marked dead and rewired
     *    to whichever StoreField happened to be last in the mapping
     *    array — silently miscompiling. */
    vtx_node_t *load_post = vtx_node_get(&graph.node_table, load_id);
    VTX_ASSERT_NOT_NULL(load_post);
    VTX_ASSERT_FALSE(load_post->dead);

    /* 2. The alloc must NOT be marked dead — the still-alive
     *    LoadField holds a remaining use on it. (The alloc_fully_replaced
     *    scan in rewrite_scalar_replacements still considers a live
     *    LoadField as a remaining use.) */
    vtx_node_t *a_post = vtx_node_get(&graph.node_table, alloc_a);
    VTX_ASSERT_NOT_NULL(a_post);
    VTX_ASSERT_FALSE(a_post->dead);

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    printf("=== PEA-3-4 regression: multiple reaching stores disables SR ===\n\n");
    vtx_test_result_t r = vtx_test_run_all();
    printf("\nPEA-3-4 regression: %u passed, %u failed, %u total\n",
           r.pass_count, r.fail_count, r.total_count);
    return (r.fail_count > 0) ? 1 : 0;
}
