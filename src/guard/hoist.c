#include "guard/hoist.h"
#include <string.h>

/* ========================================================================== */
/* Helper: check if a node is a guard opcode                                   */
/* ========================================================================== */

static bool is_guard_opcode(vtx_node_opcode_t opcode)
{
    return opcode == VTX_OP_Guard || opcode == VTX_OP_DeoptGuard;
}

/* ========================================================================== */
/* Helper: get the loop depth of a node from the schedule                      */
/* ========================================================================== */

static uint32_t node_loop_depth(const vtx_schedule_t *schedule, vtx_nodeid_t node_id)
{
    if (node_id >= schedule->node_block_count) return 0;
    uint32_t block_idx = schedule->node_block[node_id];
    if (block_idx >= schedule->count) return 0;
    return schedule->blocks[block_idx].loop_depth;
}

/* ========================================================================== */
/* Helper: get the block index of a node from the schedule                     */
/* ========================================================================== */

static uint32_t node_block_idx(const vtx_schedule_t *schedule, vtx_nodeid_t node_id)
{
    if (node_id >= schedule->node_block_count) return (uint32_t)-1;
    return schedule->node_block[node_id];
}

/* ========================================================================== */
/* Check if a guard is loop-invariant                                          */
/* ========================================================================== */

bool vtx_hoist_is_invariant(const vtx_graph_t *graph,
                              const vtx_schedule_t *schedule,
                              vtx_nodeid_t guard_node,
                              uint32_t loop_depth)
{
    if (graph == NULL || schedule == NULL) return false;
    if (loop_depth == 0) return true; /* not in a loop → trivially invariant */

    const vtx_node_t *guard = vtx_node_get_const(&graph->node_table, guard_node);
    if (guard == NULL || !is_guard_opcode(guard->opcode)) return false;

    /* A guard is loop-invariant if ALL its inputs are defined outside the loop.
     * We check each input:
     *   - If the input is a Constant or Start, it's always invariant
     *   - If the input is a Parameter, it's always invariant (defined at entry)
     *   - Otherwise, check that the input's block has loop_depth < guard's loop_depth */
    for (uint32_t i = 0; i < guard->input_count; i++) {
        vtx_nodeid_t input_id = guard->inputs[i];
        if (input_id == VTX_NODEID_INVALID) continue;

        /* Structural inputs: always invariant */
        const vtx_node_t *input = vtx_node_get_const(&graph->node_table, input_id);
        if (input == NULL) continue;

        if (input->opcode == VTX_OP_Constant ||
            input->opcode == VTX_OP_Start ||
            input->opcode == VTX_OP_Province ||
            input->opcode == VTX_OP_Parameter) {
            continue; /* these are always defined outside any loop */
        }

        /* Check the input's loop depth */
        uint32_t input_depth = node_loop_depth(schedule, input_id);
        if (input_depth >= loop_depth) {
            /* This input is defined inside the loop → guard is NOT invariant */
            return false;
        }
    }

    return true;
}

/* ========================================================================== */
/* Find preheader for a loop header                                            */
/* ========================================================================== */

uint32_t vtx_hoist_find_preheader(const vtx_schedule_t *schedule,
                                    uint32_t header_block)
{
    if (schedule == NULL || header_block >= schedule->count) return (uint32_t)-1;

    const vtx_schedule_block_t *header = &schedule->blocks[header_block];
    if (!header->is_loop_header) return (uint32_t)-1;

    /* The preheader is the unique predecessor with loop_depth < header's loop_depth.
     * For a well-formed loop, there should be exactly one such predecessor
     * (the entry edge), plus one or more back-edge predecessors (same loop_depth).
     * We look for the predecessor with lower loop_depth. */
    for (uint32_t i = 0; i < header->pred_count; i++) {
        uint32_t pred_idx = header->pred_blocks[i];
        if (pred_idx >= schedule->count) continue;

        const vtx_schedule_block_t *pred = &schedule->blocks[pred_idx];
        if (pred->loop_depth < header->loop_depth) {
            return pred_idx;
        }
    }

    /* No preheader found — this can happen for irreducible control flow.
     * In that case, we cannot hoist guards out of this loop. */
    return (uint32_t)-1;
}

/* ========================================================================== */
/* Main hoisting algorithm                                                     */
/* ========================================================================== */

