#ifndef VORTEX_GUARD_HEAP_VALUE_PROP_H
#define VORTEX_GUARD_HEAP_VALUE_PROP_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"

/**
 * VORTEX Speculative Value Propagation Through the Heap (Level 2A)
 *
 * This is the single biggest optimization VORTEX doesn't have.
 *
 * Current VORTEX: constant propagation works on local values. If obj.x = 42,
 * the compiler doesn't know that a later load obj.x returns 42 — it might have
 * been mutated.
 *
 * Aggressive version: Profile the ACTUAL VALUES stored in fields. If obj.x
 * has been 42 in 99.7% of observations, SPECULATE that it's always 42:
 *
 *   // Source code:
 *   int sum = obj->x + obj->y;  // Two memory loads
 *
 *   // After speculative value propagation:
 *   // Guard: obj->x == 42 && obj->y == 7 (observed values)
 *   int sum = 42 + 7;           // CONSTANT FOLD → sum = 49
 *   // ZERO memory loads. ZERO cache misses.
 *
 * This is exponentially powerful when chained:
 *   // Before: 4 memory loads, 2 branches, 1 call
 *   if (config->mode == PRODUCTION) {          // load + branch
 *       result = cache->compute(input->data);  // load + load + call
 *   }
 *
 *   // After speculative value propagation:
 *   // Guard: config->mode == PRODUCTION, cache->algo == FAST, input->type == INT
 *   // All guards are trap-based (zero cost on hot path)
 *   result = fast_int_compute(input->data);  // Inlined, devirtualized, constant-folded
 *   // 1 load, 0 branches, 1 inlined call
 *
 * Architecture:
 *   - Each (type_id, field_offset) pair has its own value profile with 4 slots
 *   - This extends value_profile's 2-slot design to 4 for better coverage
 *   - Heap value profiles are keyed by (type_id, field_offset) because the
 *     same field on different types can have very different distributions
 *   - A field_id is computed as (type_id << 32 | field_offset) for compact storage
 *   - Sampling reduces hot-path overhead to ~1 cycle per observation
 *
 * Observation flow:
 *   1. Interpreter/profiler calls vtx_heap_value_prop_observe_hot() at each LoadField
 *   2. Hot path: decrement counter, return (highly predictable branch)
 *   3. On sample boundary: slow path updates the top-4 values and frequencies
 *   4. After each slow-path update, check stability (top value freq > 95%)
 *   5. If stable, mark as speculation candidate and record the speculated value
 *
 * Compilation flow:
 *   1. Compiler queries vtx_heap_value_prop_is_stable() for each LoadField
 *   2. If stable, replace LoadField node with a constant (the speculated value)
 *   3. Emit a value guard (trap-based) that verifies the speculated value
 *   4. Register the guard via vtx_heap_value_prop_install_guard()
 *   5. Guard dependency graph tracks the relationship (guard → value site)
 *   6. On deopt, guard_deps records the value site for re-materialization
 *
 * Thread safety: NOT thread-safe. The caller must synchronize, same as
 * existing guard metadata (vtx_guard_meta_table_t) and value profiles
 * (vtx_value_profile_table_t).
 */

/* ========================================================================== */
/* Heap value propagation configuration                                         */
/* ========================================================================== */

/**
 * Maximum number of top values tracked per heap field.
 * Extends value_profile's 2-slot design to 4 for better coverage.
 *
 * 4 slots captures the vast majority of stable value distributions:
 *   - Monomorphic (1 dominant value): 1 slot needed
 *   - Bimodal (2 common values): 2 slots needed
 *   - Trimodal (3 values, one dominant): 3 slots needed
 *   - Quadrimodal (rare but possible for configuration fields): 4 slots
 *
 * Beyond 4 values, a field is unlikely to have a single dominant value
 * worth speculating on, so we use an "other" bucket instead.
 */
#define VTX_HEAP_VALUE_SLOTS 4

