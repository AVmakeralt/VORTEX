#ifndef VORTEX_GUARD_VALUE_PROFILE_H
#define VORTEX_GUARD_VALUE_PROFILE_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"

/**
 * VORTEX Value Profiling — Speculative Constant Folding
 *
 * Value profiling tracks the most frequently observed values at specific
 * program points (field loads, method parameters, return values). When
 * a single value dominates (frequency > 95%), the JIT can speculate
 * that the value is always that constant, enabling:
 *
 *   - Constant folding: replace runtime loads with constants
 *   - Dead code elimination: branches on constant conditions are removed
 *   - Loop unrolling: known trip counts allow full unrolling
 *   - Strength reduction: multiply by constant → shift + add
 *   - Inlining: monomorphic calls after value-based type resolution
 *
 * This is the single most impactful optimization that VORTEX doesn't
 * have yet. V8 TurboFan, Graal/Truffle, and Azul Zing all use value
 * speculation as a core optimization, and it typically provides 10-30%
 * speedup on real-world workloads with stable value distributions.
 *
 * Data structure: each profile site tracks the top 2 observed values
 * with their frequencies. The top value is the speculation candidate;
 * the second value is tracked to detect bimodal distributions early.
 *
 * Stability: a value is considered "stable" when its frequency exceeds
 * VTX_VALUE_STABILITY_THRESHOLD (0.95 = 95%). At that point, the JIT
 * can emit a value guard that checks for the expected constant and
 * deoptimizes if a different value appears.
 *
 * Sampling: to avoid overhead on the hot path, value profiling uses
 * sampling. Only every Nth observation updates the profile. The sample
 * interval adapts based on stability: stable sites get longer intervals
 * (less overhead), unstable sites get shorter intervals (more responsive).
 */

/* ========================================================================== */
/* Value profile configuration                                                  */
/* ========================================================================== */

/**
 * Frequency threshold for a value to be considered "stable" for speculation.
 * When top_frequency >= this threshold, the JIT can emit a value guard.
 *
 * Set to 0.95 (95%) — this means a value must appear at least 95% of the
 * time to be worth speculating on. This is conservative enough to avoid
 * excessive deoptimization while still capturing most stable values.
 */
#define VTX_VALUE_STABILITY_THRESHOLD 0.95

/**
 * Minimum number of observations before a value can be speculated on.
 * Prevents premature speculation based on too few samples.
 */
#define VTX_VALUE_MIN_OBSERVATIONS 100

/**
 * Maximum number of distinct values tracked per profile site.
 * We track the top 2 values plus an "other" bucket.
 */
#define VTX_VALUE_PROFILE_SLOTS 2

/**
 * Default sampling interval for value profiling.
 * Only observe every Nth execution to reduce overhead.
 */
#define VTX_VALUE_SAMPLE_INTERVAL_DEFAULT 64

/**
 * Sampling interval for very stable value sites.
 * These sites almost never change, so we sample less frequently.
 */
#define VTX_VALUE_SAMPLE_INTERVAL_STABLE 256

/**
 * Sampling interval for unstable value sites.
 * These sites need more frequent observation to converge.
 */
#define VTX_VALUE_SAMPLE_INTERVAL_UNSTABLE 16

/* ========================================================================== */
/* Value profile entry                                                          */
/* ========================================================================== */

/**
 * A single observed value with its frequency.
 *
 * The value is stored as a raw uint64_t. For pointer values, this is
 * the pointer itself. For integer values, it's the integer. For floating-
 * point values, it's the bit representation (use memcpy to convert).
 */
typedef struct {
    uint64_t value;         /* the observed value */
    uint64_t count;         /* how many times this value was observed */
} vtx_value_entry_t;

/* ========================================================================== */
/* Value profile                                                                */
/* ========================================================================== */

/**
 * Value profile for a single program point.
 *
 * Tracks the top 2 most frequently observed values and their counts.
 * An "other" bucket catches values that don't match either of the top 2.
 *
 * The profile uses sampling to reduce overhead: only every Nth execution
 * updates the profile. The sampling interval adapts based on stability.
 */
typedef struct {
    /* Top-N value entries, sorted by count (descending) */
    vtx_value_entry_t entries[VTX_VALUE_PROFILE_SLOTS];

    /* "Other" bucket: count of observations that didn't match
     * either of the top 2 values */
    uint64_t other_count;

    /* Total number of sampled observations */
    uint64_t total_observations;

    /* Sampling state */
    uint32_t sample_counter;      /* countdown to next observation */
    uint32_t sample_interval;     /* reset value for sample_counter */

    /* Stability: true if top value frequency >= VTX_VALUE_STABILITY_THRESHOLD
     * and total_observations >= VTX_VALUE_MIN_OBSERVATIONS */
    bool     is_stable;

    /* The speculated constant value (valid only when is_stable is true) */
    uint64_t speculated_value;
} vtx_value_profile_t;

