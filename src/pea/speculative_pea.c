/**
 * VORTEX Speculative PEA + Lock Elision — Implementation
 *
 * Implements two profile-guided speculative optimizations:
 *
 *   1. Speculative PEA: Speculate NoEscape for allocations where PEA
 *      proved ArgEscape/GlobalEscape but profiling suggests they usually
 *      don't escape. Add guards; deopt on failure.
 *
 *   2. Speculative Lock Elision: Elide monitor enter/exit for
 *      synchronized blocks with zero/near-zero contention. Add guards;
 *      deopt on contention.
 *
 * Both follow the same pattern:
 *   - Profile → decide → speculate → guard → adapt
 *   - Failed speculations weaken the guard (FastCheck → FullCheck → DeoptAlways)
 *   - Successful speculations remain at FastCheck or strengthen to PredicatedCheck
 */

#include "pea/speculative_pea.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ========================================================================== */
/* Speculation level names                                                      */
/* ========================================================================== */

static const char * const spec_pea_level_names[] = {
    "NONE",
    "CONSERVATIVE",
    "MODERATE",
    "AGGRESSIVE"
};

const char *vtx_spec_pea_level_name(vtx_spec_pea_level_t level)
{
    if (level > VTX_SPEC_PEA_AGGRESSIVE) return "Unknown";
    return spec_pea_level_names[level];
}

/* ========================================================================== */
/* Internal: check if a node is an allocation                                   */
/* ========================================================================== */

static inline bool is_allocation(vtx_node_opcode_t opcode)
{
    return opcode == VTX_OP_NewObject ||
           opcode == VTX_OP_NewArray  ||
           opcode == VTX_OP_Allocate;
}

/* ========================================================================== */
/* Internal: get the no-escape rate threshold for a given proven state          */
/* and speculation level                                                        */
/* ========================================================================== */

/**
 * Returns the minimum no_escape_rate required to speculate that an
 * allocation with the given proven escape state doesn't escape, at
 * the given speculation level.
 *
 * Returns INFINITY if the proven state is not eligible for speculation
 * at this level (e.g., GlobalEscape at CONSERVATIVE level).
 */
static double get_no_escape_threshold(vtx_spec_pea_level_t level,
                                       vtx_escape_state_t proven_state)
{
    switch (level) {
    case VTX_SPEC_PEA_NONE:
        /* No speculation at all */
        return INFINITY;

    case VTX_SPEC_PEA_CONSERVATIVE:
        /* Only speculate on ArgEscape */
        if (proven_state == VTX_ESCAPE_ARG) {
            return VTX_SPEC_RATE_CONSERVATIVE_ARG;  /* 0.99 */
        }
        return INFINITY;  /* GlobalEscape not eligible */

    case VTX_SPEC_PEA_MODERATE:
        /* Speculate on ArgEscape + some GlobalEscape */
        if (proven_state == VTX_ESCAPE_ARG) {
            return VTX_SPEC_RATE_MODERATE_ARG;       /* 0.95 */
        }
        if (proven_state == VTX_ESCAPE_GLOBAL) {
            return VTX_SPEC_RATE_MODERATE_GLOBAL;    /* 0.98 */
        }
        return INFINITY;

    case VTX_SPEC_PEA_AGGRESSIVE:
        /* Speculate on any escaping object */
        if (proven_state == VTX_ESCAPE_ARG) {
            return VTX_SPEC_RATE_AGGRESSIVE_ARG;     /* 0.80 */
        }
        if (proven_state == VTX_ESCAPE_GLOBAL) {
            return VTX_SPEC_RATE_AGGRESSIVE_GLOBAL;  /* 0.90 */
        }
        return INFINITY;
    }

    return INFINITY;
}

/* ========================================================================== */
/* Internal: look up profile data for an allocation site                        */
/* ========================================================================== */

/**
 * Look up value profile data for an allocation site identified by
 * its bytecode_pc. The allocation node's bytecode_pc field is used
 * to index into the value profile table.
 *
 * Returns the no_escape_rate and total_observations from the profile.
 * If no profile data is available, returns false.
 */
