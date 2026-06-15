/**
 * VORTEX Value Profiling — Implementation
 *
 * Tracks the top-2 most frequently observed values at each profile site.
 * Uses sampling to reduce hot-path overhead to ~1 cycle per observation.
 */

#include "guard/value_profile.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Lifecycle                                                                    */
/* ========================================================================== */

void vtx_value_profile_init(vtx_value_profile_t *profile)
{
    if (profile == NULL) return;

    memset(profile, 0, sizeof(*profile));
    profile->sample_counter = VTX_VALUE_SAMPLE_INTERVAL_DEFAULT;
    profile->sample_interval = VTX_VALUE_SAMPLE_INTERVAL_DEFAULT;
    profile->is_stable = false;
    profile->speculated_value = 0;
}

void vtx_value_profile_reset(vtx_value_profile_t *profile)
{
    if (profile == NULL) return;

    uint32_t interval = profile->sample_interval;
    vtx_value_profile_init(profile);
    profile->sample_interval = interval;
    profile->sample_counter = interval;
}

/* ========================================================================== */
/* Internal: sort entries by count (descending)                                 */
/* ========================================================================== */

static void sort_entries(vtx_value_profile_t *profile)
{
    /* Simple sort for 2 entries — just swap if needed */
    if (profile->entries[1].count > profile->entries[0].count) {
        vtx_value_entry_t tmp = profile->entries[0];
        profile->entries[0] = profile->entries[1];
        profile->entries[1] = tmp;
    }
}

/* ========================================================================== */
/* Internal: update stability flag                                              */
/* ========================================================================== */

static void update_stability(vtx_value_profile_t *profile)
{
    if (profile->total_observations < VTX_VALUE_MIN_OBSERVATIONS) {
        profile->is_stable = false;
        return;
    }

    if (profile->entries[0].count == 0) {
        profile->is_stable = false;
        return;
    }

    double top_freq = (double)profile->entries[0].count /
                      (double)profile->total_observations;

    if (top_freq >= VTX_VALUE_STABILITY_THRESHOLD) {
        profile->is_stable = true;
        profile->speculated_value = profile->entries[0].value;
    } else {
        profile->is_stable = false;
    }
}

/* ========================================================================== */
/* Internal: adapt sampling interval                                            */
/* ========================================================================== */

static void adapt_interval(vtx_value_profile_t *profile)
{
    if (!profile->is_stable) {
        /* Unstable: use shorter interval for faster convergence */
        profile->sample_interval = VTX_VALUE_SAMPLE_INTERVAL_UNSTABLE;
        return;
    }

    double top_freq = (double)profile->entries[0].count /
                      (double)profile->total_observations;

    if (top_freq > 0.99) {
        /* Very stable: sample less frequently */
        profile->sample_interval = VTX_VALUE_SAMPLE_INTERVAL_STABLE;
    } else {
        /* Moderately stable: default interval */
        profile->sample_interval = VTX_VALUE_SAMPLE_INTERVAL_DEFAULT;
    }
}

/* ========================================================================== */
/* Observation (slow path — called when sample counter reaches zero)            */
/* ========================================================================== */

void vtx_value_profile_observe(vtx_value_profile_t *profile, uint64_t value)
{
    if (profile == NULL) return;

    /* Increment total observations */
    if (profile->total_observations < UINT64_MAX) {
        profile->total_observations++;
    }

    /* Try to match against existing entries */
    for (int i = 0; i < VTX_VALUE_PROFILE_SLOTS; i++) {
        if (profile->entries[i].count > 0 && profile->entries[i].value == value) {
            /* Match found — increment count */
            if (profile->entries[i].count < UINT64_MAX) {
                profile->entries[i].count++;
            }
            goto done;
        }
    }

    /* No match — check if we have an empty slot */
    for (int i = 0; i < VTX_VALUE_PROFILE_SLOTS; i++) {
        if (profile->entries[i].count == 0) {
            /* Empty slot — insert value */
            profile->entries[i].value = value;
            profile->entries[i].count = 1;
            goto done;
        }
    }

    /* All slots occupied and no match — increment "other" bucket.
     * Also consider demoting the least frequent entry if the "other"
     * bucket grows too large relative to entry[1]. This prevents
     * a stale entry from blocking a rising new value. */
    if (profile->other_count < UINT64_MAX) {
        profile->other_count++;
    }

    /* Demotion heuristic: if other_count exceeds entry[1].count by
     * more than 2x, replace entry[1] with the new value. This allows
     * the profile to adapt when the value distribution shifts. */
    if (profile->other_count > profile->entries[1].count * 2) {
        profile->entries[1].value = value;
        profile->entries[1].count = 1;
        profile->other_count = 0;
    }

done:
    /* Re-sort entries by count */
    sort_entries(profile);

    /* Update stability flag */
    update_stability(profile);

    /* Adapt sampling interval */
    adapt_interval(profile);

    /* Reset sample counter */
    profile->sample_counter = profile->sample_interval;
}

