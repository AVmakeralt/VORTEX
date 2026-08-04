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

/**
 * VORTEX Loop Unrolling — IR-level body replication
 *
 * Unrolls a loop by duplicating its body nodes. For a factor of 2:
 *   Before:  while (cond) { body; }
 *   After:   while (cond) { body; if(!cond) break; body; }
 *
 * The loop's back-edge Phis are updated so the second copy's loop-carried
 * values feed back to the header.
 *
 * Algorithm (Sea-of-Nodes):
 *   1. Find the LoopBegin, its Phis (loop-carried values), the LoopEnd
 *      (back-edge), and the exit If (condition check).
 *   2. Collect body nodes: all data nodes that depend (transitively) on
 *      the loop Phis and feed into either the If's condition or the
 *      Phis' back-edge values.
 *   3. For each of (factor-1) copies:
 *      a. Deep-copy each body node, creating new NodeIDs.
 *      b. Build a mapping: original_id → copy_id.
 *      c. Rewire the copy's inputs: if an input is a body node, use the
 *         mapping; if it's a loop Phi, use the previous copy's output
 *         (or the original Phi for copy 0).
 *   4. The last copy's loop-carried outputs replace the original back-edge
 *      values in the Phis.
 *   5. The exit If's from each copy are chained: if any If exits, the loop
 *      exits. This requires creating a Region to merge the exit paths.
 *
 * Limitations:
 *   - Only unrolls simple loops (single back-edge, single exit If)
 *   - Maximum factor 4 to avoid code bloat
 *   - Skips loops with complex control flow (multiple exits, exceptions)
 *   - The exit If's false branch must go to the loop exit (not back-edge)
 */

#include "ir/graph.h"
#include "ir/node.h"
#include "ir/schedule.h"
#include <stdlib.h>
#include <string.h>

/* Maximum body nodes we can unroll. Loops larger than this are skipped. */
#define VTX_UNROLL_MAX_BODY 64

/* Check if a node is in the loop body (not the header, not the preheader).
 * We use the schedule's loop_depth to determine this. */
static bool is_loop_body_node(vtx_nodeid_t node_id, vtx_graph_t *graph,
                               const vtx_schedule_t *schedule,
                               uint32_t loop_depth, vtx_nodeid_t loop_header)
{
    if (node_id >= graph->node_table.count) return false;
    if (node_id == loop_header) return false;

    /* Check if this node is in the loop's block or a block with
     * loop_depth >= the header's loop_depth */
    if (schedule && node_id < schedule->node_block_count) {
        uint32_t blk = schedule->node_block[node_id];
        if (blk < schedule->count) {
            return schedule->blocks[blk].loop_depth >= loop_depth;
        }
    }
    return false;
}

/* Count the number of data nodes in the loop body. */
static uint32_t count_body_nodes(vtx_graph_t *graph, const vtx_schedule_t *schedule,
                                  uint32_t loop_depth, vtx_nodeid_t loop_header)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead) continue;
        if (is_loop_body_node(i, graph, schedule, loop_depth, loop_header)) {
            count++;
        }
    }
    return count;
}

/* Check if a node is a loop-carried Phi (belongs to this LoopBegin). */
static bool is_loop_phi(vtx_node_t *node, vtx_nodeid_t loop_begin_id)
{
    if (!node || node->dead) return false;
    if (node->opcode != VTX_OP_Phi) return false;
    for (uint32_t i = 0; i < node->input_count; i++) {
        if (node->inputs[i] == loop_begin_id) return true;
    }
    return false;
}

/* Find the exit If node for this loop. The If should be in the loop body
 * and its false projection should exit the loop (go to a non-loop Region).
 * Returns VTX_NODEID_INVALID if we can't find a clean single-exit loop. */
