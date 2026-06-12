#ifndef VORTEX_GUARD_METADATA_H
#define VORTEX_GUARD_METADATA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "guard/ewma.h"
#include "ir/node.h"

/**
 * VORTEX Adaptive Guards — Metadata and Strength Tracking
 *
 * Each guard in the compiled code has associated metadata that tracks
 * its execution and failure counts, enabling adaptive strength transitions.
 *
 * Guard strength levels (from strongest to weakest speculation):
 *
 *   Unconditional  — guard is always true (e.g., proved by analysis)
 *                    No runtime check emitted. Zero overhead.
 *
 *   FastCheck      — guard rarely fails; emit a simple compare+branch.
 *                    Minimal overhead. Deopt on failure.
 *
 *   FullCheck      — guard sometimes fails; emit full check with
 *                    side exit to interpreter. Moderate overhead.
 *
 *   DeoptAlways    — guard always fails; no check needed, always deopt.
 *                    Used when a speculation has been invalidated.
 *
 * Transitions (monotonic weakening):
 *   Unconditional → FastCheck    : first failure observed
 *   FastCheck     → FullCheck    : EWMA failure rate > VTX_GUARD_WEAKEN_THRESHOLD (1%)
 *   FullCheck     → DeoptAlways  : EWMA failure rate > VTX_GUARD_ABANDON_THRESHOLD (25%)
 *
 * Transitions are one-way: a guard can only get weaker, never stronger.
 * This is because the conditions that caused weakening don't go away —
 * the same types/values that caused the failure will continue to appear.
 *
 * The EWMA provides a smooth, responsive failure rate estimate that
 * adapts quickly to changes while filtering out noise from low counts.
 * Raw failure rate (failures/executions) can be noisy for small sample
 * sizes; the EWMA stabilizes the transition decision.
 *
 * Thread safety: NOT thread-safe. The caller must synchronize.
 */

/* ========================================================================== */
/* Guard strength levels                                                       */
/* ========================================================================== */

typedef enum {
    VTX_GUARD_UNCONDITIONAL    = 0,  /* always true, no check needed */
    VTX_GUARD_FAST_CHECK       = 1,  /* rarely fails, simple check */
    VTX_GUARD_PREDICATED_CHECK = 2,  /* very low failure rate, CMOVCC+INT3 (Proposal #11) */
    VTX_GUARD_FULL_CHECK       = 3,  /* sometimes fails, full check + side exit */
    VTX_GUARD_DEOPT_ALWAYS     = 4   /* always fails, unconditional deopt */
} vtx_guard_strength_t;

/* Human-readable name for guard strength level */
const char *vtx_guard_strength_name(vtx_guard_strength_t strength);

/* ========================================================================== */
/* Guard metadata                                                              */
/* ========================================================================== */

typedef struct {
    uint32_t            guard_id;          /* unique guard identifier */
    vtx_nodeid_t        guard_node;        /* SoN node ID of the guard */

    /* Execution statistics */
    uint64_t            execution_count;   /* times this guard was reached */
    uint64_t            failure_count;     /* times this guard failed */

    /* Timestamp of last failure (monotonic clock, nanoseconds) */
    uint64_t            last_failure_timestamp;

    /* Current strength level */
    vtx_guard_strength_t strength;

    /* Whether the strength has been upgraded (weakened) since last check */
    bool                strength_changed;

    /* Guard kind (for logging/debugging)
     *   0 = null_check
     *   1 = type_check
     *   2 = bounds_check
     *   3 = composite_type_check  (Proposal #2: checks full type signature)
     *   4 = shape_check           (Proposal #9: checks object shape ID)
     *   5 = trip_count_check      (Proposal #7: checks loop trip count)
     */
    uint32_t            guard_kind;

    /* Bytecode PC that this guard protects (for deopt) */
    uint32_t            bytecode_pc;

    /* EWMA of the failure rate — provides a smooth, responsive estimate
     * that is less susceptible to noise than the raw rate. Used for
     * strength transition decisions. Alpha = VTX_GUARD_ALPHA (0.1). */
    vtx_ewma_t          failure_rate_ewma;

    /* Composite guard metadata (Proposal #2).
     * For hyper-stable call sites, a single composite guard checks the
     * entire type signature at once. */
    uint64_t            expected_signature_hash; /* expected FNV hash of the type signature */
    bool                is_composite_guard;      /* true if this is a composite type signature guard */

    /* Bidirectional guard strength (Proposal #10) */
    vtx_ewma_t          long_failure_rate_ewma;  /* longer-window EWMA (alpha=0.01) for strengthening */
    uint32_t            phase_context;            /* method call stack hash at time of last weakening */
    uint64_t            residence_count;          /* executions at current strength level */

    /* Sampling-based profiling (zero-cost deopt).
     * Instead of updating the EWMA on every guard execution (~5 cycles
     * of FP math), only update every Nth execution. The counter
     * decrements on each pass; when it hits zero, the full update
     * runs and the counter resets. This reduces hot-path overhead
     * from ~5 cycles/guard to ~1 cycle (decrement + predicted branch).
     *
     * The sample_interval is adaptive: stable guards (low failure rate)
     * get longer intervals (less frequent updates), while guards near
     * a transition threshold get shorter intervals (more responsive). */
    uint32_t            sample_counter;           /* countdown to next EWMA update */
    uint32_t            sample_interval;          /* reset value for sample_counter */
    uint64_t            sampled_executions;       /* executions accumulated between samples */
    uint64_t            sampled_failures;         /* failures accumulated between samples */
} vtx_guard_meta_t;