static bool lookup_alloc_profile(const vtx_value_profile_table_t *value_profiles,
                                  uint32_t bytecode_pc,
                                  uint64_t *out_total_obs,
                                  uint64_t *out_no_escape_obs,
                                  double *out_no_escape_rate)
{
    if (value_profiles == NULL) return false;

    const vtx_value_profile_t *profile =
        vtx_value_profile_lookup(value_profiles, bytecode_pc);
    if (profile == NULL) return false;

    if (profile->total_observations < VTX_SPEC_PEA_MIN_OBSERVATIONS) {
        return false;  /* not enough data */
    }

    *out_total_obs = profile->total_observations;

    /* The value profile tracks the most frequent value. For escape
     * analysis, the "no escape" observations are stored as the top
     * value count. The value itself encodes whether the object escaped:
     *   0 = didn't escape, 1 = escaped.
     * We use the top value's count as no_escape_obs if the top value
     * is 0 (no escape), otherwise use other_count. */
    uint64_t top_value;
    double top_freq;
    if (vtx_value_profile_top(profile, &top_value, &top_freq)) {
        if (top_value == 0) {
            /* Top value is "didn't escape" */
            *out_no_escape_obs = profile->entries[0].count;
        } else {
            /* Top value is "escaped" — the no-escape count is in the
             * other bucket or second entry */
            if (profile->entries[1].count > 0 && profile->entries[1].value == 0) {
                *out_no_escape_obs = profile->entries[1].count;
            } else {
                *out_no_escape_obs = profile->other_count;
            }
        }
    } else {
        return false;
    }

    *out_no_escape_rate = (double)*out_no_escape_obs / (double)*out_total_obs;
    return true;
}

/* ========================================================================== */
/* Internal: grow the spec_decisions array if needed                            */
/* ========================================================================== */

static int grow_spec_decisions(vtx_spec_pea_analysis_t *analysis)
{
    if (analysis->spec_count < analysis->spec_capacity) {
        return 0;  /* still have room */
    }

    uint32_t new_capacity = analysis->spec_capacity * 2;
    if (new_capacity == 0) new_capacity = VTX_SPEC_PEA_INITIAL_CAPACITY;

    vtx_spec_pea_decision_t *new_arr = (vtx_spec_pea_decision_t *)realloc(
        analysis->spec_decisions,
        new_capacity * sizeof(vtx_spec_pea_decision_t));
    if (!new_arr) return -1;

    analysis->spec_decisions = new_arr;
    analysis->spec_capacity = new_capacity;
    return 0;
}

/* ========================================================================== */
/* Internal: grow the lock profiles array if needed                             */
/* ========================================================================== */

static int grow_lock_profiles(vtx_lock_elision_result_t *result)
{
    if (result->profile_count < result->profile_capacity) {
        return 0;  /* still have room */
    }

    uint32_t new_capacity = result->profile_capacity * 2;
    if (new_capacity == 0) new_capacity = VTX_LOCK_PROFILE_INITIAL_CAPACITY;

    vtx_lock_contention_profile_t *new_arr = (vtx_lock_contention_profile_t *)realloc(
        result->profiles,
        new_capacity * sizeof(vtx_lock_contention_profile_t));
    if (!new_arr) return -1;

    result->profiles = new_arr;
    result->profile_capacity = new_capacity;
    return 0;
}

/* ========================================================================== */
/* Speculative PEA: main entry point                                            */
/* ========================================================================== */

