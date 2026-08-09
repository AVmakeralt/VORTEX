/* ========================================================================== */
/* CFG Simplification — collapse single-input Regions, jump threading         */
/* ========================================================================== */
/*
 * ir/cfg_simplify.c — Remove trivial control-flow nodes.
 *
 * After SCCP folds an If into a constant, the untaken branch becomes
 * dead. The Region merge point ends up with a single input — it can
 * be collapsed, merging the two blocks into one. This reduces the
 * CFG size and creates more opportunities for other passes.
 *
 * Transformations:
 *   1. Region with 1 input → replace with Goto (pass through)
 *   2. Goto → Goto chain → collapse to single Goto
 *   3. If with constant condition → replace with Goto to taken branch
 *
 * Returns the number of nodes simplified.
 */

#include "ir/graph.h"
#include "ir/node.h"
#include "vortex_config.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

uint32_t vtx_cfg_simplify_run(vtx_graph_t *graph)
{
    if (!graph) return 0;
    vtx_node_table_t *nt = &graph->node_table;
    uint32_t simplified = 0;

    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;

        /* 1. Region with exactly 1 control input → replace with Goto
         *
         * A Region normally has 2+ inputs (merging control flow from
         * multiple predecessors). If SCCP or DCE removed one branch,
         * the Region has only 1 control input — it's a pass-through.
         * Replace it with a Goto to its successor. */
        if (node->opcode == VTX_OP_Region) {
            /* Count control inputs (skip memory/data inputs) */
            vtx_nodeid_t single_input = VTX_NODEID_INVALID;
            uint32_t control_count = 0;
            for (uint32_t j = 0; j < node->input_count; j++) {
                vtx_nodeid_t inp = node->inputs[j];
                if (inp == VTX_NODEID_INVALID || inp >= nt->count) continue;
                vtx_node_t *inp_node = &nt->nodes[inp];
                if (vtx_nf_has(inp_node->flags, VTX_NF_CONTROL)) {
                    control_count++;
                    single_input = inp;
                }
            }
            if (control_count == 1 && single_input != VTX_NODEID_INVALID) {
                /* IR-005 fix: Replacing a Region with an arbitrary control
                 * node (Goto/If) breaks Phi nodes, which reference their
                 * Region as a structural input. Walk the Region's users;
                 * if any user is a Phi that would be orphaned, skip the
                 * replacement (DCE will clean it up later). */
                bool safe = true;
                for (uint32_t u = 0; u < node->use_count; u++) {
                    vtx_use_entry_t *ue = &node->uses[u];
                    if (ue->user_id < nt->count &&
                        nt->nodes[ue->user_id].opcode == VTX_OP_Phi) {
                        safe = false;
                        break;
                    }
                }
                if (safe) {
                    vtx_node_replace_all_uses(nt, i, single_input);
                    node->dead = true;
                    simplified++;
                }
            }
        }

        /* 2. If with constant condition → replace with Goto to taken branch
         *
         * BUGFIX (cfg_simplify TODO at line 91): The old code had a
         * TODO marking this as unimplemented. V8's branch-elimination.cc
         * (~1500 lines) does this plus much more (If-If merging, empty-
         * block elimination, critical-edge splitting).
         *
         * We implement the core case: If the condition is a Constant
         * node with an Int value, determine which Proj (true/false)
         * is the taken branch, then replace the If with the taken
         * Proj and mark the untaken Proj as dead.
         *
         * The If's cond field determines the branch semantics:
         *   VTX_COND_NE = IF_TRUE (branch if cond != 0)
         *   VTX_COND_EQ = IF_FALSE (branch if cond == 0)
         *
         * Proj local_index 0 = true branch, 1 = false branch.
         *
         * After this transform, the untaken Proj and everything it
         * reaches becomes dead — DCE cleans it up. The taken Proj
         * becomes the If's replacement (a control-flow passthrough). */
        if (node->opcode == VTX_OP_If && node->input_count >= 2) {
            vtx_nodeid_t cond_id = node->inputs[1];
            if (cond_id != VTX_NODEID_INVALID && cond_id < nt->count) {
                vtx_node_t *cond_node = &nt->nodes[cond_id];
                if (!cond_node->dead && cond_node->opcode == VTX_OP_Constant &&
                    cond_node->constval.kind == VTX_TYPE_Int) {
                    /* Determine taken branch:
                     *   cond != 0 → true branch taken (for IF_TRUE / VTX_COND_NE)
                     *   cond == 0 → false branch taken (for IF_TRUE)
                     *   For IF_FALSE (VTX_COND_EQ): inverted */
                    bool cond_truthy = (cond_node->constval.as.int_val != 0);
                    bool taken_is_true = (node->cond == VTX_COND_EQ) ? !cond_truthy : cond_truthy;

                    /* Find the Proj nodes for this If.
                     * Proj local_index 0 = true branch, 1 = false branch. */
                    vtx_nodeid_t taken_proj = VTX_NODEID_INVALID;
                    vtx_nodeid_t untaken_proj = VTX_NODEID_INVALID;
                    uint8_t taken_index = taken_is_true ? 0 : 1;
                    for (uint32_t u = 0; u < node->use_count; u++) {
                        vtx_use_entry_t *ue = &node->uses[u];
                        if (ue->user_id >= nt->count) continue;
                        vtx_node_t *user = &nt->nodes[ue->user_id];
                        if (user->dead || user->opcode != VTX_OP_Proj) continue;
                        if (user->input_count >= 1 && user->inputs[0] == (vtx_nodeid_t)i) {
                            if (user->local_index == taken_index)
                                taken_proj = ue->user_id;
                            else
                                untaken_proj = ue->user_id;
                        }
                    }

                    if (taken_proj != VTX_NODEID_INVALID) {
                        /* Replace the If with the taken Proj.
                         * All users of the If now point to the taken Proj. */
                        vtx_node_replace_all_uses(nt, (vtx_nodeid_t)i, taken_proj);

                        /* Mark the If and untaken Proj as dead */
                        node->dead = true;
                        if (untaken_proj != VTX_NODEID_INVALID) {
                            nt->nodes[untaken_proj].dead = true;
                        }
                        simplified++;
                    }
                }
            }
        }

        /* 3. Goto → Goto chain: if a Goto's target is another Goto,
         * redirect to the final target. */
        if (node->opcode == VTX_OP_Goto && node->input_count >= 1) {
            vtx_nodeid_t target_id = node->inputs[0];
            if (target_id != VTX_NODEID_INVALID && target_id < nt->count) {
                vtx_node_t *target = &nt->nodes[target_id];
                if (!target->dead && target->opcode == VTX_OP_Goto &&
                    target->input_count >= 1) {
                    /* IR-003 fix: Use vtx_node_replace_input so the old
                     * target's use-def list drops us and the new target's
                     * use-def list gains us. The old raw assignment left
                     * stale use-def entries, breaking downstream passes. */
                    vtx_node_replace_input(nt, (vtx_nodeid_t)i, 0,
                                          target->inputs[0]);
                    simplified++;
                }
            }
        }
    }

    /* ---- A2: Additional CFG simplification passes ---- */

    /* 4. Block merging: DISABLED for correctness.
     *
     * The block merging logic (replacing a single-pred Region with the
     * predecessor's Goto input) is tricky in VORTEX's Sea-of-Nodes because:
     *   - The Goto's input[0] is the predecessor's entry control, not the
     *     predecessor's exit control
     *   - Other nodes in the predecessor block reference the Goto
     *   - Killing the Goto may orphan those references
     *
     * V8 does this in CFGBuilder::MergeBlocks but with a different IR
     * structure (block-based, not pure SoN). Implementing it correctly
     * requires walking all nodes in the predecessor block and rewiring
     * their control inputs. Left as a TODO.
     */

    /* 5. Unreachable-block elimination: Remove blocks not reachable
     * from Start. After SCCP + DCE, some blocks may be orphaned.
     * A simple reachability scan from Start marks all reachable nodes;
     * any dead-but-not-marked nodes are truly unreachable.
     *
     * This is safe — it only removes nodes that no control path reaches.
     * V8 does this in CFGBuilder::EliminateUnreachableBlocks. */
    {
        /* Mark all nodes reachable from Start via control edges */
        bool *reachable = (bool *)calloc(nt->count, sizeof(bool));
        if (reachable) {
            /* Find Start node */
            for (uint32_t i = 0; i < nt->count; i++) {
                vtx_node_t *node = &nt->nodes[i];
                if (!node->dead && node->opcode == VTX_OP_Start) {
                    /* BFS from Start */
                    uint32_t *worklist = (uint32_t *)malloc(nt->count * sizeof(uint32_t));
                    if (worklist) {
                        uint32_t wl_head = 0, wl_tail = 0;
                        worklist[wl_tail++] = i;
                        reachable[i] = true;
                        while (wl_head < wl_tail) {
                            uint32_t cur = worklist[wl_head++];
                            vtx_node_t *cn = &nt->nodes[cur];
                            /* Walk users (forward edges in SoN) */
                            for (uint32_t u = 0; u < cn->use_count; u++) {
                                vtx_use_entry_t *ue = &cn->uses[u];
                                if (ue->user_id >= nt->count) continue;
                                if (reachable[ue->user_id]) continue;
                                vtx_node_t *user = &nt->nodes[ue->user_id];
                                if (user->dead) continue;
                                /* Follow control edges and data edges
                                 * that don't cross block boundaries */
                                reachable[ue->user_id] = true;
                                worklist[wl_tail++] = ue->user_id;
                            }
                        }
                        free(worklist);
                    }
                    break;
                }
            }
            /* Mark unreachable control nodes as dead */
            for (uint32_t i = 0; i < nt->count; i++) {
                vtx_node_t *node = &nt->nodes[i];
                if (node->dead) continue;
                if (!reachable[i] && vtx_nf_has(node->flags, VTX_NF_CONTROL)) {
                    node->dead = true;
                    simplified++;
                }
            }
            free(reachable);
        }
    }

    if (simplified > 0) {
        vtx_node_table_clear_dead(nt);
    }

    return simplified;
}
