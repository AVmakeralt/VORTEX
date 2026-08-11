/*
 * PEA-2-1 regression test:
 *   Stale `node` pointer in materialize.c after insert_materialization_code()
 *   reallocs table->nodes.
 *
 * Bug (from PEA audit / worklog PEA-2):
 *   In each of the three main scan loops in vtx_materialize_run (escape-point
 *   scan ~line 348, FrameState scan ~line 453, Phi merge scan ~line 553):
 *
 *       for (uint32_t i = 0; i < table->count; i++) {
 *           vtx_node_t *node = &table->nodes[i];      // captured here
 *           ...
 *           for (uint32_t inp = 0; inp < node->input_count; inp++) {
 *               ...
 *               if (insert_materialization_code(graph, pt, arena) != 0) ...
 *               vtx_node_replace_input(table, node->id, inp,        // STALE
 *                                       pt->materialized_obj_id);
 *           }
 *       }
 *
 *   insert_materialization_code() calls vtx_node_create() (NewObject +
 *   StoreField per field), which may realloc table->nodes when count ==
 *   capacity. After the call returns, `node` is dangling; the read of
 *   node->id and the inner for-loop continuation (node->input_count,
 *   node->inputs[inp]) dereference freed memory.
 *
 * Reproducer strategy (per CRITICAL REPRODUCER CONSTRAINT):
 *   The node table starts with capacity 256. We build a graph whose
 *   node count is EXACTLY 256 immediately before vtx_materialize_run is
 *   invoked. The very first vtx_node_create() inside
 *   insert_materialization_code (for the NewObject materialisation node)
 *   triggers node_table_grow() → realloc(table->nodes).
 *
 *   Graph layout (256 nodes total):
 *     [0]      Start        (vtx_graph_init)
 *     [1]      Province     (vtx_graph_init)
 *     [2..253] 252 filler Constant nodes
 *     [254]    NewObject    ← alloc_id (scalar-replaceable per analysis)
 *     [255]    Return       ← escape point, input[0] = alloc_id
 *
 *   The Return node is an escape point and references the scalar-replaced
 *   NewObject as a direct input. vtx_materialize_run enters the first
 *   scan loop, finds the Return at i=255, sees the NewObject input, and
 *   calls insert_materialization_code → vtx_node_create(NewObject) → realloc.
 *
 *   A "wedge" allocation (see pea_test_setup.h) is installed immediately
 *   after the graph is built so the realloc cannot extend the table in
 *   place and must move it to a new virtual address.
 *
 * Reliability note (CRITICAL REPRODUCER CONSTRAINT disclosure):
 *   This test exercises the bug path on every run (count == capacity
 *   boundary is hit, realloc is triggered). However, in a non-ASAN
 *   build the stale-pointer READ of node->id may return the "correct"
 *   value from the freed memory (the freed region retains the old
 *   vtx_node_t bytes until something else overwrites them), in which
 *   case vtx_node_replace_input operates correctly and the post-pass
 *   assertions pass.
 *
 *   Under AddressSanitizer (-DVORTEX_ENABLE_ASAN=ON, which is the
 *   recommended way to run these tests), the freed region is poisoned
 *   immediately and the stale READ reliably aborts with:
 *       ERROR: AddressSanitizer: heap-use-after-free ... READ of size 4
 *       freed by thread T0 here: ... realloc ... vtx_node_create ...
 *       insert_materialization_code ...
 *
 *   So this test is a faithful runtime reproducer under ASAN. In a
 *   non-ASAN build, it remains a meaningful smoke test of the
 *   materialisation code path but does not in itself detect the bug.
 *
 * Verification:
 *   1. vtx_materialize_run returns non-NULL (no crash).
 *   2. result->objects_materialized >= 1.
 *   3. The Return node's first input is no longer alloc_id — it was
 *      replaced by the materialised NewObject id. This read of node->id
 *      requires the post-fix re-fetch to have run; otherwise the stale
 *      pointer produces garbage and the input is either not replaced or
 *      replaced on a bogus node.
 */

#include "pea_test_setup.h"

#define PEA2_1_CAPACITY_BOUNDARY 256

