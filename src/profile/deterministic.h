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

#ifndef VORTEX_PROFILE_DETERMINISTIC_H
#define VORTEX_PROFILE_DETERMINISTIC_H

/**
 * VORTEX Deterministic Mode (Sprint 1.4)
 *
 * When VORTEX_DETERMINISTIC=1 is set in the environment, the JIT enters a
 * fully deterministic mode suitable for CI / regression testing:
 *
 *   - Background compilation threadpool is reduced to 1 worker
 *   - Orchestrator check interval is fixed (no jitter)
 *   - Hysteresis thresholds are tightened so recomp decisions are stable
 *   - Adaptive guard EWMA is frozen (no time-based decay)
 *   - Profile persistence is disabled (each run starts fresh)
 *   - Random tie-breaking in compilation priority is replaced with
 *     method_id-ordered tie-breaking
 *
 * Why: PGO introduces nondeterminism by design — the same program may be
 * optimized differently on different runs depending on timing, OS noise,
 * and the order in which threads pick up compilation tasks. This is fine
 * for production (where any valid optimization is acceptable) but toxic
 * for CI, where flaky tests erode trust.
 *
 * Deterministic mode is opt-in via env var. Production runs are unaffected.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* Initialization                                                              */
/* ========================================================================== */

/**
 * Probe VORTEX_DETERMINISTIC at startup and cache the result.
 *
 * This is called once during JIT initialization (vortex_main). Subsequent
 * calls to vtx_deterministic_enabled() return the cached value without
 * re-reading the environment.
 *
 * The env var is checked as:
 *   VORTEX_DETERMINISTIC=1   → enabled
 *   VORTEX_DETERMINISTIC=0   → disabled (explicit)
 *   VORTEX_DETERMINISTIC=    → disabled (empty)
 *   VORTEX_DETERMINISTIC unset → disabled
 *
 * Any other value (e.g. "true", "yes") is treated as enabled for
 * convenience — only "0" and "" explicitly disable.
 */
void vtx_deterministic_init(void);

/* ========================================================================== */
/* Query                                                                       */
/* ========================================================================== */

/**
 * Returns true if deterministic mode is enabled.
 *
 * Callers should consult this before making any decision that could
 * introduce nondeterminism:
 *   - Threadpool size selection
 *   - Orchestrator sleep interval
 *   - Adaptive guard decay rate
 *   - Profile persistence enable/disable
 *   - Compilation priority tie-breaking
 */
bool vtx_deterministic_enabled(void);

/**
 * Returns the deterministic-mode threadpool size (1 if enabled, 0 otherwise).
 *
 * Use this when sizing the compilation threadpool:
 *   uint32_t nthreads = vtx_deterministic_threads();
 *   if (nthreads == 0) nthreads = VORTEX_COMPILE_THREADS;  // default
 */
uint32_t vtx_deterministic_threads(void);

/**
 * Returns the deterministic-mode orchestrator check interval in milliseconds.
 *
 * When deterministic mode is enabled, this is a fixed 100ms (no jitter).
 * When disabled, returns 0 (use the default VTX_ORCHESTRATOR_CHECK_INTERVAL_MS).
 */
uint32_t vtx_deterministic_check_interval_ms(void);

/**
 * Returns true if profile persistence should be disabled.
 *
 * In deterministic mode, we disable profile load/save so that each run
 * starts from a clean slate and produces identical compilation decisions.
 */
bool vtx_deterministic_disable_persistence(void);

/**
 * Returns true if adaptive guard EWMA decay should be frozen.
 *
 * In deterministic mode, time-based decay introduces nondeterminism because
 * it depends on wall-clock time. Freezing the EWMA means guards either fire
 * or don't based purely on observed failure counts, which is reproducible.
 */
bool vtx_deterministic_freeze_guard_ewma(void);

#ifdef __cplusplus
}
#endif

#endif /* VORTEX_PROFILE_DETERMINISTIC_H */
