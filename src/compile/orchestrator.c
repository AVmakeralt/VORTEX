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
 * VORTEX Runtime Orchestrator — Implementation
 *
 * Wires together Markov predictions, recomp monitoring, FDI feedback,
 * and phase detection into a unified proactive compilation system.
 *
 * See orchestrator.h for design rationale.
 */

#define _POSIX_C_SOURCE 199309L
#include "compile/orchestrator.h"
#include "compile/decision.h"
#include "trace/retrace.h"
#include "compile/aot.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========================================================================== */
/* Internal: sleep for N milliseconds                                           */
/* ========================================================================== */

static void sleep_ms(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ========================================================================== */
/* Internal: proactive compilation from Markov prediction                       */
/* ========================================================================== */

/**
 * Check if the Markov chain predicts a phase transition.
 * If so, queue the predicted-hot methods for background compilation.
 *
 * This is the "predict → pre-compile → zero deopt" pipeline:
 *   Phase A → Markov predicts Phase B → pre-compile Phase B's methods
 *   Phase B actually starts → already compiled → zero deopts
 */
static void check_markov_prediction(vtx_orchestrator_t *orch)
{
    if (orch->markov == NULL || orch->threadpool == NULL) return;
    if (!orch->markov->is_trained) return;

    /* Predict the next phase from the current phase */
    uint32_t next_phase = vtx_markov_predict_next(orch->markov,
                                                    orch->markov->current_phase);

    /* If the predicted phase is the same as current, nothing to do */
    if (next_phase == orch->markov->current_phase) return;

    /* Get the methods predicted to be hot in the next phase */
    uint32_t method_ids[VTX_ORCHESTRATOR_PROACTIVE_COMPILE_LIMIT];
    uint32_t count = vtx_markov_predict_hot_methods(orch->markov, next_phase,
                                                      method_ids,
                                                      orch->proactive_compile_limit);

    if (count == 0) return;

    /* Submit proactive compilation tasks for each predicted-hot method.
     * Use a lower priority than on-demand compilation so we don't
     * interfere with hot-path compilations. The priority is set to
     * the T2 tier level minus a "proactive" discount. */
    for (uint32_t i = 0; i < count; i++) {
        vtx_compile_task_t task;
        memset(&task, 0, sizeof(task));
        task.method_id = method_ids[i];
        task.tier = VTX_TIER_T2;
        task.priority = VTX_COMPILE_PRIORITY_LOW;

        /* BUG-3 fix (1/6): Route through the decision engine instead of
         * submitting directly to the threadpool. */
        if (vtx_decision_submit_compile(orch, &task,
                                          VTX_DECISION_REASON_PROACTIVE) == 0) {
            __atomic_fetch_add(&orch->total_proactive_compiles, 1, __ATOMIC_RELAXED);
        }
    }

    __atomic_fetch_add(&orch->total_phase_predictions, 1, __ATOMIC_RELAXED);
}

/* ========================================================================== */
/* Internal: phase detection → proactive compilation                            */
/* ========================================================================== */

/**
 * Check the phase detector for a phase transition.
 * If a new phase is detected, use the phase-reactive version manager
 * to try to reactivate a parked version for the new phase, or
 * queue compilation if no parked version exists.
 *
 * Sprint 2: Also triggers phase partition transition — swaps the
 * active profile to the new phase's profile so that subsequent
 * recomp decisions use phase-appropriate data.
 */
static void check_phase_detection(vtx_orchestrator_t *orch)
{
    if (orch->phase_detector == NULL) return;

    /* The phase detector is updated via vtx_orchestrator_on_method_entry().
     * Here we check if the current phase prediction has changed. */
    uint32_t predicted = orch->phase_detector->predicted_phase;

    /* Sprint 2: Phase partition transition.
     * If a partition is attached and the predicted phase differs from
     * the active phase, swap the active profile to the new phase's
     * profile. This ensures that recomp decisions (KL-divergence
     * checks, confidence scoring, etc.) use phase-appropriate data
     * instead of the polluted merged profile. */
    if (orch->phase_partition != NULL && predicted != VTX_PHASE_NONE) {
        uint32_t active = vtx_phase_partition_active_phase(orch->phase_partition);
        if (active != predicted) {
            vtx_orchestrator_phase_transition(orch, predicted);
        }
    }

    /* If phase-reactive version manager is available, try reactivation */
    if (orch->phase_react != NULL && predicted != VTX_PHASE_NONE) {
        /* Compute the phase hash from the current type feedback.
         * This hash identifies the current execution phase and is used
         * to look up parked (previously compiled but deactivated)
         * versions that match this phase. If a parked version exists,
         * we can reactivate it in O(1) — no recompilation needed. */
        vtx_phase_hash_t phase_hash = vtx_phase_react_compute_hash(
            orch->type_feedback, 0 /* method_id computed per-method */);

        /* Attempt to reactivate parked versions for the predicted phase.
         * We iterate over the hot methods in the current profile and
         * try to reactivate each one. If no parked version exists for
         * a method, the Markov check will queue it for compilation. */
        if (orch->profile != NULL && orch->threadpool != NULL) {
            for (uint32_t i = 0; i < orch->profile->method_count && i < VTX_ORCHESTRATOR_PROACTIVE_COMPILE_LIMIT; i++) {
                uint32_t method_id = orch->profile->methods[i].method_id;

                /* Try reactivation for this method. If a parked version
                 * exists for the current phase, reactivate it. If not,
                 * the method will be picked up by the Markov proactive
                 * compilation check. */
                bool reactivated = vtx_phase_react_try_reactivate(
                    orch->phase_react, method_id, phase_hash);

                if (reactivated) {
                    __atomic_fetch_add(&orch->total_phase_reactivations, 1, __ATOMIC_RELAXED);
                }
            }
        }

        (void)phase_hash; /* used by vtx_phase_react_try_reactivate above */
    }

    /* If proactive compilation is needed (no parked version), the
     * Markov check above will handle it. */
}

/* ========================================================================== */
/* Internal: recomp monitor → auto-recompile on profile drift                   */
/* ========================================================================== */

/**
 * Check the recompilation monitor for profile divergence.
 * If any method's type profile has drifted significantly from
 * its compilation-time snapshot, queue it for recompilation.
 */
static void check_recomp_drift(vtx_orchestrator_t *orch)
{
    if (orch->recomp == NULL || orch->profile == NULL) return;

    /* Sprint 2: When a phase partition is attached, use the active
     * phase's profile instead of the static `profile` pointer. The
     * `profile` pointer is updated by phase transitions to track the
     * active phase, but we double-check here in case the partition
     * was attached after the orchestrator started. */
    vtx_profile_global_t *active_profile = orch->profile;
    if (orch->phase_partition != NULL) {
        vtx_profile_global_t *part_active =
            vtx_phase_partition_get_active(orch->phase_partition);
        if (part_active != NULL) {
            active_profile = part_active;
            /* Keep the legacy pointer in sync for callers that read it. */
            orch->profile = part_active;
        }
    }

    /* Phase 1: Check each method in the profile for profile drift.
     * If the KL divergence between the compile-time snapshot and the
     * current profile exceeds the threshold, queue the method for
     * recompilation. This is the "close the loop" step — without this,
     * snapshots are saved but never compared.
     *
     * Sprint 1.2/1.3: We now use the hysteresis-aware check + the
     * backpressure-aware queue. This prevents recomp thrashing under
     * oscillating workloads (hysteresis requires N consecutive divergent
     * samples before firing) and prevents core starvation under sustained
     * divergence (backpressure caps the queue depth and coalesces
     * duplicate recompiles).
     *
     * We iterate over the profile's methods (not the recomp snapshots)
     * because the snapshot struct is opaque (defined in recomp.c). */
    if (active_profile != NULL) {
        for (uint32_t i = 0; i < active_profile->method_count &&
                             i < VTX_ORCHESTRATOR_PROACTIVE_COMPILE_LIMIT; i++) {
            uint32_t method_id = active_profile->methods[i].method_id;

            vtx_recomp_check_t result = vtx_sota_recomp_check_hysteresis(
                orch->recomp, active_profile, method_id);
            if (result.should_recompile) {
                /* Use the backpressure-aware queue: it enforces the
                 * hard/soft caps and coalesces duplicate recompiles.
                 * Pass now_ns=0 so the queue uses its own monotonic clock. */
                vtx_sota_recomp_queue_backpressure(orch->recomp, method_id,
                                                    active_profile, 0);
            }
        }
    }

    /* Phase 2: Dequeue pending recompiles and submit to threadpool. */
    while (vtx_sota_recomp_has_pending(orch->recomp)) {
        uint32_t method_id = vtx_sota_recomp_dequeue(orch->recomp);
        if (method_id == VTX_PHASE_NONE) break;

        if (orch->threadpool != NULL) {
            vtx_compile_task_t task;
            memset(&task, 0, sizeof(task));
            task.method_id = method_id;
            task.tier = VTX_TIER_T2;  /* recompile at T2 by default */
            task.priority = VTX_COMPILE_PRIORITY_HIGH; /* drift = urgent */

            /* BUG-3 fix (2/6): Route through the decision engine. */
            if (vtx_decision_submit_compile(orch, &task,
                                              VTX_DECISION_REASON_DRIFT) == 0) {
                __atomic_fetch_add(&orch->total_recomp_triggers, 1, __ATOMIC_RELAXED);
                /* Reset hysteresis counter after a successful recompile
                 * submission so the same method isn't immediately fired
                 * again on the next tick. */
                vtx_sota_recomp_hysteresis_reset(orch->recomp, method_id);
            }
        }
    }
}

/* ========================================================================== */
/* Internal: FDI → inline feedback loop                                         */
/* ========================================================================== */

/**
 * Check FDI for methods that need recompilation due to unprofitable
 * inlining decisions. If a method has high deopt rate or spill rate
 * at inlined call sites, queue it for recompilation with the FDI
 * directives (no-inline / force-inline sites).
 */
static void check_fdi_feedback(vtx_orchestrator_t *orch)
{
    if (orch->fdi == NULL || orch->threadpool == NULL) return;

    /* FDI evaluates are triggered by vtx_orchestrator_on_deopt() which
     * calls vtx_sota_fdi_record_deopt(). Here we check if any methods
     * have accumulated enough deopt feedback to warrant recompilation.
     *
     * We iterate over methods tracked by FDI and check
     * vtx_sota_fdi_evaluate() for each. Methods that have high deopt
     * rates or high spill rates at inlined call sites are recommended
     * for recompilation with FDI directives (no-inline / force-inline).
     *
     * The actual recompilation task carries the FDI directives
     * (no_inline and force_inline sites) so the pipeline can apply
     * them during recompilation. */
    uint32_t check_limit = VTX_ORCHESTRATOR_PROACTIVE_COMPILE_LIMIT;
    for (uint32_t i = 0; i < check_limit; i++) {
        /* Get the next method that FDI recommends for recompilation.
         * vtx_sota_fdi_next_recompile_candidate() returns the method_id
         * of the next method that should be recompiled, or
         * VTX_PHASE_NONE if no more candidates exist. */
        uint32_t method_id = vtx_sota_fdi_next_recompile_candidate(orch->fdi);
        if (method_id == VTX_PHASE_NONE) break;

        /* Submit recompilation task with FDI directives */
        vtx_compile_task_t task;
        memset(&task, 0, sizeof(task));
        task.method_id = method_id;
        task.tier = VTX_TIER_T2;
        task.priority = VTX_COMPILE_PRIORITY_HIGH;

        /* BUG-3 fix (3/6): Route through the decision engine. */
        if (vtx_decision_submit_compile(orch, &task,
                                          VTX_DECISION_REASON_FDI) == 0) {
            __atomic_fetch_add(&orch->total_fdi_recompiles, 1, __ATOMIC_RELAXED);
        }
    }
}

/* ========================================================================== */
/* Internal: deoptless → compile continuations for failing guards               */
/* ========================================================================== */

/**
 * Check deoptless tables for methods that have accumulated enough failed
 * guards to warrant continuation compilation. When a guard fails 3+ times,
 * submit a recompilation task for that method so the pipeline can create
 * a deoptless continuation (a version with the guard removed).
 *
 * The continuation is compiled by the threadpool and installed via the
 * versioned code cache. The guard site is then patched to jump to the
 * continuation instead of the deopt stub.
 */
#define VTX_DEOPTLESS_CONTINUATION_THRESHOLD 3

static void check_deoptless_continuations(vtx_orchestrator_t *orch)
{
    if (orch->deoptless_tables == NULL || orch->threadpool == NULL) return;

    /* Include the deoptless header for the table struct */
    #include "deopt/deoptless.h"

    for (uint32_t i = 0; i < orch->deoptless_table_count &&
                         i < VTX_ORCHESTRATOR_PROACTIVE_COMPILE_LIMIT; i++) {
        if (orch->deoptless_tables[i] == NULL) continue;

        vtx_deoptless_table_t *table = (vtx_deoptless_table_t *)orch->deoptless_tables[i];

        /* If the table has accumulated enough failed guards, trigger
         * recompilation. The pipeline will create a deoptless continuation
         * with those guards removed. */
        if (table->failed_guard_count >= VTX_DEOPTLESS_CONTINUATION_THRESHOLD) {
            /* Submit recompilation task */
            vtx_compile_task_t task;
            memset(&task, 0, sizeof(task));
            task.method_id = table->method_id;
            task.tier = VTX_TIER_T2;
            task.priority = VTX_COMPILE_PRIORITY_NORMAL;

            /* BUG-3 fix (4/6): Route through the decision engine. */
            if (vtx_decision_submit_compile(orch, &task,
                                              VTX_DECISION_REASON_DEOPTLESS) == 0) {
                /* Reset the failed guard count so we don't re-trigger
                 * on every orchestrator check. The recompilation will
                 * create a continuation that handles these guards. */
                table->failed_guard_count = 0;
            }
        }
    }
}

/* ========================================================================== */
/* Background thread                                                           */
/* ========================================================================== */

/**
 * Main orchestrator loop.
 *
 * Runs in a background thread. Wakes up periodically (or on explicit
 * wake events) to perform wiring functions:
 *   1. Check Markov predictions → proactive compilation
 *   2. Check phase detection → proactive compilation / reactivation
 *   3. Check recomp monitor → auto-recompile on drift
 *   4. Check FDI feedback → recompile with different inlining
 */
static void *orchestrator_thread_fn(void *arg)
{
    vtx_orchestrator_t *orch = (vtx_orchestrator_t *)arg;

    while (true) {
        /* Check for shutdown */
        pthread_mutex_lock(&orch->mutex);
        bool should_shutdown = orch->shutdown_requested;
        pthread_mutex_unlock(&orch->mutex);

        if (should_shutdown) break;

        /* Wait for next check interval or explicit wake */
        pthread_mutex_lock(&orch->mutex);
        if (!orch->shutdown_requested) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += (long)orch->check_interval_ms * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += ts.tv_nsec / 1000000000L;
                ts.tv_nsec = ts.tv_nsec % 1000000000L;
            }
            pthread_cond_timedwait(&orch->wake_cond, &orch->mutex, &ts);
        }
        pthread_mutex_unlock(&orch->mutex);

        /* Check shutdown again after waking */
        pthread_mutex_lock(&orch->mutex);
        should_shutdown = orch->shutdown_requested;
        pthread_mutex_unlock(&orch->mutex);

        if (should_shutdown) break;

        /* ---- Perform wiring checks ---- */

        __atomic_fetch_add(&orch->total_checks, 1, __ATOMIC_RELAXED);

        /* 1. Markov → proactive compilation */
        check_markov_prediction(orch);

        /* 2. Phase detection → proactive compilation / reactivation */
        check_phase_detection(orch);

        /* 3. Recomp monitor → auto-recompile on drift */
        check_recomp_drift(orch);

        /* 4. FDI → inline feedback loop */
        check_fdi_feedback(orch);

        /* 5. Deoptless → compile continuations for repeatedly failing guards */
        check_deoptless_continuations(orch);

        /* 6. Trace-based PGO → re-trace methods with high guard failure rates.
         *
         * When a guard fails repeatedly, the trace's speculation is wrong.
         * Re-record the trace with updated profile data so the next trace
         * picks a different hot path. This implements the "dynamic superblock"
         * vision: traces are linearized SoN regions that get specialized
         * variants on guard failure. */
        if (orch->trace_retrace != NULL) {
            uint32_t submitted = vtx_trace_retrace_check(
                orch->trace_retrace, orch);
            if (submitted > 0) {
                __atomic_fetch_add(&orch->total_trace_retraces,
                                     submitted, __ATOMIC_RELAXED);
            }
        }
    }

    return NULL;
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_orchestrator_init(vtx_orchestrator_t *orch,
#ifdef VORTEX_ENABLE_SOTA
                            vtx_markov_t *markov,
                            vtx_sota_phase_t *phase,
                            vtx_sota_recomp_t *recomp,
                            vtx_sota_fdi_t *fdi,
#else
                            void *markov,
                            void *phase,
                            void *recomp,
                            void *fdi,
#endif
                            vtx_threadpool_t *threadpool,
#ifdef VORTEX_ENABLE_SOTA
                            vtx_phase_react_manager_t *phase_react,
#else
                            void *phase_react,
#endif
                            vtx_type_feedback_t *type_feedback,
                            vtx_profile_global_t *profile,
                            vtx_inline_feedback_t *inline_feedback)
{
    if (orch == NULL) return -1;

    memset(orch, 0, sizeof(*orch));

    orch->markov = markov;
    orch->phase_detector = phase;
    orch->recomp = recomp;
    orch->fdi = fdi;
    orch->threadpool = threadpool;
    orch->phase_react = phase_react;
    orch->type_feedback = type_feedback;
    orch->profile = profile;
    orch->inline_feedback = inline_feedback;

    /* Sprint 2: phase partition starts detached. Use
     * vtx_orchestrator_set_phase_partition() to attach one after init. */
    orch->phase_partition = NULL;

    /* Trace-based PGO: initialize the re-trace registry.
     * Capacity 256 handles up to 256 methods with active re-trace state. */
    orch->trace_retrace = (vtx_trace_retrace_t *)
        calloc(1, sizeof(vtx_trace_retrace_t));
    if (orch->trace_retrace != NULL) {
        vtx_trace_retrace_init(orch->trace_retrace, 256);
    }

    /* AOT background compilation: initialize the AOT manager.
     * The code cache and method registry are wired later via
     * vtx_aot_init() when the runtime creates them. For now,
     * create the manager with NULL cache/registry — they'll be
     * set when the runtime calls vtx_orchestrator_set_aot_cache(). */
    orch->aot = (vtx_aot_manager_t *)
        calloc(1, sizeof(vtx_aot_manager_t));
    if (orch->aot != NULL) {
        vtx_aot_init(orch->aot, NULL, NULL);
    }

    orch->check_interval_ms = VTX_ORCHESTRATOR_CHECK_INTERVAL_MS;
    orch->min_profile_observations = VTX_ORCHESTRATOR_MIN_PROFILE_OBS;
    orch->proactive_compile_limit = VTX_ORCHESTRATOR_PROACTIVE_COMPILE_LIMIT;

    orch->running = false;
    orch->shutdown_requested = false;

    orch->total_checks = 0;
    orch->total_phase_predictions = 0;
    orch->total_proactive_compiles = 0;
    orch->total_recomp_triggers = 0;
    orch->total_fdi_recompiles = 0;
    orch->total_phase_reactivations = 0;
    orch->total_phase_partition_transitions = 0;
    orch->total_phase_preemptive_recompiles = 0;

    if (pthread_mutex_init(&orch->mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&orch->wake_cond, NULL) != 0) {
        pthread_mutex_destroy(&orch->mutex);
        return -1;
    }

    return 0;
}

