#ifndef VORTEX_GUARD_HOIST_H
#define VORTEX_GUARD_HOIST_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "ir/schedule.h"
#include "runtime/arena.h"

/**
 * VORTEX Adaptive Guards — Guard Hoisting
 *
 * Hoists loop-invariant guards out of loops during SoN optimization.
 * A guard is loop-invariant if all its inputs are defined outside the loop.
 * When such a guard is found, it is moved to the loop's preheader.
 *
 * This optimization reduces the overhead of repeated guard checks inside
 * hot loops. For example:
 *
 *   for (i = 0; i < n; i++) {
 *     guard(obj != null);     // hoisted: obj doesn't change in the loop
 *     guard(obj.type == A);   // hoisted: obj.type doesn't change in the loop
 *     sum += arr[i];          // stays: arr[i] depends on i
 *   }
 *
 * After hoisting:
 *   guard(obj != null);
 *   guard(obj.type == A);
 *   for (i = 0; i < n; i++) {
 *     sum += arr[i];
 *   }
 *
 * Algorithm:
 *   1. For each loop in the schedule (blocks with loop_depth > 0)
 *   2. For each Guard/DeoptGuard node in the loop body
 *   3. Check if all inputs to the guard are defined outside the loop
 *      (all inputs' defining blocks have loop_depth < guard's loop_depth)
 *   4. If so, move the guard to the loop's preheader block
 *   5. Update the schedule accordingly
 *
 * Safety: A guard can only be hoisted if failing it earlier (before the loop)
 * is safe — i.e., the guard would have also failed inside the loop.
 * This is guaranteed when all inputs are loop-invariant, because the
 * guard's condition would evaluate to the same value regardless of position.
 *
 * A guard must NOT be hoisted if:
 *   - It depends on a value computed inside the loop
 *   - It is a bounds check that depends on the loop index
 *   - Its failure would cause a different deopt state than if checked in-loop
 *     (the FrameState must still be valid at the new position)
 */

/* ========================================================================== */
/* Hoisting result                                                             */
/* ========================================================================== */

typedef struct {
    uint32_t guards_hoisted;      /* number of guards successfully hoisted */
    uint32_t guards_checked;      /* total guards checked for hoisting */
    uint32_t guards_invariant;    /* guards that were loop-invariant */
    uint32_t hoist_failed;        /* invariant guards that couldn't be hoisted */
} vtx_hoist_result_t;

/* ========================================================================== */
/* Hoisting API                                                                */
/* ========================================================================== */

/**
 * Hoist loop-invariant guards out of loops.
 *
 * @param graph    The SoN graph (modified in place)
 * @param schedule The schedule (used to determine loop structure)
 * @param arena    Arena for temporary allocations
 * @return         Result structure with hoisting statistics
 */
vtx_hoist_result_t vtx_hoist_guards(vtx_graph_t *graph,
                                      vtx_schedule_t *schedule,
                                      vtx_arena_t *arena);

/**
 * Check if a guard node is loop-invariant with respect to a given loop depth.
 *
 * A guard is loop-invariant if all its inputs are defined in blocks
 * with loop_depth < the guard's loop_depth.
 *
 * @param graph      The SoN graph
 * @param schedule   The schedule
 * @param guard_node The guard node to check
 * @param loop_depth The loop depth to check against
 * @return           true if the guard is loop-invariant
 */
bool vtx_hoist_is_invariant(const vtx_graph_t *graph,
                              const vtx_schedule_t *schedule,
                              vtx_nodeid_t guard_node,
                              uint32_t loop_depth);

/**
 * Find the preheader block for a loop header block.
 *
 * The preheader is the unique predecessor of the loop header that
 * is outside the loop (loop_depth < header's loop_depth).
 * Returns the block index, or (uint32_t)-1 if not found.
 *
 * @param schedule     The schedule
 * @param header_block Block index of the loop header
 * @return             Block index of the preheader, or (uint32_t)-1
 */
uint32_t vtx_hoist_find_preheader(const vtx_schedule_t *schedule,
                                    uint32_t header_block);

#endif /* VORTEX_GUARD_HOIST_H */
