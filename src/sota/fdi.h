#ifndef VORTEX_SOTA_FDI_H
#define VORTEX_SOTA_FDI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "inliner/feedback.h"
#include "profile/data.h"
#include "profile/phase.h"
#include "compile/version.h"
#include "runtime/arena.h"

/**
 * VORTEX SOTA — Feedback-Directed Inlining Recompilation
 *
 * Tracks inlining decision outcomes and triggers targeted recompilation
 * when inlining proves unprofitable. Maintains multiple compiled versions
 * with different inlining strategies, switching based on performance.
 *
 * When an inlined call site causes repeated deoptimizations:
 *   1. Record the call site as "unprofitable inline"
 *   2. Queue a recompilation of the method with that call site
 *      forced to no-inline
 *   3. Install the new version at the next safe point
 *   4. The old version remains active for threads still executing it
 *      (version coexistence via the version manager)
 *
 * When an un-inlined call site would benefit from inlining:
 *   1. Record the call site as "missed inline opportunity"
 *   2. Queue recompilation with that call site forced to inline
 *   3. Same version management as above
 *
 * The FDI system works closely with the inliner feedback module,
 * adding the ability to trigger recompilations based on feedback.
 */

/* ========================================================================== */
/* Per-method FDI record                                                       */
/* ========================================================================== */

typedef struct {
    uint32_t method_id;               /* method being tracked */
    bool     has_unprofitable_inlines; /* true if any inlined site is unprofitable */
    bool     has_missed_inlines;       /* true if any non-inlined site should be inlined */
    uint32_t unprofitable_site_count;  /* number of unprofitable inlined sites */
    uint32_t missed_site_count;        /* number of missed inline opportunities */

    /* Call sites forced to no-inline in the next recompilation */
    uint64_t *no_inline_sites;         /* array of call_site_ids to force no-inline */
    uint32_t  no_inline_count;
    uint32_t  no_inline_capacity;

    /* Call sites forced to inline in the next recompilation */
    uint64_t *force_inline_sites;      /* array of call_site_ids to force inline */
    uint32_t  force_inline_count;
    uint32_t  force_inline_capacity;

    /* Version tracking */
    uint32_t current_version_id;       /* currently active compiled version */
    uint32_t recompilation_count;      /* number of recompilations triggered */

    /* Performance tracking */
    uint64_t deopt_count_at_inlined_sites; /* deopts at inlined call sites */
    uint64_t total_executions;             /* total executions of this method */

    /* Per-site spill and deopt rate tracking */
    double   spill_rate;                   /* register spill rate (0.0 to 1.0) */
    double   deopt_rate;                   /* deoptimization rate (0.0 to 1.0) */
    uint64_t spill_count;                  /* total register spills */
    uint64_t total_instructions;           /* total instructions executed */

    /* Alternative versions with different inlining decisions */
    uint32_t *alternative_versions;        /* array of version IDs with different inlining */
    uint32_t  alternative_version_count;
    uint32_t  alternative_version_capacity;
    uint32_t  best_version_id;             /* version with best performance so far */
    double    best_version_score;          /* performance score of best version */
} vtx_fdi_method_record_t;

/* ========================================================================== */
/* FDI state                                                                   */
/* ========================================================================== */

#define VTX_FDI_INITIAL_CAPACITY 64

typedef struct {
    vtx_fdi_method_record_t *records;    /* array of per-method records */
    uint32_t                 record_count;
    uint32_t                 record_capacity;

    /* Reference to the inline feedback tracker (not owned) */
    vtx_inline_feedback_t   *feedback;

    /* Statistics */
    uint32_t total_recompilations;
    uint32_t total_methods_improved;  /* methods whose performance improved after recomp */
    uint32_t total_methods_worse;     /* methods whose performance got worse */
} vtx_sota_fdi_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the FDI system.
 *
 * @param fdi      FDI state to initialize
 * @param feedback Inline feedback tracker (not owned, must outlive FDI)
 * @return         0 on success, -1 on failure
 */
int vtx_sota_fdi_init(vtx_sota_fdi_t *fdi, vtx_inline_feedback_t *feedback);

/**
 * Destroy the FDI system and free memory.
 */
void vtx_sota_fdi_destroy(vtx_sota_fdi_t *fdi);

/* ========================================================================== */
/* Evaluation                                                                  */
/* ========================================================================== */

/**
 * Evaluate whether a method should be recompiled with different
 * inlining decisions based on feedback.
 *
 * Checks:
 *   1. Are any inlined call sites in this method marked as unprofitable?
 *   2. Are any non-inlined call sites in this method now good inline candidates?
 *   3. Has the method's deopt rate exceeded VTX_INLINE_DEOPT_THRESHOLD?
 *
 * @param fdi       FDI state
 * @param method_id Method to evaluate
 * @return          true if recompilation is recommended
 */
bool vtx_sota_fdi_evaluate(vtx_sota_fdi_t *fdi, uint32_t method_id);

/**
 * Get the recompilation directive for a method.
 * Returns the set of call sites to force no-inline and force inline.
 *
 * @param fdi                FDI state
 * @param method_id          Method to get directives for
 * @param[out] no_inline     Array of call site IDs to force no-inline (arena-allocated)
 * @param[out] no_inline_count Number of no-inline sites
 * @param[out] force_inline  Array of call site IDs to force inline (arena-allocated)
 * @param[out] force_inline_count Number of force-inline sites
 * @return                   0 on success, -1 on failure
 */
