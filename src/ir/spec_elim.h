#ifndef VORTEX_IR_SPEC_ELIM_H
#define VORTEX_IR_SPEC_ELIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"

/**
 * VORTEX Speculative Dead Code Elimination (Path Specialization)
 *             and Speculative Exception Elimination
 *
 * This module implements two profile-guided speculative optimizations
 * from the "Maximum Aggression" architecture:
 *
 *   2G -- Speculative Dead Code Elimination (Path Specialization):
 *        Eliminate entire code paths that have never been executed at
 *        runtime. When a branch direction is stable (one direction taken
 *        >= 95% of the time), the compiled code includes only the hot
 *        path, with a trap-based guard that deopts if the cold path is
 *        ever taken. This produces much smaller code, better I-cache
 *        utilization, and eliminates branch mispredictions on the hot path.
 *
 *        Example:
 *          void process(Request req) {
 *              if (req.type == HTTP_GET)  { ... }   95% of traffic
 *              else if (req.type == HTTP_POST) { ... }   4%
 *              else if (req.type == HTTP_PUT)  { ... }   0.9%
 *              else { 20 other HTTP methods }            0.1% -- HUGE CODE
 *          }
 *
 *          After speculative dead code elimination:
 *          -- Generate a version with ONLY the HTTP_GET path
 *          -- Guard: req.type == HTTP_GET (trap-based)
 *          -- All other paths -> deopt -> interpreter handles them
 *          -- Result: MUCH smaller code, better I-cache, no mispredictions
 *
 *   2H -- Speculative Exception Elimination:
 *        Remove all exception handling code from hot paths. When profiling
 *        shows that a call site never (or almost never) throws, the
 *        compiled code omits the exception edge entirely. If the call
 *        ever throws, a trap fires and the interpreter catches the exception.
 *
 *        Example:
 *          After: profile says none of these ever throw
 *          Guard: "no exception" (trap-based, implicit no-throw assumption)
 *          a = obj.method1();   No exception edge in compiled code
 *          b = obj.method2();   No exception edge
 *          c = obj.method3();   No exception edge
 *          If ANY throws -> SIGSEGV/trap -> deopt -> interpreter catches
 *
 * === Integration with Guard Metadata ===
 *
 * Both optimizations integrate with the VORTEX guard metadata system.
 * Each speculative elimination is associated with a guard_id that links
 * to a vtx_guard_meta_t entry. This enables:
 *   - Adaptive strength transitions (FastCheck -> FullCheck -> DeoptAlways)
 *   - Sampling-based profiling (zero-cost deopt hot path)
 *   - Guard dependency tracking for O(dependents) invalidation on deopt
 *
 * === Thread safety ===
 *
 * NOT thread-safe. The caller must synchronize, same as existing
 * guard metadata (vtx_guard_meta_table_t) and speculative PEA.
 */

/* ========================================================================== */
/* Thresholds                                                                  */
/* ========================================================================== */

/**
 * Minimum branch frequency to consider a branch direction "stable" and
 * eligible for speculative elimination. A branch where one direction is
 * taken >= 95% of the time can have the cold path eliminated.
 *
 * The 95% threshold balances aggressiveness (capturing most stable
 * branches) against deopt risk (branches that flip direction > 5% of
 * the time will cause frequent deopts, negating the optimization).
 */
#define VTX_SPEC_ELIM_BRANCH_THRESHOLD   0.95

/**
 * Minimum no-throw rate for a call site to be eligible for speculative
 * exception elimination. A call site that doesn't throw >= 99.9% of the
 * time can have its exception edge removed.
 *
 * The 99.9% threshold is very conservative because exception elimination
 * is riskier than branch elimination: an unexpected exception that isn't
 * caught by the compiled code could cause a crash rather than just a
 * deopt. We need near-certainty that the call won't throw.
 */
#define VTX_SPEC_ELIM_NO_THROW_THRESHOLD 0.999

/**
 * Minimum number of observations before a speculative elimination
 * decision can be made. Prevents premature elimination based on too
 * few samples (e.g., a branch that has been taken 10/10 times in one
 * direction is not yet statistically stable).
 */
#define VTX_SPEC_ELIM_MIN_OBSERVATIONS   1000

