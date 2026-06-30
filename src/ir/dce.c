#include "ir/dce.h"

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
    bool changed = true;

    while (changed) {
        changed = false;

        for (uint32_t i = 0; i < nt->count; i++) {
            vtx_node_t *node = &nt->nodes[i];

            /* Skip already-dead nodes */
            if (node->dead) continue;

            /* A node is dead if it has no outputs and is not essential */
            if (node->output_count == 0 && !is_node_essential(node)) {
                /* This node is dead. Disconnect its inputs. */
                for (uint32_t j = 0; j < node->input_count; j++) {
                    vtx_nodeid_t inp = node->inputs[j];
                    if (inp != VTX_NODEID_INVALID && inp < nt->count) {
                        vtx_node_t *producer = &nt->nodes[inp];
                        /* Remove this node's use entry from the producer's
                         * use-def list BEFORE decrementing output_count.
                         * Without this, the producer's use-def list retains
                         * stale entries referencing the dead node. */
                        vtx_node_remove_use_entry(producer, node->id, j);
                        if (producer->output_count > 0) {
                            producer->output_count--;
                        }
                    }
                }

                /* Mark as dead */
                node->dead = true;
                node->input_count = 0;
                /* Clear this node's own use list — no live node should
                 * reference a dead node, and stale use entries corrupt
                 * later passes that traverse use-def lists. */
                node->use_count = 0;

                total_removed++;
                changed = true;
            }
        }
    }

    /* After DCE, clean up: for any dead node that still has inputs
     * (shouldn't happen with correct output_count tracking, but be safe),
     * disconnect them. */
    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (!node->dead) continue;
        if (node->input_count > 0) {
            for (uint32_t j = 0; j < node->input_count; j++) {
                vtx_nodeid_t inp = node->inputs[j];
                if (inp != VTX_NODEID_INVALID && inp < nt->count) {
                    vtx_node_t *producer = &nt->nodes[inp];
                    /* Remove use entry from producer's use-def list to
                     * prevent stale references to this dead node. */
                    vtx_node_remove_use_entry(producer, node->id, j);
                    if (producer->output_count > 0) {
                        producer->output_count--;
                    }
                }
            }
            node->input_count = 0;
        }
        /* Clear this dead node's own use list as well. */
        node->use_count = 0;
    }

    return total_removed;
}
