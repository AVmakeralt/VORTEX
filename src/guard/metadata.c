#include "guard/metadata.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Guard strength names                                                        */
/* ========================================================================== */

static const char * const strength_names[] = {
    "Unconditional",
    "FastCheck",
    "PredicatedCheck",
    "FullCheck",
    "DeoptAlways"
};

const char *vtx_guard_strength_name(vtx_guard_strength_t strength)
{
    if (strength > VTX_GUARD_DEOPT_ALWAYS) return "Unknown";
    return strength_names[strength];
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_guard_meta_table_init(vtx_guard_meta_table_t *table)
{
    if (table == NULL) return -1;

    table->guards = (vtx_guard_meta_t *)malloc(
        VTX_GUARD_META_INITIAL_CAPACITY * sizeof(vtx_guard_meta_t));
    if (table->guards == NULL) return -1;

    table->guard_count = 0;
    table->guard_capacity = VTX_GUARD_META_INITIAL_CAPACITY;
    table->total_executions = 0;
    table->total_failures = 0;
    table->unconditional_count = 0;
    table->fast_check_count = 0;
    table->full_check_count = 0;
    table->deopt_always_count = 0;
    table->predicated_check_count = 0;

    return 0;
}

void vtx_guard_meta_table_destroy(vtx_guard_meta_table_t *table)
{
    if (table == NULL) return;

    if (table->guards != NULL) {
        free(table->guards);
        table->guards = NULL;
    }
    table->guard_count = 0;
    table->guard_capacity = 0;
}

/* ========================================================================== */
/* Internal: grow the guards array                                             */
/* ========================================================================== */

static int meta_table_grow(vtx_guard_meta_table_t *table)
{
    uint32_t new_cap = table->guard_capacity * 2;
    vtx_guard_meta_t *new_guards = (vtx_guard_meta_t *)realloc(
        table->guards, new_cap * sizeof(vtx_guard_meta_t));
    if (new_guards == NULL) return -1;

    table->guards = new_guards;
    table->guard_capacity = new_cap;
    return 0;
}

/* ========================================================================== */
/* Internal: update strength category counts                                   */
/* ========================================================================== */

static void update_strength_counts(vtx_guard_meta_table_t *table,
                                    vtx_guard_strength_t old_strength,
                                    vtx_guard_strength_t new_strength)
{
    /* Decrement old count */
    switch (old_strength) {
    case VTX_GUARD_UNCONDITIONAL:    table->unconditional_count--; break;
    case VTX_GUARD_FAST_CHECK:       table->fast_check_count--; break;
    case VTX_GUARD_PREDICATED_CHECK: table->predicated_check_count--; break;
    case VTX_GUARD_FULL_CHECK:       table->full_check_count--; break;
    case VTX_GUARD_DEOPT_ALWAYS:     table->deopt_always_count--; break;
    }

    /* Increment new count */
    switch (new_strength) {
    case VTX_GUARD_UNCONDITIONAL:    table->unconditional_count++; break;
    case VTX_GUARD_FAST_CHECK:       table->fast_check_count++; break;
    case VTX_GUARD_PREDICATED_CHECK: table->predicated_check_count++; break;
    case VTX_GUARD_FULL_CHECK:       table->full_check_count++; break;
    case VTX_GUARD_DEOPT_ALWAYS:     table->deopt_always_count++; break;
    }
}

/* ========================================================================== */
/* Guard registration                                                          */
/* ========================================================================== */

vtx_guard_meta_t *vtx_guard_meta_register(vtx_guard_meta_table_t *table,
                                            vtx_nodeid_t guard_node,
                                            uint32_t guard_kind,
                                            uint32_t bytecode_pc,
                                            vtx_guard_strength_t strength)
{
    if (table == NULL) return NULL;

    /* Check if already registered */
    vtx_guard_meta_t *existing = vtx_guard_meta_lookup(table, guard_node);
    if (existing != NULL) return existing;

    /* Grow if needed */
    if (table->guard_count >= table->guard_capacity) {
        if (meta_table_grow(table) != 0) return NULL;
    }

    /* Create new guard metadata */
    vtx_guard_meta_t *meta = &table->guards[table->guard_count];
    memset(meta, 0, sizeof(*meta));

    meta->guard_id = table->guard_count;
    meta->guard_node = guard_node;
    meta->execution_count = 0;
    meta->failure_count = 0;
    meta->last_failure_timestamp = 0;
    meta->strength = strength;
    meta->strength_changed = false;
    meta->guard_kind = guard_kind;
    meta->bytecode_pc = bytecode_pc;

    /* Initialize EWMA with the default alpha (VTX_GUARD_ALPHA = 0.1) */
    vtx_ewma_init(&meta->failure_rate_ewma);

    /* Initialize long-window EWMA for bidirectional strengthening (Proposal #10) */
    vtx_ewma_init(&meta->long_failure_rate_ewma);
    meta->long_failure_rate_ewma.alpha = 0.01;  /* long window: alpha=0.01, half-life ~70 observations */
    meta->phase_context = 0;
    meta->residence_count = 0;

    /* Initialize sampling-based profiling (zero-cost deopt) */
    meta->sample_counter = VTX_GUARD_SAMPLE_INTERVAL_DEFAULT;
    meta->sample_interval = VTX_GUARD_SAMPLE_INTERVAL_DEFAULT;
    meta->sampled_executions = 0;
    meta->sampled_failures = 0;

    table->guard_count++;

    /* Update strength category count */
    switch (strength) {
    case VTX_GUARD_UNCONDITIONAL:    table->unconditional_count++; break;
    case VTX_GUARD_FAST_CHECK:       table->fast_check_count++; break;
    case VTX_GUARD_PREDICATED_CHECK: table->predicated_check_count++; break;
    case VTX_GUARD_FULL_CHECK:       table->full_check_count++; break;
    case VTX_GUARD_DEOPT_ALWAYS:     table->deopt_always_count++; break;
    }

    return meta;
}

/* ========================================================================== */
/* Update and query                                                            */
/* ========================================================================== */

void vtx_guard_meta_update(vtx_guard_meta_t *meta, bool failed)
{
    if (meta == NULL) return;

    /* Saturating increment of execution count */
    if (meta->execution_count < UINT64_MAX) {
        meta->execution_count++;
    }

    if (failed) {
        /* Saturating increment of failure count */
        if (meta->failure_count < UINT64_MAX) {
            meta->failure_count++;
        }

        /* Update last failure timestamp.
         * The caller should set this to the current monotonic time.
         * For now, we use the execution count as a proxy. */
        meta->last_failure_timestamp = meta->execution_count;
    }

    /* Update the EWMA with the current observation.
     * Each execution is treated as a single observation:
     *   failure_rate = failed ? 1.0 : 0.0
     * The EWMA smoothly tracks the recent failure rate. */
    double obs = failed ? 1.0 : 0.0;
    vtx_ewma_update(&meta->failure_rate_ewma, obs);

    /* Update long-window EWMA for bidirectional strengthening (Proposal #10) */
    vtx_ewma_update(&meta->long_failure_rate_ewma, obs);
    if (meta->residence_count < UINT64_MAX) {
        meta->residence_count++;
    }

    /* Check for strength transition using the EWMA failure rate.
     * Transitions are monotonic: guard can only get weaker.
     * The EWMA provides a more stable signal than the raw rate,
     * preventing premature transitions from small sample noise. */
    vtx_guard_strength_t old_strength = meta->strength;
    vtx_guard_strength_t new_strength = old_strength;
    double ewma_rate = vtx_ewma_value(&meta->failure_rate_ewma);

    switch (old_strength) {
    case VTX_GUARD_UNCONDITIONAL:
        /* Unconditional → FastCheck on first failure */
        if (failed) {
            new_strength = VTX_GUARD_FAST_CHECK;
        }
        break;

    case VTX_GUARD_FAST_CHECK:
        /* FastCheck → PredicatedCheck if EWMA < PREDICATE_THRESHOLD */
        if (ewma_rate < VTX_GUARD_PREDICATE_THRESHOLD && meta->execution_count >= 10000) {
            new_strength = VTX_GUARD_PREDICATED_CHECK;
        }
        /* FastCheck → FullCheck if EWMA failure rate > VTX_GUARD_WEAKEN_THRESHOLD */
        else if (ewma_rate > VTX_GUARD_WEAKEN_THRESHOLD) {
            new_strength = VTX_GUARD_FULL_CHECK;
        }
        break;

    case VTX_GUARD_PREDICATED_CHECK:
        /* PredicatedCheck → FullCheck if EWMA > WEAKEN_THRESHOLD */
        if (ewma_rate > VTX_GUARD_WEAKEN_THRESHOLD) {
            new_strength = VTX_GUARD_FULL_CHECK;
        }
        break;

    case VTX_GUARD_FULL_CHECK:
        /* FullCheck → DeoptAlways if EWMA failure rate > VTX_GUARD_ABANDON_THRESHOLD */
        if (ewma_rate > VTX_GUARD_ABANDON_THRESHOLD) {
            new_strength = VTX_GUARD_DEOPT_ALWAYS;
        }
        break;

    case VTX_GUARD_DEOPT_ALWAYS:
        /* No further weakening possible */
        break;
    }

    /* Apply transition */
    if (new_strength != old_strength) {
        meta->strength = new_strength;
        meta->strength_changed = true;
    }
}

vtx_guard_strength_t vtx_guard_meta_strength(const vtx_guard_meta_t *meta)
{
    if (meta == NULL) return VTX_GUARD_DEOPT_ALWAYS;
    return meta->strength;
}

/* ========================================================================== */
/* Sampling-based update (zero-cost deopt)                                     */
/* ========================================================================== */

/**
 * Choose an adaptive sampling interval based on the guard's current
 * EWMA failure rate. Stable guards get longer intervals (less overhead),
 * unstable guards get shorter intervals (more responsive transitions).
 */
static uint32_t choose_sample_interval(const vtx_guard_meta_t *meta)
{
    if (meta == NULL || !meta->failure_rate_ewma.initialized) {
        return VTX_GUARD_SAMPLE_INTERVAL_DEFAULT;
    }

    double ewma = vtx_ewma_value(&meta->failure_rate_ewma);

    if (ewma < VTX_GUARD_PREDICATE_THRESHOLD) {
        /* Very stable: < 0.001% failure rate */
        return VTX_GUARD_SAMPLE_INTERVAL_STABLE;
    } else if (ewma < VTX_GUARD_WEAKEN_THRESHOLD) {
        /* Moderate: < 1% failure rate */
        return VTX_GUARD_SAMPLE_INTERVAL_DEFAULT;
    } else {
        /* Unstable: > 1% failure rate — need responsive tracking */
        return VTX_GUARD_SAMPLE_INTERVAL_UNSTABLE;
    }
}

void vtx_guard_meta_update_sampled(vtx_guard_meta_t *meta, bool failed)
{
    if (meta == NULL) return;

    /* Accumulate execution and failure counts without touching the EWMA.
     * This is the hot-path work: just two integer increments and a
     * decrement of the sample counter. The branch on counter == 0
     * is highly predictable (taken with probability 1/interval). */
    if (meta->sampled_executions < UINT64_MAX) {
        meta->sampled_executions++;
    }
    if (failed && meta->sampled_failures < UINT64_MAX) {
        meta->sampled_failures++;
    }

    /* On failure, immediately shorten the sampling interval to ensure
     * we detect instability quickly. This is critical: without it,
     * a burst of failures could be missed for up to 4096 executions. */
    if (failed) {
        uint32_t current = meta->sample_interval;
        if (current > VTX_GUARD_SAMPLE_INTERVAL_UNSTABLE) {
            meta->sample_interval = VTX_GUARD_SAMPLE_INTERVAL_UNSTABLE;
            /* Also shorten the current counter if it's far from zero */
            if (meta->sample_counter > VTX_GUARD_SAMPLE_INTERVAL_UNSTABLE) {
                meta->sample_counter = VTX_GUARD_SAMPLE_INTERVAL_UNSTABLE;
            }
        }
    }

    /* Decrement the sample counter. Only do the expensive EWMA update
     * when the counter reaches zero. */
    if (__builtin_expect(meta->sample_counter > 1, 1)) {
        meta->sample_counter--;
        return;
    }

    /* ---- Sample boundary: perform full update ---- */

    /* Apply the accumulated executions and failures to the
     * overall counters. */
    uint64_t batch_exec = meta->sampled_executions;
    uint64_t batch_fail = meta->sampled_failures;

    /* Reset sampled accumulators */
    meta->sampled_executions = 0;
    meta->sampled_failures = 0;

    /* Saturating increment of execution count */
    if (meta->execution_count < UINT64_MAX - batch_exec) {
        meta->execution_count += batch_exec;
    } else {
        meta->execution_count = UINT64_MAX;
    }

    /* Saturating increment of failure count */
    if (batch_fail > 0) {
        if (meta->failure_count < UINT64_MAX - batch_fail) {
            meta->failure_count += batch_fail;
        } else {
            meta->failure_count = UINT64_MAX;
        }

        /* Update last failure timestamp using execution count as proxy */
        meta->last_failure_timestamp = meta->execution_count;
    }

    /* Update the EWMA with the batched observation.
     * We use vtx_ewma_update_counts to compute the failure rate
     * from the batch and incorporate it into the EWMA in one shot.
     * This is equivalent to running individual updates but with
     * much less FP math overhead. */
    if (batch_exec > 0) {
        vtx_ewma_update_counts(&meta->failure_rate_ewma,
                                batch_fail, batch_exec);
        vtx_ewma_update_counts(&meta->long_failure_rate_ewma,
                                batch_fail, batch_exec);
    }

    /* Increment residence count */
    if (meta->residence_count < UINT64_MAX - batch_exec) {
        meta->residence_count += batch_exec;
    } else {
        meta->residence_count = UINT64_MAX;
    }

    /* Check for strength transition using the EWMA failure rate. */
    vtx_guard_strength_t old_strength = meta->strength;
    vtx_guard_strength_t new_strength = old_strength;
    double ewma_rate = vtx_ewma_value(&meta->failure_rate_ewma);

    switch (old_strength) {
    case VTX_GUARD_UNCONDITIONAL:
        if (batch_fail > 0) {
            new_strength = VTX_GUARD_FAST_CHECK;
        }
        break;

    case VTX_GUARD_FAST_CHECK:
        if (ewma_rate < VTX_GUARD_PREDICATE_THRESHOLD &&
            meta->execution_count >= 10000) {
            new_strength = VTX_GUARD_PREDICATED_CHECK;
        } else if (ewma_rate > VTX_GUARD_WEAKEN_THRESHOLD) {
            new_strength = VTX_GUARD_FULL_CHECK;
        }
        break;

    case VTX_GUARD_PREDICATED_CHECK:
        if (ewma_rate > VTX_GUARD_WEAKEN_THRESHOLD) {
            new_strength = VTX_GUARD_FULL_CHECK;
        }
        break;

    case VTX_GUARD_FULL_CHECK:
        if (ewma_rate > VTX_GUARD_ABANDON_THRESHOLD) {
            new_strength = VTX_GUARD_DEOPT_ALWAYS;
        }
        break;

    case VTX_GUARD_DEOPT_ALWAYS:
        break;
    }

    /* Apply transition */
    if (new_strength != old_strength) {
        meta->strength = new_strength;
        meta->strength_changed = true;
    }

    /* Adapt the sampling interval based on the current failure rate.
     * Stable guards get longer intervals (less overhead), unstable
     * guards get shorter intervals (more responsive transitions). */
    meta->sample_interval = choose_sample_interval(meta);
    meta->sample_counter = meta->sample_interval;
}

vtx_guard_meta_t *vtx_guard_meta_lookup(vtx_guard_meta_table_t *table,
                                          vtx_nodeid_t guard_node)
{
    if (table == NULL) return NULL;

    /* Linear scan — guard tables are typically small (< 1000 entries) */
    for (uint32_t i = 0; i < table->guard_count; i++) {
        if (table->guards[i].guard_node == guard_node) {
            return &table->guards[i];
        }
    }
    return NULL;
}

double vtx_guard_meta_failure_rate(const vtx_guard_meta_t *meta)
{
    if (meta == NULL || meta->execution_count == 0) return 0.0;
    return (double)meta->failure_count / (double)meta->execution_count;
}

uint32_t vtx_guard_meta_pending_transitions(const vtx_guard_meta_table_t *table)
{
    if (table == NULL) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < table->guard_count; i++) {
        if (table->guards[i].strength_changed) {
            count++;
        }
    }
    return count;
}

