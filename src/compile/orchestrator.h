#ifndef VORTEX_COMPILE_ORCHESTRATOR_H
#define VORTEX_COMPILE_ORCHESTRATOR_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "vortex_config.h"
#include "compile/threadpool.h"
#include "compile/priority.h"
#include "interp/type_feedback.h"
#include "interp/profiler.h"
#include "profile/data.h"
#include "inliner/feedback.h"

#ifdef VORTEX_ENABLE_SOTA
#include "sota/markov.h"
#include "sota/phase.h"
#include "sota/recomp.h"
#include "sota/fdi.h"
#include "compile/phase_react.h"
#endif

/**
 * VORTEX Runtime Orchestrator — Wires Together All Speculative Infrastructure
 *
 * This module is the central hub that connects the previously-disconnected
 * speculative optimization components:
 *
 *   1. Markov predictions → compilation threadpool (proactive compilation)
 *      When the Markov chain predicts a phase transition, the orchestrator
 *      queues the predicted-hot methods for background compilation.
 *
 *   2. Recomp monitor → runtime type feedback (auto-recompile on drift)
 *      Periodically checks profile divergence and queues recompilation
 *      when the type profile has drifted significantly from the snapshot
 *      taken at compilation time.
 *
 *   3. FDI → deopt events → inline feedback loop
 *      When a deoptimization occurs at an inlined call site, the orchestrator
 *      feeds the deopt event to FDI, which may recommend recompilation with
 *      different inlining decisions.
 *
 *   4. Phase detection → proactive compilation pipeline
 *      The sota/phase module detects phase transitions; the orchestrator
 *      uses this to park the current version and reactivate or compile
 *      versions for the new phase.
 *
 * The orchestrator runs as a background thread that wakes up periodically
 * (or on explicit events like deopts) to perform its wiring functions.
 * All mutations to shared state (threadpool, version manager, etc.) are
 * done through their existing thread-safe APIs.
 *
 * Thread safety: The orchestrator itself is protected by its own mutex.
 * Individual module operations use their own synchronization.
 */

/* ========================================================================== */
/* Orchestrator configuration                                                   */
/* ========================================================================== */

/**
 * How often (in milliseconds) the orchestrator wakes up to check for
 * profile drift, phase transitions, and FDI recommendations.
 */
#define VTX_ORCHESTRATOR_CHECK_INTERVAL_MS 100

/**
 * Minimum number of type feedback observations before the recomp
 * monitor starts checking for profile drift. Prevents premature
 * recompilation when the profile hasn't had time to stabilize.
 */
#define VTX_ORCHESTRATOR_MIN_PROFILE_OBS 100

/**
 * How many methods to proactively compile per phase prediction.
 * Limits the compilation burst when a new phase is predicted.
 */
#define VTX_ORCHESTRATOR_PROACTIVE_COMPILE_LIMIT 8

/* ========================================================================== */
/* Orchestrator state                                                           */
/* ========================================================================== */

typedef struct {
    /* References to subsystems (not owned by orchestrator) */
#ifdef VORTEX_ENABLE_SOTA
    vtx_markov_t              *markov;           /* phase transition predictor */
    vtx_sota_phase_t          *phase_detector;   /* phase detection + prediction */
    vtx_sota_recomp_t         *recomp;           /* profile divergence monitor */
    vtx_sota_fdi_t            *fdi;              /* feedback-directed inlining */
    vtx_phase_react_manager_t *phase_react;       /* phase-reactive version manager */
#else
    void                      *markov;           /* NULL when SOTA disabled */
    void                      *phase_detector;   /* NULL when SOTA disabled */
    void                      *recomp;           /* NULL when SOTA disabled */
    void                      *fdi;              /* NULL when SOTA disabled */
    void                      *phase_react;       /* NULL when SOTA disabled */
#endif
    vtx_threadpool_t          *threadpool;        /* compilation threadpool */
    vtx_type_feedback_t       *type_feedback;     /* runtime type feedback data */
    vtx_profile_global_t      *profile;          /* global profile data */
    vtx_inline_feedback_t     *inline_feedback;  /* inline feedback tracker */

    /* Background thread state */
    pthread_t                  orchestrator_thread;
    pthread_mutex_t            mutex;
    pthread_cond_t             wake_cond;
    bool                       running;
    bool                       shutdown_requested;

    /* Configuration */
    uint32_t                   check_interval_ms;
    uint32_t                   min_profile_observations;
    uint32_t                   proactive_compile_limit;

    /* Statistics */
    uint64_t                   total_checks;
    uint64_t                   total_phase_predictions;
    uint64_t                   total_proactive_compiles;
    uint64_t                   total_recomp_triggers;
    uint64_t                   total_fdi_recompiles;
    uint64_t                   total_phase_reactivations;
} vtx_orchestrator_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the runtime orchestrator.
 *
 * All subsystem pointers are stored but NOT owned — the caller must
 * ensure they remain valid for the lifetime of the orchestrator.
 * Any pointer may be NULL to disable that particular wiring.
 *
 * @param orch         Orchestrator to initialize
 * @param markov       Markov chain (may be NULL)
 * @param phase        Phase detector (may be NULL)
 * @param recomp       Recomp monitor (may be NULL)
 * @param fdi          FDI tracker (may be NULL)
 * @param threadpool   Compilation threadpool (may be NULL)
 * @param phase_react  Phase-reactive manager (may be NULL)
 * @param type_feedback Runtime type feedback (may be NULL)
 * @param profile      Global profile data (may be NULL)
 * @param inline_feedback Inline feedback tracker (may be NULL)
 * @return             0 on success, -1 on failure
 */
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
                            vtx_inline_feedback_t *inline_feedback);