/**
 * Frequency threshold for a heap field value to be considered "stable"
 * for speculation. When the top value's frequency exceeds this threshold,
 * the JIT can emit a value guard that speculates the field always holds
 * this constant.
 *
 * Set to 0.95 (95%) — same as VTX_VALUE_STABILITY_THRESHOLD. This means
 * a value must appear at least 95% of the time to be worth speculating on.
 * At 95%, the expected deopt rate is 1 in 20 field loads, which is
 * acceptable for most workloads.
 *
 * For very hot code paths where deopts are expensive, the guard strength
 * system (FastCheck → FullCheck → DeoptAlways) will adaptively weaken
 * the guard if the speculation proves unreliable.
 */
#define VTX_HEAP_VALUE_STABILITY_THRESHOLD 0.95

/**
 * Minimum number of observations before a heap field value can be
 * speculated on. Prevents premature speculation based on too few samples.
 *
 * Set to 200 (higher than value_profile's 100) because heap fields are
 * observed less frequently than local values (only on LoadField, not every
 * assignment). The higher threshold reduces the risk of speculating on
 * transient values that haven't converged yet.
 */
#define VTX_HEAP_VALUE_MIN_OBSERVATIONS 200

/**
 * Default sampling interval for heap value profiling.
 * Only observe every Nth LoadField execution to reduce overhead.
 *
 * Set to 64 — same as VTX_VALUE_SAMPLE_INTERVAL_DEFAULT. This means
 * only ~1.5% of field loads update the profile, reducing overhead
 * from ~5 cycles to ~1 cycle per load.
 */
#define VTX_HEAP_VALUE_SAMPLE_INTERVAL 64

/**
 * Demotion ratio for replacing the least frequent top value slot.
 * When the "other" bucket count exceeds demote_ratio times the count
 * of the least frequent slot, the least frequent slot is replaced
 * with the current observed value.
 *
 * Set to 2.0 — same as value_profile's demotion heuristic. This means
 * a rising new value needs to appear in at least half the "other"
 * observations to displace an existing slot, preventing thrashing
 * from random noise.
 */
#define VTX_HEAP_VALUE_DEMOTE_RATIO 2.0

/**
 * Initial capacity for the heap value profile array.
 * Most methods have < 32 distinct field access sites.
 */
#define VTX_HEAP_VALUE_PROP_INITIAL_CAPACITY 32

/**
 * Invalid guard ID sentinel for heap value profiles.
 * Matches the convention in guard_deps.h (VTX_GUARD_DEPS_ID_INVALID).
 */
#define VTX_HEAP_VALUE_GUARD_ID_INVALID UINT32_MAX

/* ========================================================================== */
/* Heap value profile entry                                                     */
/* ========================================================================== */

/**
 * A heap value profile: tracks the top-4 observed values at a
 * specific (type_id, field_offset) pair.
 *
 * Unlike local value_profile which tracks by bytecode_pc, heap value
 * profiles are keyed by (type_id, field_offset) because the same
 * field on different types can have very different value distributions.
 * For example, Person.age might be ~30 while Config.age might be ~5.
 *
 * The top_values array is always sorted by frequency (descending):
 *   top_values[0] = most frequent value
 *   top_values[3] = least frequent of the top 4
 *
 * Slot management:
 *   - If an observed value matches an existing slot, that slot's count
 *     is incremented (and top_frequencies is recalculated)
 *   - If no match and an empty slot exists, the value is inserted there
 *   - If all slots are full, the "other_count" bucket is incremented
 *   - Demotion: if other_count > demote_ratio * lowest slot count,
 *     the lowest slot is replaced with the new value
 */