void vtx_guard_meta_clear_transition_flags(vtx_guard_meta_table_t *table)
{
    if (table == NULL) return;

    for (uint32_t i = 0; i < table->guard_count; i++) {
        table->guards[i].strength_changed = false;
    }
}

/* ========================================================================== */
/* Bidirectional guard strength (Proposal #10)                                  */
/* ========================================================================== */

bool vtx_guard_meta_try_strengthen(vtx_guard_meta_t *meta, uint32_t new_phase)
{
    if (meta == NULL) return false;

    /* Can only strengthen from weakened states */
    if (meta->strength == VTX_GUARD_UNCONDITIONAL || meta->strength == VTX_GUARD_DEOPT_ALWAYS) {
        return false;
    }

    /* Check minimum residence time at current strength */
    if (meta->residence_count < VTX_GUARD_MIN_RESIDENCE) return false;

    /* Check that phase context has changed (otherwise no reason to strengthen) */
    if (meta->phase_context == new_phase) return false;

    /* Check long-window EWMA failure rate is below strengthen threshold */
    double long_rate = vtx_ewma_value(&meta->long_failure_rate_ewma);
    if (long_rate > VTX_GUARD_STRENGTHEN_THRESHOLD) return false;

    /* Strengthen: move one level up */
    vtx_guard_strength_t old_strength = meta->strength;
    vtx_guard_strength_t new_strength = old_strength;

    switch (old_strength) {
    case VTX_GUARD_FULL_CHECK:
        new_strength = VTX_GUARD_PREDICATED_CHECK;
        break;
    case VTX_GUARD_PREDICATED_CHECK:
        new_strength = VTX_GUARD_FAST_CHECK;
        break;
    case VTX_GUARD_FAST_CHECK:
        new_strength = VTX_GUARD_UNCONDITIONAL;
        break;
    default:
        return false;
    }

    if (new_strength != old_strength) {
        meta->strength = new_strength;
        meta->strength_changed = true;
        meta->phase_context = new_phase;
        meta->residence_count = 0;
        /* Reset both EWMAs after strengthening to avoid immediate re-weakening */
        vtx_ewma_init(&meta->failure_rate_ewma);
        vtx_ewma_init(&meta->long_failure_rate_ewma);
        meta->long_failure_rate_ewma.alpha = 0.01;
        return true;
    }

    return false;
}