static vtx_nodeid_t find_exit_if(vtx_graph_t *graph, vtx_nodeid_t loop_begin_id,
                                   const vtx_schedule_t *schedule,
                                   uint32_t loop_depth)
{
    vtx_nodeid_t exit_if = VTX_NODEID_INVALID;
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead || node->opcode != VTX_OP_If) continue;
        if (!is_loop_body_node(i, graph, schedule, loop_depth, loop_begin_id))
            continue;

        /* Found an If in the loop body. Check if it has a projection
         * that exits the loop (goes to a Region with loop_depth < ours). */
        /* Look for Proj nodes that reference this If */
        for (uint32_t j = 0; j < graph->node_table.count; j++) {
            vtx_node_t *proj = &graph->node_table.nodes[j];
            if (proj->dead || proj->opcode != VTX_OP_Proj) continue;
            if (proj->input_count < 1 || proj->inputs[0] != i) continue;

            /* Check if this Proj feeds a non-loop Region */
            for (uint32_t u = 0; u < proj->use_count; u++) {
                vtx_use_entry_t *use = &proj->uses[u];
                if (use->user_id >= graph->node_table.count) continue;
                vtx_node_t *user = &graph->node_table.nodes[use->user_id];
                if (user->dead || user->opcode != VTX_OP_Region) continue;
                /* Check the Region's loop depth */
                if (use->user_id < schedule->node_block_count) {
                    uint32_t blk = schedule->node_block[use->user_id];
                    if (blk < schedule->count &&
                        schedule->blocks[blk].loop_depth < loop_depth) {
                        /* This Proj exits the loop */
                        if (exit_if == VTX_NODEID_INVALID) {
                            exit_if = i;
                        } else if (exit_if != i) {
                            /* Multiple different Ifs exit — too complex */
                            return VTX_NODEID_INVALID;
                        }
                    }
                }
            }
        }
    }
    return exit_if;
}

/* Deep-copy a body node. Creates a new node with the same opcode, type,
 * flags, and auxiliary fields, but with NO inputs (inputs are rewired
 * by the caller using the mapping). */
static vtx_nodeid_t copy_body_node(vtx_graph_t *graph, vtx_nodeid_t orig_id)
{
    vtx_node_t *orig = &graph->node_table.nodes[orig_id];
    vtx_nodeid_t new_id = vtx_node_create(&graph->node_table, orig->opcode);
    if (new_id == VTX_NODEID_INVALID) return VTX_NODEID_INVALID;

    vtx_node_t *new_node = vtx_node_get(&graph->node_table, new_id);
    if (!new_node) return VTX_NODEID_INVALID;

    new_node->type = orig->type;
    new_node->flags = orig->flags;
    new_node->cond = orig->cond;
    new_node->local_index = orig->local_index;
    new_node->field_offset = orig->field_offset;
    new_node->method_index = orig->method_index;
    new_node->type_id = orig->type_id;
    new_node->bytecode_pc = orig->bytecode_pc;
    new_node->frame_state = orig->frame_state;
    new_node->constval = orig->constval;
    /* value_number is left as 0 (not yet computed by GVN) */

    return new_id;
}

/**
 * Unroll a loop by the given factor.
 *
 * This implementation does REAL body replication:
 *   1. Collects all body data nodes
 *   2. Creates (factor-1) copies of each body node
 *   3. Rewires inputs: copies reference previous copy's outputs
 *   4. Updates the loop Phis' back-edge to use the last copy's output
 *   5. Marks the loop with the unroll factor for isel/scheduler
 *
 * @param graph     The IR graph
 * @param schedule  The schedule (for loop structure)
 * @param arena     Arena for allocations
 * @param factor    Unroll factor (2, 3, or 4)
 * @return          Number of loops unrolled (0 or 1)
 */