typedef struct {
    /* Field identification — the (type_id, field_offset) pair this
     * profile tracks. Stored redundantly for fast access without
     * having to decode field_id on every query. */
    uint64_t field_id;            /* (type_id, field_offset) encoded as single uint64 */
    uint32_t type_id;             /* type of the object containing this field */
    uint32_t field_offset;        /* offset of the field within the object */

    /* Top-4 value tracking.
     * top_values[i] is the i-th most frequently observed value.
     * top_frequencies[i] is the frequency (0.0–1.0) of that value.
     * The counts are stored implicitly: count[i] = top_frequencies[i] * total_observations.
     * We also track raw counts for the demotion heuristic. */
    uint64_t top_values[VTX_HEAP_VALUE_SLOTS];       /* top 4 observed values */
    double   top_frequencies[VTX_HEAP_VALUE_SLOTS];  /* frequency of each (0.0–1.0) */
    uint64_t top_counts[VTX_HEAP_VALUE_SLOTS];        /* raw count for each slot */

    /* "Other" bucket: count of observations that didn't match any
     * of the top 4 values. Used for the demotion heuristic and
     * for computing the true frequency of the top values. */
    uint64_t other_count;

    /* Total number of sampled observations (including other_count
     * and all slot counts). Used to compute frequencies. */
    uint64_t total_observations;

    /* Sampling state — same pattern as value_profile and guard metadata.
     * The sample_counter decrements on each hot-path observation.
     * When it reaches zero, the full slow-path update runs and
     * the counter is reset to sample_interval. */
    uint32_t sample_counter;
    uint32_t sample_interval;

    /* Stability: true if the top value's frequency exceeds
     * VTX_HEAP_VALUE_STABILITY_THRESHOLD and total_observations
     * >= VTX_HEAP_VALUE_MIN_OBSERVATIONS.
     *
     * When is_stable is true, speculated_value is the constant that
     * the JIT can replace the field load with, and a value guard
     * will be emitted to verify this assumption at runtime. */
    bool     is_stable;

    /* The speculated constant value (valid only when is_stable is true).
     * This is the value that LoadField nodes will be replaced with. */
    uint64_t speculated_value;

    /* Frequency of the top value (0.0–1.0). Cached for fast access
     * without having to recompute from top_counts / total_observations.
     * This is the same as top_frequencies[0] but stored at the struct
     * level for ergonomic access in the compiler. */
    double   stability;

    /* Guard tracking: which guard protects this speculation.
     * When the JIT emits a value guard for this field, the guard_id
     * is recorded here so that on deopt, the runtime can find and
     * invalidate all dependent value sites.
     *
     * guard_id = VTX_HEAP_VALUE_GUARD_ID_INVALID if no guard installed. */
    uint32_t guard_id;
    bool     guard_installed;     /* whether a guard has been emitted */
} vtx_heap_value_profile_t;

/* ========================================================================== */
/* Heap value propagation result                                                */
/* ========================================================================== */

/**
 * Heap value propagation result: contains all stable heap value
 * speculations and the information needed to emit guards.
 *
 * This is the primary data structure used by the compiler to:
 *   1. Replace LoadField nodes with constants when the field's value
 *      is speculated to be stable
 *   2. Emit value guards (trap-based) that verify the speculated value
 *   3. Enable constant folding, dead code elimination, and branch
 *      elimination based on the speculated values
 *
 * The profiles array is dynamically grown as new (type_id, field_offset)
 * pairs are observed. The field_index provides a sparse lookup table
 * for fast access during compilation (avoids linear scan of profiles).
 */
typedef struct {
    /* Array of heap value profiles, one per (type_id, field_offset) pair.
     * Grown dynamically as new field access patterns are observed. */
    vtx_heap_value_profile_t *profiles;   /* array of heap value profiles */
    uint32_t                  profile_count;
    uint32_t                  profile_capacity;

    /* Sparse index: field_id → profile index.
     * For fast lookup during compilation. field_id is computed by
     * vtx_heap_value_field_id(type_id, field_offset), and the index
     * is: field_index[field_id % field_index_capacity].
     * Open-addressing with linear probing.
     * Value UINT32_MAX means "empty slot". */
    uint32_t *field_index;
    uint32_t  field_index_capacity;

    /* Statistics for profiling and debugging */
    uint32_t total_fields_profiled;       /* total distinct (type, offset) pairs observed */
    uint32_t stable_field_count;          /* fields with is_stable=true */
    uint32_t speculated_field_count;      /* fields where speculation was applied by compiler */
    uint64_t estimated_loads_eliminated;  /* estimated loads replaced with constants */
} vtx_heap_value_prop_result_t;

/* ========================================================================== */
/* Field ID computation                                                         */
/* ========================================================================== */

/**
 * Compute a field_id from (type_id, field_offset).
 *
 * The field_id is a unique 64-bit identifier for a (type, field) pair,
 * computed as: (uint64_t)type_id << 32 | field_offset
 *
 * This encoding is valid because:
 *   - type_id is a 32-bit value (typically < 65536)
 *   - field_offset is a 32-bit value (typically < 4096 for object fields)
 *   - The combination fits in 64 bits without collision
 *
 * Defined early (before observe_hot) because it's used by the inline
 * hot-path observation function.
 *
 * @param type_id       Type ID of the object
 * @param field_offset  Offset of the field within the object
 * @return              64-bit field ID
 */