vtx_spec_pea_analysis_t *vtx_spec_pea_run(vtx_graph_t *graph,
                                            vtx_arena_t *arena,
                                            vtx_spec_pea_level_t level,
                                            const vtx_value_profile_table_t *value_profiles)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    /* Step 1: Run standard PEA to get proven escape states */
    vtx_pea_analysis_t *base = vtx_pea_run(graph, arena);
    if (base == NULL) return NULL;

    /* Allocate the speculative analysis result */
    vtx_spec_pea_analysis_t *result =
        (vtx_spec_pea_analysis_t *)vtx_arena_alloc(arena, sizeof(vtx_spec_pea_analysis_t));
    if (!result) return NULL;
    memset(result, 0, sizeof(*result));

    result->base_analysis = *base;  /* copy the base result */
    result->level = level;

    /* Allocate the speculative decisions array */
    result->spec_capacity = VTX_SPEC_PEA_INITIAL_CAPACITY;
    result->spec_decisions = (vtx_spec_pea_decision_t *)malloc(
        result->spec_capacity * sizeof(vtx_spec_pea_decision_t));
    if (!result->spec_decisions) return NULL;
    result->spec_count = 0;

    /* Step 2: Build the speculative escape map.
     * Start with a copy of the base escape map; we'll override entries
     * for speculative allocations. */
    uint32_t state_count = base->escape_map.state_count;
    result->speculative_escape_map.state_count = state_count;

    result->speculative_escape_map.states =
        (vtx_escape_state_t *)malloc(state_count * sizeof(vtx_escape_state_t));
    if (!result->speculative_escape_map.states) {
        free(result->spec_decisions);
        return NULL;
    }

    /* Copy the base states as the starting point */
    memcpy(result->speculative_escape_map.states,
           base->escape_map.states,
           state_count * sizeof(vtx_escape_state_t));

    /* Copy the allocation IDs array */
    result->speculative_escape_map.alloc_count = base->escape_map.alloc_count;
    result->speculative_escape_map.alloc_capacity = base->escape_map.alloc_capacity;

    result->speculative_escape_map.alloc_ids =
        (vtx_nodeid_t *)malloc(base->escape_map.alloc_capacity * sizeof(vtx_nodeid_t));
    if (!result->speculative_escape_map.alloc_ids) {
        free(result->speculative_escape_map.states);
        free(result->spec_decisions);
        return NULL;
    }

    memcpy(result->speculative_escape_map.alloc_ids,
           base->escape_map.alloc_ids,
           base->escape_map.alloc_count * sizeof(vtx_nodeid_t));

    /* Step 3: For each allocation with proven_state > NoEscape,
     * check if profiling supports speculation. */
    vtx_node_table_t *table = &graph->node_table;

    for (uint32_t a = 0; a < base->escape_map.alloc_count; a++) {
        vtx_nodeid_t alloc_id = base->escape_map.alloc_ids[a];
        vtx_escape_state_t proven = base->escape_map.states[alloc_id];

        /* Count proven NoEscape allocations */
        if (proven == VTX_ESCAPE_NONE) {
            result->total_proven++;
            continue;  /* already NoEscape, no speculation needed */
        }

        /* Check if this proven state is eligible for speculation */
        double threshold = get_no_escape_threshold(level, proven);
        if (isinf(threshold)) continue;  /* not eligible at this level */

        /* Look up the allocation node to get its bytecode_pc */
        if (alloc_id >= table->count) continue;
        vtx_node_t *alloc_node = &table->nodes[alloc_id];
        if (alloc_node->dead) continue;

        /* Look up profile data for this allocation site */
        uint64_t total_obs = 0;
        uint64_t no_escape_obs = 0;
        double no_escape_rate = 0.0;

        if (!lookup_alloc_profile(value_profiles, alloc_node->bytecode_pc,
                                   &total_obs, &no_escape_obs, &no_escape_rate)) {
            continue;  /* no profile data available */
        }

        /* Check if the no-escape rate meets the threshold */
        if (no_escape_rate >= threshold) {
            /* Speculation is justified! Create a decision. */
            if (grow_spec_decisions(result) != 0) {
                /* Allocation failure — continue without this speculation */
                continue;
            }

            vtx_spec_pea_decision_t *decision =
                &result->spec_decisions[result->spec_count];
            memset(decision, 0, sizeof(*decision));

            decision->alloc_node_id    = alloc_id;
            decision->proven_state     = proven;
            decision->speculated_state = VTX_ESCAPE_NONE;
            decision->level            = level;
            decision->guard_id         = 0;
            decision->total_observations = total_obs;
            decision->no_escape_obs    = no_escape_obs;
            decision->no_escape_rate   = no_escape_rate;
            decision->guard_installed  = false;

            result->spec_count++;
            result->total_speculated++;

            /* Update the speculative escape map */
            result->speculative_escape_map.states[alloc_id] = VTX_ESCAPE_NONE;
        }
    }

    /* Step 4: Compute statistics */
    result->potential_sr_count = result->total_proven + result->total_speculated;

    return result;
}

/* ========================================================================== */
/* Speculative PEA: query helpers                                               */
/* ========================================================================== */

vtx_escape_state_t vtx_spec_pea_effective_escape(
    const vtx_spec_pea_analysis_t *analysis,
    vtx_nodeid_t alloc_node_id)
{
    VTX_ASSERT(analysis != NULL, "analysis must not be NULL");

    /* Query the speculative escape map first */
    if (alloc_node_id < analysis->speculative_escape_map.state_count) {
        return analysis->speculative_escape_map.states[alloc_node_id];
    }

    /* Fallback to base analysis */
    return vtx_pea_get_escape(&analysis->base_analysis, alloc_node_id);
}

bool vtx_spec_pea_is_speculative_sr(const vtx_spec_pea_analysis_t *analysis,
                                      vtx_nodeid_t alloc_node_id)
{
    return vtx_spec_pea_effective_escape(analysis, alloc_node_id) == VTX_ESCAPE_NONE;
}