/* ========================================================================== */
/* Query                                                                        */
/* ========================================================================== */

bool vtx_value_profile_top(const vtx_value_profile_t *profile,
                             uint64_t *out_value, double *out_freq)
{
    if (profile == NULL || profile->total_observations == 0) {
        if (out_value) *out_value = 0;
        if (out_freq) *out_freq = 0.0;
        return false;
    }

    if (out_value) *out_value = profile->entries[0].value;
    if (out_freq) {
        *out_freq = (double)profile->entries[0].count /
                    (double)profile->total_observations;
    }
    return profile->entries[0].count > 0;
}

bool vtx_value_profile_second(const vtx_value_profile_t *profile,
                                uint64_t *out_value, double *out_freq)
{
    if (profile == NULL || profile->entries[1].count == 0) {
        if (out_value) *out_value = 0;
        if (out_freq) *out_freq = 0.0;
        return false;
    }

    if (out_value) *out_value = profile->entries[1].value;
    if (out_freq) {
        *out_freq = (double)profile->entries[1].count /
                    (double)profile->total_observations;
    }
    return true;
}

bool vtx_value_profile_is_stable(const vtx_value_profile_t *profile)
{
    if (profile == NULL) return false;
    return profile->is_stable;
}

uint64_t vtx_value_profile_speculated_value(const vtx_value_profile_t *profile)
{
    if (profile == NULL || !profile->is_stable) return 0;
    return profile->speculated_value;
}

/* ========================================================================== */
/* Value profile table                                                          */
/* ========================================================================== */

int vtx_value_profile_table_init(vtx_value_profile_table_t *table)
{
    if (table == NULL) return -1;

    table->profiles = (vtx_value_profile_t *)malloc(
        VTX_VALUE_PROFILE_TABLE_INITIAL_CAPACITY * sizeof(vtx_value_profile_t));
    table->bytecode_pcs = (uint32_t *)malloc(
        VTX_VALUE_PROFILE_TABLE_INITIAL_CAPACITY * sizeof(uint32_t));

    if (table->profiles == NULL || table->bytecode_pcs == NULL) {
        free(table->profiles);
        free(table->bytecode_pcs);
        table->profiles = NULL;
        table->bytecode_pcs = NULL;
        return -1;
    }

    table->count = 0;
    table->capacity = VTX_VALUE_PROFILE_TABLE_INITIAL_CAPACITY;
    return 0;
}

void vtx_value_profile_table_destroy(vtx_value_profile_table_t *table)
{
    if (table == NULL) return;

    free(table->profiles);
    free(table->bytecode_pcs);
    table->profiles = NULL;
    table->bytecode_pcs = NULL;
    table->count = 0;
    table->capacity = 0;
}

vtx_value_profile_t *vtx_value_profile_get_or_create(
    vtx_value_profile_table_t *table, uint32_t bytecode_pc)
{
    if (table == NULL) return NULL;

    /* Look for existing profile */
    vtx_value_profile_t *existing = vtx_value_profile_lookup(table, bytecode_pc);
    if (existing != NULL) return existing;

    /* Grow if needed */
    if (table->count >= table->capacity) {
        uint32_t new_cap = table->capacity * 2;
        vtx_value_profile_t *new_profiles = (vtx_value_profile_t *)realloc(
            table->profiles, new_cap * sizeof(vtx_value_profile_t));
        if (new_profiles == NULL) return NULL;
        table->profiles = new_profiles; /* Commit first realloc to avoid dangling pointer */

        uint32_t *new_pcs = (uint32_t *)realloc(
            table->bytecode_pcs, new_cap * sizeof(uint32_t));
        if (new_pcs == NULL) {
            /* First realloc committed, second failed.
             * table->profiles is valid (just larger). table->bytecode_pcs
             * is still valid (unchanged). The arrays are now inconsistent
             * in size but we can handle that — the profiles array is just
             * larger than needed. */
            return NULL;
        }
        table->bytecode_pcs = new_pcs;
        table->capacity = new_cap;
    }

    /* Create new profile */
    uint32_t idx = table->count;
    vtx_value_profile_init(&table->profiles[idx]);
    table->bytecode_pcs[idx] = bytecode_pc;
    table->count++;

    return &table->profiles[idx];
}

vtx_value_profile_t *vtx_value_profile_lookup(
    const vtx_value_profile_table_t *table, uint32_t bytecode_pc)
{
    if (table == NULL) return NULL;

    /* Linear scan — tables are typically small */
    for (uint32_t i = 0; i < table->count; i++) {
        if (table->bytecode_pcs[i] == bytecode_pc) {
            return &table->profiles[i];
        }
    }
    return NULL;
}

uint32_t vtx_value_profile_stable_count(const vtx_value_profile_table_t *table)
{
    if (table == NULL) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < table->count; i++) {
        if (table->profiles[i].is_stable) {
            count++;
        }
    }
    return count;
}