/* ========================================================================== */
/* Guard metadata table                                                        */
/* ========================================================================== */

#define VTX_GUARD_META_INITIAL_CAPACITY 64

typedef struct {
    vtx_guard_meta_t   *guards;         /* array of guard metadata */
    uint32_t            guard_count;    /* number of active guards */
    uint32_t            guard_capacity; /* allocated capacity */

    /* Summary statistics */
    uint64_t            total_executions;  /* sum of all guard execution counts */
    uint64_t            total_failures;    /* sum of all guard failure counts */
    uint32_t            unconditional_count;
    uint32_t            fast_check_count;
    uint32_t            full_check_count;
    uint32_t            deopt_always_count;
    uint32_t            predicated_check_count;  /* Proposal #11 */
} vtx_guard_meta_table_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize a guard metadata table.
 * Returns 0 on success, -1 on failure.
 */
int vtx_guard_meta_table_init(vtx_guard_meta_table_t *table);

/**
 * Destroy a guard metadata table and free memory.
 */
void vtx_guard_meta_table_destroy(vtx_guard_meta_table_t *table);

/* ========================================================================== */
/* Guard registration                                                          */
/* ========================================================================== */

/**
 * Register a new guard in the metadata table.
 *
 * @param table       Guard metadata table
 * @param guard_node  SoN node ID of the guard
 * @param guard_kind  Kind of guard (0=null, 1=type, 2=bounds, etc.)
 * @param bytecode_pc Bytecode PC protected by this guard
 * @param strength    Initial strength (typically FastCheck for speculative guards)
 * @return            Pointer to the new guard metadata, or NULL on failure
 */
vtx_guard_meta_t *vtx_guard_meta_register(vtx_guard_meta_table_t *table,
                                            vtx_nodeid_t guard_node,
                                            uint32_t guard_kind,
                                            uint32_t bytecode_pc,
                                            vtx_guard_strength_t strength);

/* ========================================================================== */
/* Update and query                                                            */
/* ========================================================================== */

/**
 * Update a guard's metadata after an execution.
 *
 * @param meta    Guard metadata to update
 * @param failed  Whether this execution resulted in a guard failure
 *
 * Side effects:
 *   - Increments execution_count (saturating)
 *   - If failed: increments failure_count, updates last_failure_timestamp
 *   - Updates the EWMA failure rate
 *   - Checks for strength transition based on EWMA failure rate
 *   - Sets strength_changed flag if a transition occurred
 */
void vtx_guard_meta_update(vtx_guard_meta_t *meta, bool failed);

/**
 * Sampling-based guard metadata update (zero-cost deopt).
 *
 * Instead of updating the EWMA on every execution, only updates
 * every sample_interval executions. Between samples, execution and
 * failure counts are accumulated and batch-applied when the counter
 * reaches zero.
 *
 * This reduces the hot-path cost from ~5 cycles (2 FP multiplies +
 * counter increments + transition checks) to ~1 cycle (decrement +
 * predicted branch). The branch is highly predictable since the
 * counter rarely hits zero.
 *
 * The sample interval adapts based on guard stability:
 *   - Stable guards (EWMA < 0.01%): interval = 4096
 *   - Moderate guards (EWMA < 1%):   interval = 1024
 *   - Unstable guards (EWMA > 1%):   interval = 256
 *   - After a failure:               interval = min(interval, 256)
 *
 * @param meta    Guard metadata to update
 * @param failed  Whether this execution resulted in a guard failure
 */
void vtx_guard_meta_update_sampled(vtx_guard_meta_t *meta, bool failed);

/* ========================================================================== */
/* Hot-path inline update (zero-cost deopt)                                    */
/* ========================================================================== */

/**
 * Hot-path inline guard metadata update for JIT-compiled code.
 *
 * This is the function that JIT code should call on every guard execution.
 * It provides the zero-cost deopt hot path:
 *   1. Increment sampled_executions (integer add)
 *   2. If failed: increment sampled_failures, shorten interval
 *   3. Decrement sample_counter
 *   4. If counter == 0: call the slow path (vtx_guard_meta_update_sampled_slow)
 *
 * The hot-path cost is ~1 cycle (decrement + predicted branch) when the
 * guard passes and the counter hasn't reached zero. The branch on
 * counter == 0 is taken with probability 1/interval (highly predictable).
 *
 * On failure, the interval is immediately shortened to UNSTABLE (256)
 * to ensure rapid detection of instability.
 *
 * This function is designed to be callable from JIT-compiled code via
 * a function pointer stored in a global or per-guard stub.
 *
 * @param meta    Guard metadata to update
 * @param failed  Whether this execution resulted in a guard failure
 */
