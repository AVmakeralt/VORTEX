/* ============================================================================ *
 * AI-GENERATED CODE NOTICE
 *
 * This file was written or substantially modified by an AI assistant
 * (GLM/Z.ai). It is part of the VORTEX JIT compiler project.
 *
 * Human-written original code exists in the interpreter dispatch loop
 * (src/interp/), baseline codegen (src/baseline/codegen.c), runtime
 * (src/runtime/), and the main entry point (src/main_new.c).
 *
 * AI-generated components include: IR construction, optimization passes
 * (GVN, SCCP, DCE, LICM, strength reduction, SMI tag elision, PEA),
 * instruction selection, register allocation, code emission, the
 * compilation pipeline, the decision engine, PGO subsystems (phase
 * partitioning, ensemble aggregation, input-shape-keyed profiles, patch
 * logging, T1 code persistence), deopt/OSR, trace recording, guard
 * optimization, and the inliner.
 *
 * If you are reviewing this code, please verify correctness independently.
 * ============================================================================ */

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
