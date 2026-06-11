/**
 * VORTEX Cross-Trace Optimization — Implementation
 *
 * Runs GVN, constant propagation, and DCE across an entire stitched
 * hyperblock. These optimizations exploit the fact that multiple traces
 * are now in the same compilation unit, enabling:
 *   - CSE across the main path and branches
 *   - Constant propagation from main path into branches
 *   - Dead code elimination of branch-only computations
 */

#include "region/cross_trace.h"
#include "region/stitch.h"
#include "ir/gvn.h"
#include "ir/constant_prop.h"
#include "ir/dce.h"
#include "runtime/arena.h"
#include <string.h>

/* ========================================================================== */
/* Main optimization entry point                                               */
/* ========================================================================== */

vtx_cross_trace_stats_t vtx_cross_trace_optimize(vtx_hyperblock_t *hyperblock,
                                                   vtx_graph_t *graph,
                                                   vtx_arena_t *arena)
{
    vtx_cross_trace_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    VTX_ASSERT(hyperblock != NULL, "hyperblock must not be NULL");
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    (void)arena; /* arena may be used for future scratch allocations */

    /* Phase 1: Global Value Numbering
     * Eliminates redundant computations across the entire hyperblock.
     * This catches:
     *   - The same arithmetic computed in both root trace and a branch
     *   - Redundant type checks (if a dominating guard proved the type)
     *   - Redundant null checks (if a dominating guard proved non-null)
     */
    stats.gvn_eliminated_first = vtx_gvn_run(graph);

    /* Phase 2: Sparse Conditional Constant Propagation
     * Propagates constants from the main path into branches and folds
     * constant expressions. This can:
     *   - Fold branch conditions to constants (eliminating cold branches)
     *   - Propagate type information from guards into branch code
     *   - Simplify computations that depend on constant inputs
     */
    stats.sccp_simplified = vtx_constant_prop_run(graph);

    /* Phase 3: Dead Code Elimination
     * Removes code made dead by constant propagation:
     *   - Unreachable branches (condition folded to false)
     *   - Computations whose results are never used
     *   - Branch-only code that doesn't contribute to observable state
     */
    stats.dce_removed_first = vtx_dce_run(graph);

    /* Phase 4: Second GVN pass
     * After constant propagation and DCE, new redundancies may be
     * exposed. For example, if two branches both compute the same
     * value after their divergent conditions are folded, GVN can
     * now merge them.
     */
    stats.gvn_eliminated_second = vtx_gvn_run(graph);

    /* Phase 5: Final DCE
     * Remove any code made dead by the second GVN pass.
     */
    stats.dce_removed_second = vtx_dce_run(graph);

    /* Compute total */
    stats.total_eliminated = stats.gvn_eliminated_first +
                              stats.sccp_simplified +
                              stats.dce_removed_first +
                              stats.gvn_eliminated_second +
                              stats.dce_removed_second;

    /* Update the hyperblock's estimated native size after optimization.
     * The actual size may be smaller due to eliminated nodes. */
    if (hyperblock->nodes != NULL && hyperblock->node_count > 0) {
        uint32_t live_count = 0;
        for (uint32_t i = 0; i < hyperblock->node_count; i++) {
            vtx_node_t *node = vtx_node_get(&graph->node_table, hyperblock->nodes[i]);
            if (node != NULL && !node->dead) {
                live_count++;
            }
        }
        /* Roughly scale the native size estimate by the fraction of
         * remaining live nodes. */
        if (hyperblock->node_count > 0) {
            double ratio = (double)live_count / (double)hyperblock->node_count;
            hyperblock->estimated_native_size = (uint32_t)(
                (double)hyperblock->estimated_native_size * ratio);
        }
    }

    return stats;
}

/* ========================================================================== */
/* Quick optimization (GVN + DCE only)                                         */
/* ========================================================================== */

uint32_t vtx_cross_trace_optimize_quick(vtx_graph_t *graph)
{
    if (graph == NULL) return 0;

    uint32_t gvn_elim = vtx_gvn_run(graph);
    uint32_t dce_elim = vtx_dce_run(graph);

    return gvn_elim + dce_elim;
}

/* ========================================================================== */
/* Path classification                                                         */
/* ========================================================================== */

bool vtx_cross_trace_is_main_path(const vtx_hyperblock_t *hyperblock,
                                   const vtx_graph_t *graph,
                                   vtx_nodeid_t node_id)
{
    if (hyperblock == NULL || graph == NULL) return false;
    if (hyperblock->source_tree == NULL) return false;
    if (hyperblock->source_tree->root == NULL) return false;

    /* Check if the node is in the root trace's node list */
    const vtx_trace_t *root = hyperblock->source_tree->root;
    for (uint32_t i = 0; i < root->node_count; i++) {
        if (root->nodes[i] == node_id) {
            return true;
        }
    }

    return false;
}

/* ========================================================================== */
/* Live node counting                                                          */
/* ========================================================================== */

uint32_t vtx_cross_trace_live_node_count(const vtx_hyperblock_t *hyperblock,
                                           const vtx_graph_t *graph)
{
    if (hyperblock == NULL || graph == NULL) return 0;
    if (hyperblock->nodes == NULL) return 0;

    uint32_t live = 0;
    for (uint32_t i = 0; i < hyperblock->node_count; i++) {
        const vtx_node_t *node = vtx_node_get_const(&graph->node_table,
                                                      hyperblock->nodes[i]);
        if (node != NULL && !node->dead) {
            live++;
        }
    }
    return live;
}
