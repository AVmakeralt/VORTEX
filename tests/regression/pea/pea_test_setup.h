#ifndef VORTEX_PEA_TEST_SETUP_H
#define VORTEX_PEA_TEST_SETUP_H

/*
 * Shared setup helpers for the PEA use-after-realloc regression tests.
 *
 * All four bugs (PEA-3-1, PEA-3-2, PEA-2-1, PEA-2-2) share the same root
 * cause: a `vtx_node_t *` pointer captured by the PEA pass becomes
 * dangling after vtx_node_create() inside the same pass reallocs
 * table->nodes via node_table_grow().
 *
 * The default node table capacity is VTX_NODE_TABLE_INITIAL_CAPACITY (256).
 * Each test builds a synthetic graph whose node count is exactly 256
 * (= capacity) immediately before the PEA pass is invoked, so that the
 * first vtx_node_create() inside the pass triggers the realloc.
 *
 * The tests verify the fix by inspecting the post-pass graph state —
 * specifically, that "dead" flags set by the PEA pass through a fresh
 * (re-fetched) pointer actually land on the live node, instead of being
 * written through a stale pointer to freed memory.
 */

#include "test_framework.h"
#include "vortex_config.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "runtime/arena.h"
#include "pea/analysis.h"
#include "pea/cross_object_sr.h"
#include "pea/materialize.h"
#include "pea/virtual.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Build a synthetic graph with `filler_count` extra Constant nodes
 * between Start/Province (created by vtx_graph_init) and the "real"
 * test nodes added later by the caller.
 *
 * Returns the NodeID of the most-recently created filler, or 0 if
 * filler_count == 0. The graph is left in a state where the caller
 * can append test-specific nodes (alloc, LoadField, Phi, etc.) and
 * precisely position the node-table capacity boundary.
 */
static inline vtx_nodeid_t vtx_pea_test_fill_graph(vtx_graph_t *graph,
                                                    uint32_t filler_count)
{
    vtx_nodeid_t last = graph->start_node; /* Start already exists */
    for (uint32_t i = 0; i < filler_count; i++) {
        last = vtx_node_create(&graph->node_table, VTX_OP_Constant);
        if (last == VTX_NODEID_INVALID) {
            return VTX_NODEID_INVALID;
        }
    }
    return last;
}

/*
 * Manually construct a vtx_pea_analysis_t for the given graph.
 * Marks each alloc_id in `alloc_ids` (count `alloc_count`) as NoEscape
 * (scalar-replaceable). All other nodes get VTX_ESCAPE_GLOBAL.
 *
 * The analysis is arena-allocated; the caller is responsible for
 * arena lifetime management.
 *
 * Returns 0 on success, -1 on failure.
 */
static inline int vtx_pea_test_build_analysis(vtx_arena_t *arena,
                                               vtx_graph_t *graph,
                                               const vtx_nodeid_t *alloc_ids,
                                               uint32_t alloc_count,
                                               vtx_pea_analysis_t *out)
{
    vtx_node_table_t *table = &graph->node_table;
    uint32_t state_count = table->count;

    memset(out, 0, sizeof(*out));

    /* states[] indexed by NodeID — initialise everything to GLOBAL
     * (the conservative default per analysis.h). */
    out->escape_map.state_count = state_count;
    out->escape_map.states = (vtx_escape_state_t *)vtx_arena_alloc(
        arena, state_count * sizeof(vtx_escape_state_t));
    if (!out->escape_map.states) return -1;
    for (uint32_t i = 0; i < state_count; i++) {
        out->escape_map.states[i] = VTX_ESCAPE_GLOBAL;
    }

    /* Mark the listed allocations as NoEscape. */
    out->escape_map.alloc_ids = (vtx_nodeid_t *)vtx_arena_alloc(
        arena, alloc_count * sizeof(vtx_nodeid_t));
    if (!out->escape_map.alloc_ids) return -1;
    for (uint32_t i = 0; i < alloc_count; i++) {
        vtx_nodeid_t aid = alloc_ids[i];
        if (aid < state_count) {
            out->escape_map.states[aid] = VTX_ESCAPE_NONE;
        }
        out->escape_map.alloc_ids[i] = aid;
    }
    out->escape_map.alloc_count = alloc_count;
    out->escape_map.alloc_capacity = alloc_count;

    out->total_allocs = alloc_count;
    out->no_escape_count = alloc_count;
    out->block_states = NULL;
    out->block_state_count = 0;
    out->iterations = 0;
    return 0;
}

/*
 * Force the next realloc(table->nodes, 2 * capacity * sizeof(vtx_node_t))
 * to MOVE memory rather than extend in place.
 *
 * Without a wedge, glibc's realloc may extend the table->nodes allocation
 * in place (returning the same pointer), in which case the stale `node`
 * pointers in the PEA passes happen to remain dereferenceable — and the
 * use-after-realloc bugs we are testing for do NOT manifest as observable
 * corruption in a non-ASAN build.
 *
 * The wedge occupies the address range IMMEDIATELY after the current
 * table->nodes allocation, so the next realloc (which doubles the size)
 * cannot extend in place and must move to a new region. This makes the
 * UAF observable: reads/writes through the stale pointer hit freed memory.
 *
 * The wedge is returned and must be freed by the caller after the PEA
 * pass under test has run.
 *
 * Returns NULL on allocation failure (test should fail-fast).
 */
static inline void *vtx_pea_test_install_realloc_wedge(vtx_graph_t *graph)
{
    /* The table currently has capacity `cap`; the next grow will need
     * an additional `cap * sizeof(vtx_node_t)` bytes contiguous after
     * the existing allocation. A wedge of that exact size placed right
     * after table->nodes forces the move.
     *
     * We add a small extra 16 bytes to defeat the allocator's bucket
     * rounding and guarantee the wedge is in a different alloc chunk. */
    size_t wedge_bytes = (size_t)graph->node_table.capacity
                         * sizeof(vtx_node_t) + 16;
    void *wedge = malloc(wedge_bytes);
    /* Touch every page so the kernel actually maps it (prevents the
     * allocator from lazily returning the same virtual range). */
    if (wedge) {
        memset(wedge, 0xAB, wedge_bytes);
    }
    return wedge;
}

#endif /* VORTEX_PEA_TEST_SETUP_H */