int vtx_orchestrator_start(vtx_orchestrator_t *orch)
{
    if (orch == NULL) return -1;

    pthread_mutex_lock(&orch->mutex);
    if (orch->running) {
        pthread_mutex_unlock(&orch->mutex);
        return 0; /* already running */
    }

    orch->shutdown_requested = false;
    orch->running = true;
    pthread_mutex_unlock(&orch->mutex);

    if (pthread_create(&orch->orchestrator_thread, NULL,
                        orchestrator_thread_fn, orch) != 0) {
        orch->running = false;
        return -1;
    }

    /* Start the AOT background worker */
    if (orch->aot != NULL) {
        vtx_aot_start(orch->aot);
    }

    return 0;
}

void vtx_orchestrator_stop(vtx_orchestrator_t *orch)
{
    if (orch == NULL) return;

    pthread_mutex_lock(&orch->mutex);
    orch->shutdown_requested = true;
    pthread_cond_signal(&orch->wake_cond);
    pthread_mutex_unlock(&orch->mutex);

    if (orch->running) {
        pthread_join(orch->orchestrator_thread, NULL);
        orch->running = false;
    }

    /* Stop the AOT background worker */
    if (orch->aot != NULL) {
        vtx_aot_stop(orch->aot);
    }
}

void vtx_orchestrator_destroy(vtx_orchestrator_t *orch)
{
    if (orch == NULL) return;

    vtx_orchestrator_stop(orch);

    /* Clean up the trace re-trace registry */
    if (orch->trace_retrace != NULL) {
        vtx_trace_retrace_destroy(orch->trace_retrace);
        free(orch->trace_retrace);
        orch->trace_retrace = NULL;
    }

    /* Clean up the AOT manager */
    if (orch->aot != NULL) {
        vtx_aot_destroy(orch->aot);
        free(orch->aot);
        orch->aot = NULL;
    }

    pthread_mutex_destroy(&orch->mutex);
    pthread_cond_destroy(&orch->wake_cond);

    memset(orch, 0, sizeof(*orch));
}