/**
 * Initial capacity for the branch and exception observation arrays.
 * Most methods have < 50 branch points and < 30 call sites, so 32
 * is a reasonable starting point.
 */
#define VTX_SPEC_ELIM_INITIAL_CAPACITY   32

/* ========================================================================== */
/* Branch observation                                                          */
/* ========================================================================== */

/**
 * Branch observation: tracks which direction a branch takes over time.
 *
 * For each branch point (identified by bytecode_pc), we record:
 *   - Which direction is "hot" (taken more frequently)
 *   - How many times each direction was observed
 *   - The frequency of the hot direction (hot_count / total)
 *   - Whether the branch is "stable" (hot_frequency >= threshold)
 *   - Whether a guard has been installed for this elimination
 *
 * A branch observation transitions from "not stable" to "stable" when:
 *   1. Total observations >= VTX_SPEC_ELIM_MIN_OBSERVATIONS
 *   2. hot_frequency >= VTX_SPEC_ELIM_BRANCH_THRESHOLD
 *
 * Once stable, the branch direction can be speculatively fixed in
 * the compiled code with a guard that deopts if the cold direction
 * is ever taken.
 */
typedef struct {
    uint32_t bytecode_pc;        /* the branch point in bytecode */
    uint32_t hot_direction;      /* 0 = not taken, 1 = taken (observed direction) */
    uint64_t hot_count;          /* times the hot direction was taken */
    uint64_t cold_count;         /* times the cold direction was taken */
    double   hot_frequency;      /* hot_count / (hot_count + cold_count) */
    bool     is_stable;          /* hot_frequency >= VTX_SPEC_ELIM_BRANCH_THRESHOLD
                                    AND total >= VTX_SPEC_ELIM_MIN_OBSERVATIONS */
    uint32_t guard_id;           /* guard metadata ID for this elimination (0 = none) */
    bool     guard_installed;    /* whether guard has been emitted in compiled code */
} vtx_branch_observation_t;

/* ========================================================================== */
/* Exception observation                                                       */
/* ========================================================================== */

/**
 * Exception observation: tracks whether a call site throws exceptions.
 *
 * For each call site (identified by call_site_pc), we record:
 *   - Total times the call was executed
 *   - Times the call threw an exception
 *   - The no-throw rate (1 - throw_rate)
 *   - Whether the call is "no-throw" (no_throw_rate >= threshold)
 *   - Whether a guard has been installed for this elimination
 *
 * A call site is marked as "no-throw" when:
 *   1. Total calls >= VTX_SPEC_ELIM_MIN_OBSERVATIONS
 *   2. no_throw_rate >= VTX_SPEC_ELIM_NO_THROW_THRESHOLD (99.9%)
 *
 * Once marked as no-throw, the exception edge can be removed from the
 * compiled code. A guard is implicitly present: if the call throws,
 * the trap fires and the interpreter handles the exception.
 */
typedef struct {
    uint32_t call_site_pc;       /* the call site in bytecode */
    uint64_t total_calls;        /* total times this call was executed */
    uint64_t throw_count;        /* times this call threw an exception */
    double   no_throw_rate;      /* (total_calls - throw_count) / total_calls */
    bool     is_no_throw;        /* no_throw_rate >= VTX_SPEC_ELIM_NO_THROW_THRESHOLD
                                    AND total >= VTX_SPEC_ELIM_MIN_OBSERVATIONS */
    uint32_t guard_id;           /* guard metadata ID for exception elimination (0 = none) */
    bool     guard_installed;    /* whether guard has been emitted in compiled code */
} vtx_exception_observation_t;

/* ========================================================================== */
/* Speculative elimination result                                              */
/* ========================================================================== */

/**
 * Speculative elimination result: contains all branch and exception
 * observations that are stable enough for speculation.
 *
 * This structure is populated by the interpreter/profiler during
 * execution and consumed by the compiler during compilation. The
 * compiler queries it to determine which branches can be eliminated
 * and which call sites can have their exception edges removed.
 *
 * The branch and exception arrays use linear scan for lookup. This
 * is efficient because the tables are typically small (< 1000 entries
 * per method) and the lookup is only performed during compilation,
 * not on the hot execution path.
 *
 * Statistics track the total number of observations, stable branches,
 * and no-throw calls, as well as estimated code size savings from
 * the eliminations.
 */