int vtx_sota_fdi_get_directives(vtx_sota_fdi_t *fdi,
                                  uint32_t method_id,
                                  vtx_arena_t *arena,
                                  uint64_t **no_inline,
                                  uint32_t *no_inline_count,
                                  uint64_t **force_inline,
                                  uint32_t *force_inline_count);

/* ========================================================================== */
/* Recording                                                                   */
/* ========================================================================== */

/**
 * Record that a deoptimization occurred at an inlined call site.
 * This may trigger a recompilation recommendation.
 *
 * @param fdi          FDI state
 * @param method_id    Method containing the inlined call site
 * @param call_site_id Call site where deopt occurred
 */
void vtx_sota_fdi_record_deopt(vtx_sota_fdi_t *fdi,
                                 uint32_t method_id,
                                 uint64_t call_site_id);

/**
 * Record that a method execution completed successfully.
 * Used to compute deopt rates and detect when inlining is profitable.
 *
 * @param fdi       FDI state
 * @param method_id Method that executed
 */
void vtx_sota_fdi_record_execution(vtx_sota_fdi_t *fdi, uint32_t method_id);

/**
 * Record that a recompilation was completed for a method.
 * Updates the current version tracking.
 *
 * @param fdi          FDI state
 * @param method_id    Method that was recompiled
 * @param new_version  New version ID
 */
void vtx_sota_fdi_record_recompilation(vtx_sota_fdi_t *fdi,
                                         uint32_t method_id,
                                         uint32_t new_version);

/* ========================================================================== */
/* Performance tracking                                                        */
/* ========================================================================== */

/**
 * Record performance metrics for a compiled method.
 *
 * Tracks spill rate and deopt rate to detect when inlining is
 * unprofitable. When either rate exceeds its threshold:
 *   - spill_rate > VTX_SPILL_THRESHOLD (10%): inlining increased register
 *     pressure beyond available registers, causing excessive spills
 *   - deopt_rate > VTX_INLINE_DEOPT_THRESHOLD (5%): inlined guards are
 *     failing too often, making the inlined code path useless
 *
 * If either threshold is exceeded, the method is flagged for
 * recompilation with different inlining decisions.
 *
 * This also feeds back to the ML model immediately: the feature
 * vector and outcome are recorded for the next training cycle.
 *
 * @param fdi         FDI state
 * @param method_id   Method to record performance for
 * @param spill_rate  Register spill rate (0.0 to 1.0)
 * @param deopt_rate  Deoptimization rate (0.0 to 1.0)
 */
void vtx_sota_fdi_record_performance(vtx_sota_fdi_t *fdi,
                                       uint32_t method_id,
                                       double spill_rate,
                                       double deopt_rate);

/**
 * Get the best replacement version for a method.
 *
 * If a method has multiple compiled versions with different inlining
 * decisions, this function returns the version that has the best
 * performance so far. This enables dynamic switching between
 * compiled versions at safe points.
 *
 * If no better version exists, returns the current version ID.
 *
 * @param fdi       FDI state
 * @param method_id Method to query
 * @return          Version ID of the best-performing version
 */
uint32_t vtx_sota_fdi_get_replacement_version(vtx_sota_fdi_t *fdi,
                                                 uint32_t method_id);

/**
 * Register an alternative compiled version for a method.
 *
 * When a recompilation produces a new version with different inlining
 * decisions, it is registered here. The FDI system tracks multiple
 * versions and can switch between them based on performance.
 *
 * @param fdi         FDI state
 * @param method_id   Method that has a new alternative version
 * @param version_id  New version ID
 */
void vtx_sota_fdi_register_version(vtx_sota_fdi_t *fdi,
                                     uint32_t method_id,
                                     uint32_t version_id);

/**
 * Update the performance score of a specific version.
 *
 * The score is computed as:
 *   score = (1 - spill_rate) * (1 - deopt_rate) * execution_speedup
 * Higher is better. This is used to determine which version to use.
 *
 * @param fdi         FDI state
 * @param method_id   Method
 * @param version_id  Version to update
 * @param score       Performance score (higher is better)
 */
void vtx_sota_fdi_update_version_score(vtx_sota_fdi_t *fdi,
                                         uint32_t method_id,
                                         uint32_t version_id,
                                         double score);

/* ========================================================================== */
/* Recompilation candidate iteration                                            */
/* ========================================================================== */

/**
 * Get the next method that FDI recommends for recompilation.
 *
 * Iterates over all tracked methods and returns the method_id of
 * the next method that vtx_sota_fdi_evaluate() recommends for
 * recompilation. Returns VTX_PHASE_NONE if no more candidates exist.
 *
 * Each call advances an internal cursor so that repeated calls
 * enumerate all candidates without duplication.
 *
 * @param fdi       FDI state
 * @return          Method ID of the next recompilation candidate,
 *                  or VTX_PHASE_NONE if no more candidates
 */
uint32_t vtx_sota_fdi_next_recompile_candidate(vtx_sota_fdi_t *fdi);

#endif /* VORTEX_SOTA_FDI_H */
