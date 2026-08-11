/*
 * PEA-3-1 / PEA-3-2 regression test:
 *   Stale `node` and `alloc_node` pointers in cross_object_sr.c
 *   after vtx_node_create() reallocs table->nodes.
 *
 * Bug (from PEA audit / worklog PEA-3):
 *   At line 575 of cross_object_sr.c (rewrite_scalar_replacements),
 *   the "Field was never stored" branch calls
 *       vtx_node_create(table, VTX_OP_Constant)
 *   which may realloc table->nodes via node_table_grow(). After the
 *   create:
 *     - PEA-3-1: `node` (captured at line 536, the current LoadField
 *       pointer) is dangling. Reads of node->field_offset (line 593),
 *       node->id (line 602), and the dead-mark write node->dead = true
 *       (line 607) all touch freed memory.
 *     - PEA-3-2: `alloc_node` (captured at line 483) is dangling. The
 *       dead-mark write alloc_node->dead = true (line 638) touches
 *       freed memory and the allocation's dead flag silently fails
 *       to be set on the live node.
 *
 * Reproducer strategy (per CRITICAL REPRODUCER CONSTRAINT):
 *   The node table starts with capacity 256 (VTX_NODE_TABLE_INITIAL_CAPACITY).
 *   We build a graph whose node count is EXACTLY 256 immediately before
 *   vtx_cross_object_sr_run is invoked, so that the very first
 *   vtx_node_create() inside rewrite_scalar_replacements triggers
 *   node_table_grow() → realloc(table->nodes).
 *
 *   Graph layout (256 nodes total):
 *     [0]      Start        (vtx_graph_init)
 *     [1]      Province     (vtx_graph_init)
 *     [2..253] 252 filler Constant nodes
 *     [254]    NewObject    ← alloc_id (NoEscape)
 *     [255]    LoadField    ← receiver=alloc_id, field_offset=0
 *
 *   The LoadField has no prior StoreField, so cross_object_sr enters
 *   the "Field was never stored" branch and calls vtx_node_create(
 *   table, VTX_OP_Constant). count was 256 == capacity → realloc.
 *
 * Verification:
 *   Post-fix, the re-fetches make the dead-mark writes land on the
 *   live node table. We verify:
 *     1. vtx_cross_object_sr_run returns non-NULL (no crash).
 *     2. The LoadField node (id 255) is marked dead (PEA-3-1 dead-mark
 *        reached the live node, not freed memory).
 *     3. The NewObject alloc node (id 254) is marked dead (PEA-3-2
 *        dead-mark reached the live node, not freed memory).
 *
 *   Pre-fix (without the re-fetches), the dead-mark writes would
 *   target the old (freed) memory; the live nodes' `dead` flags would
 *   remain false, and assertions 2 and 3 would FAIL. So the test is a
 *   genuine end-to-end reproducer, not a source-grep test.
 */

#include "pea_test_setup.h"

#define PEA3_CAPACITY_BOUNDARY 256

VTX_TEST(pea3_1_2_realloc_during_cross_object_sr)
{
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 0), 0);

    /* 2 nodes already (Start=0, Province=1). Add 252 filler Constants
     * to bring count to 254. */
    uint32_t fillers = PEA3_CAPACITY_BOUNDARY - 2 /* Start,Province */
                                          - 1 /* NewObject */
                                          - 1 /* LoadField */;
    VTX_ASSERT_EQUAL(fillers, 252);
    vtx_nodeid_t last_filler = vtx_pea_test_fill_graph(&graph, fillers);
    VTX_ASSERT_TRUE(last_filler != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(graph.node_table.count, 254);

    /* NewObject alloc_id = 254, count = 255. */
    vtx_nodeid_t alloc_id = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    VTX_ASSERT_TRUE(alloc_id != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(alloc_id, 254);
    VTX_ASSERT_EQUAL(graph.node_table.count, 255);

    /* LoadField on alloc_id, field_offset = 0. count = 256 == capacity.
     * The NEXT vtx_node_create (inside cross_object_sr) will realloc. */
    vtx_nodeid_t load_id = vtx_node_create(&graph.node_table, VTX_OP_LoadField);
    VTX_ASSERT_TRUE(load_id != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(load_id, 255);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, load_id, alloc_id), 0);
    vtx_node_t *load_node = vtx_node_get(&graph.node_table, load_id);
    VTX_ASSERT_TRUE(load_node != NULL);
    load_node->field_offset = 0;

    VTX_ASSERT_EQUAL(graph.node_table.count, PEA3_CAPACITY_BOUNDARY);
    VTX_ASSERT_EQUAL(graph.node_table.capacity, PEA3_CAPACITY_BOUNDARY);

    /* Install a "wedge" allocation immediately after the table so the
     * next realloc (which doubles table->nodes) cannot extend in place
     * and must MOVE memory. This is critical for non-ASAN builds — see
     * the Reliability note in test_pea2_1_materialize_realloc.c for the
     * full explanation. For PEA-3-1/3-2, the bug is a WRITE to freed
     * memory (node->dead = true; alloc_node->dead = true), so the live
     * node's dead flag remains false post-realloc — observable even
     * without ASAN. The wedge guarantees the realloc moves so the
     * stale-write check is reliable. */
    void *wedge = vtx_pea_test_install_realloc_wedge(&graph);
    VTX_ASSERT_NOT_NULL(wedge);

    /* Build analysis: alloc_id is NoEscape. */
    vtx_pea_analysis_t analysis;
    vtx_nodeid_t allocs[1] = { alloc_id };
    VTX_ASSERT_EQUAL(
        vtx_pea_test_build_analysis(&arena, &graph, allocs, 1, &analysis),
        0);

    /* Run cross-object scalar replacement. This is the function under
     * test — it triggers vtx_node_create inside rewrite_scalar_replacements
     * when the LoadField is processed (no prior StoreField → null constant
     * created → realloc). */
    vtx_cross_sr_result_t *result = vtx_cross_object_sr_run(&graph, &analysis, &arena);
    VTX_ASSERT_NOT_NULL(result);

    free(wedge);

    /* PEA-3-1 verification: the LoadField (id 255) must be dead.
     * Pre-fix, node->dead = true wrote to freed memory and the live
     * LoadField's dead flag remained false. */
    vtx_node_t *load_after = vtx_node_get(&graph.node_table, load_id);
    VTX_ASSERT_NOT_NULL(load_after);
    VTX_ASSERT_TRUE(load_after->dead);

    /* PEA-3-2 verification: the NewObject alloc (id 254) must be dead.
     * Pre-fix, alloc_node->dead = true wrote to freed memory and the
     * live NewObject's dead flag remained false. */
    vtx_node_t *alloc_after = vtx_node_get(&graph.node_table, alloc_id);
    VTX_ASSERT_NOT_NULL(alloc_after);
    VTX_ASSERT_TRUE(alloc_after->dead);

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    printf("=== PEA-3-1 / PEA-3-2 regression: cross_object_sr realloc ===\n\n");
    vtx_test_result_t r = vtx_test_run_all();
    printf("\nPEA-3-1/3-2 regression: %u passed, %u failed, %u total\n",
           r.pass_count, r.fail_count, r.total_count);
    return (r.fail_count > 0) ? 1 : 0;
}
