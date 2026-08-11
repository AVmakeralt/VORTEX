/*
 * PEA-2-2 regression test:
 *   Stale `node` pointer in virtual.c resolve_virtual_phis after
 *   vtx_node_create() reallocs table->nodes.
 *
 * Bug (from PEA audit / worklog PEA-2):
 *   At line 226 of virtual.c, resolve_virtual_phis captures
 *       vtx_node_t *node = &table->nodes[i];
 *   inside the per-Phi scan loop. The per-field-Phi creation loop
 *   below calls:
 *       vtx_node_create(table, VTX_OP_Phi)        (line 327)
 *       vtx_node_create(table, VTX_OP_Constant)   (line 356, when an
 *       input's field value is unknown)
 *   Both may realloc table->nodes. After the creates:
 *     - node->inputs[inp] (line 333, inside the inner per-input loop)
 *       dereferences freed memory.
 *     - node->id (line 373, the post-loop result->virtual_states write)
 *       dereferences freed memory.
 *
 * Reproducer strategy (per CRITICAL REPRODUCER CONSTRAINT):
 *   The node table starts with capacity 256. We build a graph whose
 *   node count is EXACTLY 256 immediately before vtx_virtual_run is
 *   invoked. The first vtx_node_create(table, VTX_OP_Phi) inside
 *   resolve_virtual_phis triggers node_table_grow() → realloc.
 *
 *   Graph layout (256 nodes total):
 *     [0]      Start        (vtx_graph_init)
 *     [1]      Province     (vtx_graph_init)
 *     [2..249] 248 filler Constant nodes
 *     [250]    Constant V   ← value stored into A's field
 *     [251]    NewObject A  ← alloc A (NoEscape / virtual)
 *     [252]    NewObject B  ← alloc B (NoEscape / virtual)
 *     [253]    StoreField   ← A.f0 = V (gives A one field, so the per-
 *                              field Phi creation loop runs at least once)
 *     [254]    Region R     ← control input of the Phi
 *     [255]    Phi P        ← inputs [R, A, B] — virtual merge point
 *
 *   vtx_virtual_run:
 *     - classify_allocations: A and B → VTX_VIRTUAL_YES (NoEscape).
 *     - Step 2 (collect field values): scans StoreField, A.f0 = V →
 *       vobj_A.field_count = 1.
 *     - Step 3 (resolve_virtual_phis): scans Phi P, finds all inputs
 *       virtual (R skipped as control; A and B skipped as memory). All-
 *       virtual=true. first_vobj = vobj_A (field_count=1). Enters the
 *       per-field loop:
 *         vtx_node_create(table, VTX_OP_Phi)  ← count 256 → REALLOC.
 *         Inner per-input loop reads node->inputs[inp] (line 333) —
 *         STALE pre-fix, FRESH post-fix.
 *         vtx_node_create(table, VTX_OP_Constant) for B's missing
 *         field → another create.
 *       After the field loop, line 373 reads node->id — STALE pre-fix,
 *       FRESH post-fix.
 *
 *   A "wedge" allocation (see pea_test_setup.h) is installed immediately
 *   after the graph is built so the realloc cannot extend the table in
 *   place and must move it to a new virtual address.
 *
 * Reliability note (CRITICAL REPRODUCER CONSTRAINT disclosure):
 *   This test exercises the bug path on every run (count == capacity
 *   boundary is hit, realloc is triggered). However, in a non-ASAN
 *   build the stale-pointer READs of node->inputs[inp] and node->id
 *   may return the "correct" values from the freed memory (the freed
 *   region retains the old vtx_node_t bytes until something else
 *   overwrites them), in which case the post-pass assertions pass.
 *
 *   Under AddressSanitizer (-DVORTEX_ENABLE_ASAN=ON, which is the
 *   recommended way to run these tests), the freed region is poisoned
 *   immediately and the stale READ reliably aborts with:
 *       ERROR: AddressSanitizer: heap-use-after-free ... READ of size 4
 *       freed by thread T0 here: ... realloc ... vtx_node_create ...
 *
 *   So this test is a faithful runtime reproducer under ASAN. In a
 *   non-ASAN build, it remains a meaningful smoke test of the
 *   virtual Phi resolution code path but does not in itself detect the
 *   bug.
 *
 * Verification:
 *   1. vtx_virtual_run returns non-NULL (no crash).
 *   2. result->phis_resolved >= 1 (the Phi merge was resolved).
 *   3. The Phi P (id 255) is marked VTX_VIRTUAL_YES in result->virtual_states.
 */

#include "pea_test_setup.h"

#define PEA2_2_CAPACITY_BOUNDARY 256