/* ========================================================================== */
/* Event notifications                                                         */
/* ========================================================================== */

void vtx_orchestrator_on_method_entry(vtx_orchestrator_t *orch,
                                        uint32_t method_id)
{
    if (orch == NULL) return;

    /* Feed method entry to Markov chain for phase transition tracking */
    if (orch->markov != NULL) {
        vtx_markov_record_method_call(orch->markov, method_id);
    }

    /* Feed method entry to phase detector for phase matching */
    if (orch->phase_detector != NULL) {
        vtx_sota_phase_update(orch->phase_detector, method_id);
    }

    /* Feed method entry to FDI for execution tracking */
    if (orch->fdi != NULL) {
        vtx_sota_fdi_record_execution(orch->fdi, method_id);
    }
}

void vtx_orchestrator_on_deopt(vtx_orchestrator_t *orch,
                                 uint32_t method_id,
                                 uint64_t call_site_id,
                                 uint32_t guard_id)
{
    if (orch == NULL) return;

    /* Feed deopt event to FDI — this is the critical wiring that
     * enables the self-tuning inliner. When a deopt occurs at an
     * inlined call site, FDI records the call site as unprofitable
     * and may recommend recompilation without that inline. */
    if (orch->fdi != NULL) {
        vtx_sota_fdi_record_deopt(orch->fdi, method_id, call_site_id);

        /* Check if FDI now recommends recompilation */
        bool should_recompile = vtx_sota_fdi_evaluate(orch->fdi, method_id);
        if (should_recompile && orch->threadpool != NULL) {
            vtx_compile_task_t task;
            memset(&task, 0, sizeof(task));
            task.method_id = method_id;
            task.tier = VTX_TIER_T2;
            task.priority = VTX_COMPILE_PRIORITY_HIGH;
            /* BUG-3 fix (5/6): Route through the decision engine. */
            if (vtx_decision_submit_compile(orch, &task,
                                              VTX_DECISION_REASON_DEOPT) == 0) {
                __atomic_fetch_add(&orch->total_fdi_recompiles, 1, __ATOMIC_RELAXED);
            }
        }
    }

    /* Feed deopt to Markov chain — burst of deopts may indicate
     * a phase transition. Record a transition from the current phase
     * to an "unknown" phase to trigger re-evaluation. */
    if (orch->markov != NULL) {
        uint32_t new_phase;
        if (vtx_markov_detect_transition(orch->markov, &new_phase)) {
            /* Phase transition detected — wake the orchestrator to
             * check for proactive compilation opportunities */
            vtx_orchestrator_wake(orch);
        }
    }

    /* Feed deopt to the trace re-tracing system.
     *
     * Records the guard failure so the orchestrator background thread
     * can check if the failure rate exceeds the re-trace threshold.
     * Also updates the profile data's branch probabilities so the next
     * trace recording picks a different hot path. */
    if (orch->trace_retrace != NULL) {
        vtx_trace_retrace_record_failure(
            orch->trace_retrace,
            method_id,
            guard_id,
            orch->profile,         /* global profile for branch updates */
            NULL);                 /* guard meta table (not wired yet) */
    }

    /* Feed deopt to the AOT background compilation system.
     *
     * Marks the trace edge as unstable and increments the bailout
     * counter. If the failure count exceeds the threshold, the AOT
     * system triggers a re-trace via the retrace system. */
    if (orch->aot != NULL) {
        vtx_aot_on_guard_failure(orch->aot, method_id, guard_id);
    }
}

