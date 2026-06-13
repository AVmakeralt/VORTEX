#ifndef VORTEX_PEA_SPECULATIVE_PEA_H
#define VORTEX_PEA_SPECULATIVE_PEA_H

/**
 * VORTEX Speculative PEA + Lock Elision
 *
 * This module extends the standard Partial Escape Analysis with two
 * profile-guided speculative optimizations:
 *
 *   1. Speculative PEA: For allocations where the standard PEA cannot
 *      prove NoEscape but profiling suggests the object usually doesn't
 *      escape at runtime, speculate that it doesn't escape and add a
 *      guard. If the guard fires (the object actually escapes), deopt
 *      and recompile without the speculation.
 *
 *   2. Speculative Lock Elision: For synchronized methods/blocks where
 *      profiling shows zero (or near-zero) contention, elide the monitor
 *      enter/exit and add a guard that fires if contention is observed.
 *      On guard failure, deopt and recompile with the lock restored.
 *
 * Both optimizations are "zero-cost deopt" features: when the speculation
 * holds (which it usually does, because we only speculate when profiling
 * is confident), the optimized code runs faster than the unoptimized
 * version. When the speculation fails, we pay the deopt cost and fall
 * back to the conservative version.
 *
 * === Speculative PEA ===
 *
 * Standard PEA is conservative: it only scalar-replaces objects that it
 * can PROVABLY prove don't escape (NoEscape). Many objects "technically"
 * escape — e.g., they are passed as arguments to methods — but in practice
 * their identity is never observed by the callee. The callee only reads
 * their fields and never stores the object reference anywhere.
 *
 * Speculative PEA bridges this gap: it uses value profiling data to
 * determine which escaping allocations are likely to be non-escaping in
 * practice, and speculates that they don't escape. A guard is added at
 * each point where the object could potentially have its identity
 * observed. If the guard fires, deopt → recompile without the speculation.
 *
 * Example:
 *   Pair p = new Pair(x, y);      // PEA says ArgEscape (passed to process())
 *   int result = process(p);       // process() only reads p.x and p.y
 *   return p.x + p.y;             // could be scalar-replaced if p doesn't escape
 *
 *   After speculative PEA:
 *   // Guard: p's identity is never observed (trap if it is)
 *   int local_x = x, local_y = y; // scalar replacement
 *   int result = process_fields(local_x, local_y); // rewrite call
 *   return local_x + local_y;      // no allocation, no memory access
 *
 * === Speculative Lock Elision ===
 *
 * Synchronized methods/blocks always acquire and release monitors, even
 * when there's no contention. This is unnecessary overhead for
 * single-threaded or low-contention code paths. Lock elision removes the
 * monitor enter/exit when profiling shows the lock is never contended.
 *
 * Example:
 *   synchronized void update(SharedState s) {
 *       s.counter++;  // Atomic operation
 *   }
 *
 *   After speculative lock elision:
 *   // Guard: no contention on this monitor (trap if contention observed)
 *   void update(SharedState s) {
 *       s.counter++;  // Plain increment — no lock, no atomic
 *   }
 *   // If another thread contends → guard fires → deopt → synchronized version
 *
 * === Integration with Guard Metadata ===
 *
 * Both optimizations integrate with the VORTEX guard metadata system.
 * Each speculative decision is associated with a guard_id that links
 * to a vtx_guard_meta_t entry. This enables:
 *   - Adaptive strength transitions (FastCheck → FullCheck → DeoptAlways)
 *   - Sampling-based profiling (zero-cost deopt hot path)
 *   - Deoptless continuation versions (avoid full deopt on guard failure)
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "pea/analysis.h"
#include "guard/value_profile.h"
#include "runtime/arena.h"

/* ========================================================================== */
/* Speculation confidence levels                                                */
/* ========================================================================== */

/**
 * Speculation confidence levels for speculative PEA.
 *
 * These levels control how aggressively we speculate on escape states.
 * Higher levels speculate on more objects but risk more deoptimizations.
 *
 * The level determines which proven escape states are eligible for
 * speculation and what no_escape_rate threshold is required:
 *
 *   NONE:       No speculation at all. Only proven NoEscape is SR-eligible.
 *               Equivalent to standard PEA. Zero deopt risk.
 *
 *   CONSERVATIVE: Speculate on ArgEscape objects only. Requires
 *               no_escape_rate >= 99%. Low deopt risk.
 *
 *   MODERATE:   Speculate on ArgEscape + some GlobalEscape. ArgEscape
 *               requires no_escape_rate >= 95%, GlobalEscape requires
 *               no_escape_rate >= 98%. Moderate deopt risk.
 *
 *   AGGRESSIVE: Speculate on any escaping object. Requires
 *               no_escape_rate >= 80%. High deopt risk.
 */