VTX_TEST(pea2_1_materialize_realloc_in_escape_scan)
{
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 0), 0);

    /* 2 + 252 + 1 (NewObject) + 1 (Return) = 256. */
    uint32_t fillers = PEA2_1_CAPACITY_BOUNDARY - 4;
    VTX_ASSERT_EQUAL(fillers, 252);
    vtx_nodeid_t last_filler = vtx_pea_test_fill_graph(&graph, fillers);
    VTX_ASSERT_TRUE(last_filler != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(graph.node_table.count, 254);

    /* NewObject alloc_id = 254 (NoEscape / scalar-replaceable). */
    vtx_nodeid_t alloc_id = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    VTX_ASSERT_TRUE(alloc_id != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(alloc_id, 254);

    /* Return with input = alloc_id. This is an escape point that
     * references a scalar-replaced allocation, forcing materialisation. */
    vtx_nodeid_t return_id = vtx_node_create(&graph.node_table, VTX_OP_Return);
    VTX_ASSERT_TRUE(return_id != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(return_id, 255);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, return_id, alloc_id), 0);

    VTX_ASSERT_EQUAL(graph.node_table.count, 256);
    VTX_ASSERT_EQUAL(graph.node_table.capacity, 256);

    /* Install a "wedge" allocation immediately after the table so the
     * next realloc (which doubles table->nodes) cannot extend in place
     * and must MOVE memory. Without the wedge, glibc may extend in
     * place, in which case the stale `node` pointer in materialize.c
     * remains dereferenceable and the bug does not manifest as
     * observable corruption in a non-ASAN build. */
    void *wedge = vtx_pea_test_install_realloc_wedge(&graph);
    VTX_ASSERT_NOT_NULL(wedge);

    /* Mark alloc_id as NoEscape (scalar-replaceable). */
    vtx_pea_analysis_t analysis;
    vtx_nodeid_t allocs[1] = { alloc_id };
    VTX_ASSERT_EQUAL(
        vtx_pea_test_build_analysis(&arena, &graph, allocs, 1, &analysis),
        0);

    /* Run materialisation. virtual_result is NULL — the materialiser
     * falls back to collect_field_values (no fields here, so
     * insert_materialization_code creates only the NewObject node). */
    vtx_materialize_result_t *result = vtx_materialize_run(
        &graph, &analysis, NULL, &arena);
    VTX_ASSERT_NOT_NULL(result);

    free(wedge);

    /* PEA-2-1 verification 1: exactly one object was materialised. */
    VTX_ASSERT_TRUE(result->objects_materialized >= 1);

    /* PEA-2-1 verification 2: the Return's input[0] is no longer
     * alloc_id — it was rewritten to the materialised NewObject.
     * This read of node->id (via vtx_node_replace_input) requires the
     * post-fix re-fetch to have run; otherwise the stale pointer
     * produced garbage and the input was either not replaced or
     * replaced on a bogus node. */
    vtx_node_t *return_after = vtx_node_get(&graph.node_table, return_id);
    VTX_ASSERT_NOT_NULL(return_after);
    VTX_ASSERT_TRUE(return_after->input_count >= 1);
    VTX_ASSERT_TRUE(return_after->inputs[0] != alloc_id);

    /* And the new input must be a valid node id (the materialised obj). */
    vtx_nodeid_t mat_obj_id = return_after->inputs[0];
    VTX_ASSERT_TRUE(mat_obj_id != VTX_NODEID_INVALID);
    vtx_node_t *mat_obj = vtx_node_get(&graph.node_table, mat_obj_id);
    VTX_ASSERT_NOT_NULL(mat_obj);
    VTX_ASSERT_TRUE(mat_obj->opcode == VTX_OP_NewObject);

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    printf("=== PEA-2-1 regression: materialize realloc ===\n\n");
    vtx_test_result_t r = vtx_test_run_all();
    printf("\nPEA-2-1 regression: %u passed, %u failed, %u total\n",
           r.pass_count, r.fail_count, r.total_count);
    return (r.fail_count > 0) ? 1 : 0;
}