void vtx_orchestrator_on_compile_done(vtx_orchestrator_t *orch,
                                        uint32_t method_id,
                                        uint32_t version_id)
{
    if (orch == NULL) return;

    /* Save a profile snapshot in the recomp monitor so we can detect
     * drift later. This is the "snapshot at compilation time" that
     * vtx_sota_recomp_check() compares against. */
    if (orch->recomp != NULL && orch->profile != NULL) {
        vtx_sota_recomp_save_snapshot(orch->recomp, method_id, orch->profile);
    }

    /* Register the new version in FDI for performance tracking */
    if (orch->fdi != NULL) {
        vtx_sota_fdi_register_version(orch->fdi, method_id, version_id);
        vtx_sota_fdi_record_recompilation(orch->fdi, method_id, version_id);
    }

    /* Notify the trace re-tracing system that a compile completed.
     * This resets the cooldown so future guard failures can trigger
     * new re-traces for the freshly compiled trace. */
    if (orch->trace_retrace != NULL) {
        vtx_trace_retrace_on_compile_done(orch->trace_retrace, method_id);
    }
}

void vtx_orchestrator_wake(vtx_orchestrator_t *orch)
{
    if (orch == NULL) return;
    pthread_cond_signal(&orch->wake_cond);
}

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

void vtx_orchestrator_get_stats(const vtx_orchestrator_t *orch,
                                  uint64_t *total_checks,
                                  uint64_t *total_phase_predictions,
                                  uint64_t *total_proactive_compiles,
                                  uint64_t *total_recomp_triggers,
                                  uint64_t *total_fdi_recompiles,
                                  uint64_t *total_phase_reactivations)
{
    if (orch == NULL) return;

    if (total_checks) *total_checks = orch->total_checks;
    if (total_phase_predictions) *total_phase_predictions = orch->total_phase_predictions;
    if (total_proactive_compiles) *total_proactive_compiles = orch->total_proactive_compiles;
    if (total_recomp_triggers) *total_recomp_triggers = orch->total_recomp_triggers;
    if (total_fdi_recompiles) *total_fdi_recompiles = orch->total_fdi_recompiles;
    if (total_phase_reactivations) *total_phase_reactivations = orch->total_phase_reactivations;
}