typedef enum {
    VTX_SPEC_PEA_NONE       = 0,  /* no speculation, only provable NoEscape */
    VTX_SPEC_PEA_CONSERVATIVE = 1, /* speculate on ArgEscape (frequency > 99%) */
    VTX_SPEC_PEA_MODERATE   = 2,  /* speculate on ArgEscape + some GlobalEscape (freq > 95%) */
    VTX_SPEC_PEA_AGGRESSIVE = 3   /* speculate on any object (frequency > 80%) */
} vtx_spec_pea_level_t;

/* Human-readable name for speculation level */
const char *vtx_spec_pea_level_name(vtx_spec_pea_level_t level);

/* ========================================================================== */
/* No-escape rate thresholds per speculation level                              */
/* ========================================================================== */

/**
 * Minimum no_escape_rate thresholds for each proven escape state and
 * speculation level. These are conservative enough to avoid excessive
 * deoptimization while capturing most stable patterns.
 *
 * The thresholds form a 2D table indexed by [level][proven_state]:
 *   - CONSERVATIVE + ArgEscape: 0.99 (only nearly-always-non-escaping)
 *   - CONSERVATIVE + GlobalEscape: not eligible (INFINITY)
 *   - MODERATE + ArgEscape: 0.95
 *   - MODERATE + GlobalEscape: 0.98
 *   - AGGRESSIVE + ArgEscape: 0.80
 *   - AGGRESSIVE + GlobalEscape: 0.90
 */
#define VTX_SPEC_RATE_CONSERVATIVE_ARG    0.99   /* 99% no-escape for ArgEscape at CONSERVATIVE */
#define VTX_SPEC_RATE_MODERATE_ARG        0.95   /* 95% no-escape for ArgEscape at MODERATE */
#define VTX_SPEC_RATE_MODERATE_GLOBAL     0.98   /* 98% no-escape for GlobalEscape at MODERATE */
#define VTX_SPEC_RATE_AGGRESSIVE_ARG      0.80   /* 80% no-escape for ArgEscape at AGGRESSIVE */
#define VTX_SPEC_RATE_AGGRESSIVE_GLOBAL   0.90   /* 90% no-escape for GlobalEscape at AGGRESSIVE */

/**
 * Minimum number of profile observations before a speculative decision
 * can be made. Prevents premature speculation based on too few samples.
 */
#define VTX_SPEC_PEA_MIN_OBSERVATIONS 100

/* ========================================================================== */
/* Per-allocation speculative PEA decision                                      */
/* ========================================================================== */

/**
 * Per-allocation speculative PEA decision.
 *
 * For each allocation where we decide to speculate, this struct records:
 *   - The proven escape state (from standard PEA)
 *   - The speculated escape state (always NoEscape for now)
 *   - The confidence level that justified this speculation
 *   - The guard metadata ID for the speculation guard
 *   - Profile data that supports the speculation
 */
typedef struct {
    vtx_nodeid_t        alloc_node_id;     /* the allocation node */
    vtx_escape_state_t  proven_state;      /* what PEA proved (e.g., ArgEscape) */
    vtx_escape_state_t  speculated_state;  /* what we're speculating (e.g., NoEscape) */
    vtx_spec_pea_level_t level;            /* confidence level */

    uint32_t            guard_id;          /* guard metadata ID (0 = no guard yet) */

    /* Profile data supporting this speculation.
     * These come from value profiling at the allocation site. */
    uint64_t            total_observations; /* times this alloc site was observed */
    uint64_t            no_escape_obs;      /* times the object didn't actually escape */
    double              no_escape_rate;     /* no_escape_obs / total_observations */

    /* Whether this speculation has been validated by installing a guard
     * in the compiled code. Before guard_installed is true, the speculation
     * is only a decision — no runtime check exists yet. */
    bool                guard_installed;
} vtx_spec_pea_decision_t;

/* ========================================================================== */
/* Speculative PEA analysis result                                              */
/* ========================================================================== */

#define VTX_SPEC_PEA_INITIAL_CAPACITY 16