typedef struct {
    /* Branch observations */
    vtx_branch_observation_t *branches;     /* array of branch observations */
    uint32_t                  branch_count;
    uint32_t                  branch_capacity;

    /* Exception observations */
    vtx_exception_observation_t *exceptions;  /* array of exception observations */
    uint32_t                    exception_count;
    uint32_t                    exception_capacity;

    /* Statistics */
    uint32_t total_branches_observed;       /* total distinct branch points seen */
    uint32_t stable_branch_count;           /* branches eligible for elimination */
    uint32_t total_calls_observed;          /* total distinct call sites seen */
    uint32_t no_throw_call_count;           /* calls eligible for exception elimination */
    uint64_t estimated_code_size_saved;     /* estimated bytes of eliminated code */
    uint32_t estimated_mispredictions_saved;/* branch mispredictions eliminated per 1K execs */
} vtx_spec_elim_result_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize a speculative elimination result.
 *
 * Allocates the branch and exception observation arrays with initial
 * capacity. All counters are zeroed.
 *
 * Returns 0 on success, -1 on failure (e.g., malloc failure).
 */
int vtx_spec_elim_init(vtx_spec_elim_result_t *result);

/**
 * Destroy a speculative elimination result and free all memory.
 *
 * Frees the branch and exception observation arrays and resets all
 * counters. The result struct itself is not freed (caller owns it).
 */
void vtx_spec_elim_destroy(vtx_spec_elim_result_t *result);

/* ========================================================================== */
/* Branch observation (called from interpreter/profiler)                        */
/* ========================================================================== */

/**
 * Record a branch observation at a bytecode branch point.
 *
 * Finds or creates the observation record for the given bytecode_pc
 * and updates the taken/not-taken counts. After updating, recomputes
 * the hot frequency and stability flag.
 *
 * The "hot direction" is the direction that has been observed more
 * frequently. If both directions have equal counts, direction 1
 * (taken) is preferred as the hot direction (branches are more
 * commonly taken than not taken in hot code).
 *
 * A branch becomes stable when:
 *   - hot_count + cold_count >= VTX_SPEC_ELIM_MIN_OBSERVATIONS
 *   - hot_frequency >= VTX_SPEC_ELIM_BRANCH_THRESHOLD
 *
 * @param result       Speculative elimination result
 * @param bytecode_pc  The branch point in bytecode
 * @param taken        true if the branch was taken, false if not taken
 */
void vtx_spec_elim_observe_branch(vtx_spec_elim_result_t *result,
                                    uint32_t bytecode_pc,
                                    bool taken);

/* ========================================================================== */
/* Exception observation (called from interpreter/profiler)                     */
/* ========================================================================== */

/**
 * Record a call site observation.
 *
 * Finds or creates the observation record for the given call_site_pc
 * and updates the total/throw counts. After updating, recomputes the
 * no-throw rate and no-throw flag.
 *
 * A call site becomes no-throw when:
 *   - total_calls >= VTX_SPEC_ELIM_MIN_OBSERVATIONS
 *   - no_throw_rate >= VTX_SPEC_ELIM_NO_THROW_THRESHOLD
 *
 * @param result          Speculative elimination result
 * @param call_site_pc    The call site in bytecode
 * @param threw_exception true if the call threw, false otherwise
 */
void vtx_spec_elim_observe_call(vtx_spec_elim_result_t *result,
                                  uint32_t call_site_pc,
                                  bool threw_exception);

/* ========================================================================== */
/* Branch queries                                                              */
/* ========================================================================== */

/**
 * Check if a branch is stable enough for speculative elimination.
 *
 * Returns true if the branch at bytecode_pc has been observed enough
 * times and one direction dominates (hot_frequency >= threshold).
 *
 * @param result       Speculative elimination result
 * @param bytecode_pc  The branch point to query
 * @return             true if the branch is stable and can be eliminated
 */
bool vtx_spec_elim_is_branch_stable(const vtx_spec_elim_result_t *result,
                                      uint32_t bytecode_pc);

