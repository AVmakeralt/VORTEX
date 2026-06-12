/**
 * VORTEX Speculative Value Propagation Through the Heap — Implementation
 *
 * Tracks the top-4 most frequently observed values at each heap field
 * access site, keyed by (type_id, field_offset). Uses sampling to reduce
 * hot-path overhead to ~1 cycle per observation.
 *
 * This is the core data-flow optimization that enables:
 *   - Replacing memory loads with speculated constants
 *   - Constant folding of expressions involving heap fields
 *   - Branch elimination when heap field values determine conditions
 *   - Devirtualization when vtable/function pointer fields are stable
 *   - Inlining of methods whose targets are determined by stable fields
 */

#include "guard/heap_value_prop.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Internal: initialize a single heap value profile                             */
/* ========================================================================== */

static void heap_value_profile_init(vtx_heap_value_profile_t *profile,
                                      uint32_t type_id,
                                      uint32_t field_offset)
{
    memset(profile, 0, sizeof(*profile));

    profile->type_id       = type_id;
    profile->field_offset  = field_offset;
    profile->field_id      = vtx_heap_value_field_id(type_id, field_offset);

    /* Initialize all top value slots to "empty" (count = 0) */
    for (int i = 0; i < VTX_HEAP_VALUE_SLOTS; i++) {
        profile->top_values[i]     = 0;
        profile->top_frequencies[i] = 0.0;
        profile->top_counts[i]     = 0;
    }

    profile->other_count       = 0;
    profile->total_observations = 0;

    /* Sampling: start with the default interval */
    profile->sample_counter = VTX_HEAP_VALUE_SAMPLE_INTERVAL;
    profile->sample_interval = VTX_HEAP_VALUE_SAMPLE_INTERVAL;

    profile->is_stable        = false;
    profile->speculated_value = 0;
    profile->stability        = 0.0;

    profile->guard_id         = VTX_HEAP_VALUE_GUARD_ID_INVALID;
    profile->guard_installed  = false;
}

/* ========================================================================== */
/* Internal: sort profile slots by count (descending)                          */
/* ========================================================================== */

/**
 * Sort the top_values/top_counts/top_frequencies arrays so that
 * slot 0 has the highest count and slot [SLOTS-1] has the lowest.
 *
 * Uses insertion sort — with only 4 elements, this is both simple
 * and fast (4 comparisons worst case, 0 comparisons best case).
 */
static void sort_profile_slots(vtx_heap_value_profile_t *profile)
{
    for (int i = 1; i < VTX_HEAP_VALUE_SLOTS; i++) {
        if (profile->top_counts[i] > profile->top_counts[i - 1]) {
            /* Swap slot i with slot i-1, then bubble up */
            int j = i;
            while (j > 0 && profile->top_counts[j] > profile->top_counts[j - 1]) {
                /* Swap values */
                uint64_t tmp_val = profile->top_values[j];
                profile->top_values[j] = profile->top_values[j - 1];
                profile->top_values[j - 1] = tmp_val;

                /* Swap counts */
                uint64_t tmp_cnt = profile->top_counts[j];
                profile->top_counts[j] = profile->top_counts[j - 1];
                profile->top_counts[j - 1] = tmp_cnt;

                /* Swap frequencies */
                double tmp_freq = profile->top_frequencies[j];
                profile->top_frequencies[j] = profile->top_frequencies[j - 1];
                profile->top_frequencies[j - 1] = tmp_freq;

                j--;
            }
        }
    }
}

/* ========================================================================== */
/* Internal: update stability flag                                              */
/* ========================================================================== */

/**
 * Check if the top value's frequency exceeds the stability threshold.
 * If so, mark the profile as stable and record the speculated value.
 *
 * A field is considered stable when:
 *   1. total_observations >= VTX_HEAP_VALUE_MIN_OBSERVATIONS (200)
 *   2. top value frequency >= VTX_HEAP_VALUE_STABILITY_THRESHOLD (95%)
 *
 * When the field becomes stable, the speculated_value is set to the
 * top value. This is the constant that LoadField nodes will be
 * replaced with during compilation.
 *
 * When the field becomes unstable (frequency drops below threshold),
 * is_stable is cleared. However, if a guard is already installed,
 * the guard dependency system will handle invalidation — we don't
 * need to do anything special here.
 */
