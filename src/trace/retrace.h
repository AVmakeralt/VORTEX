/* ========================================================================== */
/* Trace-Based PGO Re-Scoping                                                  */
/* ========================================================================== */
/*
 * vtx_trace_retrace.h — Trace-based PGO: re-record traces on guard failure.
 *
 * When a guard fails repeatedly in a compiled trace, the trace's speculative
 * assumptions are no longer valid. Instead of just deoptimizing to the
 * interpreter (which loses the trace's optimizations), the re-tracing system:
 *
 *   1. Detects that a guard is failing frequently (via the guard metadata
 *      EWMA failure rate or side exit counter).
 *   2. Invalidates the old trace's compiled code (so the interpreter
 *      dispatches to T0/T1 instead of the stale trace).
 *   3. Feeds the guard failure information back into the profile data
 *      (updates branch probabilities so the next trace recording picks
 *      a different hot path).
 *   4. Re-runs the trace selector + recorder + pipeline to produce a
 *      new specialized trace that accounts for the new type information.
 *
 * This implements the "Dynamic Superblock" vision: traces are linearized
 * SoN regions with guaranteed control flow. When the speculation fails,
 * we don't just deopt — we generate a new specialized variant.
 *
 * Integration points:
 *   - Orchestrator background thread calls vtx_trace_retrace_check()
 *   - Deopt handler calls vtx_trace_retrace_record_failure()
 *   - Pipeline compile-done callback calls vtx_trace_retrace_on_compile_done()
 */

#ifndef VORTEX_TRACE_RETRACE_H
#define VORTEX_TRACE_RETRACE_H

#include <stdint.h>
#include <stdbool.h>

#include "vortex_config.h"
#include "profile/data.h"
#include "guard/metadata.h"

/* Forward declaration to avoid circular include:
 * orchestrator.h includes retrace.h, so we can't include it here.
 * The actual struct is `struct vtx_orchestrator_struct` typedef'd to
 * `vtx_orchestrator_t` in orchestrator.h. */
struct vtx_orchestrator_struct;
typedef struct vtx_orchestrator_struct vtx_orchestrator_fwd_t;

/* ========================================================================== */
/* Configuration                                                               */
/* ========================================================================== */

/* EWMA failure rate threshold above which a guard triggers re-tracing.
 * 0.05 = 5% of executions fail → the trace's speculation is wrong. */
#define VTX_RETRACE_FAILURE_RATE_THRESHOLD 0.05

/* Minimum number of guard failures before considering re-tracing.
 * Prevents re-tracing on transient failures. */
#define VTX_RETRACE_MIN_FAILURES 10

/* Minimum executions before the EWMA is trustworthy. */
#define VTX_RETRACE_MIN_EXECUTIONS 1000

/* Cooldown between re-traces of the same method (in orchestrator checks).
 * Prevents re-trace storms. */
#define VTX_RETRACE_COOLDOWN_CHECKS 10

/* Maximum re-traces per method (gives up after this many attempts). */
#define VTX_RETRACE_MAX_ATTEMPTS 5

/* ========================================================================== */
/* Per-method re-trace state                                                   */
/* ========================================================================== */

typedef struct {
    uint32_t method_id;
    bool     active;              /* is this slot in use? */
    uint32_t check_cooldown;      /* checks remaining before next re-trace allowed */
    uint32_t attempt_count;       /* how many times we've re-traced this method */
    uint32_t last_failed_guard;   /* guard_id of the last failure that triggered */
    uint32_t failure_count;        /* guard failures since last re-trace (for threshold) */
    uint64_t total_retraces;       /* total re-traces performed */
} vtx_retrace_state_t;

/* ========================================================================== */
/* Re-trace registry                                                           */
/* ========================================================================== */

typedef struct {
    vtx_retrace_state_t *states;   /* array, indexed by method_id % capacity */
    uint32_t            capacity;
    uint32_t            retrace_count;  /* total re-traces across all methods */
} vtx_trace_retrace_t;

/* Initialize a re-trace registry. */
int vtx_trace_retrace_init(vtx_trace_retrace_t *rt, uint32_t capacity);

/* Destroy a re-trace registry. */
void vtx_trace_retrace_destroy(vtx_trace_retrace_t *rt);

/* Record a guard failure. Called from the deopt handler.
 *
 * Updates the per-method re-trace state with the failure information.
 * Does NOT trigger re-tracing directly — the orchestrator background
 * thread polls via vtx_trace_retrace_check().
 *
 * Parameters:
 *   rt        - the re-trace registry
 *   method_id - the method that deoptimized
 *   guard_id  - the guard that failed
 *   profile   - the global profile data (to update branch probabilities)
 *   guard_table - guard metadata table (to check failure rate)
 */
void vtx_trace_retrace_record_failure(vtx_trace_retrace_t *rt,
                                        uint32_t method_id,
                                        uint32_t guard_id,
                                        vtx_profile_global_t *profile,
                                        vtx_guard_meta_table_t *guard_table);

/* Check if any methods need re-tracing. Called from the orchestrator
 * background thread.
 *
 * For each method with active re-trace state:
 *   - Decrements the cooldown counter
 *   - Checks if the guard failure rate exceeds the threshold
 *   - If so, submits a re-trace compile task via the decision engine
 *
 * Parameters:
 *   rt    - the re-trace registry
 *   orch  - the orchestrator (for submitting compile tasks)
 *
 * Returns the number of re-trace tasks submitted.
 */
uint32_t vtx_trace_retrace_check(vtx_trace_retrace_t *rt,
                                   vtx_orchestrator_fwd_t *orch);

/* Called when a compile task completes. Resets the re-trace state
 * for the method so future guard failures can trigger new re-traces.
 */
void vtx_trace_retrace_on_compile_done(vtx_trace_retrace_t *rt,
                                         uint32_t method_id);

/* Get statistics for debugging. */
typedef struct {
    uint32_t active_methods;     /* methods with active re-trace state */
    uint32_t total_retraces;     /* total re-traces performed */
    uint32_t methods_at_max;     /* methods that hit VTX_RETRACE_MAX_ATTEMPTS */
} vtx_retrace_stats_t;

vtx_retrace_stats_t vtx_trace_retrace_stats(const vtx_trace_retrace_t *rt);

#endif /* VORTEX_TRACE_RETRACE_H */
