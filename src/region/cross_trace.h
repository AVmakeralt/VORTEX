#ifndef VORTEX_CROSS_TRACE_H
#define VORTEX_CROSS_TRACE_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "ir/gvn.h"
#include "ir/constant_prop.h"
#include "ir/dce.h"
#include "runtime/arena.h"
#include "region/stitch.h"

/**
 * VORTEX Cross-Trace Optimization
 *
 * After stitching multiple traces into a hyperblock, this module runs
 * global optimizations across the entire hyperblock. These optimizations
 * are impossible within a single trace because they require seeing
 * computations from multiple paths simultaneously.
 *
 * Optimizations performed:
 *
 *   1. Global Value Numbering (GVN):
 *      - Identifies and eliminates redundant computations that appear
 *        in both the main path and an inlined branch.
 *      - Common subexpression elimination across the entire hyperblock.
 *      - Redundant type check elimination: if a dominating Guard
 *        already proved a type, subsequent type checks on the same
 *        value are eliminated.
 *      - Redundant null check elimination: if a dominating Guard
 *        already proved non-null, subsequent null checks are eliminated.
 *
 *   2. Constant Propagation (SCCP):
 *      - Propagates constants from the main path into branches.
 *      - Folds constant expressions in branch-only code.
 *      - Eliminates dead branches whose condition is constant-false.
 *      - Simplifies Phis that have all-constant inputs.
 *
 *   3. Dead Code Elimination (DCE):
 *      - Removes branch-only computations whose results are never
 *        used on the rejoin path.
 *      - Removes code made dead by constant propagation.
 *      - Iterative: removing dead code may expose more dead code.
 *
 * The overall optimization pipeline:
 *   GVN → SCCP → DCE → GVN → DCE
 *
 * The second GVN pass catches redundancies exposed by constant
 * propagation (e.g., after a branch condition is folded to a constant,
 * both branches may now compute the same value). The final DCE pass
 * removes any newly dead code.
 */

/* ========================================================================== */
/* Cross-trace optimization result                                             */
/* ========================================================================== */

/**
 * Statistics from cross-trace optimization.
 */
typedef struct {
    uint32_t gvn_eliminated_first;   /* nodes eliminated by first GVN pass */
    uint32_t sccp_simplified;        /* nodes simplified by SCCP */
    uint32_t dce_removed_first;      /* nodes removed by first DCE pass */
    uint32_t gvn_eliminated_second;  /* nodes eliminated by second GVN pass */
    uint32_t dce_removed_second;     /* nodes removed by second DCE pass */
    uint32_t total_eliminated;       /* sum of all eliminated/removed nodes */
} vtx_cross_trace_stats_t;

/* ========================================================================== */
/* Cross-trace optimization entry point                                        */
/* ========================================================================== */

/**
 * Run cross-trace optimization on a stitched hyperblock.
 *
 * Applies GVN, constant propagation, and DCE across the entire
 * hyperblock. The graph is modified in place.
 *
 * Optimization pipeline:
 *   1. GVN — eliminate redundant computations
 *   2. SCCP — propagate constants and fold
 *   3. DCE — remove dead code
 *   4. GVN — second pass to catch redundancies exposed by SCCP
 *   5. DCE — remove any newly dead code
 *
 * The hyperblock's node list is NOT updated (it remains as recorded
 * at stitch time). The actual optimizations modify the graph's node
 * table directly. Callers should re-traverse the graph to find the
 * optimized nodes.
 *
 * Returns statistics about the optimization passes.
 */
vtx_cross_trace_stats_t vtx_cross_trace_optimize(vtx_hyperblock_t *hyperblock,
                                                   vtx_graph_t *graph,
                                                   vtx_arena_t *arena);

/**
 * Run a single round of cross-trace optimization (GVN + DCE only).
 * Useful for incremental optimization after small changes.
 */
uint32_t vtx_cross_trace_optimize_quick(vtx_graph_t *graph);

/**
 * Check if a node in the hyperblock is on the main path (root trace)
 * or on a branch path. This is determined by checking the node's
 * bytecode_pc against the root trace's PC range.
 *
 * Returns true if the node is on the main path.
 */
bool vtx_cross_trace_is_main_path(const vtx_hyperblock_t *hyperblock,
                                   const vtx_graph_t *graph,
                                   vtx_nodeid_t node);

/**
 * Count the number of nodes in the hyperblock that are reachable
 * (not dead) after optimization.
 */
uint32_t vtx_cross_trace_live_node_count(const vtx_hyperblock_t *hyperblock,
                                           const vtx_graph_t *graph);

#endif /* VORTEX_CROSS_TRACE_H */
