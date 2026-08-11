/*
 * VORTEX PEA-2-11 Regression Test
 *
 * Bug: rewrite_virtual_field_accesses in src/pea/virtual.c marks
 * StoreField (and LoadField) nodes `dead = true` after rewriting
 * their effect into the virtual field map, but does NOT remove the
 * node's input edges. The receiver/value/memory-chain producers
 * therefore retain inflated output_count, and a later DCE pass
 * relying on `output_count == 0` cannot collect them — the
 * (dead) inputs linger in the graph, may keep the virtual
 * allocation alive, and prevent downstream optimizations.
 *
 * Fix: before marking the node dead, call vtx_node_remove_input on
 * every input slot (via the new node_clear_inputs helper) to
 * decrement each producer's output_count.
 *
 * Test:
 *   - Build one NewObject A (virtual / NoEscape).
 *   - Build a Constant V (value).
 *   - Build StoreField A.f0 = V with inputs [A, V]. Pre-fix, after
 *     vtx_virtual_run, A.output_count remains 1 (V) + 1 (StoreField
 *     receiver) + 1 (StoreField memory chain) = inflated.
 *   - Post-fix, after vtx_virtual_run, A.output_count should equal
 *     the number of NON-StoreField users — i.e. 0 in this test
 *     (the StoreField was the only user and its edges are gone).
 *
 * Verification:
 *   1. vtx_virtual_run returns non-NULL.
 *   2. The StoreField node is marked dead.
 *   3. A.output_count == 0 (PEA-2-11: the StoreField's input edges
 *      to A were dropped).
 *   4. V.output_count == 0 (the value edge was also dropped).
 *
 * Per CRITICAL REPRODUCER CONSTRAINT: this is a genuine end-to-end
 * reproducer — it directly inspects the post-pass output_count on
 * the producer nodes, which is the exact observable that PEA-2-11
 * addresses. Pre-fix, assertions 3 and 4 fail (output_count stays
 * at 1 because the StoreField never decremented it).
 */

#include "pea_test_setup.h"

VTX_TEST(pea2_11_dead_storefield_drops_input_output_counts)
{
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 0), 0);

    /* NewObject A — virtual. */
    vtx_nodeid_t alloc_a = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    VTX_ASSERT_TRUE(alloc_a != VTX_NODEID_INVALID);
    vtx_node_t *a_n = vtx_node_get(&graph.node_table, alloc_a);
    a_n->type_id = 0;

    /* Constant V — value stored. */
    vtx_nodeid_t val = vtx_node_create(&graph.node_table, VTX_OP_Constant);
    VTX_ASSERT_TRUE(val != VTX_NODEID_INVALID);
    vtx_node_t *v_n = vtx_node_get(&graph.node_table, val);
    v_n->type = VTX_TYPE_Int;

    /* StoreField A.f0 = V. Inputs: [mem=A, recv=A, value=V]. */
    vtx_nodeid_t sf = vtx_node_create(&graph.node_table, VTX_OP_StoreField);
    VTX_ASSERT_TRUE(sf != VTX_NODEID_INVALID);
    vtx_node_t *sf_n = vtx_node_get(&graph.node_table, sf);
    sf_n->field_offset = 0;
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, sf, alloc_a), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, sf, alloc_a), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, sf, val), 0);

    /* Pre-pass sanity: A has 2 users (mem + recv of StoreField),
     * V has 1 user (value of StoreField). */
    vtx_node_t *a_pre = vtx_node_get(&graph.node_table, alloc_a);
    vtx_node_t *v_pre = vtx_node_get(&graph.node_table, val);
    VTX_ASSERT_EQUAL(a_pre->output_count, 2);
    VTX_ASSERT_EQUAL(v_pre->output_count, 1);

    /* Build analysis: A is NoEscape → virtual. */
    vtx_pea_analysis_t analysis;
    vtx_nodeid_t allocs[1] = { alloc_a };
    VTX_ASSERT_EQUAL(
        vtx_pea_test_build_analysis(&arena, &graph, allocs, 1, &analysis),
        0);

    vtx_virtual_result_t *result = vtx_virtual_run(&graph, &analysis, &arena);
    VTX_ASSERT_NOT_NULL(result);

    /* 1. StoreField was rewritten → dead. */
    vtx_node_t *sf_post = vtx_node_get(&graph.node_table, sf);
    VTX_ASSERT_NOT_NULL(sf_post);
    VTX_ASSERT_TRUE(sf_post->dead);

    /* 2. PEA-2-11: the StoreField's input edges were dropped, so the
     *    producers' output_count is decremented. Pre-fix, both stayed
     *    at their pre-pass values. */
    vtx_node_t *a_post = vtx_node_get(&graph.node_table, alloc_a);
    vtx_node_t *v_post = vtx_node_get(&graph.node_table, val);
    VTX_ASSERT_NOT_NULL(a_post);
    VTX_ASSERT_NOT_NULL(v_post);
    VTX_ASSERT_EQUAL(a_post->output_count, 0);
    VTX_ASSERT_EQUAL(v_post->output_count, 0);

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    printf("=== PEA-2-11 regression: dead StoreField drops input output_count ===\n\n");
    vtx_test_result_t r = vtx_test_run_all();
    printf("\nPEA-2-11 regression: %u passed, %u failed, %u total\n",
           r.pass_count, r.fail_count, r.total_count);
    return (r.fail_count > 0) ? 1 : 0;
}
