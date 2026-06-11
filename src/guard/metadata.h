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
    VTX_GUARD_UNCONDITIONAL = 0,  /* always true, no check needed */
    VTX_GUARD_FAST_CHECK    = 1,  /* rarely fails, simple check */
    VTX_GUARD_FULL_CHECK    = 2,  /* sometimes fails, full check + side exit */
    VTX_GUARD_DEOPT_ALWAYS  = 3   /* always fails, unconditional deopt */
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

    /* Guard kind (for logging/debugging) */
    uint32_t            guard_kind;  /* 0=null_check, 1=type_check, 2=bounds_check, etc. */

    /* Bytecode PC that this guard protects (for deopt) */
    uint32_t            bytecode_pc;

    /* EWMA of the failure rate — provides a smooth, responsive estimate
     * that is less susceptible to noise than the raw rate. Used for
     * strength transition decisions. Alpha = VTX_GUARD_ALPHA (0.1). */
    vtx_ewma_t          failure_rate_ewma;
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

#endif /* VORTEX_GUARD_METADATA_H */
