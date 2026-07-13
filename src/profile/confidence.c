/* ============================================================================ *
 * AI-GENERATED CODE NOTICE
 *
 * This file was written or substantially modified by an AI assistant
 * (GLM/Z.ai). It is part of the VORTEX JIT compiler project.
 *
 * Human-written original code exists in the interpreter dispatch loop
 * (src/interp/), baseline codegen (src/baseline/codegen.c), runtime
 * (src/runtime/), and the main entry point (src/main_new.c).
 *
 * AI-generated components include: IR construction, optimization passes
 * (GVN, SCCP, DCE, LICM, strength reduction, SMI tag elision, PEA),
 * instruction selection, register allocation, code emission, the
 * compilation pipeline, the decision engine, PGO subsystems (phase
 * partitioning, ensemble aggregation, input-shape-keyed profiles, patch
 * logging, T1 code persistence), deopt/OSR, trace recording, guard
 * optimization, and the inliner.
 *
 * If you are reviewing this code, please verify correctness independently.
 * ============================================================================ */

/**
 * VORTEX Profile Confidence Scoring (Sprint 1.1) — Implementation
 *
 * Confidence is a pure function of (sample_count, threshold). The recording
 * path is unchanged; the compiler / orchestrator queries confidence at
 * decision time.
 *
 * See confidence.h for the design rationale.
 */

#include "profile/confidence.h"
#include <math.h>

/* ========================================================================== */
/* Internal helpers                                                            */
/* ========================================================================== */

static double confidence_from_count(uint64_t sample_count, uint32_t threshold)
{
    if (threshold == 0) return 1.0;          /* no threshold = always confident */
    if (sample_count == 0) return 0.0;        /* no samples = no confidence */
    double c = (double)sample_count / (double)threshold;
    return c > 1.0 ? 1.0 : c;
}

/* ========================================================================== */
/* Per-feature confidence queries                                              */
/* ========================================================================== */

double vtx_confidence_branch(const vtx_branch_profile_t *branch)
{
    if (branch == NULL) return 0.0;
    uint64_t total = branch->taken + branch->not_taken;
    /* Saturating add guard — if either counter saturated, total may wrap. */
    if (total < branch->taken) total = UINT64_MAX;
    return confidence_from_count(total, VTX_CONFIDENCE_THRESHOLD_BRANCH);
}

double vtx_confidence_call_target(const vtx_callsite_profile_t *callsite)
{
    if (callsite == NULL) return 0.0;
    /* Megamorphic: we cannot speculate on a single target. */
    if (callsite->megamorphic) return 0.0;
    /* A site that has seen only 1 type but never been called is still
     * low-confidence — we want call_count samples, not type_count. The
     * callsite profile doesn't directly track call count, but the
     * observation count is the closest proxy: we use the count of distinct
     * types as a *lower bound* on the observation count and require that
     * the site be monomorphic (count == 1) AND have been observed at
     * least VTX_CONFIDENCE_THRESHOLD_CALL_TARGET times.
     *
     * Because the profile only records distinct types (not total calls),
     * we approximate "samples" as: monomorphic ? 1 : 0. This is a known
     * limitation of the profile format; when D5 per-type frequency data
     * is available (vtx_type_freq_t), the orchestrator should use that
     * instead. For now, we gate on monomorphicity + the D5 total_count
     * if it's been wired into the callsite profile (it hasn't yet, but
     * the query path is ready for when it is). */
    if (callsite->count != 1) return 0.0;  /* not monomorphic */
    /* Monomorphic site: confidence is 1.0 if we have any samples at all.
     * The CALL_TARGET threshold is a soft gate — once we've seen the site
     * be monomorphic, we trust it. (Stricter gating requires D5 data,
     * which is wired separately.) */
    return 1.0;
}

