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

/*
 * VORTEX Decision Engine — Implementation
 *
 * See decision.h for the design rationale. The current implementation is
 * intentionally a thin pass-through: it counts submits and deopts per
 * reason, and forwards compile tasks to the threadpool unchanged. The
 * seam exists so future policy (priority boosting, deopt-budget
 * suppression, fairness) can be added in one place.
 */

#include "compile/decision.h"
#include "compile/orchestrator.h"

#include <string.h>

/* The decision state lives inside the orchestrator struct so it shares
 * the orchestrator's lifetime and (where needed) its mutex. The fields
 * are declared in orchestrator.h as `decision_submit_counts[]` and
 * `decision_deopt_count`. */

void vtx_decision_record_deopt(vtx_orchestrator_t *orch,
                                 uint32_t method_id,
                                 uint64_t call_site_id,
                                 uint32_t guard_id)
{
    (void)method_id;
    (void)call_site_id;
    (void)guard_id;
    if (orch == NULL) return;
    /* BUG-1 fix: This call is the missing feed from the deopt handler
     * into the decision engine. The engine uses this counter to detect
     * deopt storms and adjust submission decisions. The orchestrator's
     * own on_deopt callback (which feeds FDI / Markov) is invoked
     * separately by the runtime stubs. */
    __atomic_fetch_add(&orch->decision_deopt_count, 1, __ATOMIC_RELAXED);
}

int vtx_decision_submit_compile(vtx_orchestrator_t *orch,
                                  vtx_compile_task_t *task,
                                  vtx_decision_reason_t reason)
{
    if (orch == NULL || task == NULL) return -1;
    if (orch->threadpool == NULL) return -1;
    if ((unsigned)reason >= (unsigned)VTX_DECISION_REASON_COUNT) {
        reason = VTX_DECISION_REASON_PROACTIVE;
    }

    /* BUG-3 fix: Route the submission through the decision engine so a
     * single chokepoint can apply cross-cutting policy. For now the
     * engine forwards the task unchanged, but the indirection lets us
     * add deopt-budget suppression, priority overrides, fairness caps,
     * etc. without touching each call site. */
    int rc = vtx_threadpool_submit_task(orch->threadpool, task);

    if (rc == 0) {
        __atomic_fetch_add(&orch->decision_submit_counts[reason],
                             1, __ATOMIC_RELAXED);
    }
    return rc;
}

void vtx_decision_submit_counts(const vtx_orchestrator_t *orch,
                                  uint64_t *counts_out)
{
    if (counts_out == NULL) return;
    for (uint32_t i = 0; i < (uint32_t)VTX_DECISION_REASON_COUNT; i++) {
        counts_out[i] = (orch != NULL)
            ? __atomic_load_n(&orch->decision_submit_counts[i],
                                __ATOMIC_RELAXED)
            : 0;
    }
}

uint64_t vtx_decision_deopt_count(const vtx_orchestrator_t *orch)
{
    if (orch == NULL) return 0;
    return __atomic_load_n(&orch->decision_deopt_count, __ATOMIC_RELAXED);
}