static inline void vtx_guard_meta_update_hot(vtx_guard_meta_t *meta, bool failed)
{
    if (meta == NULL) return;

    /* Increment sampled execution counter */
    if (meta->sampled_executions < UINT64_MAX) {
        meta->sampled_executions++;
    }

    /* Track failures */
    if (failed) {
        if (meta->sampled_failures < UINT64_MAX) {
            meta->sampled_failures++;
        }
        /* Shorten interval on failure for rapid instability detection */
        if (meta->sample_interval > VTX_GUARD_SAMPLE_INTERVAL_UNSTABLE) {
            meta->sample_interval = VTX_GUARD_SAMPLE_INTERVAL_UNSTABLE;
            if (meta->sample_counter > VTX_GUARD_SAMPLE_INTERVAL_UNSTABLE) {
                meta->sample_counter = VTX_GUARD_SAMPLE_INTERVAL_UNSTABLE;
            }
        }
    }

    /* Decrement counter — the branch on zero is highly predictable */
    if (__builtin_expect(meta->sample_counter > 1, 1)) {
        meta->sample_counter--;
        return;
    }

    /* Sample boundary reached — call the slow path */
    vtx_guard_meta_update_sampled(meta, failed);
}

/**
 * Default sampling interval for guard metadata updates.
 * Only update the EWMA every Nth execution to reduce hot-path overhead.
 */
#define VTX_GUARD_SAMPLE_INTERVAL_DEFAULT  1024

/**
 * Sampling interval for very stable guards (EWMA failure rate < 0.01%).
 * These guards almost never fail, so we check less frequently.
 */
#define VTX_GUARD_SAMPLE_INTERVAL_STABLE   4096

/**
 * Sampling interval for unstable guards (EWMA failure rate > 1%).
 * These guards are near transition thresholds and need responsive tracking.
 */
#define VTX_GUARD_SAMPLE_INTERVAL_UNSTABLE 256

/**
 * Get the current strength level of a guard.
 * This reflects any transitions that have occurred based on
 * the EWMA failure rate.
 *
 * @param meta Guard metadata
 * @return     Current strength level
 */
vtx_guard_strength_t vtx_guard_meta_strength(const vtx_guard_meta_t *meta);

/**
 * Look up a guard by its SoN node ID.
 * Returns NULL if not found.
 */
vtx_guard_meta_t *vtx_guard_meta_lookup(vtx_guard_meta_table_t *table,
                                          vtx_nodeid_t guard_node);

/**
 * Compute the failure rate for a guard using raw counts.
 * Returns 0.0 if the guard has never been executed.
 * For transition decisions, use the EWMA value via
 * vtx_ewma_value(&meta->failure_rate_ewma) instead.
 */
double vtx_guard_meta_failure_rate(const vtx_guard_meta_t *meta);

/**
 * Check whether any guard in the table has a pending strength change.
 * Returns the number of guards with strength_changed = true.
 */
uint32_t vtx_guard_meta_pending_transitions(const vtx_guard_meta_table_t *table);

/**
 * Clear all strength_changed flags in the table.
 */
void vtx_guard_meta_clear_transition_flags(vtx_guard_meta_table_t *table);

/* ========================================================================== */
/* Bidirectional guard strength (Proposal #10)                                  */
/* ========================================================================== */

/**
 * EWMA failure rate threshold for guard predication.
 * Below this rate, guards transition to PredicatedCheck which
 * uses CMOVCC+INT3 instead of JCC to avoid branch misprediction.
 */
#define VTX_GUARD_PREDICATE_THRESHOLD 0.00001  /* 0.001% */

/**
 * Guard strengthening threshold: if the long-window EWMA failure rate
 * drops below this, the guard may strengthen. Set to 0.1% (one-tenth
 * of the weaken threshold) to avoid thrashing.
 */
#define VTX_GUARD_STRENGTHEN_THRESHOLD 0.001

/**
 * Minimum number of executions at current strength before a guard
 * can be considered for strengthening. Prevents rapid oscillation.
 */
#define VTX_GUARD_MIN_RESIDENCE 10000

/**
 * Try to strengthen a guard based on long-window EWMA and phase context.
 * Called from the safepoint handler when a phase transition is detected.
 *
 * @param meta           Guard metadata
 * @param new_phase      New phase context hash
 * @return               true if the guard was strengthened
 */
bool vtx_guard_meta_try_strengthen(vtx_guard_meta_t *meta, uint32_t new_phase);

#endif /* VORTEX_GUARD_METADATA_H */