bool vtx_spec_pea_has_decision(const vtx_spec_pea_analysis_t *analysis,
                                 vtx_nodeid_t alloc_node_id)
{
    VTX_ASSERT(analysis != NULL, "analysis must not be NULL");

    if (analysis->spec_decisions == NULL) return false;

    for (uint32_t i = 0; i < analysis->spec_count; i++) {
        if (analysis->spec_decisions[i].alloc_node_id == alloc_node_id) {
            return true;
        }
    }
    return false;
}

const vtx_spec_pea_decision_t *vtx_spec_pea_get_decision(
    const vtx_spec_pea_analysis_t *analysis,
    vtx_nodeid_t alloc_node_id)
{
    VTX_ASSERT(analysis != NULL, "analysis must not be NULL");

    if (analysis->spec_decisions == NULL) return NULL;

    for (uint32_t i = 0; i < analysis->spec_count; i++) {
        if (analysis->spec_decisions[i].alloc_node_id == alloc_node_id) {
            return &analysis->spec_decisions[i];
        }
    }
    return NULL;
}

/* ========================================================================== */
/* Speculative PEA: guard installation                                          */
/* ========================================================================== */

int vtx_spec_pea_install_guard(vtx_spec_pea_analysis_t *analysis,
                                 vtx_nodeid_t alloc_node_id,
                                 uint32_t guard_id)
{
    VTX_ASSERT(analysis != NULL, "analysis must not be NULL");
    VTX_ASSERT(guard_id != 0, "guard_id must not be 0");

    if (analysis->spec_decisions == NULL) return -1;

    for (uint32_t i = 0; i < analysis->spec_count; i++) {
        if (analysis->spec_decisions[i].alloc_node_id == alloc_node_id) {
            analysis->spec_decisions[i].guard_id = guard_id;
            analysis->spec_decisions[i].guard_installed = true;
            return 0;
        }
    }

    return -1;  /* no speculative decision for this allocation */
}

/* ========================================================================== */
/* Speculative PEA: destruction                                                 */
/* ========================================================================== */

void vtx_spec_pea_analysis_destroy(vtx_spec_pea_analysis_t *analysis)
{
    if (analysis == NULL) return;

    /* Free the spec_decisions array (malloc'd) */
    if (analysis->spec_decisions != NULL) {
        free(analysis->spec_decisions);
        analysis->spec_decisions = NULL;
    }

    /* Free the speculative escape map (malloc'd) */
    if (analysis->speculative_escape_map.states != NULL) {
        free(analysis->speculative_escape_map.states);
        analysis->speculative_escape_map.states = NULL;
    }
    if (analysis->speculative_escape_map.alloc_ids != NULL) {
        free(analysis->speculative_escape_map.alloc_ids);
        analysis->speculative_escape_map.alloc_ids = NULL;
    }

    analysis->spec_count = 0;
    analysis->spec_capacity = 0;

    /* Note: base_analysis was arena-allocated and will be freed
     * when the arena is destroyed. We don't free it here. */
}

/* ========================================================================== */
/* Lock Elision: initialization                                                 */
/* ========================================================================== */

int vtx_lock_elision_init(vtx_lock_elision_result_t *result)
{
    if (result == NULL) return -1;

    result->profiles = (vtx_lock_contention_profile_t *)malloc(
        VTX_LOCK_PROFILE_INITIAL_CAPACITY * sizeof(vtx_lock_contention_profile_t));
    if (result->profiles == NULL) return -1;

    result->profile_count = 0;
    result->profile_capacity = VTX_LOCK_PROFILE_INITIAL_CAPACITY;
    result->total_sites = 0;
    result->eligible_sites = 0;
    result->elided_sites = 0;

    return 0;
}

/* ========================================================================== */
/* Lock Elision: destruction                                                    */
/* ========================================================================== */

void vtx_lock_elision_result_destroy(vtx_lock_elision_result_t *result)
{
    if (result == NULL) return;

    if (result->profiles != NULL) {
        free(result->profiles);
        result->profiles = NULL;
    }

    result->profile_count = 0;
    result->profile_capacity = 0;
    result->total_sites = 0;
    result->eligible_sites = 0;
    result->elided_sites = 0;
}

/* ========================================================================== */
/* Lock Elision: internal: find or create a profile for a monitor site          */
/* ========================================================================== */

/**
 * Find the profile for a monitor site, or create a new one if it
 * doesn't exist yet.
 *
 * Returns the profile index in the profiles array, or -1 on failure.
 */