static void update_stability(vtx_heap_value_profile_t *profile)
{
    if (profile->total_observations < VTX_HEAP_VALUE_MIN_OBSERVATIONS) {
        profile->is_stable = false;
        profile->stability = 0.0;
        return;
    }

    if (profile->top_counts[0] == 0) {
        profile->is_stable = false;
        profile->stability = 0.0;
        return;
    }

    double top_freq = (double)profile->top_counts[0] /
                      (double)profile->total_observations;
    profile->stability = top_freq;

    if (top_freq >= VTX_HEAP_VALUE_STABILITY_THRESHOLD) {
        profile->is_stable = true;
        profile->speculated_value = profile->top_values[0];
    } else {
        profile->is_stable = false;
    }
}

/* ========================================================================== */
/* Internal: adapt sampling interval                                            */
/* ========================================================================== */

/**
 * Choose an adaptive sampling interval based on the profile's stability.
 *
 * Stable profiles (high top value frequency) get longer intervals to
 * reduce overhead — the value distribution is unlikely to change.
 * Unstable profiles get shorter intervals for faster convergence
 * and more responsive tracking of value distribution shifts.
 *
 * Intervals (same pattern as value_profile and guard metadata):
 *   - Very stable (freq > 0.99): 256 — sample rarely
 *   - Moderately stable (freq >= 0.95): 64 — default interval
 *   - Unstable (freq < 0.95): 16 — sample frequently
 */
static void adapt_interval(vtx_heap_value_profile_t *profile)
{
    if (profile->total_observations < VTX_HEAP_VALUE_MIN_OBSERVATIONS) {
        /* Not enough data yet — use shorter interval for faster convergence */
        profile->sample_interval = 16;
        return;
    }

    if (profile->top_counts[0] == 0) {
        profile->sample_interval = 16;
        return;
    }

    double top_freq = (double)profile->top_counts[0] /
                      (double)profile->total_observations;

    if (top_freq > 0.99) {
        /* Very stable: sample less frequently */
        profile->sample_interval = 256;
    } else if (top_freq >= 0.95) {
        /* Moderately stable: default interval */
        profile->sample_interval = VTX_HEAP_VALUE_SAMPLE_INTERVAL;
    } else if (top_freq >= 0.80) {
        /* Somewhat unstable: shorter interval */
        profile->sample_interval = 32;
    } else {
        /* Very unstable: shortest interval for fastest convergence */
        profile->sample_interval = 16;
    }
}

/* ========================================================================== */
/* Internal: update frequency cache                                             */
/* ========================================================================== */

/**
 * Recompute top_frequencies from top_counts and total_observations.
 * Called after any mutation to the counts (observe, demote).
 */
static void update_frequencies(vtx_heap_value_profile_t *profile)
{
    if (profile->total_observations == 0) {
        for (int i = 0; i < VTX_HEAP_VALUE_SLOTS; i++) {
            profile->top_frequencies[i] = 0.0;
        }
        return;
    }

    double total = (double)profile->total_observations;
    for (int i = 0; i < VTX_HEAP_VALUE_SLOTS; i++) {
        profile->top_frequencies[i] = (double)profile->top_counts[i] / total;
    }
}

/* ========================================================================== */
/* Internal: grow the field_index (sparse hash table)                           */
/* ========================================================================== */

/**
 * Grow and rehash the sparse field_index.
 *
 * The field_index is a simple open-addressing hash table that maps
 * field_id → profile array index. When the load factor exceeds ~50%,
 * we double the capacity and rehash all entries.
 *
 * We use a load factor of 50% because the field_index is on the
 * critical path of the hot observe function. Lower load factors
 * mean shorter probe sequences (typically 1 step).
 */
static int field_index_grow(vtx_heap_value_prop_result_t *result)
{
    uint32_t new_cap = result->field_index_capacity * 2;
    uint32_t *new_index = (uint32_t *)malloc(new_cap * sizeof(uint32_t));
    if (new_index == NULL) return -1;

    /* Initialize all slots to "empty" */
    for (uint32_t i = 0; i < new_cap; i++) {
        new_index[i] = UINT32_MAX;
    }

    /* Rehash all existing entries */
    for (uint32_t i = 0; i < result->profile_count; i++) {
        uint64_t fid = result->profiles[i].field_id;
        uint32_t slot = (uint32_t)(fid % new_cap);

        /* Linear probe for an empty slot */
        while (new_index[slot] != UINT32_MAX) {
            slot = (slot + 1) % new_cap;
        }
        new_index[slot] = i;
    }

    free(result->field_index);
    result->field_index = new_index;
    result->field_index_capacity = new_cap;
    return 0;
}