double vtx_confidence_type_dist(const vtx_callsite_profile_t *callsite)
{
    if (callsite == NULL) return 0.0;
    if (callsite->megamorphic) return 0.0;
    /* BUGFIX P10: The callsite profile tracks DISTINCT types (max
     * VTX_POLY_LIMIT=4), NOT total observation count. The old code
     * divided callsite->count (max 4) by the threshold (200), giving
     * a max confidence of 0.02. This meant no method with call sites
     * could ever reach the T2 promotion gate (0.5), making the entire
     * PGO tier promotion system non-functional.
     *
     * The fix: a monomorphic callsite (count==1) is high-confidence
     * by definition — we've seen exactly one type, and that's the
     * strongest possible signal. A polymorphic site (2-4 types) has
     * lower confidence proportional to how many types are seen.
     * Megamorphic sites have confidence 0 (can't speculate on a
     * stable distribution).
     *
     * The TYPE_DIST threshold (200) was calibrated for total sample
     * count, which we don't have here. Since the callsite profile
     * only gives us distinct-type count, we use a different formula:
     *   - monomorphic (1 type): confidence 1.0 (strongest signal)
     *   - polymorphic (2-4 types): confidence = 1.0 / count (more
     *     types = less confidence in any single one)
     * This correctly reflects that a monomorphic site is highly
     * predictable while a 4-type polymorphic site is less so. */
    if (callsite->count == 0) return 0.0;
    if (callsite->count == 1) return 1.0;  /* monomorphic = max confidence */
    /* Polymorphic: 2 types = 0.5, 3 types = 0.33, 4 types = 0.25.
     * This naturally gates T2 promotion (needs 0.5) to monomorphic
     * and 2-type polymorphic sites, which is the right behavior. */
    return 1.0 / (double)callsite->count;
}

double vtx_confidence_loop_trip(const vtx_loop_profile_t *loop)
{
    if (loop == NULL) return 0.0;
    /* Backedge count is a good proxy for "how many times we've observed
     * this loop's trip count". */
    double base = confidence_from_count(loop->backedge_count,
                                          VTX_CONFIDENCE_THRESHOLD_LOOP_TRIP);
    /* If the loop is trip-stable (constant trip count for
     * VTX_TRIP_STABILITY_WINDOW observations), boost confidence to 1.0. */
    if (loop->is_trip_stable) return 1.0;
    return base;
}

double vtx_confidence_field_shape(const vtx_field_profile_t *field)
{
    if (field == NULL) return 0.0;
    if (field->megamorphic) return 0.0;
    /* BUGFIX P10: Same bug as type_dist — field->count is the number of
     * DISTINCT shapes (max VTX_POLY_LIMIT=4), NOT total observations.
     * Dividing 4 by 100 gives max confidence 0.04, which blocks T2
     * promotion for any method with field accesses.
     *
     * Fix: monomorphic field access (1 shape) = confidence 1.0.
     * Polymorphic = 1.0/count. This correctly gates T2 to monomorphic
     * and 2-shape polymorphic sites. */
    if (field->count == 0) return 0.0;
    if (field->count == 1) return 1.0;
    return 1.0 / (double)field->count;
}

/* ========================================================================== */
/* Method-level aggregate confidence                                           */
/* ========================================================================== */

