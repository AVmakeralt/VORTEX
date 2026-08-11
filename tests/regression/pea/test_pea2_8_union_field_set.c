/*
 * VORTEX PEA-2-8 Regression Test
 *
 * Bug: resolve_virtual_phis in src/pea/virtual.c only inspected the
 * field set of the FIRST virtual input when deciding which per-field
 * Phis to create. If two virtual inputs had different field sets
 * (e.g. vobj1 = {fields 0,1}, vobj2 = {fields 0,2}), vobj2's field 2
 * never got a per-field Phi, and any subsequent LoadField(field 2)
 * resolved to VTX_NODEID_INVALID — reading dead memory.
 *
 * Fix: build the UNION of all virtual inputs' field offsets first,
 * then create one per-field Phi per offset in the union. Missing
 * inputs fall through to the existing null-constant fallback.
 *
 * Test: Construct two virtual allocations A and B where:
 *   - A has StoreField at field_offset 0 (value val_a0)
 *   - B has StoreField at field_offset 1 (value val_b1)
 * Build a Phi merging A and B with a Region control input.
 * Run vtx_virtual_run. After the pass:
 *   - Two per-field Phis must exist (one for offset 0, one for offset 1).
 *   - vobj for the merged Phi must have field_count == 2 (the union).
 *
 * Verification:
 *   1. vtx_virtual_run returns non-NULL.
 *   2. result->phis_resolved >= 1.
 *   3. The merged Phi P is marked VTX_VIRTUAL_YES.
 *   4. vtx_virtual_get_field(result, P_id, 0) != INVALID
 *      AND vtx_virtual_get_field(result, P_id, 1) != INVALID
 *      (both fields of the union have per-field Phis).
 */

#include "pea_test_setup.h"

VTX_TEST(pea2_8_union_field_set_creates_phi_for_both_fields)
{
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 0), 0);

    /* Two virtual allocations. */
    vtx_nodeid_t alloc_a = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    vtx_nodeid_t alloc_b = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    VTX_ASSERT_TRUE(alloc_a != VTX_NODEID_INVALID);
    VTX_ASSERT_TRUE(alloc_b != VTX_NODEID_INVALID);
    vtx_node_t *a_n = vtx_node_get(&graph.node_table, alloc_a);
    vtx_node_t *b_n = vtx_node_get(&graph.node_table, alloc_b);
    a_n->type_id = 7;
    b_n->type_id = 7;  /* same type so the Phi is classified virtual */

    /* Two distinct value constants. */
    vtx_nodeid_t val_a0 = vtx_node_create(&graph.node_table, VTX_OP_Constant);
    vtx_nodeid_t val_b1 = vtx_node_create(&graph.node_table, VTX_OP_Constant);
    VTX_ASSERT_TRUE(val_a0 != VTX_NODEID_INVALID);
    VTX_ASSERT_TRUE(val_b1 != VTX_NODEID_INVALID);

    /* A.f0 = val_a0 ; B.f1 = val_b1 (different field sets). */
    vtx_nodeid_t sf_a = vtx_node_create(&graph.node_table, VTX_OP_StoreField);
    vtx_nodeid_t sf_b = vtx_node_create(&graph.node_table, VTX_OP_StoreField);
    VTX_ASSERT_TRUE(sf_a != VTX_NODEID_INVALID);
    VTX_ASSERT_TRUE(sf_b != VTX_NODEID_INVALID);
    vtx_node_t *sf_a_n = vtx_node_get(&graph.node_table, sf_a);
    vtx_node_t *sf_b_n = vtx_node_get(&graph.node_table, sf_b);
    sf_a_n->field_offset = 0;
    sf_b_n->field_offset = 1;
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, sf_a, alloc_a), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, sf_a, alloc_a), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, sf_a, val_a0), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, sf_b, alloc_b), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, sf_b, alloc_b), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, sf_b, val_b1), 0);

    /* Region + Phi merging A and B. */
    vtx_nodeid_t region = vtx_node_create(&graph.node_table, VTX_OP_Region);
    VTX_ASSERT_TRUE(region != VTX_NODEID_INVALID);
    vtx_nodeid_t phi = vtx_node_create(&graph.node_table, VTX_OP_Phi);
    VTX_ASSERT_TRUE(phi != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, phi, alloc_a), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, phi, alloc_b), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, phi, region), 0);

    /* Build analysis with both allocations NoEscape. */
    vtx_pea_analysis_t analysis;
    vtx_nodeid_t allocs[2] = { alloc_a, alloc_b };
    VTX_ASSERT_EQUAL(
        vtx_pea_test_build_analysis(&arena, &graph, allocs, 2, &analysis),
        0);

    vtx_virtual_result_t *result = vtx_virtual_run(&graph, &analysis, &arena);
    VTX_ASSERT_NOT_NULL(result);

    /* PEA-2-8: a per-field Phi must have been created for BOTH offsets
     * 0 (only A has it) and 1 (only B has it) — the union. */
    VTX_ASSERT_TRUE(result->phis_resolved >= 1);
    VTX_ASSERT_TRUE(phi < result->state_count);
    VTX_ASSERT_TRUE(result->virtual_states[phi] == VTX_VIRTUAL_YES);

    /* Both fields of the merged virtual object must resolve to a
     * valid value NodeID (the per-field Phi node). Pre-fix, field 1
     * (only present on B) had no per-field Phi and would return INVALID. */
    vtx_nodeid_t f0_phi = vtx_virtual_get_field(result, phi, 0);
    vtx_nodeid_t f1_phi = vtx_virtual_get_field(result, phi, 1);
    VTX_ASSERT_TRUE(f0_phi != VTX_NODEID_INVALID);
    VTX_ASSERT_TRUE(f1_phi != VTX_NODEID_INVALID);

    /* The merged virtual object should record BOTH fields. */
    const vtx_virtual_obj_t *obj = vtx_virtual_get_obj(result, phi);
    VTX_ASSERT_NOT_NULL(obj);
    VTX_ASSERT_TRUE(obj->field_count >= 2);

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    printf("=== PEA-2-8 regression: union field set in resolve_virtual_phis ===\n\n");
    vtx_test_result_t r = vtx_test_run_all();
    printf("\nPEA-2-8 regression: %u passed, %u failed, %u total\n",
           r.pass_count, r.fail_count, r.total_count);
    return (r.fail_count > 0) ? 1 : 0;
}
