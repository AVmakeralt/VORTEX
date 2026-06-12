#ifndef VORTEX_IR_LICM_H
#define VORTEX_IR_LICM_H

#include "ir/graph.h"
#include "ir/schedule.h"
#include "runtime/arena.h"

/**
 * VORTEX Loop-Invariant Code Motion (LICM)
 *
 * Moves loop-invariant computations out of loop bodies into preheaders.
 *
 * A node is loop-invariant if:
 *   1. It is not pinned (not a Phi, Region, FrameState, etc.)
 *   2. It has no side effects
 *   3. It is not a control or memory node
 *   4. All of its inputs are defined outside the loop OR are themselves
 *      loop-invariant
 *
 * Guard nodes can be hoisted if the guarded condition doesn't change
 * inside the loop.
 *
 * Memory loads (Load/LoadField/LoadIndexed) can be hoisted ONLY if there
 * is no potentially-aliasing store (Store/StoreField/StoreIndexed) in the
 * loop body.
 *
 * With TBAA (Type-Based Alias Analysis), this check is refined:
 * a load of one type (e.g., int[]) can be hoisted past a store of a
 * different type (e.g., ref[]) because they can never alias. This
 * enables 50%+ of loop-invariant load hoisting.
 *
 * Prerequisites: Graph must be scheduled (vtx_schedule_run already called).
 *
 * @param graph    The SoN graph
 * @param schedule The schedule (identifies loop structure)
 * @param arena    Arena for temporary allocations
 * @return         Number of nodes hoisted, or -1 on error
 */
int vtx_licm_run(vtx_graph_t *graph, const vtx_schedule_t *schedule, vtx_arena_t *arena);

#endif /* VORTEX_IR_LICM_H */