/* ========================================================================== */
/* Internal: insert a profile index into the field_index                        */
/* ========================================================================== */

/**
 * Insert a mapping from field_id to profile_index in the sparse index.
 * Grows the index if the load factor exceeds 50%.
 */
static int field_index_insert(vtx_heap_value_prop_result_t *result,
                                uint64_t field_id,
                                uint32_t profile_index)
{
    /* Check load factor — grow if > 50% */
    if (result->profile_count * 2 > result->field_index_capacity) {
        if (field_index_grow(result) != 0) return -1;
    }

    uint32_t slot = (uint32_t)(field_id % result->field_index_capacity);

    /* Linear probe for an empty slot */
    uint32_t cap = result->field_index_capacity;
    while (result->field_index[slot] != UINT32_MAX) {
        slot = (slot + 1) % cap;
    }
    result->field_index[slot] = profile_index;
    return 0;
}

/* ========================================================================== */
/* Internal: look up a profile by field_id via the sparse index                 */
/* ========================================================================== */

/**
 * Look up a profile index by field_id using the sparse hash table.
 * Returns the profile array index, or UINT32_MAX if not found.
 */
static uint32_t field_index_lookup(const vtx_heap_value_prop_result_t *result,
                                     uint64_t field_id)
{
    if (result->field_index == NULL || result->field_index_capacity == 0) {
        return UINT32_MAX;
    }

    uint32_t slot = (uint32_t)(field_id % result->field_index_capacity);
    uint32_t cap = result->field_index_capacity;

    /* Linear probe */
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t idx = result->field_index[slot];
        if (idx == UINT32_MAX) {
            /* Empty slot — not found */
            return UINT32_MAX;
        }
        if (result->profiles[idx].field_id == field_id) {
            return idx;
        }
        slot = (slot + 1) % cap;
    }

    return UINT32_MAX;
}

/* ========================================================================== */
/* Internal: look up or create a profile                                        */
/* ========================================================================== */

/**
 * Find the profile for (type_id, field_offset), or create one if it
 * doesn't exist. Returns the profile, or NULL on allocation failure.
 */
static vtx_heap_value_profile_t *lookup_or_create_profile(
    vtx_heap_value_prop_result_t *result,
    uint32_t type_id,
    uint32_t field_offset)
{
    uint64_t fid = vtx_heap_value_field_id(type_id, field_offset);

    /* Try the sparse index first (O(1) amortized) */
    uint32_t idx = field_index_lookup(result, fid);
    if (idx != UINT32_MAX) {
        return &result->profiles[idx];
    }

    /* Not found — fall back to linear scan in case the index
     * hasn't been populated yet (e.g., after init). */
    for (uint32_t i = 0; i < result->profile_count; i++) {
        if (result->profiles[i].field_id == fid) {
            /* Found but not in index — insert into index for next time */
            field_index_insert(result, fid, i);
            return &result->profiles[i];
        }
    }

    /* Need to create a new profile — grow the array if needed */
    if (result->profile_count >= result->profile_capacity) {
        uint32_t new_cap = result->profile_capacity * 2;
        vtx_heap_value_profile_t *new_profiles =
            (vtx_heap_value_profile_t *)realloc(
                result->profiles,
                new_cap * sizeof(vtx_heap_value_profile_t));
        if (new_profiles == NULL) return NULL;

        result->profiles = new_profiles;
        result->profile_capacity = new_cap;
    }

    /* Create the new profile */
    uint32_t new_idx = result->profile_count;
    heap_value_profile_init(&result->profiles[new_idx], type_id, field_offset);
    result->profile_count++;
    result->total_fields_profiled++;

    /* Insert into the sparse index */
    field_index_insert(result, fid, new_idx);

    return &result->profiles[new_idx];
}

/* ========================================================================== */
/* Lifecycle                                                                    */
/* ========================================================================== */