/* ========================================================================== */
/* Lifecycle                                                                    */
/* ========================================================================== */

/**
 * Initialize a value profile.
 */
void vtx_value_profile_init(vtx_value_profile_t *profile);

/**
 * Reset a value profile to its initial state.
 */
void vtx_value_profile_reset(vtx_value_profile_t *profile);

/* ========================================================================== */
/* Observation (hot path)                                                       */
/* ========================================================================== */

/**
 * Observe a value at this profile site (sampling-based).
 *
 * This is the hot-path function. It decrements the sample counter
 * and only records an observation when the counter reaches zero.
 * The branch on counter == 0 is highly predictable (taken with
 * probability 1/interval).
 *
 * When an observation is recorded:
 *   1. If the value matches entry[0] or entry[1], increment that count
 *   2. Otherwise, increment other_count
 *   3. Re-sort entries so the most frequent is entry[0]
 *   4. Update stability flag
 *   5. Adapt sampling interval
 *
 * @param profile  Value profile to update
 * @param value    The observed value (raw uint64_t representation)
 */
void vtx_value_profile_observe(vtx_value_profile_t *profile, uint64_t value);

/**
 * Inline hot-path version of observe.
 * Only records on sample boundary; caller should use this from JIT code.
 */
static inline void vtx_value_profile_observe_hot(vtx_value_profile_t *profile,
                                                   uint64_t value)
{
    if (profile == NULL) return;

    /* Decrement counter — highly predictable branch */
    if (__builtin_expect(profile->sample_counter > 1, 1)) {
        profile->sample_counter--;
        return;
    }

    /* Sample boundary — call the slow path */
    vtx_value_profile_observe(profile, value);
}

/* ========================================================================== */
/* Query                                                                        */
/* ========================================================================== */

/**
 * Get the top observed value and its frequency.
 *
 * @param profile    Value profile
 * @param out_value  Output: the most frequently observed value
 * @param out_freq   Output: frequency of the top value (0.0-1.0)
 * @return           true if any value has been observed, false otherwise
 */
bool vtx_value_profile_top(const vtx_value_profile_t *profile,
                             uint64_t *out_value, double *out_freq);

/**
 * Get the second most observed value and its frequency.
 *
 * @param profile    Value profile
 * @param out_value  Output: the second most frequently observed value
 * @param out_freq   Output: frequency of the second value (0.0-1.0)
 * @return           true if a second value has been observed, false otherwise
 */
bool vtx_value_profile_second(const vtx_value_profile_t *profile,
                                uint64_t *out_value, double *out_freq);

/**
 * Check if the profile site is stable enough for value speculation.
 * Returns true if:
 *   - total_observations >= VTX_VALUE_MIN_OBSERVATIONS
 *   - top value frequency >= VTX_VALUE_STABILITY_THRESHOLD
 */
bool vtx_value_profile_is_stable(const vtx_value_profile_t *profile);

/**
 * Get the speculated constant value.
 * Only valid when vtx_value_profile_is_stable() returns true.
 */
uint64_t vtx_value_profile_speculated_value(const vtx_value_profile_t *profile);

/* ========================================================================== */
/* Value profile table                                                          */
/* ========================================================================== */

#define VTX_VALUE_PROFILE_TABLE_INITIAL_CAPACITY 64

/**
 * Global table of value profiles, indexed by bytecode PC.
 * Each entry corresponds to a field load, parameter, or return value
 * that the profiler is tracking.
 */
typedef struct {
    vtx_value_profile_t *profiles;     /* array of value profiles */
    uint32_t            *bytecode_pcs;  /* bytecode PC for each profile */
    uint32_t             count;         /* number of active profiles */
    uint32_t             capacity;      /* allocated capacity */
} vtx_value_profile_table_t;

/**
 * Initialize a value profile table.
 */
int vtx_value_profile_table_init(vtx_value_profile_table_t *table);

/**
 * Destroy a value profile table and free memory.
 */
void vtx_value_profile_table_destroy(vtx_value_profile_table_t *table);

/**
 * Get or create a value profile for a bytecode PC.
 * Returns the profile, or NULL on failure.
 */
vtx_value_profile_t *vtx_value_profile_get_or_create(
    vtx_value_profile_table_t *table, uint32_t bytecode_pc);

/**
 * Look up an existing value profile by bytecode PC.
 * Returns NULL if not found.
 */
vtx_value_profile_t *vtx_value_profile_lookup(
    const vtx_value_profile_table_t *table, uint32_t bytecode_pc);

/**
 * Get the number of stable profile sites (candidates for value speculation).
 */
uint32_t vtx_value_profile_stable_count(const vtx_value_profile_table_t *table);

#endif /* VORTEX_GUARD_VALUE_PROFILE_H */
