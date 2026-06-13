/**
 * VORTEX Speculative Dead Code Elimination (Path Specialization)
 *             and Speculative Exception Elimination — Implementation
 *
 * Implements two profile-guided speculative optimizations:
 *
 *   2G — Speculative Dead Code Elimination (Path Specialization):
 *        Track branch directions at each branch point. When one direction
 *        dominates (>= 95% of observations), the cold path can be
 *        eliminated from compiled code with a guard that deopts if the
 *        cold direction is ever taken.
 *
 *   2H — Speculative Exception Elimination:
 *        Track whether call sites throw exceptions. When a call site
 *        never (or almost never) throws (>= 99.9% no-throw rate), the
 *        exception edge can be removed from compiled code with an
 *        implicit guard that deopts on exception.
 *
 * Both follow the same pattern:
 *   - Profile -> decide -> speculate -> guard -> adapt
 *   - Failed speculations weaken the guard (FastCheck -> FullCheck -> DeoptAlways)
 *   - Successful speculations remain at FastCheck or strengthen to PredicatedCheck
 *
 * Lookup strategy:
 *   Linear scan for both branch and exception observations. This is
 *   efficient because the tables are typically small (< 1000 entries
 *   per method) and the lookup is only performed during compilation,
 *   not on the hot execution path.
 */

#include "ir/spec_elim.h"

#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Internal: grow the branch observation array                                  */
/* ========================================================================== */

/**
 * Double the capacity of the branch observation array.
 * Returns 0 on success, -1 on realloc failure.
 */
static int branch_array_grow(vtx_spec_elim_result_t *result)
{
    uint32_t new_cap = result->branch_capacity * 2;
    vtx_branch_observation_t *new_arr = (vtx_branch_observation_t *)realloc(
        result->branches, new_cap * sizeof(vtx_branch_observation_t));
    if (new_arr == NULL) return -1;

    result->branches = new_arr;
    result->branch_capacity = new_cap;
    return 0;
}

/* ========================================================================== */
/* Internal: grow the exception observation array                               */
/* ========================================================================== */

/**
 * Double the capacity of the exception observation array.
 * Returns 0 on success, -1 on realloc failure.
 */
static int exception_array_grow(vtx_spec_elim_result_t *result)
{
    uint32_t new_cap = result->exception_capacity * 2;
    vtx_exception_observation_t *new_arr = (vtx_exception_observation_t *)realloc(
        result->exceptions, new_cap * sizeof(vtx_exception_observation_t));
    if (new_arr == NULL) return -1;

    result->exceptions = new_arr;
    result->exception_capacity = new_cap;
    return 0;
}

/* ========================================================================== */
/* Internal: find a branch observation by bytecode PC                          */
/* ========================================================================== */

/**
 * Linear scan for a branch observation with the given bytecode_pc.
 * Returns the index, or (uint32_t)-1 if not found.
 */
static uint32_t find_branch_index(const vtx_spec_elim_result_t *result,
                                    uint32_t bytecode_pc)
{
    for (uint32_t i = 0; i < result->branch_count; i++) {
        if (result->branches[i].bytecode_pc == bytecode_pc) {
            return i;
        }
    }
    return (uint32_t)-1;
}

/* ========================================================================== */
/* Internal: find an exception observation by call site PC                     */
/* ========================================================================== */

/**
 * Linear scan for an exception observation with the given call_site_pc.
 * Returns the index, or (uint32_t)-1 if not found.
 */
static uint32_t find_exception_index(const vtx_spec_elim_result_t *result,
                                       uint32_t call_site_pc)
{
    for (uint32_t i = 0; i < result->exception_count; i++) {
        if (result->exceptions[i].call_site_pc == call_site_pc) {
            return i;
        }
    }
    return (uint32_t)-1;
}

/* ========================================================================== */
/* Internal: recompute branch stability                                        */
/* ========================================================================== */

/**
 * Recompute the hot_frequency and is_stable flag for a branch
 * observation. Called after updating hot_count or cold_count.
 */