int vtx_heap_value_prop_init(vtx_heap_value_prop_result_t *result)
{
    if (result == NULL) return -1;

    /* Allocate the initial profiles array */
    result->profiles = (vtx_heap_value_profile_t *)malloc(
        VTX_HEAP_VALUE_PROP_INITIAL_CAPACITY * sizeof(vtx_heap_value_profile_t));
    if (result->profiles == NULL) return -1;

    /* Allocate the sparse field index.
     * Start with 2x the profile capacity for low load factor. */
    result->field_index_capacity = VTX_HEAP_VALUE_PROP_INITIAL_CAPACITY * 2;
    result->field_index = (uint32_t *)malloc(
        result->field_index_capacity * sizeof(uint32_t));
    if (result->field_index == NULL) {
        free(result->profiles);
        result->profiles = NULL;
        return -1;
    }

    /* Initialize all index slots to "empty" */
    for (uint32_t i = 0; i < result->field_index_capacity; i++) {
        result->field_index[i] = UINT32_MAX;
    }

    result->profile_count    = 0;
    result->profile_capacity = VTX_HEAP_VALUE_PROP_INITIAL_CAPACITY;

    result->total_fields_profiled    = 0;
    result->stable_field_count       = 0;
    result->speculated_field_count   = 0;
    result->estimated_loads_eliminated = 0;

    return 0;
}

void vtx_heap_value_prop_destroy(vtx_heap_value_prop_result_t *result)
{
    if (result == NULL) return;

    free(result->profiles);
    free(result->field_index);

    result->profiles         = NULL;
    result->field_index      = NULL;
    result->profile_count    = 0;
    result->profile_capacity = 0;
    result->field_index_capacity = 0;
    result->total_fields_profiled    = 0;
    result->stable_field_count       = 0;
    result->speculated_field_count   = 0;
    result->estimated_loads_eliminated = 0;
}

/* ========================================================================== */
/* Observation (slow path — called on sample boundary)                          */
/* ========================================================================== */

void vtx_heap_value_prop_observe(vtx_heap_value_prop_result_t *result,
                                   uint32_t type_id,
                                   uint32_t field_offset,
                                   uint64_t value)
{
    if (result == NULL) return;

    /* Find or create the profile for this (type_id, field_offset) */
    vtx_heap_value_profile_t *profile =
        lookup_or_create_profile(result, type_id, field_offset);
    if (profile == NULL) return;  /* allocation failure — skip this observation */

    /* Track the previous stable state to detect transitions */
    bool was_stable = profile->is_stable;

    /* Increment total observations (saturating) */
    if (profile->total_observations < UINT64_MAX) {
        profile->total_observations++;
    }

    /* ---- Match against existing top-4 slots ---- */
    for (int i = 0; i < VTX_HEAP_VALUE_SLOTS; i++) {
        if (profile->top_counts[i] > 0 && profile->top_values[i] == value) {
            /* Match found — increment count (saturating) */
            if (profile->top_counts[i] < UINT64_MAX) {
                profile->top_counts[i]++;
            }
            goto done;
        }
    }

    /* ---- No match — check for an empty slot ---- */
    for (int i = 0; i < VTX_HEAP_VALUE_SLOTS; i++) {
        if (profile->top_counts[i] == 0) {
            /* Empty slot — insert value */
            profile->top_values[i] = value;
            profile->top_counts[i] = 1;
            goto done;
        }
    }

    /* ---- All slots occupied and no match — increment "other" bucket ---- */
    if (profile->other_count < UINT64_MAX) {
        profile->other_count++;
    }

    /* ---- Demotion heuristic ----
     *
     * If the "other" bucket grows too large relative to the least
     * frequent slot, replace the least frequent slot with the new
     * value. This allows the profile to adapt when the value
     * distribution shifts.
     *
     * The demotion ratio (2.0) means the other bucket must have
     * at least 2x the count of the lowest slot before demotion
     * occurs. This prevents thrashing from random noise.
     *
     * After demotion, we reset other_count to 0 because the
     * displaced value's count is added to the other bucket
     * conceptually, but we don't track individual "other" values.
     * This is a slight approximation but works well in practice.
     */
    {
        /* Find the slot with the lowest count (last slot after sort,
         * but we haven't sorted yet, so scan for it) */
        int min_slot = VTX_HEAP_VALUE_SLOTS - 1;
        uint64_t min_count = profile->top_counts[min_slot];
        for (int i = VTX_HEAP_VALUE_SLOTS - 2; i >= 0; i--) {
            if (profile->top_counts[i] < min_count) {
                min_count = profile->top_counts[i];
                min_slot = i;
            }
        }

        if (min_count > 0 &&
            (double)profile->other_count > VTX_HEAP_VALUE_DEMOTE_RATIO * (double)min_count) {
            /* Demote the least frequent slot — replace with the new value.
             * The old value's observations are effectively absorbed into
             * the "other" bucket (we reset other_count since we can't
             * attribute them to a specific value). */
            profile->top_values[min_slot] = value;
            profile->top_counts[min_slot] = 1;
            profile->other_count = 0;
        }
    }

done:
    /* Re-sort slots by count (descending) */
    sort_profile_slots(profile);

    /* Update frequency cache */
    update_frequencies(profile);

    /* Update stability flag */
    update_stability(profile);

    /* Update stable_field_count if stability changed */
    if (profile->is_stable && !was_stable) {
        result->stable_field_count++;
    } else if (!profile->is_stable && was_stable) {
        result->stable_field_count--;
    }

    /* Adapt sampling interval */
    adapt_interval(profile);

    /* Reset sample counter */
    profile->sample_counter = profile->sample_interval;
}