static int32_t find_or_create_profile(vtx_lock_elision_result_t *result,
                                        uint32_t monitor_site_id)
{
    /* Search for existing profile */
    for (uint32_t i = 0; i < result->profile_count; i++) {
        if (result->profiles[i].monitor_site_id == monitor_site_id) {
            return (int32_t)i;
        }
    }

    /* Create new profile */
    if (grow_lock_profiles(result) != 0) return -1;

    vtx_lock_contention_profile_t *profile =
        &result->profiles[result->profile_count];
    memset(profile, 0, sizeof(*profile));

    profile->monitor_site_id    = monitor_site_id;
    profile->total_acquisitions = 0;
    profile->contention_count   = 0;
    profile->contention_rate    = 0.0;
    profile->sample_counter     = VTX_LOCK_SAMPLE_INTERVAL_DEFAULT;
    profile->sample_interval    = VTX_LOCK_SAMPLE_INTERVAL_DEFAULT;
    profile->is_eligible        = false;
    profile->guard_id           = 0;
    profile->guard_installed    = false;

    result->profile_count++;
    result->total_sites++;

    return (int32_t)(result->profile_count - 1);
}

/* ========================================================================== */
/* Lock Elision: internal: update eligibility after a sample                    */
/* ========================================================================== */

/**
 * Recompute the contention rate and eligibility for a profile.
 * Called at sample boundaries.
 */
static void update_eligibility(vtx_lock_contention_profile_t *profile)
{
    /* Recompute contention rate */
    if (profile->total_acquisitions > 0) {
        profile->contention_rate =
            (double)profile->contention_count / (double)profile->total_acquisitions;
    } else {
        profile->contention_rate = 0.0;
    }

    /* Check eligibility:
     *   - contention_rate < VTX_LOCK_CONTENTION_THRESHOLD (0.1%)
     *   - total_acquisitions >= VTX_LOCK_MIN_OBSERVATIONS (1000) */
    bool was_eligible = profile->is_eligible;
    profile->is_eligible =
        (profile->total_acquisitions >= VTX_LOCK_MIN_OBSERVATIONS) &&
        (profile->contention_rate < VTX_LOCK_CONTENTION_THRESHOLD);

    /* Update statistics: if eligibility changed, update the result's
     * eligible_sites count. (This is approximate — the caller should
     * recompute stats periodically.) */
    (void)was_eligible;  /* used for incremental update in a full integration */
}

/* ========================================================================== */
/* Lock Elision: internal: adapt sampling interval                              */
/* ========================================================================== */

/**
 * Adapt the sampling interval based on the contention stability.
 *
 * Stable sites (contention_rate near 0) get longer intervals
 * (less frequent checking → less overhead).
 * Unstable sites (contention_rate near threshold) get shorter intervals
 * (more responsive tracking).
 *
 * This mirrors the adaptive sampling in guard metadata and value profiles.
 */
static void adapt_sample_interval(vtx_lock_contention_profile_t *profile,
                                    bool had_contention)
{
    if (had_contention) {
        /* Contention observed → shorten interval for rapid detection */
        if (profile->sample_interval > VTX_LOCK_SAMPLE_INTERVAL_UNSTABLE) {
            profile->sample_interval = VTX_LOCK_SAMPLE_INTERVAL_UNSTABLE;
        }
    } else if (profile->contention_rate < VTX_LOCK_CONTENTION_THRESHOLD * 0.1) {
        /* Very stable (contention rate < 0.01%) → lengthen interval */
        profile->sample_interval = VTX_LOCK_SAMPLE_INTERVAL_STABLE;
    } else if (profile->contention_rate < VTX_LOCK_CONTENTION_THRESHOLD * 0.5) {
        /* Moderately stable → default interval */
        profile->sample_interval = VTX_LOCK_SAMPLE_INTERVAL_DEFAULT;
    } else {
        /* Near threshold → keep short for responsiveness */
        profile->sample_interval = VTX_LOCK_SAMPLE_INTERVAL_UNSTABLE;
    }
}

/* ========================================================================== */
/* Lock Elision: record acquisition (hot path)                                  */
/* ========================================================================== */