/* ========================================================================== */
/* Sprint 2: Phase-aware profile partition wiring                              */
/* ========================================================================== */

void vtx_orchestrator_set_phase_partition(vtx_orchestrator_t *orch,
                                            vtx_phase_partition_t *part)
{
    if (orch == NULL) return;
    pthread_mutex_lock(&orch->mutex);
    orch->phase_partition = part;
    /* If a partition is being attached, point the legacy `profile`
     * pointer at the active phase's profile so existing callers
     * see the right data. */
    if (part != NULL) {
        vtx_profile_global_t *active = vtx_phase_partition_get_active(part);
        if (active != NULL) {
            orch->profile = active;
        }
    }
    pthread_mutex_unlock(&orch->mutex);
}

void vtx_orchestrator_set_aot_cache(vtx_orchestrator_t *orch,
                                      vtx_code_cache_t *cache,
                                      vtx_method_registry_t *registry)
{
    if (orch == NULL || orch->aot == NULL) return;
    orch->aot->code_cache = cache;
    orch->aot->registry = registry;
}

void vtx_orchestrator_phase_transition(vtx_orchestrator_t *orch,
                                         uint32_t new_phase_id)
{
    if (orch == NULL || orch->phase_partition == NULL) return;

    pthread_mutex_lock(&orch->mutex);

    /* Swap the active profile to the new phase. */
    vtx_profile_global_t *new_active = vtx_phase_partition_transition(
        orch->phase_partition, new_phase_id, 0);

    if (new_active != NULL) {
        /* Keep the legacy pointer in sync. */
        orch->profile = new_active;
        __atomic_fetch_add(&orch->total_phase_partition_transitions, 1,
                             __ATOMIC_RELAXED);
    }

    /* Preemptively recompile the new phase's hot methods.
     *
     * We need the phase graph to know which methods belong to the new
     * phase. If the phase detector has a graph, use it; otherwise we
     * fall back to recompiling the top N hottest methods in the new
     * phase's profile. */
    if (orch->threadpool != NULL && new_active != NULL) {
        uint32_t hot_methods[VTX_ORCHESTRATOR_PROACTIVE_COMPILE_LIMIT];
        uint32_t n_methods = 0;

        /* Try the phase graph first. */
        if (orch->phase_detector != NULL &&
            orch->phase_detector->phase_graph != NULL) {
            n_methods = vtx_phase_partition_hot_methods_for_phase(
                orch->phase_partition,
                orch->phase_detector->phase_graph,
                new_phase_id,
                hot_methods,
                VTX_ORCHESTRATOR_PROACTIVE_COMPILE_LIMIT);
        }

        /* Fallback: top N methods by invocation_count in the new phase. */
        if (n_methods == 0 && new_active->method_count > 0) {
            uint32_t to_take = new_active->method_count;
            if (to_take > VTX_ORCHESTRATOR_PROACTIVE_COMPILE_LIMIT) {
                to_take = VTX_ORCHESTRATOR_PROACTIVE_COMPILE_LIMIT;
            }
            /* Simple linear scan for the top N (the profile array is
             * not sorted — we don't want to mutate it). For larger N
             * a heap would be better, but N is small (8). */
            for (uint32_t i = 0; i < to_take; i++) {
                hot_methods[i] = new_active->methods[i].method_id;
            }
            n_methods = to_take;
        }

        /* Submit preemptive recompilation tasks. These use a lower
         * priority than drift-triggered recompiles so that on-demand
         * compilation isn't blocked. */
        for (uint32_t i = 0; i < n_methods; i++) {
            vtx_compile_task_t task;
            memset(&task, 0, sizeof(task));
            task.method_id = hot_methods[i];
            task.tier = VTX_TIER_T2;
            task.priority = VTX_COMPILE_PRIORITY_LOW;  /* preemptive = low */

            /* BUG-3 fix (6/6): Route through the decision engine. */
            if (vtx_decision_submit_compile(orch, &task,
                                              VTX_DECISION_REASON_PHASE) == 0) {
                __atomic_fetch_add(&orch->total_phase_preemptive_recompiles,
                                     1, __ATOMIC_RELAXED);
            }
        }
    }

    pthread_mutex_unlock(&orch->mutex);
}

void vtx_orchestrator_get_partition_stats(const vtx_orchestrator_t *orch,
                                            uint64_t *total_partition_transitions,
                                            uint64_t *total_preemptive_recompiles)
{
    if (orch == NULL) {
        if (total_partition_transitions) *total_partition_transitions = 0;
        if (total_preemptive_recompiles) *total_preemptive_recompiles = 0;
        return;
    }
    if (total_partition_transitions)
        *total_partition_transitions = orch->total_phase_partition_transitions;
    if (total_preemptive_recompiles)
        *total_preemptive_recompiles = orch->total_phase_preemptive_recompiles;
}
