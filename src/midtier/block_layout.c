/* ============================================================================ *
 * AI-GENERATED CODE
 *
 * Profile-guided block layout for VORTEX JIT.
 * See block_layout.h for algorithm description.
 * ============================================================================ */

#include "midtier/block_layout.h"
#include "ir/node.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/* Edge probability lookup                                                    */
/* ========================================================================== */

/**
 * Get the probability of taking the "true" branch at an If node.
 * Returns 0.5 if no profile data is available.
 *
 * The If node's bytecode_pc is used to look up the branch probability
 * from the profiler's per-PC branch profile data.
 */
static double get_branch_probability(const vtx_schedule_t *schedule,
                                      const vtx_graph_t *graph,
                                      const vtx_profiler_t *profiler,
                                      const vtx_method_desc_t *method,
                                      uint32_t block_idx)
{
    if (block_idx >= schedule->count) return 0.5;
    const vtx_schedule_block_t *blk = &schedule->blocks[block_idx];

    /* Find the If node in this block (it's the control-flow terminator) */
    for (uint32_t i = 0; i < blk->node_count; i++) {
        vtx_nodeid_t nid = blk->nodes[i];
        if (nid >= graph->node_table.count) continue;
        const vtx_node_t *node = &graph->node_table.nodes[nid];
        if (node->opcode == VTX_OP_If) {
            /* Found the If node — look up its branch probability */
            if (profiler != NULL && method != NULL) {
                double p = vtx_profiler_get_branch_probability(
                    profiler, method, node->bytecode_pc);
                return p;
            }
            return 0.5;
        }
    }

    /* No If node — this block doesn't branch, so "fallthrough" prob = 1.0 */
    return 1.0;
}

/* ========================================================================== */
/* Block layout main                                                          */
/* ========================================================================== */

