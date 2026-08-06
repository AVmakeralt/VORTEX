/* ============================================================================ *
 * AI-MODIFIED CODE
 *
 * This file was originally written by a human developer. It has been
 * substantially modified by an AI assistant (GLM/Z.ai) for bug fixes,
 * performance improvements, and feature additions.
 *
 * Original human-written structure is preserved; AI changes are marked
 * with bug fix IDs (B1-B28) or perf notes (Perf 1-10) in comments.
 *
 * If reviewing, please verify AI changes against the original logic.
 * ============================================================================ */

#include "ir/dce.h"

#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Is a node essential (must be kept even with no outputs)?                     */
/* ========================================================================== */

static bool is_node_essential(const vtx_node_t *node)
{
    /* Start node is always essential */
    if (node->opcode == VTX_OP_Start) return true;

    /* Province (initial memory) is essential */
    if (node->opcode == VTX_OP_Province) return true;

    /* Control nodes are essential — they define the CFG structure */
    if (vtx_nf_has(node->flags, VTX_NF_CONTROL)) return true;

    /* Memory nodes are essential — they define memory ordering */
    if (vtx_nf_has(node->flags, VTX_NF_MEMORY)) return true;

    /* Side-effecting nodes must be kept — this includes Guards,
     * DeoptGuards, Stores, Calls, CheckCasts, etc.
     *
     * DESIGN PRINCIPLE: No pass may reduce the number of runtime decision
     * points without explicit authorization. Guards and DeoptGuards are
     * runtime safety checks — they deopt to the interpreter if a
     * speculative assumption fails. Even if a guard has zero data users,
     * it is still a runtime decision point that must be preserved.
     *
     * Only an explicit, authorized pass (e.g., bounds_check_elimination
     * with a formal proof, or guard_merging which replaces two guards
     * with one) may remove guards. DCE must not. */
    if (vtx_nf_has(node->flags, VTX_NF_SIDE_EFFECT)) return true;

    /* Pinned nodes must be kept (their position in the graph matters) */
    if (vtx_nf_has(node->flags, VTX_NF_PINNED)) return true;

    /* Parameters are essential — they represent method inputs */
    if (node->opcode == VTX_OP_Parameter) return true;

    /* FrameState nodes are essential for deopt — they hold the interpreter
     * state needed to reconstruct the frame if a guard fails. Even if no
     * live guard references them directly, they may be needed by the side
     * table for deopt. Only an explicit deopt-table-cleanup pass may
     * remove unused FrameStates. */
    if (node->opcode == VTX_OP_FrameState) return true;

    /* CheckCast has side effects (can throw ClassCastException) */
    if (node->opcode == VTX_OP_CheckCast) return true;

    return false;
}

/* ========================================================================== */
/* DCE implementation                                                          */
/* ========================================================================== */

uint32_t vtx_dce_run(vtx_graph_t *graph)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");

    vtx_node_table_t *nt = &graph->node_table;
    uint32_t total_removed = 0;

    /* IR-009 fix: use a worklist of newly-dead nodes instead of O(N²)
     * rescan. Seed the worklist with all nodes that are already dead
     * (output_count == 0 and not essential). When we kill a node, we
     * decrement its producers' output_count and push any producer that
     * reaches zero. This is O(N) total instead of O(N²). */
    vtx_nodeid_t *wl = NULL;
    if (nt->count > 0) {
        wl = (vtx_nodeid_t *)malloc(nt->count * sizeof(vtx_nodeid_t));
        if (!wl) {
            /* OOM — fall back to the old O(N²) scan */
            bool changed = true;
            while (changed) {
                changed = false;
                for (uint32_t i = 0; i < nt->count; i++) {
                    vtx_node_t *node = &nt->nodes[i];
                    if (node->dead) continue;
                    if (node->output_count == 0 && !is_node_essential(node)) {
                        for (uint32_t j = 0; j < node->input_count; j++) {
                            vtx_nodeid_t inp = node->inputs[j];
                            if (inp != VTX_NODEID_INVALID && inp < nt->count) {
                                vtx_node_t *producer = &nt->nodes[inp];
                                vtx_node_remove_use_entry(producer, node->id, j);
                                if (producer->output_count > 0) producer->output_count--;
                            }
                        }
                        node->dead = true;
                        node->input_count = 0;
                        node->use_count = 0;
                        total_removed++;
                        changed = true;
                    }
                }
            }
            return total_removed;
        }
    }

    uint32_t wl_count = 0;
    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (!node->dead && node->output_count == 0 && !is_node_essential(node)) {
            wl[wl_count++] = (vtx_nodeid_t)i;
        }
    }

    while (wl_count > 0) {
        vtx_nodeid_t nid = wl[--wl_count];
        vtx_node_t *node = &nt->nodes[nid];
        if (node->dead) continue;

        /* Disconnect inputs */
        for (uint32_t j = 0; j < node->input_count; j++) {
            vtx_nodeid_t inp = node->inputs[j];
            if (inp != VTX_NODEID_INVALID && inp < nt->count) {
                vtx_node_t *producer = &nt->nodes[inp];
                vtx_node_remove_use_entry(producer, nid, j);
                if (producer->output_count > 0) producer->output_count--;
                /* If the producer is now dead and not essential, push it */
                if (producer->output_count == 0 && !producer->dead &&
                    !is_node_essential(producer)) {
                    /* Guard against worklist overflow — shouldn't happen
                     * since each node is pushed at most once, but check. */
                    if (wl_count < nt->count) {
                        wl[wl_count++] = inp;
                    }
                }
            }
        }

        node->dead = true;
        node->input_count = 0;
        node->use_count = 0;
        total_removed++;
    }

    free(wl);
    return total_removed;
}
