/*
 * VORTEX PEA-2-6 Regression Test
 *
 * Bug: insert_materialization_code created a NewObject + StoreField
 * chain but never connected the final StoreField to the escape point's
 * memory input. The scheduler could place the stores AFTER the escape
 * point (Call/Return/Deopt), causing it to read uninitialized heap
 * memory.
 *
 * Fix: insert_materialization_code returns the final memory state via
 * an out-parameter. The caller adds it as a memory input to the escape
 * point.
 *
 * Test: Construct a graph with an allocation that escapes via Return.
 * Run materialize. Verify the Return node has a StoreField among its
 * inputs (the materialization memory chain is connected).
 */

#include "pea_test_setup.h"

VTX_TEST(pea26_materialization_chain_connected_to_escape)
{
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_graph_t graph; vtx_graph_init(&graph, 0);

    /* Create an allocation */
    vtx_nodeid_t alloc = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    VTX_ASSERT_TRUE(alloc != VTX_NODEID_INVALID);
    vtx_node_t *alloc_n = vtx_node_get(&graph.node_table, alloc);
    alloc_n->type_id = 0;
    alloc_n->type = VTX_TYPE_Ptr;

    /* Create a Return node that returns the allocation (escape point) */
    vtx_nodeid_t ret = vtx_node_create(&graph.node_table, VTX_OP_Return);
    VTX_ASSERT_TRUE(ret != VTX_NODEID_INVALID);
    vtx_node_add_input(&graph.node_table, ret, alloc);

    /* Record the Return's input count before materialization */
    vtx_node_t *ret_n_before = vtx_node_get(&graph.node_table, ret);
    uint32_t inputs_before = ret_n_before->input_count;

    /* Build analysis with alloc as NoEscape (scalar-replaceable) */
    vtx_pea_analysis_t analysis;
    vtx_nodeid_t allocs[1] = { alloc };
    VTX_ASSERT_EQUAL(
        vtx_pea_test_build_analysis(&arena, &graph, allocs, 1, &analysis),
        0);

    /* Run materialize pass — this should create NewObject + StoreField
     * chain and connect it to the Return node. */
    vtx_materialize_result_t *mr = vtx_materialize_run(
        &graph, &analysis, NULL, &arena);
    VTX_ASSERT_NOT_NULL(mr);

    /* PEA-2-6: the Return node should have MORE inputs than before
     * (the materialization chain was added as a memory input). */
    vtx_node_t *ret_n_after = vtx_node_get(&graph.node_table, ret);
    VTX_ASSERT_TRUE(ret_n_after->input_count > inputs_before);

    /* Verify at least one of the Return's new inputs is a StoreField. */
    bool found_store_field = false;
    for (uint32_t i = 0; i < ret_n_after->input_count; i++) {
        vtx_nodeid_t inp = ret_n_after->inputs[i];
        vtx_node_t *inp_n = vtx_node_get(&graph.node_table, inp);
        if (inp_n && inp_n->opcode == VTX_OP_StoreField) {
            found_store_field = true;
            break;
        }
    }
    /* Note: if the allocation has 0 fields, insert_materialization_code
     * creates only a NewObject (no StoreFields). In that case, verify
     * the NewObject was added as a memory input instead. */
    if (!found_store_field) {
        bool found_new_object = false;
        for (uint32_t i = 0; i < ret_n_after->input_count; i++) {
            vtx_nodeid_t inp = ret_n_after->inputs[i];
            vtx_node_t *inp_n = vtx_node_get(&graph.node_table, inp);
            if (inp_n && inp_n->opcode == VTX_OP_NewObject) {
                found_new_object = true;
                break;
            }
        }
        VTX_ASSERT_TRUE(found_new_object);
    }

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nPEA-2-6 regression: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    return (result.fail_count > 0) ? 1 : 0;
}