void vtx_lock_elision_record_acquisition(vtx_lock_elision_result_t *result,
                                           uint32_t monitor_site_id,
                                           bool is_contended)
{
    if (result == NULL) return;

    int32_t idx = find_or_create_profile(result, monitor_site_id);
    if (idx < 0) return;  /* allocation failure */

    vtx_lock_contention_profile_t *profile = &result->profiles[idx];

    /* Sampling: decrement counter, only record at sample boundary.
     * This is the zero-cost deopt hot path pattern:
     *   1. Decrement counter (1 cycle)
     *   2. If counter > 1, return (predicted branch)
     *   3. At boundary, do full update and reset counter */
    if (__builtin_expect(profile->sample_counter > 1, 1)) {
        profile->sample_counter--;

        /* Even between samples, we accumulate contention info.
         * This ensures we don't miss rare contention events. */
        if (is_contended) {
            profile->contention_count++;
            /* Shorten interval on contention for rapid detection */
            if (profile->sample_interval > VTX_LOCK_SAMPLE_INTERVAL_UNSTABLE) {
                profile->sample_interval = VTX_LOCK_SAMPLE_INTERVAL_UNSTABLE;
                if (profile->sample_counter > VTX_LOCK_SAMPLE_INTERVAL_UNSTABLE) {
                    profile->sample_counter = VTX_LOCK_SAMPLE_INTERVAL_UNSTABLE;
                }
            }
        }
        return;
    }

    /* Sample boundary — do the full update */
    profile->total_acquisitions += profile->sample_interval;
    if (is_contended) {
        profile->contention_count++;
    }

    /* Recompute eligibility */
    update_eligibility(profile);

    /* Adapt sampling interval based on stability */
    adapt_sample_interval(profile, is_contended);

    /* Reset sample counter */
    profile->sample_counter = profile->sample_interval;
}

/* ========================================================================== */
/* Lock Elision: eligibility check                                              */
/* ========================================================================== */

bool vtx_lock_elision_is_eligible(const vtx_lock_elision_result_t *result,
                                    uint32_t monitor_site_id)
{
    if (result == NULL || result->profiles == NULL) return false;

    for (uint32_t i = 0; i < result->profile_count; i++) {
        if (result->profiles[i].monitor_site_id == monitor_site_id) {
            return result->profiles[i].is_eligible;
        }
    }

    return false;  /* no profile for this site */
}

/* ========================================================================== */
/* Lock Elision: guard installation                                             */
/* ========================================================================== */

int vtx_lock_elision_install_guard(vtx_lock_elision_result_t *result,
                                     uint32_t monitor_site_id,
                                     uint32_t guard_id)
{
    VTX_ASSERT(result != NULL, "result must not be NULL");
    VTX_ASSERT(guard_id != 0, "guard_id must not be 0");

    if (result == NULL || result->profiles == NULL) return -1;

    for (uint32_t i = 0; i < result->profile_count; i++) {
        if (result->profiles[i].monitor_site_id == monitor_site_id) {
            result->profiles[i].guard_id = guard_id;
            result->profiles[i].guard_installed = true;
            result->elided_sites++;
            return 0;
        }
    }

    return -1;  /* no profile for this site */
}

/* ========================================================================== */
/* Lock Elision: statistics query                                               */
/* ========================================================================== */

void vtx_lock_elision_get_stats(const vtx_lock_elision_result_t *result,
                                  uint32_t *total_sites,
                                  uint32_t *eligible_sites,
                                  uint32_t *elided_sites)
{
    if (result == NULL) {
        if (total_sites)    *total_sites = 0;
        if (eligible_sites) *eligible_sites = 0;
        if (elided_sites)   *elided_sites = 0;
        return;
    }

    /* Recompute eligible_sites from profiles (may have changed since
     * last check due to profile updates) */
    uint32_t eligible = 0;
    for (uint32_t i = 0; i < result->profile_count; i++) {
        if (result->profiles[i].is_eligible) {
            eligible++;
        }
    }

    /* Update the cached count */
    ((vtx_lock_elision_result_t *)result)->eligible_sites = eligible;

    if (total_sites)    *total_sites = result->total_sites;
    if (eligible_sites) *eligible_sites = eligible;
    if (elided_sites)   *elided_sites = result->elided_sites;
}

/* ========================================================================== */
/* Lock Elision: profile lookup                                                 */
/* ========================================================================== */

const vtx_lock_contention_profile_t *vtx_lock_elision_get_profile(
    const vtx_lock_elision_result_t *result,
    uint32_t monitor_site_id)
{
    if (result == NULL || result->profiles == NULL) return NULL;

    for (uint32_t i = 0; i < result->profile_count; i++) {
        if (result->profiles[i].monitor_site_id == monitor_site_id) {
            return &result->profiles[i];
        }
    }

    return NULL;
}