static void recompute_branch_stability(vtx_branch_observation_t *obs)
{
    uint64_t total = obs->hot_count + obs->cold_count;

    if (total == 0) {
        obs->hot_frequency = 0.0;
        obs->is_stable = false;
        return;
    }

    obs->hot_frequency = (double)obs->hot_count / (double)total;

    /*
     * A branch is stable when:
     *   1. We have enough observations (>= VTX_SPEC_ELIM_MIN_OBSERVATIONS)
     *   2. The hot direction dominates (hot_frequency >= threshold)
     *
     * The minimum observation threshold prevents premature elimination
     * based on too few samples. Even if a branch is 10/10 in one
     * direction, that's not statistically significant.
     */
    obs->is_stable = (total >= VTX_SPEC_ELIM_MIN_OBSERVATIONS) &&
                     (obs->hot_frequency >= VTX_SPEC_ELIM_BRANCH_THRESHOLD);
}

/* ========================================================================== */
/* Internal: recompute exception no-throw status                               */
/* ========================================================================== */

/**
 * Recompute the no_throw_rate and is_no_throw flag for an exception
 * observation. Called after updating total_calls or throw_count.
 */
static void recompute_exception_no_throw(vtx_exception_observation_t *obs)
{
    if (obs->total_calls == 0) {
        obs->no_throw_rate = 0.0;
        obs->is_no_throw = false;
        return;
    }

    uint64_t no_throw_count = obs->total_calls - obs->throw_count;
    obs->no_throw_rate = (double)no_throw_count / (double)obs->total_calls;

    /*
     * A call site is no-throw when:
     *   1. We have enough observations (>= VTX_SPEC_ELIM_MIN_OBSERVATIONS)
     *   2. The no-throw rate is very high (>= 99.9%)
     *
     * The 99.9% threshold is more conservative than the branch threshold
     * (95%) because exception elimination is riskier: a missed exception
     * can crash the program, while a missed branch just causes a deopt.
     */
    obs->is_no_throw = (obs->total_calls >= VTX_SPEC_ELIM_MIN_OBSERVATIONS) &&
                       (obs->no_throw_rate >= VTX_SPEC_ELIM_NO_THROW_THRESHOLD);
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_spec_elim_init(vtx_spec_elim_result_t *result)
{
    if (result == NULL) return -1;

    /* Allocate branch observation array */
    result->branches = (vtx_branch_observation_t *)malloc(
        VTX_SPEC_ELIM_INITIAL_CAPACITY * sizeof(vtx_branch_observation_t));
    if (result->branches == NULL) return -1;

    /* Allocate exception observation array */
    result->exceptions = (vtx_exception_observation_t *)malloc(
        VTX_SPEC_ELIM_INITIAL_CAPACITY * sizeof(vtx_exception_observation_t));
    if (result->exceptions == NULL) {
        free(result->branches);
        result->branches = NULL;
        return -1;
    }

    result->branch_count = 0;
    result->branch_capacity = VTX_SPEC_ELIM_INITIAL_CAPACITY;
    result->exception_count = 0;
    result->exception_capacity = VTX_SPEC_ELIM_INITIAL_CAPACITY;

    /* Statistics */
    result->total_branches_observed = 0;
    result->stable_branch_count = 0;
    result->total_calls_observed = 0;
    result->no_throw_call_count = 0;
    result->estimated_code_size_saved = 0;
    result->estimated_mispredictions_saved = 0;

    return 0;
}

void vtx_spec_elim_destroy(vtx_spec_elim_result_t *result)
{
    if (result == NULL) return;

    if (result->branches != NULL) {
        free(result->branches);
        result->branches = NULL;
    }
    if (result->exceptions != NULL) {
        free(result->exceptions);
        result->exceptions = NULL;
    }

    result->branch_count = 0;
    result->branch_capacity = 0;
    result->exception_count = 0;
    result->exception_capacity = 0;
}

/* ========================================================================== */
/* Branch observation                                                          */
/* ========================================================================== */

void vtx_spec_elim_observe_branch(vtx_spec_elim_result_t *result,
                                    uint32_t bytecode_pc,
                                    bool taken)
{
    if (result == NULL) return;

    /* Check for duplicate: update existing observation */
    uint32_t idx = find_branch_index(result, bytecode_pc);
    if (idx != (uint32_t)-1) {
        vtx_branch_observation_t *obs = &result->branches[idx];

        /*
         * Update the hot/cold counts. The "hot direction" is the direction
         * that has been observed more frequently overall. We track counts
         * for the hot direction and cold direction separately.
         *
         * On each observation:
         *   - If this observation matches the current hot direction,
         *     increment hot_count.
         *   - Otherwise, increment cold_count.
         *   - After updating, check if the hot direction should flip
         *     (i.e., the cold direction now has more observations).
         *   - Recompute stability.
         */
        uint32_t direction = taken ? 1 : 0;

        if (direction == obs->hot_direction) {
            if (obs->hot_count < UINT64_MAX) {
                obs->hot_count++;
            }
        } else {
            if (obs->cold_count < UINT64_MAX) {
                obs->cold_count++;
            }
        }

        /*
         * Check if the hot direction should flip. This happens when the
         * cold direction now has more observations than the hot direction.
         * When flipping:
         *   - Swap hot and cold counts
         *   - Update hot_direction
         */
        if (obs->cold_count > obs->hot_count) {
            uint64_t tmp_count = obs->hot_count;
            obs->hot_count = obs->cold_count;
            obs->cold_count = tmp_count;
            obs->hot_direction = direction;
        }

        /* Recompute stability */
        bool was_stable = obs->is_stable;
        recompute_branch_stability(obs);

        /* Update stable count statistics */
        if (!was_stable && obs->is_stable) {
            result->stable_branch_count++;
            /*
             * Rough estimate: each eliminated cold path saves ~64 bytes
             * of code (average cold path length in x86-64). This is a
             * conservative estimate used for logging/telemetry only.
             */
            result->estimated_code_size_saved += 64;
            /*
             * Estimate saved mispredictions: if the branch was predicted
             * incorrectly even 1% of the time (the cold direction), that's
             * ~10 mispredictions per 1000 executions. We estimate based
             * on the cold frequency.
             */
            double cold_freq = 1.0 - obs->hot_frequency;
            result->estimated_mispredictions_saved +=
                (uint32_t)(cold_freq * 1000.0);
        } else if (was_stable && !obs->is_stable) {
            if (result->stable_branch_count > 0) {
                result->stable_branch_count--;
            }
            /* Adjust estimates (approximate — may not exactly undo) */
            if (result->estimated_code_size_saved >= 64) {
                result->estimated_code_size_saved -= 64;
            }
        }

        return;
    }

    /* New branch observation: create a new entry */
    if (result->branch_count >= result->branch_capacity) {
        if (branch_array_grow(result) != 0) return; /* silently skip on OOM */
    }

    vtx_branch_observation_t *obs = &result->branches[result->branch_count];
    memset(obs, 0, sizeof(*obs));

    obs->bytecode_pc = bytecode_pc;
    obs->hot_direction = taken ? 1 : 0;
    obs->hot_count = 1;         /* first observation is always "hot" */
    obs->cold_count = 0;
    obs->hot_frequency = 1.0;   /* 1/1 = 100% — not yet stable (too few obs) */
    obs->is_stable = false;     /* need >= MIN_OBSERVATIONS before speculating */
    obs->guard_id = 0;
    obs->guard_installed = false;

    result->branch_count++;
    result->total_branches_observed++;
}

/* ========================================================================== */
/* Exception observation                                                       */
/* ========================================================================== */

void vtx_spec_elim_observe_call(vtx_spec_elim_result_t *result,
                                  uint32_t call_site_pc,
                                  bool threw_exception)
{
    if (result == NULL) return;

    /* Check for duplicate: update existing observation */
    uint32_t idx = find_exception_index(result, call_site_pc);
    if (idx != (uint32_t)-1) {
        vtx_exception_observation_t *obs = &result->exceptions[idx];

        /* Saturating increment of total calls */
        if (obs->total_calls < UINT64_MAX) {
            obs->total_calls++;
        }

        /* Track exceptions */
        if (threw_exception) {
            if (obs->throw_count < UINT64_MAX) {
                obs->throw_count++;
            }
        }

        /* Recompute no-throw rate and stability */
        bool was_no_throw = obs->is_no_throw;
        recompute_exception_no_throw(obs);

        /* Update no-throw count statistics */
        if (!was_no_throw && obs->is_no_throw) {
            result->no_throw_call_count++;
            /*
             * Rough estimate: each eliminated exception edge saves ~32 bytes
             * of code (exception handler stub + landing pad + stack unwinding
             * metadata). This is a conservative estimate used for
             * logging/telemetry only.
             */
            result->estimated_code_size_saved += 32;
        } else if (was_no_throw && !obs->is_no_throw) {
            if (result->no_throw_call_count > 0) {
                result->no_throw_call_count--;
            }
            if (result->estimated_code_size_saved >= 32) {
                result->estimated_code_size_saved -= 32;
            }
        }

        return;
    }

    /* New exception observation: create a new entry */
    if (result->exception_count >= result->exception_capacity) {
        if (exception_array_grow(result) != 0) return; /* silently skip on OOM */
    }

    vtx_exception_observation_t *obs = &result->exceptions[result->exception_count];
    memset(obs, 0, sizeof(*obs));

    obs->call_site_pc = call_site_pc;
    obs->total_calls = 1;
    obs->throw_count = threw_exception ? 1 : 0;
    obs->no_throw_rate = threw_exception ? 0.0 : 1.0;
    obs->is_no_throw = false;  /* need >= MIN_OBSERVATIONS before speculating */
    obs->guard_id = 0;
    obs->guard_installed = false;

    result->exception_count++;
    result->total_calls_observed++;
}

/* ========================================================================== */
/* Branch queries                                                              */
/* ========================================================================== */

bool vtx_spec_elim_is_branch_stable(const vtx_spec_elim_result_t *result,
                                      uint32_t bytecode_pc)
{
    if (result == NULL) return false;

    uint32_t idx = find_branch_index(result, bytecode_pc);
    if (idx == (uint32_t)-1) return false;

    return result->branches[idx].is_stable;
}

uint32_t vtx_spec_elim_branch_direction(const vtx_spec_elim_result_t *result,
                                          uint32_t bytecode_pc)
{
    if (result == NULL) return 0;

    uint32_t idx = find_branch_index(result, bytecode_pc);
    if (idx == (uint32_t)-1) return 0;

    const vtx_branch_observation_t *obs = &result->branches[idx];
    if (!obs->is_stable) return 0;

    return obs->hot_direction;
}

const vtx_branch_observation_t *vtx_spec_elim_lookup_branch(
    const vtx_spec_elim_result_t *result, uint32_t bytecode_pc)
{
    if (result == NULL) return NULL;

    uint32_t idx = find_branch_index(result, bytecode_pc);
    if (idx == (uint32_t)-1) return NULL;

    return &result->branches[idx];
}

/* ========================================================================== */
/* Exception queries                                                           */
/* ========================================================================== */

bool vtx_spec_elim_is_no_throw(const vtx_spec_elim_result_t *result,
                                 uint32_t call_site_pc)
{
    if (result == NULL) return false;

    uint32_t idx = find_exception_index(result, call_site_pc);
    if (idx == (uint32_t)-1) return false;

    return result->exceptions[idx].is_no_throw;
}

const vtx_exception_observation_t *vtx_spec_elim_lookup_exception(
    const vtx_spec_elim_result_t *result, uint32_t call_site_pc)
{
    if (result == NULL) return NULL;

    uint32_t idx = find_exception_index(result, call_site_pc);
    if (idx == (uint32_t)-1) return NULL;

    return &result->exceptions[idx];
}

/* ========================================================================== */
/* Guard management                                                            */
/* ========================================================================== */

int vtx_spec_elim_install_branch_guard(vtx_spec_elim_result_t *result,
                                         uint32_t bytecode_pc,
                                         uint32_t guard_id)
{
    if (result == NULL) return -1;

    uint32_t idx = find_branch_index(result, bytecode_pc);
    if (idx == (uint32_t)-1) return -1;

    vtx_branch_observation_t *obs = &result->branches[idx];
    obs->guard_id = guard_id;
    obs->guard_installed = true;

    return 0;
}

int vtx_spec_elim_install_exception_guard(vtx_spec_elim_result_t *result,
                                            uint32_t call_site_pc,
                                            uint32_t guard_id)
{
    if (result == NULL) return -1;

    uint32_t idx = find_exception_index(result, call_site_pc);
    if (idx == (uint32_t)-1) return -1;

    vtx_exception_observation_t *obs = &result->exceptions[idx];
    obs->guard_id = guard_id;
    obs->guard_installed = true;

    return 0;
}

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

void vtx_spec_elim_get_stats(const vtx_spec_elim_result_t *result,
                               uint32_t *stable_branches,
                               uint32_t *no_throw_calls,
                               uint64_t *code_size_saved)
{
    if (result == NULL) {
        if (stable_branches) *stable_branches = 0;
        if (no_throw_calls)  *no_throw_calls = 0;
        if (code_size_saved) *code_size_saved = 0;
        return;
    }

    if (stable_branches) *stable_branches = result->stable_branch_count;
    if (no_throw_calls)  *no_throw_calls = result->no_throw_call_count;
    if (code_size_saved) *code_size_saved = result->estimated_code_size_saved;
}