static inline uint64_t vtx_heap_value_field_id(uint32_t type_id,
                                                 uint32_t field_offset)
{
    return ((uint64_t)type_id << 32) | (uint64_t)field_offset;
}

/* ========================================================================== */
/* Lifecycle                                                                    */
/* ========================================================================== */

/**
 * Initialize a heap value propagation result.
 * Returns 0 on success, -1 on failure (allocation error).
 */
int vtx_heap_value_prop_init(vtx_heap_value_prop_result_t *result);

/**
 * Destroy a heap value propagation result and free all memory.
 */
void vtx_heap_value_prop_destroy(vtx_heap_value_prop_result_t *result);

/* ========================================================================== */
/* Observation (hot path)                                                       */
/* ========================================================================== */

/**
 * Observe a value at a heap field load (slow path — called on sample boundary).
 *
 * This is the slow-path function. It performs the full update:
 *   1. Look up or create the profile for (type_id, field_offset)
 *   2. Match the observed value against existing top-4 slots
 *   3. If no match, try an empty slot or increment other_count
 *   4. Apply demotion heuristic if other_count is too large
 *   5. Re-sort slots by count (descending)
 *   6. Update stability flag and speculated value
 *   7. Adapt sampling interval
 *   8. Update the field_index for fast lookup
 *
 * @param result        Heap value propagation result
 * @param type_id       Type ID of the object containing the field
 * @param field_offset  Offset of the field within the object
 * @param value         The observed value (raw uint64_t representation)
 */
void vtx_heap_value_prop_observe(vtx_heap_value_prop_result_t *result,
                                   uint32_t type_id,
                                   uint32_t field_offset,
                                   uint64_t value);

/**
 * Inline hot-path version of observe (sampling-based).
 *
 * Only records on sample boundary; caller should use this from
 * JIT code and interpreter hot loops. The branch on counter == 0
 * is highly predictable (taken with probability 1/interval).
 *
 * Hot-path cost: ~1 cycle (decrement + predicted branch) when the
 * counter hasn't reached zero.
 *
 * @param result        Heap value propagation result (may be NULL)
 * @param type_id       Type ID of the object containing the field
 * @param field_offset  Offset of the field within the object
 * @param value         The observed value (raw uint64_t representation)
 */
static inline void vtx_heap_value_prop_observe_hot(
    vtx_heap_value_prop_result_t *result,
    uint32_t type_id,
    uint32_t field_offset,
    uint64_t value)
{
    if (result == NULL) return;

    /* Decrement counter — highly predictable branch.
     * The counter hits zero with probability 1/interval, so the
     * branch to the slow path is almost never taken. */
    if (__builtin_expect(result->profiles != NULL, 1)) {
        /* Fast lookup: compute field_id and find the profile.
         * We need to find the profile to decrement its sample_counter.
         * For the hot path, we do a quick index lookup. */
        uint64_t fid = vtx_heap_value_field_id(type_id, field_offset);
        /* Hash into the sparse index */
        uint32_t idx = (uint32_t)(fid % result->field_index_capacity);
        uint32_t prof_idx = result->field_index[idx];

        /* Linear probe (very short — typically 1-2 steps) */
        uint32_t cap = result->field_index_capacity;
        while (prof_idx != UINT32_MAX && prof_idx < result->profile_count) {
            if (result->profiles[prof_idx].field_id == fid) {
                /* Found the profile — check its sample counter */
                vtx_heap_value_profile_t *prof = &result->profiles[prof_idx];
                if (__builtin_expect(prof->sample_counter > 1, 1)) {
                    prof->sample_counter--;
                    return;
                }
                /* Sample boundary — call the slow path */
                vtx_heap_value_prop_observe(result, type_id, field_offset, value);
                return;
            }
            idx = (idx + 1) % cap;
            prof_idx = result->field_index[idx];
        }
    }

    /* Profile not yet created (first observation or index miss) —
     * fall through to slow path which will create the profile. */
    vtx_heap_value_prop_observe(result, type_id, field_offset, value);
}