/**
 * Speculative PEA result: extends the standard PEA analysis with
 * speculative escape states for allocations that PEA couldn't prove
 * NoEscape but profiling suggests don't escape in practice.
 *
 * The speculative_escape_map contains the effective escape states:
 *   - For allocations with speculative decisions: the speculated state
 *     (NoEscape) instead of the proven state (ArgEscape/GlobalEscape)
 *   - For all other allocations: same as the base analysis
 *
 * Downstream passes (scalar replacement, virtual object tracking,
 * materialization) should use the speculative_escape_map instead of
 * the base escape_map when speculative PEA is enabled.
 */
typedef struct {
    vtx_pea_analysis_t       base_analysis;    /* the standard PEA result */
    vtx_spec_pea_decision_t *spec_decisions;   /* speculative decisions array */
    uint32_t                 spec_count;        /* number of speculative decisions */
    uint32_t                 spec_capacity;     /* allocated capacity */

    /* Override escape map: for nodes with speculative decisions,
     * this map contains the speculated (lower) escape state instead
     * of the proven (higher) escape state. Downstream passes should
     * query this map for effective escape states. */
    vtx_escape_map_t         speculative_escape_map;

    /* Statistics */
    uint32_t                 total_speculated;    /* allocations with speculative NoEscape */
    uint32_t                 total_proven;        /* allocations with proven NoEscape */
    uint32_t                 potential_sr_count;  /* total SR-eligible (proven + speculated) */

    /* Speculation level used for this analysis */
    vtx_spec_pea_level_t     level;
} vtx_spec_pea_analysis_t;

/* ========================================================================== */
/* Lock elision configuration                                                   */
/* ========================================================================== */

/**
 * Contention rate threshold for lock elision eligibility.
 * If a monitor's contention rate is below this threshold, it is
 * eligible for lock elision. Set to 0.1% — this means that for
 * every 1000 monitor acquisitions, at most 1 can be contended.
 */
#define VTX_LOCK_CONTENTION_THRESHOLD 0.001

/**
 * Minimum number of monitor acquisitions before a lock elision
 * decision can be made. Prevents premature elision based on too
 * few samples.
 */
#define VTX_LOCK_MIN_OBSERVATIONS     1000

/**
 * Initial capacity for the lock contention profile array.
 */
#define VTX_LOCK_PROFILE_INITIAL_CAPACITY 16

/**
 * Default sampling interval for lock contention profiling.
 * Only check contention every Nth acquisition to reduce overhead.
 */
#define VTX_LOCK_SAMPLE_INTERVAL_DEFAULT 64

/**
 * Sampling interval for stable (low-contention) monitor sites.
 */
#define VTX_LOCK_SAMPLE_INTERVAL_STABLE 256

/**
 * Sampling interval for unstable (contention observed) sites.
 */
#define VTX_LOCK_SAMPLE_INTERVAL_UNSTABLE 16

/* ========================================================================== */
/* Lock contention profile                                                      */
/* ========================================================================== */

/**
 * Monitor contention profile for lock elision.
 *
 * Tracks the contention rate for a single monitor acquisition site.
 * When the contention rate is below VTX_LOCK_CONTENTION_THRESHOLD
 * and total_acquisitions >= VTX_LOCK_MIN_OBSERVATIONS, the site
 * becomes eligible for speculative lock elision.
 *
 * Sampling: To avoid overhead on the hot path, contention is only
 * checked every Nth acquisition (similar to guard metadata sampling).
 * The sample_interval adapts: stable sites (no contention) get longer
 * intervals; sites that recently observed contention get shorter ones.
 */
typedef struct {
    uint32_t          monitor_site_id;    /* identifies the monitor acquisition site */
    uint64_t          total_acquisitions; /* times this monitor was acquired */
    uint64_t          contention_count;   /* times acquisition was contested */

    /* Derived: contention_count / total_acquisitions.
     * Cached to avoid recomputation on every query. */
    double            contention_rate;

    /* Sampling state */
    uint32_t          sample_counter;      /* countdown to next observation */
    uint32_t          sample_interval;     /* reset value for sample_counter */

    /* Whether lock elision is speculative for this site */
    bool              is_eligible;         /* contention_rate < threshold && total >= min */
    uint32_t          guard_id;            /* guard for speculation (0 = no guard) */
    bool              guard_installed;     /* whether guard has been emitted */
} vtx_lock_contention_profile_t;

/* ========================================================================== */
/* Lock elision result                                                          */
/* ========================================================================== */

/**
 * Speculative lock elision result.
 *
 * Contains per-site contention profiles and summary statistics.
 * After profiling, sites with is_eligible=true can have their
 * monitor enter/exit elided with a guard.
 */
