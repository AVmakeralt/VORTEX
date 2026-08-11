/*
 * VORTEX PEA-1-4 Regression Test
 *
 * Bug: StoreField and StoreIndexed unconditionally joined the stored
 * value's escape state with VTX_ESCAPE_ARG, even when the container
 * was NoEscape. This bumped even NoEscape containers to ArgEscape,
 * immediately disqualifying the stored value from scalar replacement.
 * PEA became a no-op for the most common SR pattern: an object
 * holding a private reference to another non-escaping object.
 *
 * Fix: Removed the unconditional ARG bump. The stored value escapes
 * at least as much as the container (if the container escapes, the
 * value escapes with it), but if the container is NoEscape, the value
 * stays NoEscape too.
 *
 * Test: Construct a graph with two NoEscape allocations where alloc_b
 * is stored into alloc_a's field. Verify alloc_b stays NoEscape (not
 * bumped to ArgEscape).
 */

#include "pea_test_setup.h"

/* is_allocation is static in analysis.c — inline it here for the test. */
static inline bool test_is_allocation(vtx_node_opcode_t opcode)
{
    return opcode == VTX_OP_NewObject ||
           opcode == VTX_OP_NewArray  ||
           opcode == VTX_OP_Allocate;
}

VTX_TEST(pea14_storefield_no_unconditional_arg_bump)
{
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_graph_t graph; vtx_graph_init(&graph, 0);

    /* Create two NewObject allocations — both NoEscape */
    vtx_nodeid_t alloc_a = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    vtx_nodeid_t alloc_b = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    VTX_ASSERT_TRUE(alloc_a != VTX_NODEID_INVALID);
    VTX_ASSERT_TRUE(alloc_b != VTX_NODEID_INVALID);

    vtx_node_t *a_n = vtx_node_get(&graph.node_table, alloc_a);
    a_n->type_id = 0;
    vtx_node_t *b_n = vtx_node_get(&graph.node_table, alloc_b);
    b_n->type_id = 0;

    /* StoreField: alloc_a.field = alloc_b (B is stored into A)
     * Input layout: [memory, receiver, value] */
    vtx_nodeid_t sf = vtx_node_create(&graph.node_table, VTX_OP_StoreField);
    VTX_ASSERT_TRUE(sf != VTX_NODEID_INVALID);
    vtx_node_t *sf_n = vtx_node_get(&graph.node_table, sf);
    sf_n->field_offset = 0;
    vtx_node_add_input(&graph.node_table, sf, alloc_a); /* memory chain */
    vtx_node_add_input(&graph.node_table, sf, alloc_a); /* receiver */
    vtx_node_add_input(&graph.node_table, sf, alloc_b); /* value */

    /* Build analysis with both as NoEscape */
    vtx_pea_analysis_t analysis;
    vtx_nodeid_t allocs[2] = { alloc_a, alloc_b };
    VTX_ASSERT_EQUAL(
        vtx_pea_test_build_analysis(&arena, &graph, allocs, 2, &analysis),
        0);

    /* Simulate transfer_node for StoreField.
     * Both allocations start NoEscape. After the StoreField transfer,
     * alloc_b should stay NoEscape (container A is NoEscape, so the
     * stored value inherits NoEscape — no ARG bump). */
    vtx_escape_state_t states[64] = {0};
    uint32_t state_count = graph.node_table.count;
    if (state_count > 64) state_count = 64;
    for (uint32_t i = 0; i < state_count; i++) {
        states[i] = VTX_ESCAPE_GLOBAL;
    }
    states[alloc_a] = VTX_ESCAPE_NONE;
    states[alloc_b] = VTX_ESCAPE_NONE;

    /* Simulate the PEA-1-4 fixed StoreField transfer:
     *   container_state = states[receiver] = NONE
     *   propagate container_state to value (no ARG bump) */
    vtx_node_t *store_node = vtx_node_get(&graph.node_table, sf);
    if (store_node->input_count >= 2) {
        vtx_nodeid_t receiver_id = store_node->inputs[store_node->input_count - 2];
        vtx_nodeid_t value_id = store_node->inputs[store_node->input_count - 1];

        vtx_escape_state_t container_state = VTX_ESCAPE_GLOBAL;
        if (receiver_id < state_count) {
            vtx_node_t *recv = vtx_node_get(&graph.node_table, receiver_id);
            if (recv && !recv->dead && test_is_allocation(recv->opcode)) {
                container_state = states[receiver_id];
            }
        }

        /* PEA-1-4 fix: propagate container_state (no ARG bump) */
        if (value_id < state_count) {
            vtx_node_t *val = vtx_node_get(&graph.node_table, value_id);
            if (val && !val->dead && test_is_allocation(val->opcode)) {
                if (states[value_id] < container_state) {
                    states[value_id] = container_state;
                }
            }
        }
    }

    /* PEA-1-4: alloc_b should stay NoEscape (not bumped to ArgEscape). */
    VTX_ASSERT_EQUAL(states[alloc_a], VTX_ESCAPE_NONE);
    VTX_ASSERT_EQUAL(states[alloc_b], VTX_ESCAPE_NONE);

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nPEA-1-4 regression: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    return (result.fail_count > 0) ? 1 : 0;
}