double vtx_confidence_method(const vtx_profile_method_t *method)
{
    if (method == NULL) return 0.0;

    /* If the method has never been invoked, it has no profile. */
    if (method->invocation_count == 0) return 0.0;

    double min_confidence = 1.0;
    bool   any_feature_seen = false;

    /* Branches: weighted-by-sample-count average, but the aggregate is the
     * MINIMUM across features, so we compute the per-feature average first
     * and then take the min. */
    if (method->branch_count > 0) {
        double weighted_sum = 0.0;
        uint64_t total_samples = 0;
        for (uint32_t i = 0; i < method->branch_count; i++) {
            const vtx_branch_profile_t *b = &method->branches[i];
            uint64_t samples = b->taken + b->not_taken;
            if (samples < b->taken) samples = UINT64_MAX;
            weighted_sum += vtx_confidence_branch(b) * (double)samples;
            total_samples += samples;
        }
        double branch_conf = (total_samples > 0)
            ? (weighted_sum / (double)total_samples)
            : 0.0;
        if (branch_conf < min_confidence) min_confidence = branch_conf;
        any_feature_seen = true;
    }

    /* Call sites: type-distribution confidence, weighted by sample count.
     * We use the proxy (count) as the weight since the profile doesn't
     * track total call count per site. */
    if (method->call_site_count > 0) {
        double weighted_sum = 0.0;
        uint64_t total_weight = 0;
        for (uint32_t i = 0; i < method->call_site_count; i++) {
            const vtx_callsite_profile_t *cs = &method->call_sites[i];
            uint64_t w = cs->megamorphic ? VTX_POLY_LIMIT : cs->count;
            weighted_sum += vtx_confidence_type_dist(cs) * (double)w;
            total_weight += w;
        }
        double cs_conf = (total_weight > 0)
            ? (weighted_sum / (double)total_weight)
            : 0.0;
        if (cs_conf < min_confidence) min_confidence = cs_conf;
        any_feature_seen = true;
    }

    /* Loops: trip-count confidence. */
    if (method->loop_count > 0) {
        double min_loop = 1.0;
        for (uint32_t i = 0; i < method->loop_count; i++) {
            double lc = vtx_confidence_loop_trip(&method->loops[i]);
            if (lc < min_loop) min_loop = lc;
        }
        if (min_loop < min_confidence) min_confidence = min_loop;
        any_feature_seen = true;
    }

    /* Field accesses: shape confidence. */
    if (method->field_access_count > 0) {
        double weighted_sum = 0.0;
        uint64_t total_weight = 0;
        for (uint32_t i = 0; i < method->field_access_count; i++) {
            const vtx_field_profile_t *f = &method->field_accesses[i];
            uint64_t w = f->megamorphic ? VTX_POLY_LIMIT : f->count;
            weighted_sum += vtx_confidence_field_shape(f) * (double)w;
            total_weight += w;
        }
        double f_conf = (total_weight > 0)
            ? (weighted_sum / (double)total_weight)
            : 0.0;
        if (f_conf < min_confidence) min_confidence = f_conf;
        any_feature_seen = true;
    }

    /* If we have invocation data but no per-site data yet, the method is
     * being called but we haven't observed its internals. Confidence is
     * low — we shouldn't speculate on its branches/types/loops because
     * we have no data. */
    if (!any_feature_seen) return 0.0;

    return min_confidence;
}

/* ========================================================================== */
/* Classification                                                              */
/* ========================================================================== */

vtx_confidence_level_t vtx_confidence_classify(double confidence)
{
    if (confidence < 0.0) confidence = 0.0;
    if (confidence > 1.0) confidence = 1.0;

    if (confidence < VTX_PROMOTION_CONFIDENCE_T2) return VTX_CONFIDENCE_LOW;
    if (confidence < VTX_PROMOTION_CONFIDENCE_T3) return VTX_CONFIDENCE_MEDIUM;
    return VTX_CONFIDENCE_HIGH;
}

bool vtx_confidence_eligible_for_tier(const vtx_profile_method_t *method,
                                        uint64_t hot_thresh,
                                        uint32_t tier)
{
    if (method == NULL) return false;

    /* T1 is non-speculative — always eligible if hot enough. */
    if (tier <= 1) {
        return method->invocation_count >= hot_thresh;
    }

    /* Speculative tiers require both heat and confidence. */
    if (method->invocation_count < hot_thresh) return false;

    double confidence = vtx_confidence_method(method);
    double required;

    switch (tier) {
        case 2:  required = VTX_PROMOTION_CONFIDENCE_T2; break;
        case 3:  required = VTX_PROMOTION_CONFIDENCE_T3; break;
        case 4:  required = VTX_PROMOTION_CONFIDENCE_T4; break;
        default: return false;  /* unknown tier */
    }

    return confidence >= required;
}