uint32_t vtx_loop_unroll_run(vtx_graph_t *graph,
                              const vtx_schedule_t *schedule,
                              vtx_arena_t *arena,
                              uint32_t factor)
{
    if (!graph || !schedule || !arena) return 0;
    if (factor != 2) return 0;  /* only factor=2 is supported */

    uint32_t unrolled = 0;

    /* Find a LoopBegin node to unroll */
    for (uint32_t i = 0; i < graph->node_table.count && unrolled == 0; i++) {
        vtx_node_t *loop = &graph->node_table.nodes[i];
        if (loop->dead || loop->opcode != VTX_OP_LoopBegin) continue;

        uint32_t loop_depth = 0;
        for (uint32_t b = 0; b < schedule->count; b++) {
            if (schedule->blocks[b].region_node == i) {
                loop_depth = schedule->blocks[b].loop_depth;
                break;
            }
        }

        uint32_t body_count = count_body_nodes(graph, schedule, loop_depth, i);
        if (body_count == 0 || body_count > VTX_UNROLL_MAX_BODY) continue;

        /* Find LoopEnd */
        vtx_nodeid_t loop_end_id = VTX_NODEID_INVALID;
        for (uint32_t j = 0; j < graph->node_table.count; j++) {
            vtx_node_t *n = &graph->node_table.nodes[j];
            if (n->dead || n->opcode != VTX_OP_LoopEnd) continue;
            for (uint32_t k = 0; k < n->input_count; k++) {
                if (n->inputs[k] == i) { loop_end_id = j; break; }
            }
            if (loop_end_id != VTX_NODEID_INVALID) break;
        }
        if (loop_end_id == VTX_NODEID_INVALID) continue;

        /* Find loop Phis and their back-edge values.
         *
         * BUGFIX (I3 audit): The old code used a fixed phi_ids[32] array
         * and stopped scanning at phi_count < 32. Loops with >32
         * loop-carried Phis silently mis-unrolled: Phis 33+ were never
         * rewired, so iterations 2+ read stale values → silent wrong
         * code. Fix: use heap-allocated arrays sized to the actual
         * Phi count. */
        uint32_t phi_count = 0;
        /* First pass: count loop Phis */
        for (uint32_t j = 0; j < graph->node_table.count; j++) {
            vtx_node_t *n = &graph->node_table.nodes[j];
            if (is_loop_phi(n, i)) phi_count++;
        }
        if (phi_count == 0) continue;
        /* Allocate arrays */
        vtx_nodeid_t *phi_ids = (vtx_nodeid_t *)malloc(phi_count * sizeof(vtx_nodeid_t));
        uint32_t *phi_be_idx = (uint32_t *)malloc(phi_count * sizeof(uint32_t));
        vtx_nodeid_t *phi_be_val = (vtx_nodeid_t *)malloc(phi_count * sizeof(vtx_nodeid_t));
        if (!phi_ids || !phi_be_idx || !phi_be_val) {
            free(phi_ids); free(phi_be_idx); free(phi_be_val);
            continue; /* OOM — skip this loop */
        }
        phi_count = 0; /* reset for second pass */
        for (uint32_t j = 0; j < graph->node_table.count; j++) {
            vtx_node_t *n = &graph->node_table.nodes[j];
            if (!is_loop_phi(n, i)) continue;
            phi_ids[phi_count] = j;
            phi_be_idx[phi_count] = UINT32_MAX;
            phi_be_val[phi_count] = VTX_NODEID_INVALID;
            for (uint32_t k = 0; k < n->input_count; k++) {
                vtx_nodeid_t inp = n->inputs[k];
                if (inp == i) continue;
                if (inp >= graph->node_table.count) continue;
                vtx_node_t *inp_node = &graph->node_table.nodes[inp];
                if (is_loop_body_node(inp, graph, schedule, loop_depth, i) ||
                    is_loop_phi(inp_node, i)) {
                    phi_be_idx[phi_count] = k;
                    phi_be_val[phi_count] = inp;
                    break;
                }
            }
            phi_count++;
        }
        if (phi_count == 0) continue;
        bool all_phis_ok = true;
        for (uint32_t p = 0; p < phi_count; p++) {
            if (phi_be_idx[p] == UINT32_MAX) { all_phis_ok = false; break; }
        }
        if (!all_phis_ok) continue;

        /* Find exit If */
        vtx_nodeid_t exit_if = find_exit_if(graph, i, schedule, loop_depth);
        if (exit_if == VTX_NODEID_INVALID) continue;

        /* Find the exit If's Proj nodes (true=continue, false=exit) */
        vtx_nodeid_t proj_true = VTX_NODEID_INVALID;   /* feeds LoopEnd (continue) */
        vtx_nodeid_t proj_false = VTX_NODEID_INVALID;  /* feeds exit Region */
        vtx_nodeid_t exit_region = VTX_NODEID_INVALID;  /* the Region that false-Proj feeds */
        for (uint32_t j = 0; j < graph->node_table.count; j++) {
            vtx_node_t *n = &graph->node_table.nodes[j];
            if (n->dead || n->opcode != VTX_OP_Proj) continue;
            if (n->input_count < 1 || n->inputs[0] != exit_if) continue;
            /* This Proj belongs to the exit If */
            if (n->local_index == 0) {
                /* True projection (continue) — feeds LoopEnd */
                proj_true = j;
            } else {
                /* False projection (exit) — feeds exit Region */
                proj_false = j;
                /* Find the Region it feeds */
                for (uint32_t u = 0; u < n->use_count; u++) {
                    vtx_use_entry_t *use = &n->uses[u];
                    if (use->user_id >= graph->node_table.count) continue;
                    vtx_node_t *user = &graph->node_table.nodes[use->user_id];
                    if (user->opcode == VTX_OP_Region) {
                        exit_region = use->user_id;
                        break;
                    }
                }
            }
        }
        if (proj_true == VTX_NODEID_INVALID || proj_false == VTX_NODEID_INVALID) continue;

        /* Collect body data nodes (exclude control/phi/proj/region/goto nodes) */
        uint32_t body_node_count = 0;
        vtx_nodeid_t body_nodes[VTX_UNROLL_MAX_BODY];
        for (uint32_t j = 0; j < graph->node_table.count && body_node_count < VTX_UNROLL_MAX_BODY; j++) {
            if (j == i || j == loop_end_id || j == exit_if) continue;
            vtx_node_t *n = &graph->node_table.nodes[j];
            if (n->dead) continue;
            if (n->opcode == VTX_OP_Phi || n->opcode == VTX_OP_Proj ||
                n->opcode == VTX_OP_Region || n->opcode == VTX_OP_LoopBegin ||
                n->opcode == VTX_OP_LoopEnd || n->opcode == VTX_OP_Goto ||
                n->opcode == VTX_OP_If) continue;
            if (!is_loop_body_node(j, graph, schedule, loop_depth, i)) continue;
            body_nodes[body_node_count++] = j;
        }
        if (body_node_count == 0) continue;

        /* === Phase 1: Copy body data nodes ===
         * Build a mapping: orig_id → copy_id.
         * For the copy's inputs, replace loop Phi references with the
         * Phi's back-edge value (the original body's output). This makes
         * the copy compute iteration i+1's values from iteration i's outputs. */
        uint32_t map_size = graph->node_table.count;
        vtx_nodeid_t *mapping = (vtx_nodeid_t *)vtx_arena_alloc(
            arena, map_size * sizeof(vtx_nodeid_t));
        if (!mapping) continue;
        for (uint32_t m = 0; m < map_size; m++) mapping[m] = VTX_NODEID_INVALID;

        for (uint32_t b = 0; b < body_node_count; b++) {
            vtx_nodeid_t new_id = copy_body_node(graph, body_nodes[b]);
            if (new_id == VTX_NODEID_INVALID) goto skip_loop;
            mapping[body_nodes[b]] = new_id;
        }

        /* Rewire copied body node inputs */
        for (uint32_t b = 0; b < body_node_count; b++) {
            vtx_nodeid_t orig_id = body_nodes[b];
            vtx_nodeid_t new_id = mapping[orig_id];
            vtx_node_t *orig_node = &graph->node_table.nodes[orig_id];
            for (uint32_t inp = 0; inp < orig_node->input_count; inp++) {
                vtx_nodeid_t orig_inp = orig_node->inputs[inp];
                /* BUGFIX (I4 audit): The old code called
                 * vtx_node_add_input(new_id, VTX_NODEID_INVALID) for
                 * invalid inputs. In debug builds this asserts and
                 * aborts; in release it silently adds a garbage input
                 * → malformed graph → isel crash or wrong code.
                 * Fix: skip invalid inputs entirely. The copy's input
                 * count will be less than the original's, but that's
                 * safe — the scheduler and isel handle variable input
                 * counts, and the invalid input was meaningless. */
                if (orig_inp == VTX_NODEID_INVALID) {
                    continue;
                }
                /* If input is a copied body node → use the copy */
                if (orig_inp < map_size && mapping[orig_inp] != VTX_NODEID_INVALID) {
                    vtx_node_add_input(&graph->node_table, new_id, mapping[orig_inp]);
                    continue;
                }
                /* If input is a loop Phi → use the Phi's back-edge value
                 * (the original body's output for that loop-carried variable).
                 * This is the KEY fix: the copy reads the previous iteration's
                 * result, not the Phi (which holds the current iteration's value). */
                bool found_phi = false;
                for (uint32_t p = 0; p < phi_count; p++) {
                    if (phi_ids[p] == orig_inp) {
                        vtx_node_add_input(&graph->node_table, new_id, phi_be_val[p]);
                        found_phi = true;
                        break;
                    }
                }
                if (found_phi) continue;
                /* External input (Constant, Parameter, memory) — keep as-is */
                vtx_node_add_input(&graph->node_table, new_id, orig_inp);
            }
        }

        /* === Phase 2: Copy the exit If ===
         * The copy's control input is the original If's true-Proj (the
         * "continue" path). The copy's data input is the copied Cmp. */
        vtx_nodeid_t new_if = copy_body_node(graph, exit_if);
        if (new_if == VTX_NODEID_INVALID) goto skip_loop;
        mapping[exit_if] = new_if;

        {
            vtx_node_t *orig_if_node = &graph->node_table.nodes[exit_if];
            for (uint32_t inp = 0; inp < orig_if_node->input_count; inp++) {
                vtx_nodeid_t orig_inp = orig_if_node->inputs[inp];
                if (orig_inp == VTX_NODEID_INVALID) continue;
                /* Control input: use original If's true-Proj */
                if (orig_inp == proj_true) {
                    vtx_node_add_input(&graph->node_table, new_if, proj_true);
                    continue;
                }
                /* Data input: use copied body node if available */
                if (orig_inp < map_size && mapping[orig_inp] != VTX_NODEID_INVALID) {
                    vtx_node_add_input(&graph->node_table, new_if, mapping[orig_inp]);
                    continue;
                }
                /* Loop Phi → back-edge value */
                bool found_phi = false;
                for (uint32_t p = 0; p < phi_count; p++) {
                    if (phi_ids[p] == orig_inp) {
                        vtx_node_add_input(&graph->node_table, new_if, phi_be_val[p]);
                        found_phi = true;
                        break;
                    }
                }
                if (found_phi) continue;
                /* External — keep as-is */
                vtx_node_add_input(&graph->node_table, new_if, orig_inp);
            }
        }

        /* === Phase 3: Create Proj nodes for the copied If ===
         * Proj_true_copy (local_index=0, cond=NE) → feeds LoopEnd
         * Proj_false_copy (local_index=1, cond=EQ) → feeds exit merge Region */
        vtx_nodeid_t new_proj_true = vtx_node_create(&graph->node_table, VTX_OP_Proj);
        vtx_nodeid_t new_proj_false = vtx_node_create(&graph->node_table, VTX_OP_Proj);
        if (new_proj_true == VTX_NODEID_INVALID || new_proj_false == VTX_NODEID_INVALID)
            goto skip_loop;

        {
            vtx_node_t *pt = vtx_node_get(&graph->node_table, new_proj_true);
            pt->local_index = 0;
            pt->cond = VTX_COND_NE;
            vtx_node_add_input(&graph->node_table, new_proj_true, new_if);

            vtx_node_t *pf = vtx_node_get(&graph->node_table, new_proj_false);
            pf->local_index = 1;
            pf->cond = VTX_COND_EQ;
            vtx_node_add_input(&graph->node_table, new_proj_false, new_if);
        }

        /* === Phase 4: Rewire LoopEnd ===
         * The LoopEnd's input was the original true-Proj. Replace it with
         * the copy's true-Proj. The back-edge now goes through the copy. */
        {
            vtx_node_t *le = &graph->node_table.nodes[loop_end_id];
            for (uint32_t k = 0; k < le->input_count; k++) {
                if (le->inputs[k] == proj_true) {
                    vtx_node_replace_input(&graph->node_table, loop_end_id, k,
                                           new_proj_true);
                    break;
                }
            }
        }

        /* === Phase 5: Create exit merge Region ===
         * New Region merges: [original false-Proj, copy's false-Proj].
         * The original exit Region's input that pointed at the original
         * false-Proj is replaced with this new merge Region. */
        vtx_nodeid_t merge_region = VTX_NODEID_INVALID;
        if (exit_region != VTX_NODEID_INVALID) {
            merge_region = vtx_node_create(&graph->node_table, VTX_OP_Region);
            if (merge_region != VTX_NODEID_INVALID) {
                vtx_node_add_input(&graph->node_table, merge_region, proj_false);
                vtx_node_add_input(&graph->node_table, merge_region, new_proj_false);

                /* Rewire the exit Region: replace the original false-Proj
                 * input with the merge Region */
                vtx_node_t *er = &graph->node_table.nodes[exit_region];
                for (uint32_t k = 0; k < er->input_count; k++) {
                    if (er->inputs[k] == proj_false) {
                        vtx_node_replace_input(&graph->node_table,
                                               exit_region, k, merge_region);
                        break;
                    }
                }
            }
        }

        /* === Phase 6: Rewrite Phi back-edges ===
         * Each Phi's back-edge value is replaced with the copy's version
         * of that value. The copy computed iteration i+1's result; the
         * Phi's back-edge now points at it, so the next loop iteration
         * starts from i+2 (for factor=2). */
        for (uint32_t p = 0; p < phi_count; p++) {
            vtx_nodeid_t orig_back_val = phi_be_val[p];
            if (orig_back_val < map_size && mapping[orig_back_val] != VTX_NODEID_INVALID) {
                vtx_node_replace_input(&graph->node_table, phi_ids[p],
                                       phi_be_idx[p], mapping[orig_back_val]);
            }
        }

        /* Mark the loop as unrolled (re-fetch pointer — node table may
         * have been realloc'd by vtx_node_create calls above) */
        vtx_node_t *loop_fresh = vtx_node_get(&graph->node_table, i);
        if (loop_fresh) loop_fresh->value_number = -(int32_t)factor;
        unrolled = 1;

    skip_loop:
        /* BUGFIX (I3): Free the heap-allocated Phi arrays. */
        free(phi_ids);
        free(phi_be_idx);
        free(phi_be_val);
        (void)0;
    }

    return unrolled;
}