/* ========================================================================== */
/* Query                                                                        */
/* ========================================================================== */

bool vtx_heap_value_prop_is_stable(const vtx_heap_value_prop_result_t *result,
                                     uint32_t type_id,
                                     uint32_t field_offset)
{
    if (result == NULL) return false;

    uint64_t fid = vtx_heap_value_field_id(type_id, field_offset);
    uint32_t idx = field_index_lookup(result, fid);
    if (idx == UINT32_MAX) return false;

    return result->profiles[idx].is_stable;
}

uint64_t vtx_heap_value_prop_speculated_value(
    const vtx_heap_value_prop_result_t *result,
    uint32_t type_id,
    uint32_t field_offset)
{
    if (result == NULL) return 0;

    uint64_t fid = vtx_heap_value_field_id(type_id, field_offset);
    uint32_t idx = field_index_lookup(result, fid);
    if (idx == UINT32_MAX) return 0;

    const vtx_heap_value_profile_t *profile = &result->profiles[idx];
    if (!profile->is_stable) return 0;

    return profile->speculated_value;
}

const vtx_heap_value_profile_t *vtx_heap_value_prop_lookup(
    const vtx_heap_value_prop_result_t *result,
    uint32_t type_id,
    uint32_t field_offset)
{
    if (result == NULL) return NULL;

    uint64_t fid = vtx_heap_value_field_id(type_id, field_offset);
    uint32_t idx = field_index_lookup(result, fid);
    if (idx == UINT32_MAX) return NULL;

    return &result->profiles[idx];
}

/* ========================================================================== */
/* Guard management                                                             */
/* ========================================================================== */

int vtx_heap_value_prop_install_guard(vtx_heap_value_prop_result_t *result,
                                        uint32_t type_id,
                                        uint32_t field_offset,
                                        uint32_t guard_id)
{
    if (result == NULL) return -1;

    uint64_t fid = vtx_heap_value_field_id(type_id, field_offset);
    uint32_t idx = field_index_lookup(result, fid);
    if (idx == UINT32_MAX) return -1;

    vtx_heap_value_profile_t *profile = &result->profiles[idx];
    profile->guard_id = guard_id;
    profile->guard_installed = true;

    return 0;
}

void vtx_heap_value_prop_remove_guard(vtx_heap_value_prop_result_t *result,
                                        uint32_t type_id,
                                        uint32_t field_offset)
{
    if (result == NULL) return;

    uint64_t fid = vtx_heap_value_field_id(type_id, field_offset);
    uint32_t idx = field_index_lookup(result, fid);
    if (idx == UINT32_MAX) return;

    vtx_heap_value_profile_t *profile = &result->profiles[idx];
    profile->guard_id = VTX_HEAP_VALUE_GUARD_ID_INVALID;
    profile->guard_installed = false;
}

/* ========================================================================== */
/* Statistics                                                                   */
/* ========================================================================== */

uint32_t vtx_heap_value_prop_stable_count(const vtx_heap_value_prop_result_t *result)
{
    if (result == NULL) return 0;
    return result->stable_field_count;
}

uint32_t vtx_heap_value_prop_guard_count(const vtx_heap_value_prop_result_t *result)
{
    if (result == NULL) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < result->profile_count; i++) {
        if (result->profiles[i].guard_installed) {
            count++;
        }
    }
    return count;
}

uint64_t vtx_heap_value_prop_loads_eliminated(const vtx_heap_value_prop_result_t *result)
{
    if (result == NULL) return 0;
    return result->estimated_loads_eliminated;
}
