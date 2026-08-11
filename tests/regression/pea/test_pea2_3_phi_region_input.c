/*
 * VORTEX PEA-2-3 Regression Test
 *
 * Bug: In resolve_virtual_phis (virtual.c), the per-field Phi creation
 * loop iterated ALL inputs of the original Phi, including the Region/
 * LoopBegin control input. When the input was a control node (not a
 * virtual allocation), the code fell through to the null-constant
 * path and added a VTX_OP_Constant(NULL) as the field Phi's input
 * instead of passing the control input through.
 *
 * Fix: When the input is a control node (Region/LoopBegin), pass it
 * through directly to the field Phi instead of creating a null constant.
 *
 * Test: Construct a graph with a Phi that merges two allocations.
 * Run the virtual pass with a pre-built analysis marking both as
 * NoEscape. Verify the per-field Phi created by resolve_virtual_phis
 * has the Region among its inputs (not a null constant replacing it).
 */

#include "pea_test_setup.h"

VTX_TEST(pea23_per_field_phi_has_region_input)
{
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_graph_t graph; vtx_graph_init(&graph, 0);

    /* Create a Region node (merge point) */
    vtx_nodeid_t region = vtx_node_create(&graph.node_table, VTX_OP_Region);
    VTX_ASSERT_TRUE(region != VTX_NODEID_INVALID);

    /* Create two NewObject nodes (the virtual objects to merge) */
    vtx_nodeid_t alloc1 = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    vtx_nodeid_t alloc2 = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    VTX_ASSERT_TRUE(alloc1 != VTX_NODEID_INVALID);
    VTX_ASSERT_TRUE(alloc2 != VTX_NODEID_INVALID);

    vtx_node_t *a1 = vtx_node_get(&graph.node_table, alloc1);
    a1->type_id = 0;
    vtx_node_t *a2 = vtx_node_get(&graph.node_table, alloc2);
    a2->type_id = 0;

    /* Create StoreField nodes to give the allocations a field at offset 0.
     * Without fields, resolve_virtual_phis creates no per-field Phis. */
    vtx_nodeid_t val1 = vtx_node_create(&graph.node_table, VTX_OP_Constant);
    vtx_nodeid_t val2 = vtx_node_create(&graph.node_table, VTX_OP_Constant);
    vtx_nodeid_t sf1 = vtx_node_create(&graph.node_table, VTX_OP_StoreField);
    vtx_nodeid_t sf2 = vtx_node_create(&graph.node_table, VTX_OP_StoreField);
    vtx_node_t *sf1_n = vtx_node_get(&graph.node_table, sf1);
    vtx_node_t *sf2_n = vtx_node_get(&graph.node_table, sf2);
    sf1_n->field_offset = 0;
    sf2_n->field_offset = 0;
    /* StoreField inputs: [memory, receiver, value] */
    vtx_node_add_input(&graph.node_table, sf1, alloc1); /* memory chain */
    vtx_node_add_input(&graph.node_table, sf1, alloc1); /* receiver */
    vtx_node_add_input(&graph.node_table, sf1, val1);   /* value */
    vtx_node_add_input(&graph.node_table, sf2, alloc2); /* memory chain */
    vtx_node_add_input(&graph.node_table, sf2, alloc2); /* receiver */
    vtx_node_add_input(&graph.node_table, sf2, val2);   /* value */

    /* Create a Phi that merges the two allocations.
     * Per VORTEX's convention: [data_0, data_1, ..., Region] */
    vtx_nodeid_t phi = vtx_node_create(&graph.node_table, VTX_OP_Phi);
    VTX_ASSERT_TRUE(phi != VTX_NODEID_INVALID);
    vtx_node_add_input(&graph.node_table, phi, alloc1);
    vtx_node_add_input(&graph.node_table, phi, alloc2);
    vtx_node_add_input(&graph.node_table, phi, region);

    /* Build analysis with both allocations as NoEscape */
    vtx_pea_analysis_t analysis;
    vtx_nodeid_t allocs[2] = { alloc1, alloc2 };
    VTX_ASSERT_EQUAL(
        vtx_pea_test_build_analysis(&arena, &graph, allocs, 2, &analysis),
        0);

    /* Run virtual pass (calls classify_allocations then resolve_virtual_phis) */
    vtx_virtual_result_t *vr = vtx_virtual_run(&graph, &analysis, &arena);
    VTX_ASSERT_NOT_NULL(vr);

    /* The per-field Phi should have been created. Find it by scanning
     * for Phi nodes created after the original phi. */
    bool found_field_phi = false;
    bool found_region_in_field_phi = false;
    for (uint32_t i = phi + 1; i < graph.node_table.count; i++) {
        vtx_node_t *n = vtx_node_get(&graph.node_table, i);
        if (!n || n->dead) continue;
        if (n->opcode != VTX_OP_Phi) continue;

        found_field_phi = true;
        for (uint32_t inp = 0; inp < n->input_count; inp++) {
            vtx_nodeid_t input_id = n->inputs[inp];
            vtx_node_t *input_node = vtx_node_get(&graph.node_table, input_id);
            if (input_node && input_node->opcode == VTX_OP_Region) {
                found_region_in_field_phi = true;
                break;
            }
        }
    }

    VTX_ASSERT_TRUE(found_field_phi);
    VTX_ASSERT_TRUE(found_region_in_field_phi);

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nPEA-2-3 regression: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    return (result.fail_count > 0) ? 1 : 0;
}
