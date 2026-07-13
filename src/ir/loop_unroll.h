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

#ifndef VORTEX_LOOP_UNROLL_H
#define VORTEX_LOOP_UNROLL_H

#include "ir/graph.h"
#include "ir/schedule.h"

/**
 * Unroll loops by duplicating their body.
 *
 * Conservative implementation: only unrolls small loops (<= 20 body nodes)
 * with a single back-edge. The actual body replication is a future
 * enhancement — currently just marks the loop with the unroll factor.
 *
 * @param graph     The IR graph
 * @param schedule  The schedule (for loop structure)
 * @param arena     Arena for allocations
 * @param factor    Unroll factor (2, 3, or 4)
 * @return          Number of loops unrolled
 */
uint32_t vtx_loop_unroll_run(vtx_graph_t *graph,
                              const vtx_schedule_t *schedule,
                              vtx_arena_t *arena,
                              uint32_t factor);

#endif /* VORTEX_LOOP_UNROLL_H */