vtx_hoist_result_t vtx_hoist_guards(vtx_graph_t *graph,
                                      vtx_schedule_t *schedule,
                                      vtx_arena_t *arena)
{
    vtx_hoist_result_t result;
    memset(&result, 0, sizeof(result));

    if (graph == NULL || schedule == NULL || arena == NULL) {
        return result;
    }

    /* Iterate over all blocks in the schedule */
    for (uint32_t block_idx = 0; block_idx < schedule->count; block_idx++) {
        vtx_schedule_block_t *block = &schedule->blocks[block_idx];

        /* Only process blocks that are inside loops */
        if (block->loop_depth == 0) continue;

        /* Find the preheader for this loop */
        uint32_t preheader_idx = (uint32_t)-1;

        /* Walk up to find the loop header for this block.
         * The loop header is the nearest ancestor block with is_loop_header=true
         * and the same loop_depth as this block, or the block itself if it is
         * the loop header. For simplicity, we search for the loop header that
         * dominates this block by checking all loop headers. */
        for (uint32_t h = 0; h < schedule->count; h++) {
            if (schedule->blocks[h].is_loop_header &&
                schedule->blocks[h].loop_depth == block->loop_depth) {
                /* Check if this loop header dominates our block.
                 * Simplified check: if the block's loop_depth matches,
                 * it's in this loop's body. A more precise check would
                 * use dominator tree, but this approximation works for
                 * structured loops. */
                preheader_idx = vtx_hoist_find_preheader(schedule, h);
                if (preheader_idx != (uint32_t)-1) break;
            }
        }

        if (preheader_idx == (uint32_t)-1) continue;

        /* Check each node in this block for guard opcodes */
        for (uint32_t n = 0; n < block->node_count; n++) {
            vtx_nodeid_t node_id = block->nodes[n];
            vtx_node_t *node = vtx_node_get(&graph->node_table, node_id);
            if (node == NULL || node->dead) continue;

            if (!is_guard_opcode(node->opcode)) continue;

            result.guards_checked++;

            /* Check if this guard is loop-invariant */
            if (!vtx_hoist_is_invariant(graph, schedule, node_id, block->loop_depth)) {
                continue;
            }

            result.guards_invariant++;

            /* Safety check: verify the guard's FrameState is valid at the
             * preheader position. If the FrameState references nodes that
             * are defined inside the loop, hoisting would produce an invalid
             * deopt state. In that case, skip this guard. */
            if (node->frame_state != VTX_NODEID_INVALID) {
                const vtx_node_t *fs = vtx_node_get_const(
                    &graph->node_table, node->frame_state);
                if (fs != NULL) {
                    /* Check FrameState inputs for loop-internal references */
                    for (uint32_t fi = 0; fi < fs->input_count; fi++) {
                        vtx_nodeid_t fs_input = fs->inputs[fi];
                        if (fs_input == VTX_NODEID_INVALID) continue;
                        uint32_t fs_input_depth = node_loop_depth(schedule, fs_input);
                        if (fs_input_depth >= block->loop_depth) {
                            /* FrameState references a node inside the loop —
                             * hoisting would make the deopt state invalid */
                            result.hoist_failed++;
                            goto next_guard;
                        }
                    }
                }
            }

            /* Hoist the guard: move it from the current block to the preheader.
             *
             * In the SoN IR, guards don't have a fixed position — they float
             * freely based on their inputs. The schedule determines their
             * position. To "move" a guard, we update the schedule:
             *   1. Remove the guard from the current block's node list
             *   2. Add the guard to the preheader block's node list
             *   3. Update the node_block mapping
             *
             * The guard's inputs don't change because they're all defined
             * outside the loop (that's the invariant condition). */

            /* Step 1: Remove from current block */
            bool removed = false;
            for (uint32_t i = 0; i < block->node_count; i++) {
                if (block->nodes[i] == node_id) {
                    /* Shift remaining nodes down */
                    for (uint32_t j = i; j + 1 < block->node_count; j++) {
                        block->nodes[j] = block->nodes[j + 1];
                    }
                    block->node_count--;
                    removed = true;
                    break;
                }
            }

            if (!removed) {
                result.hoist_failed++;
                continue;
            }

            /* Step 2: Add to preheader block */
            vtx_schedule_block_t *preheader = &schedule->blocks[preheader_idx];

            /* Grow preheader's node array if needed.
             * For simplicity, we use arena allocation for the new array. */
            if (preheader->node_count >= preheader->node_capacity) {
                uint32_t new_cap = preheader->node_capacity * 2;
                if (new_cap == 0) new_cap = 8;
                vtx_nodeid_t *new_nodes = (vtx_nodeid_t *)vtx_arena_alloc(
                    arena, new_cap * sizeof(vtx_nodeid_t));
                if (new_nodes == NULL) {
                    result.hoist_failed++;
                    /* Put the node back in the original block */
                    block->nodes[block->node_count++] = node_id;
                    continue;
                }
                /* Copy existing nodes to new array */
                memcpy(new_nodes, preheader->nodes,
                       preheader->node_count * sizeof(vtx_nodeid_t));
                preheader->nodes = new_nodes;
                preheader->node_capacity = new_cap;
            }

            /* Insert guard at the end of the preheader (before the terminator).
             * For a preheader block, the terminator is typically a Goto to
             * the loop header. We want the guard before the Goto. */
            preheader->nodes[preheader->node_count++] = node_id;

            /* Step 3: Update node_block mapping */
            if (node_id < schedule->node_block_count) {
                schedule->node_block[node_id] = preheader_idx;
            }

            /* Step 4: Also move the guard's FrameState if it has one.
             * The FrameState must be in the same block as the guard for
             * deopt to work correctly. */
            if (node->frame_state != VTX_NODEID_INVALID) {
                vtx_nodeid_t fs_id = node->frame_state;
                if (fs_id < schedule->node_block_count) {
                    schedule->node_block[fs_id] = preheader_idx;
                }
            }

            result.guards_hoisted++;
        next_guard:
            continue;
        }
    }

    return result;
}