/* ========================================================================== */
/* Query                                                                        */
/* ========================================================================== */

/**
 * Check if a heap field's value is speculated to be stable.
 *
 * Returns true if:
 *   - A profile exists for (type_id, field_offset)
 *   - total_observations >= VTX_HEAP_VALUE_MIN_OBSERVATIONS
 *   - top value frequency >= VTX_HEAP_VALUE_STABILITY_THRESHOLD
 *
 * @param result        Heap value propagation result
 * @param type_id       Type ID of the object containing the field
 * @param field_offset  Offset of the field within the object
 * @return              true if the field's value is stable enough to speculate on
 */
bool vtx_heap_value_prop_is_stable(const vtx_heap_value_prop_result_t *result,
                                     uint32_t type_id,
                                     uint32_t field_offset);

/**
 * Get the speculated constant value for a heap field.
 *
 * Only valid when vtx_heap_value_prop_is_stable() returns true.
 * Returns 0 if the field is not stable or doesn't have a profile.
 *
 * @param result        Heap value propagation result
 * @param type_id       Type ID of the object containing the field
 * @param field_offset  Offset of the field within the object
 * @return              The speculated constant value
 */
uint64_t vtx_heap_value_prop_speculated_value(
    const vtx_heap_value_prop_result_t *result,
    uint32_t type_id,
    uint32_t field_offset);

/**
 * Get the full profile for a heap field.
 *
 * Returns NULL if no profile exists for (type_id, field_offset).
 * The returned pointer is valid until the result is destroyed or
 * the next call to vtx_heap_value_prop_observe() that triggers
 * a reallocation (i.e., profile array growth).
 *
 * @param result        Heap value propagation result
 * @param type_id       Type ID of the object containing the field
 * @param field_offset  Offset of the field within the object
 * @return              Pointer to the heap value profile, or NULL
 */
const vtx_heap_value_profile_t *vtx_heap_value_prop_lookup(
    const vtx_heap_value_prop_result_t *result,
    uint32_t type_id,
    uint32_t field_offset);

/* ========================================================================== */
/* Guard management                                                             */
/* ========================================================================== */

/**
 * Install a guard for a heap value speculation.
 *
 * Called by the compiler after emitting a value guard for this field.
 * Records the guard_id so that on deoptimization, the runtime can
 * find all value sites that depend on this guard.
 *
 * @param result        Heap value propagation result
 * @param type_id       Type ID of the object containing the field
 * @param field_offset  Offset of the field within the object
 * @param guard_id      Guard metadata ID for the emitted value guard
 * @return              0 on success, -1 on failure (no profile found)
 */
int vtx_heap_value_prop_install_guard(vtx_heap_value_prop_result_t *result,
                                        uint32_t type_id,
                                        uint32_t field_offset,
                                        uint32_t guard_id);

/**
 * Remove a guard association from a heap value profile.
 *
 * Called when a guard is invalidated (deoptimization). Clears the
 * guard_id and guard_installed fields so the profile can be reused
 * in a future recompilation.
 *
 * @param result        Heap value propagation result
 * @param type_id       Type ID of the object containing the field
 * @param field_offset  Offset of the field within the object
 */
void vtx_heap_value_prop_remove_guard(vtx_heap_value_prop_result_t *result,
                                        uint32_t type_id,
                                        uint32_t field_offset);

/* ========================================================================== */
/* Statistics                                                                   */
/* ========================================================================== */

/**
 * Get the number of stable heap field profiles (candidates for speculation).
 */
uint32_t vtx_heap_value_prop_stable_count(const vtx_heap_value_prop_result_t *result);

/**
 * Get the number of heap field profiles that have guards installed.
 */
uint32_t vtx_heap_value_prop_guard_count(const vtx_heap_value_prop_result_t *result);

/**
 * Get the estimated number of loads eliminated by speculation.
 * This is the sum of speculated_field_count multiplied by the average
 * number of times each field is loaded per execution (estimated from
 * the total_observations count).
 */
uint64_t vtx_heap_value_prop_loads_eliminated(const vtx_heap_value_prop_result_t *result);

#endif /* VORTEX_GUARD_HEAP_VALUE_PROP_H */
