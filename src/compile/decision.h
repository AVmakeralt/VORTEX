/* ============================================================================ *
 * AI-GENERATED CODE
 *
 * This file was written from scratch by an AI assistant (GLM/Z.ai).
 * It is part of the VORTEX JIT compiler project.
 *
 * Human-written code lives in: src/interp/ (dispatch loop), src/baseline/
 * (codegen), src/runtime/ (GC, type system, arena), src/main_new.c.
 *
 * If reviewing, please verify correctness independently.
 * ============================================================================ */

#ifndef VORTEX_COMPILE_DECISION_H
#define VORTEX_COMPILE_DECISION_H

/*
 * VORTEX Decision Engine — Central Compile-Decision Routing
 *
 * The decision engine sits between the orchestrator's wiring logic
 * (Markov predictions, FDI feedback, deoptless continuations, phase
 * transitions, drift detection, deopt handlers) and the compilation
 * threadpool. Every compile-task submission and every deopt event
 * flows through this module.
 *
 * Why centralize? Previously each of the orchestrator's wiring
 * callbacks called `vtx_threadpool_submit_task` directly. That meant:
 *   - Deopt-driven priority boosting had nowhere to live — the deopt
 *     handler called `vtx_orchestrator_on_deopt` (which feeds FDI /
 *     Markov) but never told the engine "a deopt just happened, please
 *     factor that into the next submit decision".
 *   - Deopt storms couldn't be suppressed at the submission layer.
 *   - There was no single chokepoint to add cross-cutting policies
 *     (fairness, priority caps, deopt-budget enforcement, etc.).
 *
 * This module is intentionally thin right now: the submit API wraps
 * `vtx_threadpool_submit_task` and the record API increments counters.
 * The point is to establish the routing seam so future decision logic
 * can be added without touching the 6+ call sites in the orchestrator
 * and the deopt handler.
 *
 * Thread safety: relies on the caller's existing synchronization
 * (the orchestrator mutex for orchestrator-owned counters; the
 * threadpool's internal lock for task submission).
 */

#include <stdint.h>
#include <stdbool.h>

#include "vortex_config.h"
#include "compile/threadpool.h"
#include "compile/orchestrator.h"

/* ========================================================================== */
/* Decision reasons                                                            */
/* ========================================================================== */

typedef enum {
    VTX_DECISION_REASON_PROACTIVE = 0,  /* Markov-predicted hot method */
    VTX_DECISION_REASON_DRIFT,          /* Profile drift recompile */
    VTX_DECISION_REASON_FDI,            /* FDI-recommended recompile */
    VTX_DECISION_REASON_DEOPTLESS,      /* Deoptless continuation compile */
    VTX_DECISION_REASON_DEOPT,          /* Deopt-triggered recompile */
    VTX_DECISION_REASON_PHASE,          /* Phase transition recompile */
    VTX_DECISION_REASON_RETRACE,        /* Trace-based PGO re-tracing */
    VTX_DECISION_REASON_COUNT
} vtx_decision_reason_t;

/* ========================================================================== */
/* Deopt recording                                                             */
/* ========================================================================== */

/*
 * Record a deopt event in the decision engine.
 *
 * Called by the deopt handler in addition to (not instead of)
 * `vtx_orchestrator_on_deopt`. The orchestrator call feeds FDI /
 * Markov; this call feeds the decision engine's own counters and
 * deopt-recently history, which the submit path can consult to
 * boost priority or suppress recompiles for poisoned methods.
 *
 * Safe to call with a NULL orchestrator (no-op).
 */
void vtx_decision_record_deopt(vtx_orchestrator_t *orch,
                                 uint32_t method_id,
                                 uint64_t call_site_id,
                                 uint32_t guard_id);

/* ========================================================================== */
/* Compile-task submission                                                     */
/* ========================================================================== */

/*
 * Submit a compile task through the decision engine.
 *
 * Currently a thin wrapper around `vtx_threadpool_submit_task` that
 * also increments the per-reason submit counter. Future versions can
 * apply priority overrides, deopt-budget suppression, fairness caps,
 * etc. before the task reaches the threadpool.
 *
 * Returns 0 on successful submission to the threadpool, non-zero
 * otherwise (including when the engine decides to suppress the task).
 *
 * Safe to call with a NULL orchestrator or NULL task (returns -1).
 */
int vtx_decision_submit_compile(vtx_orchestrator_t *orch,
                                  vtx_compile_task_t *task,
                                  vtx_decision_reason_t reason);

/* ========================================================================== */
/* Introspection                                                               */
/* ========================================================================== */

/*
 * Read the per-reason submit counters.
 * `counts_out` must point to an array of at least VTX_DECISION_REASON_COUNT
 * uint64_t slots. Safe to call with NULL orch (zeroes the array).
 */
void vtx_decision_submit_counts(const vtx_orchestrator_t *orch,
                                  uint64_t *counts_out);

/*
 * Read the total deopt events recorded by the engine.
 */
uint64_t vtx_decision_deopt_count(const vtx_orchestrator_t *orch);

#endif /* VORTEX_COMPILE_DECISION_H */
