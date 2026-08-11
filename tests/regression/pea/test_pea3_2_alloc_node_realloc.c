/*
 * PEA-3-2 regression test (alloc_node dead-mark path):
 *   Stale `alloc_node` pointer in cross_object_sr.c after a realloc
 *   caused by the inner LoadField null-constant creation.
 *
 * Bug (from PEA audit / worklog PEA-3):
 *   This is the same root cause as PEA-3-1, but specifically exercises
 *   the alloc_node->dead = true write at line 638 of cross_object_sr.c.
 *
 *   The alloc_node pointer is captured at line 483:
 *       vtx_node_t *alloc_node = vtx_node_get(table, alloc_id);
 *   It is used LATER (line 638) only when:
 *     (a) the allocation has effective NoEscape, AND
 *     (b) the LoadField loop processed at least one never-stored field
 *         (which calls vtx_node_create → realloc), AND
 *     (c) the allocation has no remaining non-field-store/non-field-load
 *         uses (so has_remaining_uses == false → alloc_node->dead = true).
 *
 * Reproducer strategy (per CRITICAL REPRODUCER CONSTRAINT):
 *   The node table starts with capacity 256. We build a graph with
 *   exactly 256 nodes before invoking cross_object_sr_run:
 *
 *     [0]      Start        (vtx_graph_init)
 *     [1]      Province     (vtx_graph_init)
 *     [2..253] 252 filler Constant nodes
 *     [254]    NewObject    ← alloc_id (NoEscape)
 *     [255]    LoadField    ← receiver=alloc_id, field_offset=0
 *
 *   The LoadField has no prior StoreField → null constant creation
 *   inside cross_object_sr triggers realloc. The allocation has no
 *   remaining uses after the LoadField is rewritten → alloc_node->dead
 *   = true is executed through the stale pointer.
 *
 * Verification:
 *   Post-fix, alloc_node is re-fetched after the vtx_node_create, so
 *   the dead-mark write lands on the live NewObject node. We verify
 *   that alloc_node (id 254) is dead after the pass.
 *
 *   Pre-fix, the dead-mark write goes to freed memory; the live
 *   NewObject's dead flag remains false, and the assertion FAILS.
 *
 *   (This is the same graph as test_pea3_1_cross_object_sr_realloc.c,
 *   but the test name and the explicit alloc_node->dead check make
 *   the PEA-3-2 fix surface — i.e., the re-fetch of `alloc_node`
 *   specifically, independent of the `node` re-fetch.)
 */

#include "pea_test_setup.h"

#define PEA3_2_CAPACITY_BOUNDARY 256

VTX_TEST(pea3_2_alloc_node_dead_mark_after_realloc)
{
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 0), 0);

    /* 2 + 252 + 1 (NewObject) + 1 (LoadField) = 256. */
    uint32_t fillers = PEA3_2_CAPACITY_BOUNDARY - 4;
    VTX_ASSERT_EQUAL(fillers, 252);
    vtx_nodeid_t last_filler = vtx_pea_test_fill_graph(&graph, fillers);
    VTX_ASSERT_TRUE(last_filler != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(graph.node_table.count, 254);

    /* NewObject alloc_id = 254. */
    vtx_nodeid_t alloc_id = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    VTX_ASSERT_TRUE(alloc_id != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(alloc_id, 254);

    /* LoadField on alloc_id with field_offset = 0.
     * Field has no prior store, so cross_object_sr will create a null
     * Constant for the default value → realloc. */
    vtx_nodeid_t load_id = vtx_node_create(&graph.node_table, VTX_OP_LoadField);
    VTX_ASSERT_TRUE(load_id != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(load_id, 255);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, load_id, alloc_id), 0);
    vtx_node_t *load_node = vtx_node_get(&graph.node_table, load_id);
    VTX_ASSERT_TRUE(load_node != NULL);
    load_node->field_offset = 0;

    VTX_ASSERT_EQUAL(graph.node_table.count, 256);
    VTX_ASSERT_EQUAL(graph.node_table.capacity, 256);

    /* Install a "wedge" allocation immediately after the table so the
     * next realloc (which doubles table->nodes) cannot extend in place
     * and must MOVE memory. For PEA-3-2, the bug is a WRITE to freed
     * memory (alloc_node->dead = true), so the live NewObject's dead
     * flag remains false post-realloc — observable without ASAN.
     * The wedge guarantees the realloc moves so the stale-write check
     * is reliable. */
    void *wedge = vtx_pea_test_install_realloc_wedge(&graph);
    VTX_ASSERT_NOT_NULL(wedge);

    /* Mark alloc_id as NoEscape. */
    vtx_pea_analysis_t analysis;
    vtx_nodeid_t allocs[1] = { alloc_id };
    VTX_ASSERT_EQUAL(
        vtx_pea_test_build_analysis(&arena, &graph, allocs, 1, &analysis),
        0);

    /* Run the pass under test. */
    vtx_cross_sr_result_t *result = vtx_cross_object_sr_run(&graph, &analysis, &arena);
    VTX_ASSERT_NOT_NULL(result);

    free(wedge);

    /* PEA-3-2 verification: the NewObject alloc MUST be dead.
     * Pre-fix, alloc_node->dead = true at line 638 wrote to the OLD
     * (freed) nodes array. The live NewObject in the realloc'd array
     * retained dead=false. */
    vtx_node_t *alloc_after = vtx_node_get(&graph.node_table, alloc_id);
    VTX_ASSERT_NOT_NULL(alloc_after);
    VTX_ASSERT_TRUE(alloc_after->dead);

    /* And the result counter for allocs_replaced must be >= 1. */
    VTX_ASSERT_TRUE(result->allocs_replaced >= 1);

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    printf("=== PEA-3-2 regression: alloc_node dead-mark after realloc ===\n\n");
    vtx_test_result_t r = vtx_test_run_all();
    printf("\nPEA-3-2 regression: %u passed, %u failed, %u total\n",
           r.pass_count, r.fail_count, r.total_count);
    return (r.fail_count > 0) ? 1 : 0;
}