VTX_TEST(pea2_2_virtual_phi_realloc_during_resolution)
{
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 0), 0);

    /* 2 + 248 + 1 (V) + 2 (A,B) + 1 (S1) + 1 (R) + 1 (P) = 256. */
    uint32_t fillers = PEA2_2_CAPACITY_BOUNDARY - 8;
    VTX_ASSERT_EQUAL(fillers, 248);
    vtx_nodeid_t last_filler = vtx_pea_test_fill_graph(&graph, fillers);
    VTX_ASSERT_TRUE(last_filler != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(graph.node_table.count, 250);

    /* Constant V (value stored into A's field). id = 250. */
    vtx_nodeid_t val_id = vtx_node_create(&graph.node_table, VTX_OP_Constant);
    VTX_ASSERT_TRUE(val_id != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(val_id, 250);

    /* NewObject A. id = 251. */
    vtx_nodeid_t alloc_a_id = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    VTX_ASSERT_TRUE(alloc_a_id != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(alloc_a_id, 251);

    /* NewObject B. id = 252. */
    vtx_nodeid_t alloc_b_id = vtx_node_create(&graph.node_table, VTX_OP_NewObject);
    VTX_ASSERT_TRUE(alloc_b_id != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(alloc_b_id, 252);

    /* StoreField A.f0 = V. id = 253. Inputs: [A, V]. */
    vtx_nodeid_t store_id = vtx_node_create(&graph.node_table, VTX_OP_StoreField);
    VTX_ASSERT_TRUE(store_id != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(store_id, 253);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, store_id, alloc_a_id), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, store_id, val_id), 0);
    vtx_node_t *store_node = vtx_node_get(&graph.node_table, store_id);
    VTX_ASSERT_TRUE(store_node != NULL);
    store_node->field_offset = 0;

    /* Region R. id = 254. */
    vtx_nodeid_t region_id = vtx_node_create(&graph.node_table, VTX_OP_Region);
    VTX_ASSERT_TRUE(region_id != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(region_id, 254);

    /* Phi P with inputs [R, A, B]. id = 255. count = 256 = capacity. */
    vtx_nodeid_t phi_id = vtx_node_create(&graph.node_table, VTX_OP_Phi);
    VTX_ASSERT_TRUE(phi_id != VTX_NODEID_INVALID);
    VTX_ASSERT_EQUAL(phi_id, 255);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, phi_id, region_id), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, phi_id, alloc_a_id), 0);
    VTX_ASSERT_EQUAL(vtx_node_add_input(&graph.node_table, phi_id, alloc_b_id), 0);

    VTX_ASSERT_EQUAL(graph.node_table.count, 256);
    VTX_ASSERT_EQUAL(graph.node_table.capacity, 256);

    /* Install a "wedge" allocation immediately after the table so the
     * next realloc (which doubles table->nodes) cannot extend in place
     * and must MOVE memory. Without the wedge, glibc may extend in
     * place, in which case the stale `node` pointer in virtual.c
     * resolve_virtual_phis remains dereferenceable and the bug does not
     * manifest as observable corruption in a non-ASAN build. */
    void *wedge = vtx_pea_test_install_realloc_wedge(&graph);
    VTX_ASSERT_NOT_NULL(wedge);

    /* Mark A and B as NoEscape (virtual). */
    vtx_pea_analysis_t analysis;
    vtx_nodeid_t allocs[2] = { alloc_a_id, alloc_b_id };
    VTX_ASSERT_EQUAL(
        vtx_pea_test_build_analysis(&arena, &graph, allocs, 2, &analysis),
        0);

    /* Run virtual object tracking. This is the function under test. */
    vtx_virtual_result_t *result = vtx_virtual_run(&graph, &analysis, &arena);
    VTX_ASSERT_NOT_NULL(result);

    free(wedge);

    /* PEA-2-2 verification 1: at least one Phi was resolved. */
    VTX_ASSERT_TRUE(result->phis_resolved >= 1);

    /* PEA-2-2 verification 2: the Phi P (id 255) must be classified
     * VTX_VIRTUAL_YES. Pre-fix, the line 373 write
     *       result->virtual_states[node->id] = VTX_VIRTUAL_YES;
     * wrote through the stale node pointer (realloc'd memory), so
     * virtual_states[255] remained VTX_VIRTUAL_UNKNOWN (0). */
    VTX_ASSERT_TRUE(phi_id < result->state_count);
    VTX_ASSERT_TRUE(result->virtual_states[phi_id] == VTX_VIRTUAL_YES);

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    printf("=== PEA-2-2 regression: virtual Phi realloc during resolution ===\n\n");
    vtx_test_result_t r = vtx_test_run_all();
    printf("\nPEA-2-2 regression: %u passed, %u failed, %u total\n",
           r.pass_count, r.fail_count, r.total_count);
    return (r.fail_count > 0) ? 1 : 0;
}