uint32_t vtx_block_layout_run(vtx_schedule_t *schedule,
                                const vtx_graph_t *graph,
                                const vtx_profiler_t *profiler,
                                const vtx_method_desc_t *method)
{
    if (schedule == NULL || schedule->count == 0) return 0;
    if (schedule->count <= 1) return 0;  /* nothing to reorder */

    uint32_t n = schedule->count;

    /* Step 1: Build the new block order using a greedy chain layout.
     *
     * Start from block 0 (entry). At each block, pick the hottest
     * successor as the next block in the layout. If the hottest successor
     * is already placed, or there are no successors, start a new chain
     * from the first unplaced block.
     *
     * This is O(n^2) worst case but n is typically small (<50 blocks). */

    /* new_order[i] = old block index that should be at position i */
    uint32_t *new_order = (uint32_t *)malloc(n * sizeof(uint32_t));
    /* placed[old_idx] = true if block is already in new_order */
    bool *placed = (bool *)calloc(n, sizeof(bool));
    /* old_to_new[old_idx] = new position in the layout */
    uint32_t *old_to_new = (uint32_t *)malloc(n * sizeof(uint32_t));

    if (!new_order || !placed || !old_to_new) {
        free(new_order);
        free(placed);
        free(old_to_new);
        return 0;
    }

    uint32_t placed_count = 0;

    /* Start with block 0 (entry) */
    uint32_t current = 0;
    new_order[placed_count++] = current;
    placed[current] = true;

    while (placed_count < n) {
        const vtx_schedule_block_t *blk = &schedule->blocks[current];

        /* Find the hottest unplaced successor.
         *
         * IMPORTANT: resolve_branch_targets in isel.c assumes that for JCC
         * blocks, the "not-taken" (fallthrough) successor is at position b+1
         * in the layout. If we reorder blocks and place the taken successor
         * at b+1, the JCC condition and fallthrough become inconsistent,
         * causing the branch to go the wrong way.
         *
         * Fix: for blocks with 2 successors (conditional branches), place
         * the COLD successor (not-taken / fallthrough) next. The hot
         * successor becomes the branch target. This preserves the
         * resolve_branch_targets convention: succ[0]=taken=branch target,
         * succ[1]=not-taken=fallthrough at b+1.
         *
         * For blocks with 1 successor (unconditional GOTO), place the
         * successor next (fallthrough, no branch needed). */
        uint32_t best_succ = UINT32_MAX;
        double best_prob = -1.0;

        if (blk->succ_count == 1) {
            /* Unconditional — place the single successor next */
            uint32_t succ = blk->succ_blocks[0];
            if (succ < n && !placed[succ]) {
                best_succ = succ;
            }
        } else if (blk->succ_count == 2) {
            /* Conditional branch — place the COLD successor next (fallthrough).
             * The hot successor will be reached via JCC.
             * This preserves the resolve_branch_targets convention. */
            double branch_prob = get_branch_probability(
                schedule, graph, profiler, method, current);

            /* succ[0] = taken (hot), succ[1] = not-taken (cold).
             * Place succ[1] (cold) as fallthrough at b+1. */
            for (uint32_t s = 0; s < blk->succ_count; s++) {
                uint32_t succ = blk->succ_blocks[s];
                if (succ >= n || placed[succ]) continue;

                /* For s=0 (taken/hot): prob = branch_prob
                 * For s=1 (not-taken/cold): prob = 1.0 - branch_prob
                 * We want the COLD successor as fallthrough, so pick the
                 * one with LOWER probability. */
                double succ_prob = (s == 0) ? branch_prob : (1.0 - branch_prob);
                if (best_succ == UINT32_MAX || succ_prob < best_prob) {
                    best_prob = succ_prob;
                    best_succ = succ;
                }
            }
        } else if (blk->succ_count > 2) {
            /* Switch-like — just pick the first unplaced successor */
            for (uint32_t s = 0; s < blk->succ_count; s++) {
                uint32_t succ = blk->succ_blocks[s];
                if (succ < n && !placed[succ]) {
                    best_succ = succ;
                    break;
                }
            }
        }

        if (best_succ == UINT32_MAX) {
            /* No unplaced successor — start a new chain from the first
             * unplaced block. This handles cold paths and disconnected
             * regions. */
            for (uint32_t i = 0; i < n; i++) {
                if (!placed[i]) {
                    best_succ = i;
                    break;
                }
            }
        }

        if (best_succ == UINT32_MAX) break;  /* all placed */

        new_order[placed_count++] = best_succ;
        placed[best_succ] = true;
        current = best_succ;
    }

    /* Step 2: Build the old_to_new mapping */
    for (uint32_t i = 0; i < n; i++) {
        old_to_new[new_order[i]] = i;
    }

    /* Step 3: Check if the order actually changed. If not, skip the
     * expensive copy. */
    bool changed = false;
    for (uint32_t i = 0; i < n; i++) {
        if (new_order[i] != i) {
            changed = true;
            break;
        }
    }

    if (!changed) {
        free(new_order);
        free(placed);
        free(old_to_new);
        return 0;
    }

    /* Step 4: Remap all successor and predecessor indices to new positions.
     * We must do this BEFORE moving the blocks (since we read from the old
     * positions). Use a temporary copy to avoid corruption. */

    /* Allocate a temporary copy of the blocks array */
    vtx_schedule_block_t *old_blocks = (vtx_schedule_block_t *)malloc(
        n * sizeof(vtx_schedule_block_t));
    if (!old_blocks) {
        free(new_order);
        free(placed);
        free(old_to_new);
        return 0;
    }
    memcpy(old_blocks, schedule->blocks, n * sizeof(vtx_schedule_block_t));

    /* Remap successors and predecessors in the old copy */
    for (uint32_t i = 0; i < n; i++) {
        vtx_schedule_block_t *blk = &old_blocks[i];
        for (uint32_t s = 0; s < blk->succ_count; s++) {
            if (blk->succ_blocks[s] < n) {
                blk->succ_blocks[s] = old_to_new[blk->succ_blocks[s]];
            }
        }
        for (uint32_t p = 0; p < blk->pred_count; p++) {
            if (blk->pred_blocks[p] < n) {
                blk->pred_blocks[p] = old_to_new[blk->pred_blocks[p]];
            }
        }
    }

    /* Step 5: Rebuild the schedule->blocks array in new order.
     * This is a shallow copy — the internal arrays (nodes, succ_blocks,
     * pred_blocks, df_blocks) are moved by pointer, not deep-copied. */
    for (uint32_t i = 0; i < n; i++) {
        schedule->blocks[i] = old_blocks[new_order[i]];
        schedule->blocks[i].block_id = i;  /* update block_id to new position */
    }

    /* Step 6: Remap node_block array (node_id → new block index) */
    for (uint32_t node_id = 0; node_id < schedule->node_block_count; node_id++) {
        uint32_t old_blk = schedule->node_block[node_id];
        if (old_blk < n) {
            schedule->node_block[node_id] = old_to_new[old_blk];
        }
    }

    uint32_t reordered = placed_count;

    free(old_blocks);
    free(new_order);
    free(placed);
    free(old_to_new);

    return reordered;
}