/**
 * Get the hot direction of a stable branch.
 *
 * Returns the direction that should be unconditionally taken in the
 * compiled code:
 *   - 1 = taken (the branch should be compiled as always-taken)
 *   - 0 = not taken (the branch should be compiled as always-not-taken)
 *
 * Returns 0 if the branch is not stable or not observed.
 *
 * @param result       Speculative elimination result
 * @param bytecode_pc  The branch point to query
 * @return             Hot direction (0 or 1), or 0 if not stable
 */
uint32_t vtx_spec_elim_branch_direction(const vtx_spec_elim_result_t *result,
                                          uint32_t bytecode_pc);

/**
 * Look up the full branch observation for a bytecode PC.
 *
 * Returns a pointer to the branch observation, or NULL if no
 * observation exists for the given bytecode_pc.
 *
 * The returned pointer is valid until the result is destroyed or
 * the next observation is added (which may trigger reallocation).
 *
 * @param result       Speculative elimination result
 * @param bytecode_pc  The branch point to look up
 * @return             Branch observation, or NULL if not found
 */
const vtx_branch_observation_t *vtx_spec_elim_lookup_branch(
    const vtx_spec_elim_result_t *result, uint32_t bytecode_pc);

/* ========================================================================== */
/* Exception queries                                                           */
/* ========================================================================== */

/**
 * Check if a call site is eligible for speculative exception elimination.
 *
 * Returns true if the call site at call_site_pc has been observed enough
 * times and the no-throw rate is above the threshold (99.9%).
 *
 * @param result        Speculative elimination result
 * @param call_site_pc  The call site to query
 * @return              true if the call site can have its exception edge removed
 */
bool vtx_spec_elim_is_no_throw(const vtx_spec_elim_result_t *result,
                                 uint32_t call_site_pc);

/**
 * Look up the full exception observation for a call site PC.
 *
 * Returns a pointer to the exception observation, or NULL if no
 * observation exists for the given call_site_pc.
 *
 * The returned pointer is valid until the result is destroyed or
 * the next observation is added (which may trigger reallocation).
 *
 * @param result        Speculative elimination result
 * @param call_site_pc  The call site to look up
 * @return              Exception observation, or NULL if not found
 */
const vtx_exception_observation_t *vtx_spec_elim_lookup_exception(
    const vtx_spec_elim_result_t *result, uint32_t call_site_pc);

/* ========================================================================== */
/* Guard management                                                            */
/* ========================================================================== */

/**
 * Install a guard for a branch elimination.
 *
 * Called after the compiled code is emitted and the guard's native
 * offset is known. Links the guard metadata ID to the branch
 * observation so that guard failures can be attributed to the
 * correct speculative elimination.
 *
 * @param result       Speculative elimination result (mutable)
 * @param bytecode_pc  The branch point that was eliminated
 * @param guard_id     Guard metadata ID from the guard metadata table
 * @return             0 on success, -1 if the branch is not found
 */
int vtx_spec_elim_install_branch_guard(vtx_spec_elim_result_t *result,
                                         uint32_t bytecode_pc,
                                         uint32_t guard_id);

/**
 * Install a guard for an exception elimination.
 *
 * Called after the compiled code is emitted and the guard's native
 * offset is known. Links the guard metadata ID to the exception
 * observation so that guard failures can be attributed to the
 * correct speculative elimination.
 *
 * @param result        Speculative elimination result (mutable)
 * @param call_site_pc  The call site whose exception edge was eliminated
 * @param guard_id      Guard metadata ID from the guard metadata table
 * @return              0 on success, -1 if the call site is not found
 */
int vtx_spec_elim_install_exception_guard(vtx_spec_elim_result_t *result,
                                            uint32_t call_site_pc,
                                            uint32_t guard_id);

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

/**
 * Get speculative elimination statistics.
 *
 * Any of the output pointers may be NULL if the caller doesn't need
 * that particular statistic.
 *
 * @param result             Speculative elimination result
 * @param stable_branches    Output: number of branches eligible for elimination
 * @param no_throw_calls     Output: number of calls eligible for exception elim
 * @param code_size_saved    Output: estimated bytes of eliminated code
 */
void vtx_spec_elim_get_stats(const vtx_spec_elim_result_t *result,
                               uint32_t *stable_branches,
                               uint32_t *no_throw_calls,
                               uint64_t *code_size_saved);

#endif /* VORTEX_IR_SPEC_ELIM_H */