typedef struct {
    vtx_lock_contention_profile_t *profiles;  /* per-site contention profiles */
    uint32_t                       profile_count;
    uint32_t                       profile_capacity;

    /* Statistics */
    uint32_t                       total_sites;      /* total synchronized sites */
    uint32_t                       eligible_sites;   /* sites eligible for elision */
    uint32_t                       elided_sites;     /* sites where lock was actually elided */
} vtx_lock_elision_result_t;

/* ========================================================================== */
/* Speculative PEA API                                                          */
/* ========================================================================== */

/**
 * Run speculative PEA: extends standard PEA with speculation on
 * allocations that couldn't be proven NoEscape.
 *
 * Algorithm:
 *   1. Run standard PEA: vtx_pea_run(graph, arena) → proven escape states
 *   2. For each allocation with proven_state > VTX_ESCAPE_NONE:
 *      a. Look up value profile data for this allocation site
 *      b. Compute no_escape_rate from profile observations
 *      c. If no_escape_rate >= threshold (depends on level and proven_state):
 *         - Create a speculative decision: speculated_state = NoEscape
 *         - Update the speculative escape map
 *   3. Build the speculative escape map by copying the base map and
 *      overriding speculative allocations with their speculated state
 *
 * The speculation level controls which proven states are eligible and
 * the required no_escape_rate thresholds:
 *
 *   CONSERVATIVE: Only ArgEscape, no_escape_rate >= 99%
 *   MODERATE:     ArgEscape >= 95%, GlobalEscape >= 98%
 *   AGGRESSIVE:   ArgEscape >= 80%, GlobalEscape >= 90%
 *
 * When value_profiles is NULL, no speculation is possible — the result
 * is equivalent to standard PEA with no speculative decisions.
 *
 * @param graph          The SoN graph to analyze
 * @param arena          Arena for allocating the result
 * @param level          Speculation confidence level
 * @param value_profiles Value profile table (may be NULL)
 * @return               Speculative PEA result, or NULL on failure
 */
vtx_spec_pea_analysis_t *vtx_spec_pea_run(vtx_graph_t *graph,
                                            vtx_arena_t *arena,
                                            vtx_spec_pea_level_t level,
                                            const vtx_value_profile_table_t *value_profiles);

/**
 * Get the effective escape state for an allocation, taking speculation
 * into account.
 *
 * Returns the speculated state (NoEscape) if a speculative decision
 * exists for this allocation, otherwise returns the proven state from
 * the base PEA analysis.
 *
 * For non-allocation nodes, returns VTX_ESCAPE_GLOBAL (conservative).
 *
 * @param analysis       Speculative PEA analysis result
 * @param alloc_node_id  Node ID of the allocation
 * @return               Effective escape state
 */
vtx_escape_state_t vtx_spec_pea_effective_escape(
    const vtx_spec_pea_analysis_t *analysis,
    vtx_nodeid_t alloc_node_id);

/**
 * Check if an allocation is speculatively scalar-replaceable.
 *
 * Returns true if the effective escape state for this allocation is
 * NoEscape (either proven or speculated).
 *
 * @param analysis       Speculative PEA analysis result
 * @param alloc_node_id  Node ID of the allocation
 * @return               true if the allocation is SR-eligible
 */
bool vtx_spec_pea_is_speculative_sr(const vtx_spec_pea_analysis_t *analysis,
                                      vtx_nodeid_t alloc_node_id);

/**
 * Check if an allocation has a speculative decision (i.e., its escape
 * state was downgraded from the proven state based on profiling).
 *
 * @param analysis       Speculative PEA analysis result
 * @param alloc_node_id  Node ID of the allocation
 * @return               true if there is a speculative decision for this allocation
 */
bool vtx_spec_pea_has_decision(const vtx_spec_pea_analysis_t *analysis,
                                 vtx_nodeid_t alloc_node_id);

/**
 * Get the speculative decision for an allocation.
 *
 * Returns the decision struct, or NULL if no speculative decision exists
 * for this allocation.
 *
 * @param analysis       Speculative PEA analysis result
 * @param alloc_node_id  Node ID of the allocation
 * @return               Decision struct, or NULL if no speculation
 */
const vtx_spec_pea_decision_t *vtx_spec_pea_get_decision(
    const vtx_spec_pea_analysis_t *analysis,
    vtx_nodeid_t alloc_node_id);

/**
 * Install a guard for a speculative PEA decision.
 *
 * Called after the compiled code is emitted and the guard's native
 * offset is known. Links the guard metadata ID to the speculative
 * decision so that guard failures can be attributed to the correct
 * speculation.
 *
 * @param analysis       Speculative PEA analysis result (mutable)
 * @param alloc_node_id  Node ID of the allocation
 * @param guard_id       Guard metadata ID from the guard metadata table
 * @return               0 on success, -1 on failure
 */
