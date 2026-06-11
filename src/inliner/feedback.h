#ifndef VORTEX_INLINER_FEEDBACK_H
#define VORTEX_INLINER_FEEDBACK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "inliner/features.h"

/**
 * VORTEX ML Inliner — Online Feedback
 *
 * Tracks inlining decision outcomes to enable:
 *   1. Runtime detection of unprofitable inlines (deopt at inlined guard)
 *   2. Confirmation of profitable inlines (stable execution for window)
 *   3. CSV export of feature→label data for offline model retraining
 *   4. Recompilation triggers when inlining decisions prove suboptimal
 *
 * Each inlining decision is recorded with:
 *   - call_site_id: unique identifier for the call site
 *   - features: the feature vector that was used for the decision
 *   - inlined: whether the decision was to inline
 *   - outcome: not yet determined at decision time
 *
 * Outcomes are recorded later:
 *   - If a deoptimization occurs at an inlined call site's guard,
 *     the inlining is marked as potentially unprofitable.
 *   - If the compiled code runs stably for VTX_FEEDBACK_WINDOW executions
 *     without deopt at that site, the inlining is marked as profitable.
 *
 * Thread safety: this module is NOT thread-safe. The caller must
 * synchronize access if used from multiple threads.
 */

/* ========================================================================== */
/* Outcome types                                                               */
/* ========================================================================== */

typedef enum {
    VTX_OUTCOME_UNKNOWN = 0,     /* no outcome recorded yet */
    VTX_OUTCOME_PROFITABLE = 1,  /* stable execution for feedback window */
    VTX_OUTCOME_UNPROFITABLE = 2 /* deopt at inlined guard */
} vtx_inline_outcome_t;

/* ========================================================================== */
/* Per-decision record                                                         */
/* ========================================================================== */

typedef struct {
    uint64_t                call_site_id;  /* unique call site identifier */
    vtx_inline_features_t   features;      /* feature vector at decision time */
    bool                    inlined;       /* was the decision to inline? */
    vtx_inline_outcome_t    outcome;       /* observed outcome */
    uint64_t                execution_count; /* executions since decision */
    uint64_t                deopt_count;     /* deopts at this site since decision */
    uint64_t                decision_timestamp; /* when the decision was made (ns) */
} vtx_inline_decision_t;

/* ========================================================================== */
/* Feedback tracker                                                            */
/* ========================================================================== */

#define VTX_FEEDBACK_INITIAL_CAPACITY 256

typedef struct {
    vtx_inline_decision_t  *decisions;      /* array of recorded decisions */
    uint32_t                decision_count;  /* number of decisions */
    uint32_t                decision_capacity;

    /* Index for fast lookup by call_site_id.
     * Simple linear scan for now (decisions array is typically small).
     * For production, this could be replaced with a hash table. */

    /* Counters for CSV writing */
    uint32_t                profitable_count;
    uint32_t                unprofitable_count;
    uint32_t                unknown_count;
} vtx_inline_feedback_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize a feedback tracker.
 * Returns 0 on success, -1 on failure.
 */
int vtx_feedback_init(vtx_inline_feedback_t *feedback);

/**
 * Destroy a feedback tracker and free all memory.
 */
void vtx_feedback_destroy(vtx_inline_feedback_t *feedback);

/* ========================================================================== */
/* Recording                                                                   */
/* ========================================================================== */

/**
 * Record an inlining decision.
 *
 * @param feedback     Feedback tracker
 * @param call_site_id Unique identifier for the call site
 * @param features     Feature vector used for the decision
 * @param inlined      Whether the decision was to inline
 * @return             0 on success, -1 on failure
 */
int vtx_feedback_record_decision(vtx_inline_feedback_t *feedback,
                                  uint64_t call_site_id,
                                  const vtx_inline_features_t *features,
                                  bool inlined);

/**
 * Record an outcome for a previously recorded decision.
 *
 * If the decision has already been marked with an outcome, this
 * updates the outcome only if the new outcome is worse
 * (PROFITABLE → UNPROFITABLE is allowed; UNPROFITABLE → PROFITABLE is not).
 *
 * @param feedback     Feedback tracker
 * @param call_site_id Call site identifier
 * @param profitable   true if the inlining was profitable, false if not
 * @return             0 on success, -1 if call_site_id not found
 */
int vtx_feedback_record_outcome(vtx_inline_feedback_t *feedback,
                                 uint64_t call_site_id,
                                 bool profitable);

/**
 * Increment the execution count for a call site.
 * Called each time the compiled code containing this inlined call site executes.
 *
 * If the execution count reaches VTX_FEEDBACK_WINDOW and the outcome
 * is still UNKNOWN, automatically mark it as PROFITABLE.
 *
 * @param feedback     Feedback tracker
 * @param call_site_id Call site identifier
 */
void vtx_feedback_increment_executions(vtx_inline_feedback_t *feedback,
                                        uint64_t call_site_id);

/**
 * Increment the deopt count for a call site.
 * Called when a deoptimization occurs at an inlined call site's guard.
 *
 * If the deopt rate exceeds VTX_INLINE_DEOPT_THRESHOLD, automatically
 * mark the outcome as UNPROFITABLE.
 *
 * @param feedback     Feedback tracker
 * @param call_site_id Call site identifier
 */
void vtx_feedback_increment_deopts(vtx_inline_feedback_t *feedback,
                                    uint64_t call_site_id);

/* ========================================================================== */
/* Queries                                                                     */
/* ========================================================================== */

/**
 * Check whether a call site should be recompiled with a different
 * inlining strategy.
 *
 * A site should be recompiled if:
 *   - It was inlined and the outcome is UNPROFITABLE
 *   - It was NOT inlined but the call site is now hot and the
 *     features suggest inlining would be profitable (heuristic check)
 *
 * @param feedback     Feedback tracker
 * @param call_site_id Call site identifier
 * @return             true if recompilation is recommended
 */
bool vtx_feedback_should_recompile(vtx_inline_feedback_t *feedback,
                                    uint64_t call_site_id);

/**
 * Look up the decision for a call site.
 * Returns NULL if no decision has been recorded for this call site.
 */
const vtx_inline_decision_t *vtx_feedback_lookup(
    const vtx_inline_feedback_t *feedback,
    uint64_t call_site_id);

/* ========================================================================== */
/* CSV export                                                                  */
/* ========================================================================== */

/**
 * Write all recorded decisions with known outcomes to a CSV file.
 *
 * Format: one line per decision
 *   f0,f1,...,f14,label
 * where label is 1 for PROFITABLE, 0 for UNPROFITABLE.
 * Decisions with UNKNOWN outcome are excluded.
 *
 * @param feedback Feedback tracker
 * @param filename Path to the output CSV file
 * @return         Number of records written, or -1 on error
 */
int vtx_feedback_write_csv(const vtx_inline_feedback_t *feedback,
                            const char *filename);

#endif /* VORTEX_INLINER_FEEDBACK_H */
