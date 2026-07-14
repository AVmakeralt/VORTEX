/* ============================================================================ *
 * AI-GENERATED CODE
 *
 * This file was written from scratch by an AI assistant (GLM/Z.ai).
 * It is part of the VORTEX JIT compiler project.
 *
 * Human-written code lives in: src/interp/ (dispatch loop), src/baseline/
 * (codegen), src/runtime/ (GC, type system, arena), src/main_new.c.
 *
 * If reviewing, please verify correctness independently.
 * ============================================================================ */

#ifndef VORTEX_PROFILE_CONFIDENCE_H
#define VORTEX_PROFILE_CONFIDENCE_H

/**
 * VORTEX Profile Confidence Scoring (Sprint 1.1)
 *
 * Every profiled value carries a confidence in [0.0, 1.0]:
 *   confidence = min(sample_count / threshold, 1.0)
 *
 * Below the per-feature threshold, the datum is "low-confidence" and MUST
 * NOT be used for speculative optimization. This prevents the dominant
 * failure mode of bad PGO: speculating on noise ("saw it once, now I
 * speculate") and then deopting constantly.
 *
 * Tier promotion is gated on BOTH heat (invocation count) AND confidence:
 *   - Hot + low-confidence  → stay T1, keep profiling
 *   - Hot + high-confidence → T2
 *   - Hot + high-confidence + known phase → T3
 *   - Hot + high-confidence + known phase + stable input shapes → T4
 *
 * Confidence is computed on the QUERY side (when the compiler / orchestrator
 * asks "should I speculate on this?"), not on the RECORD side. This keeps
 * the hot recording path cheap — we already store sample counts; confidence
 * is a pure function of those counts and the per-feature threshold.
 */

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "profile/data.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* Per-feature confidence                                                      */
/* ========================================================================== */

/**
 * Confidence classification for a single profile datum.
 *
 * LOW    — below the promotion gate (T1 only, no speculation)
 * MEDIUM — meets the T2 gate (T2 speculation OK, T3 not yet)
 * HIGH   — meets the T3/T4 gate (full speculation OK)
 */
typedef enum {
    VTX_CONFIDENCE_LOW    = 0,
    VTX_CONFIDENCE_MEDIUM = 1,
    VTX_CONFIDENCE_HIGH   = 2
} vtx_confidence_level_t;

/* ========================================================================== */
/* Per-feature confidence queries                                              */
/* ========================================================================== */

/**
 * Branch-direction confidence in [0.0, 1.0].
 *
 * Based on the total observation count (taken + not_taken) relative to
 * VTX_CONFIDENCE_THRESHOLD_BRANCH. A branch with 100 samples has confidence
 * 1.0; a branch with 50 samples has confidence 0.5; a branch with 0 samples
 * has confidence 0.0.
 *
 * @param branch  Branch profile (may be NULL → returns 0.0)
 * @return        Confidence in [0.0, 1.0]
 */
double vtx_confidence_branch(const vtx_branch_profile_t *branch);

/**
 * Call-target (monomorphic) confidence in [0.0, 1.0].
 *
 * Based on the invocation count observed at the call site relative to
 * VTX_CONFIDENCE_THRESHOLD_CALL_TARGET. A megamorphic site is always
 * confidence 0.0 (we cannot speculate on a single target).
 *
 * @param callsite  Call-site profile (may be NULL → returns 0.0)
 * @return          Confidence in [0.0, 1.0]
 */
double vtx_confidence_call_target(const vtx_callsite_profile_t *callsite);

/**
 * Type-distribution confidence in [0.0, 1.0].
 *
 * Based on the total observation count across all observed types at the
 * call site relative to VTX_CONFIDENCE_THRESHOLD_TYPE_DIST. Megamorphic
 * sites are confidence 0.0 (we cannot speculate on a stable distribution).
 *
 * @param callsite  Call-site profile (may be NULL → returns 0.0)
 * @return          Confidence in [0.0, 1.0]
 */
double vtx_confidence_type_dist(const vtx_callsite_profile_t *callsite);

/**
 * Loop trip-count confidence in [0.0, 1.0].
 *
 * Based on the backedge count relative to VTX_CONFIDENCE_THRESHOLD_LOOP_TRIP.
 * A trip-stable loop with sufficient samples is confidence 1.0; an unstable
 * loop is confidence 0.0.
 *
 * @param loop  Loop profile (may be NULL → returns 0.0)
 * @return      Confidence in [0.0, 1.0]
 */
double vtx_confidence_loop_trip(const vtx_loop_profile_t *loop);

/**
 * Field-shape confidence in [0.0, 1.0].
 *
 * Based on the observation count at the field site relative to
 * VTX_CONFIDENCE_THRESHOLD_FIELD_SHAPE. Megamorphic sites are confidence 0.0.
 *
 * @param field  Field-access profile (may be NULL → returns 0.0)
 * @return       Confidence in [0.0, 1.0]
 */
double vtx_confidence_field_shape(const vtx_field_profile_t *field);

/* ========================================================================== */
/* Method-level aggregate confidence                                           */
/* ========================================================================== */

/**
 * Aggregate confidence for a single method.
 *
 * The aggregate is the MINIMUM of:
 *   - branch confidence averaged across all profiled branches (weighted
 *     by sample count, so a branch with 0 samples doesn't drag the average
 *     down)
 *   - call-site type-distribution confidence (same weighting)
 *   - loop trip-count confidence
 *   - field-shape confidence
 *
 * The minimum (not the mean) is used because speculation is only as safe
 * as its weakest link: if any one feature is low-confidence, the whole
 * method's speculation is low-confidence.
 *
 * @param method  Method profile (may be NULL → returns 0.0)
 * @return        Aggregate confidence in [0.0, 1.0]
 */
double vtx_confidence_method(const vtx_profile_method_t *method);

/**
 * Classify a confidence value into a tier-promotion level.
 *
 *   < VTX_PROMOTION_CONFIDENCE_T2 → LOW    (T1 only)
 *   < VTX_PROMOTION_CONFIDENCE_T3 → MEDIUM (T2 OK)
 *   else                          → HIGH   (T3/T4 OK)
 *
 * @param confidence  Confidence in [0.0, 1.0]
 * @return            Classification
 */
vtx_confidence_level_t vtx_confidence_classify(double confidence);

/**
 * Check whether a method is eligible for promotion to a given tier,
 * considering BOTH heat and confidence.
 *
 *   tier 1 (T1)  — always eligible (T1 is non-speculative)
 *   tier 2 (T2)  — hot AND confidence >= VTX_PROMOTION_CONFIDENCE_T2
 *   tier 3 (T3)  — hot AND confidence >= VTX_PROMOTION_CONFIDENCE_T3
 *   tier 4 (T4)  — hot AND confidence >= VTX_PROMOTION_CONFIDENCE_T4
 *
 * @param method     Method profile
 * @param hot_thresh Heat threshold (invocation count) — caller passes the
 *                   tier's own heat threshold
 * @param tier       Target tier (1..4)
 * @return           true if eligible for promotion
 */
bool vtx_confidence_eligible_for_tier(const vtx_profile_method_t *method,
                                        uint64_t hot_thresh,
                                        uint32_t tier);

#ifdef __cplusplus
}
#endif

#endif /* VORTEX_PROFILE_CONFIDENCE_H */
