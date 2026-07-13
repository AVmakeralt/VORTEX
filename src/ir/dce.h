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

#ifndef VORTEX_DCE_H
#define VORTEX_DCE_H

#include <stdint.h>
#include <stdbool.h>
#include "ir/graph.h"

/**
 * VORTEX Dead Code Elimination (DCE)
 *
 * Removes nodes whose results are never used and that have no side effects.
 * Runs iteratively because removing a dead node may expose more dead nodes
 * (its inputs may lose their last user).
 *
 * A node is kept alive if:
 *   1. It has output_count > 0 (some other node depends on it), OR
 *   2. It has side effects (stores, calls, allocations, guards, etc.), OR
 *   3. It is a control node (structural — defines the control flow graph), OR
 *   4. It is a memory node (defines memory ordering), OR
 *   5. It is pinned (Phi, FrameState — structural position matters), OR
 *   6. It is the Start node or a Parameter projection.
 *
 * A node is dead if:
 *   - output_count == 0 AND none of the above exceptions apply, OR
 *   - It was already marked dead (from a previous pass).
 *
 * When a node is removed:
 *   - Its inputs' output counts are decremented.
 *   - It is marked as dead.
 *   - The iteration continues until no more dead nodes are found.
 */

/**
 * Run DCE on the graph. Modifies the graph in place.
 * Returns the number of nodes removed.
 */
uint32_t vtx_dce_run(vtx_graph_t *graph);

#endif /* VORTEX_DCE_H */