/**
 * Start the orchestrator background thread.
 *
 * The thread wakes up periodically and on explicit events to
 * perform wiring functions (proactive compilation, drift checks, etc.)
 *
 * @param orch  Orchestrator to start
 * @return      0 on success, -1 on failure
 */
int vtx_orchestrator_start(vtx_orchestrator_t *orch);

/**
 * Stop the orchestrator background thread and wait for it to finish.
 *
 * @param orch  Orchestrator to stop
 */
void vtx_orchestrator_stop(vtx_orchestrator_t *orch);

/**
 * Destroy the orchestrator and release resources.
 * Stops the background thread if running.
 *
 * @param orch  Orchestrator to destroy
 */
void vtx_orchestrator_destroy(vtx_orchestrator_t *orch);

/* ========================================================================== */
/* Event notifications (called from runtime/deopt paths)                        */
/* ========================================================================== */

/**
 * Notify the orchestrator that a method entry occurred.
 *
 * This feeds the method entry to the Markov chain and phase detector,
 * which may trigger proactive compilation for the predicted next phase.
 *
 * @param orch         Orchestrator
 * @param method_id    Method that was just entered
 */
void vtx_orchestrator_on_method_entry(vtx_orchestrator_t *orch,
                                        uint32_t method_id);

/**
 * Notify the orchestrator that a deoptimization occurred.
 *
 * This feeds the deopt event to FDI, which tracks unprofitable inlines
 * and may recommend recompilation with different inlining decisions.
 * Also records the failed guard in the guard metadata system.
 *
 * @param orch         Orchestrator
 * @param method_id    Method where the deopt occurred
 * @param call_site_id Call site where the deopt occurred
 * @param guard_id     Guard that failed (may be VTX_GUARD_ID_INVALID)
 */
void vtx_orchestrator_on_deopt(vtx_orchestrator_t *orch,
                                 uint32_t method_id,
                                 uint64_t call_site_id,
                                 uint32_t guard_id);

/**
 * Notify the orchestrator that a compilation completed.
 *
 * This saves a profile snapshot in the recomp monitor (so we can
 * detect drift later) and records the new version in FDI.
 *
 * @param orch         Orchestrator
 * @param method_id    Method that was compiled
 * @param version_id   New version ID
 */
void vtx_orchestrator_on_compile_done(vtx_orchestrator_t *orch,
                                        uint32_t method_id,
                                        uint32_t version_id);

/**
 * Wake the orchestrator immediately (e.g., after a burst of deopts).
 *
 * Normally the orchestrator wakes on its timer. This forces an
 * immediate check, which is useful when the system detects that
 * something has changed significantly (e.g., a phase transition
 * was detected by the interpreter).
 *
 * @param orch  Orchestrator to wake
 */
void vtx_orchestrator_wake(vtx_orchestrator_t *orch);

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

/**
 * Get orchestrator statistics.
 */
void vtx_orchestrator_get_stats(const vtx_orchestrator_t *orch,
                                  uint64_t *total_checks,
                                  uint64_t *total_phase_predictions,
                                  uint64_t *total_proactive_compiles,
                                  uint64_t *total_recomp_triggers,
                                  uint64_t *total_fdi_recompiles,
                                  uint64_t *total_phase_reactivations);

#endif /* VORTEX_COMPILE_ORCHESTRATOR_H */