int vtx_spec_pea_install_guard(vtx_spec_pea_analysis_t *analysis,
                                 vtx_nodeid_t alloc_node_id,
                                 uint32_t guard_id);

/**
 * Destroy a speculative PEA analysis and free memory.
 *
 * Frees the spec_decisions array and the speculative_escape_map.
 * Does NOT free the base_analysis (it was arena-allocated and will
 * be freed when the arena is destroyed).
 *
 * @param analysis  Speculative PEA analysis to destroy
 */
void vtx_spec_pea_analysis_destroy(vtx_spec_pea_analysis_t *analysis);

/* ========================================================================== */
/* Lock Elision API                                                            */
/* ========================================================================== */

/**
 * Initialize a lock elision result.
 *
 * Allocates the profiles array with initial capacity.
 * Returns 0 on success, -1 on failure.
 *
 * @param result  Lock elision result to initialize
 * @return        0 on success, -1 on failure
 */
int vtx_lock_elision_init(vtx_lock_elision_result_t *result);

/**
 * Destroy a lock elision result and free memory.
 *
 * @param result  Lock elision result to destroy
 */
void vtx_lock_elision_result_destroy(vtx_lock_elision_result_t *result);

/**
 * Record a monitor acquisition event (sampling-based).
 *
 * This is the hot-path function for recording contention. It uses
 * sampling to reduce overhead: only every Nth acquisition updates
 * the profile. The sample counter is decremented on each call;
 * when it hits zero, the full update runs and the counter resets.
 *
 * When the sample boundary is reached:
 *   1. Increment total_acquisitions (by the number of skipped samples + 1)
 *   2. If is_contended, increment contention_count
 *   3. Recompute contention_rate
 *   4. Update is_eligible flag
 *   5. Adapt sampling interval based on contention stability
 *
 * @param result          Lock elision result
 * @param monitor_site_id Identifies the monitor acquisition site
 * @param is_contended    Whether this acquisition was contended
 */
void vtx_lock_elision_record_acquisition(vtx_lock_elision_result_t *result,
                                           uint32_t monitor_site_id,
                                           bool is_contended);

/**
 * Check if a monitor site is eligible for lock elision.
 *
 * A site is eligible when:
 *   - contention_rate < VTX_LOCK_CONTENTION_THRESHOLD (0.1%)
 *   - total_acquisitions >= VTX_LOCK_MIN_OBSERVATIONS (1000)
 *
 * @param result          Lock elision result
 * @param monitor_site_id Identifies the monitor acquisition site
 * @return                true if the site is eligible for lock elision
 */
bool vtx_lock_elision_is_eligible(const vtx_lock_elision_result_t *result,
                                    uint32_t monitor_site_id);

/**
 * Install a guard for a lock elision speculation.
 *
 * Called after the compiled code is emitted and the guard's native
 * offset is known. Links the guard metadata ID to the contention
 * profile so that guard failures can be attributed to the correct
 * speculation.
 *
 * @param result          Lock elision result
 * @param monitor_site_id Identifies the monitor acquisition site
 * @param guard_id        Guard metadata ID from the guard metadata table
 * @return                0 on success, -1 on failure
 */
int vtx_lock_elision_install_guard(vtx_lock_elision_result_t *result,
                                     uint32_t monitor_site_id,
                                     uint32_t guard_id);

/**
 * Query lock elision statistics.
 *
 * @param result          Lock elision result
 * @param total_sites     Output: total synchronized sites (may be NULL)
 * @param eligible_sites  Output: sites eligible for elision (may be NULL)
 * @param elided_sites    Output: sites where lock was actually elided (may be NULL)
 */
void vtx_lock_elision_get_stats(const vtx_lock_elision_result_t *result,
                                  uint32_t *total_sites,
                                  uint32_t *eligible_sites,
                                  uint32_t *elided_sites);

/**
 * Look up the contention profile for a monitor site.
 *
 * Returns the profile, or NULL if no profile exists for the site.
 *
 * @param result          Lock elision result
 * @param monitor_site_id Identifies the monitor acquisition site
 * @return                Contention profile, or NULL if not found
 */
const vtx_lock_contention_profile_t *vtx_lock_elision_get_profile(
    const vtx_lock_elision_result_t *result,
    uint32_t monitor_site_id);

#endif /* VORTEX_PEA_SPECULATIVE_PEA_H */
