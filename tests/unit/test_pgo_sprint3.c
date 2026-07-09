/**
 * VORTEX PGO Sprint 3 Ensemble Profiles Tests
 *
 * Tests for:
 *   - Sprint 3.1: Ensemble storage (add_run, ring buffer eviction)
 *   - Sprint 3.2: Robust aggregate (median branches, mode types, intersection shapes)
 *   - Sprint 3.3: Quality scoring + bad-profile demotion
 *   - Sprint 3.4: Rollback on high deopt rate
 *
 * The headline test: "outlier can't dominate aggregate" — 4 runs with
 * P(taken)=80% and 1 outlier run with P(taken)=20% should produce an
 * aggregate P(taken) near 80%, not the 68% that a simple average would give.
 */

#include "test_framework.h"
#include "vortex_config.h"
#include "profile/data.h"
#include "profile/merge.h"
#include "profile/ensemble.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Helpers                                                                     */
/* ========================================================================== */

/* Build a profile with a single method that has one branch with the given
 * taken/not_taken counts. */
static void build_profile_with_branch(vtx_profile_global_t *g,
                                        uint32_t method_id,
                                        uint64_t taken,
                                        uint64_t not_taken)
{
    vtx_profile_global_init(g);
    vtx_profile_method_t *m = vtx_profile_add_method(g, method_id);
    VTX_ASSERT_NOT_NULL(m);
    m->invocation_count = taken + not_taken;

    /* Add a branch at PC 10. */
    if (m->branch_count >= m->branch_capacity) {
        uint32_t new_cap = m->branch_capacity == 0 ? 8 : m->branch_capacity * 2;
        m->branches = realloc(m->branches, new_cap * sizeof(vtx_branch_profile_t));
        memset(m->branches + m->branch_capacity, 0,
               (new_cap - m->branch_capacity) * sizeof(vtx_branch_profile_t));
        m->branch_capacity = new_cap;
    }
    m->branches[m->branch_count].bytecode_pc = 10;
    m->branches[m->branch_count].taken = taken;
    m->branches[m->branch_count].not_taken = not_taken;
    m->branch_count++;
}

/* Build a profile with a single method that has one call site with the
 * given types. */
static void build_profile_with_callsite(vtx_profile_global_t *g,
                                          uint32_t method_id,
                                          const vtx_typeid_t *types,
                                          uint32_t type_count)
{
    vtx_profile_global_init(g);
    vtx_profile_method_t *m = vtx_profile_add_method(g, method_id);
    VTX_ASSERT_NOT_NULL(m);
    m->invocation_count = 1000;

    /* Add a call site at index 0. */
    if (m->call_site_count >= m->call_site_capacity) {
        uint32_t new_cap = m->call_site_capacity == 0 ? 8 : m->call_site_capacity * 2;
        m->call_sites = realloc(m->call_sites, new_cap * sizeof(vtx_callsite_profile_t));
        memset(m->call_sites + m->call_site_capacity, 0,
               (new_cap - m->call_site_capacity) * sizeof(vtx_callsite_profile_t));
        m->call_site_capacity = new_cap;
    }
    vtx_callsite_profile_t *cs = &m->call_sites[m->call_site_count++];
    memset(cs, 0, sizeof(*cs));
    for (uint32_t i = 0; i < type_count && i < VTX_POLY_LIMIT; i++) {
        cs->types[i] = types[i];
        cs->count++;
    }
}

/* Build a good run metadata (high quality). */
static vtx_ensemble_run_meta_t good_meta(void)
{
    vtx_ensemble_run_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.sample_count = 10000;       /* well above MIN_SAMPLES */
    meta.deopt_count = 0;
    meta.deopt_rate = 0.0;
    meta.runtime_duration_s = 10.0;  /* well above MIN_DURATION */
    return meta;
}

/* Build a bad run metadata (low quality — should be demoted). */
static vtx_ensemble_run_meta_t bad_meta(void)
{
    vtx_ensemble_run_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.sample_count = 50;          /* below MIN_SAMPLES */
    meta.deopt_count = 25;
    meta.deopt_rate = 0.5;           /* 50% deopt rate */
    meta.runtime_duration_s = 0.1;   /* below MIN_DURATION */
    return meta;
}

/* ========================================================================== */
/* Sprint 3.1: Ensemble storage                                                */
/* ========================================================================== */

VTX_TEST(ensemble_init_empty)
{
    vtx_ensemble_t ens;
    VTX_ASSERT_EQUAL(0, vtx_ensemble_init(&ens));

    uint32_t run_count, demoted;
    vtx_ensemble_stats(&ens, &run_count, &demoted, NULL, NULL, NULL);
    VTX_ASSERT_EQUAL(0u, run_count);
    VTX_ASSERT_EQUAL(0u, demoted);
    VTX_ASSERT_NULL(vtx_ensemble_get_working(&ens));

    vtx_ensemble_destroy(&ens);
}

VTX_TEST(ensemble_add_run_increments_count)
{
    vtx_ensemble_t ens;
    VTX_ASSERT_EQUAL(0, vtx_ensemble_init(&ens));

    vtx_profile_global_t g;
    build_profile_with_branch(&g, 1, 100, 50);

    VTX_ASSERT_EQUAL(0, vtx_ensemble_add_run(&ens, &g, good_meta()));

    uint32_t run_count;
    vtx_ensemble_stats(&ens, &run_count, NULL, NULL, NULL, NULL);
    VTX_ASSERT_EQUAL(1u, run_count);

    vtx_profile_global_destroy(&g);
    vtx_ensemble_destroy(&ens);
}

VTX_TEST(ensemble_ring_buffer_evicts_oldest)
{
    vtx_ensemble_t ens;
    VTX_ASSERT_EQUAL(0, vtx_ensemble_init(&ens));

    /* Add K+3 runs — the first 3 should be evicted. */
    for (uint32_t i = 0; i < VTX_ENSEMBLE_MAX_RUNS + 3; i++) {
        vtx_profile_global_t g;
        build_profile_with_branch(&g, 1, 100 + i, 50);
        VTX_ASSERT_EQUAL(0, vtx_ensemble_add_run(&ens, &g, good_meta()));
        vtx_profile_global_destroy(&g);
    }

    uint32_t run_count;
    vtx_ensemble_stats(&ens, &run_count, NULL, NULL, NULL, NULL);
    /* run_count should be capped at K. */
    VTX_ASSERT_EQUAL(VTX_ENSEMBLE_MAX_RUNS, run_count);

    vtx_ensemble_destroy(&ens);
}

/* ========================================================================== */
/* Sprint 3.2: Robust aggregate — the headline test                            */
/* ========================================================================== */

/**
 * 4 runs with P(taken) = 80% (taken=800, not_taken=200)
 * 1 outlier run with P(taken) = 20% (taken=200, not_taken=800)
 *
 * Simple average would give: (4*0.8 + 0.2) / 5 = 0.68 = 68%
 * Median gives: 0.80 = 80% (the outlier is the 5th value, doesn't affect median)
 *
 * The aggregate should have P(taken) ≈ 80%, proving the outlier
 * can't dominate.
 */
VTX_TEST(ensemble_outlier_branch_cant_dominate)
{
    vtx_ensemble_t ens;
    VTX_ASSERT_EQUAL(0, vtx_ensemble_init(&ens));

    /* 4 good runs: P(taken) = 80%. */
    for (uint32_t i = 0; i < 4; i++) {
        vtx_profile_global_t g;
        build_profile_with_branch(&g, 1, 800, 200);
        VTX_ASSERT_EQUAL(0, vtx_ensemble_add_run(&ens, &g, good_meta()));
        vtx_profile_global_destroy(&g);
    }

    /* 1 outlier run: P(taken) = 20%. */
    {
        vtx_profile_global_t g;
        build_profile_with_branch(&g, 1, 200, 800);
        VTX_ASSERT_EQUAL(0, vtx_ensemble_add_run(&ens, &g, good_meta()));
        vtx_profile_global_destroy(&g);
    }

    vtx_profile_global_t *agg = vtx_ensemble_compute_aggregate(&ens);
    VTX_ASSERT_NOT_NULL(agg);

    const vtx_profile_method_t *m = vtx_profile_get_method(agg, 1);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(1u, m->branch_count);

    /* Compute P(taken) from the aggregate. */
    uint64_t total = m->branches[0].taken + m->branches[0].not_taken;
    VTX_ASSERT_TRUE(total > 0);
    double p_taken = (double)m->branches[0].taken / (double)total;

    /* The median of [0.8, 0.8, 0.8, 0.8, 0.2] is 0.8.
     * The aggregate should be ≈ 80%, NOT 68% (simple average). */
    VTX_ASSERT_TRUE(p_taken >= 0.75 && p_taken <= 0.85);

    vtx_ensemble_destroy(&ens);
}

/**
 * Without ensemble (using merge_into which sums), the outlier WOULD
 * dominate. This test documents the bug that Sprint 3 fixes.
 */
VTX_TEST(ensemble_old_merge_lets_outlier_dominate)
{
    vtx_profile_global_t merged;
    vtx_profile_global_init(&merged);

    /* 4 good runs: P(taken) = 80%. */
    for (uint32_t i = 0; i < 4; i++) {
        vtx_profile_global_t g;
        build_profile_with_branch(&g, 1, 800, 200);
        vtx_profile_merge_into(&merged, &g);
        vtx_profile_global_destroy(&g);
    }

    /* 1 outlier run: P(taken) = 20%. */
    {
        vtx_profile_global_t g;
        build_profile_with_branch(&g, 1, 200, 800);
        vtx_profile_merge_into(&merged, &g);
        vtx_profile_global_destroy(&g);
    }

    const vtx_profile_method_t *m = vtx_profile_get_method(&merged, 1);
    VTX_ASSERT_NOT_NULL(m);
    uint64_t total = m->branches[0].taken + m->branches[0].not_taken;
    double p_taken = (double)m->branches[0].taken / (double)total;

    /* Sum: taken = 4*800 + 200 = 3400, total = 5*1000 = 5000.
     * P(taken) = 3400/5000 = 0.68 = 68%. The outlier dragged it down. */
    VTX_ASSERT_TRUE(p_taken >= 0.65 && p_taken <= 0.71);

    vtx_profile_global_destroy(&merged);
}

/**
 * Type distribution: mode — types seen in >50% of runs are included.
 *
 * 3 runs: runs 1 and 2 see type 100, run 3 sees type 200.
 * Type 100 appears in 2/3 = 67% of runs → included.
 * Type 200 appears in 1/3 = 33% of runs → excluded.
 *
 * Without ensemble (union), both types would be included, potentially
 * making the site polymorphic when it should be monomorphic.
 */
VTX_TEST(ensemble_type_mode_filters_rare_types)
{
    vtx_ensemble_t ens;
    VTX_ASSERT_EQUAL(0, vtx_ensemble_init(&ens));

    /* Run 1: type 100. */
    {
        vtx_profile_global_t g;
        vtx_typeid_t types[] = {100};
        build_profile_with_callsite(&g, 1, types, 1);
        vtx_ensemble_add_run(&ens, &g, good_meta());
        vtx_profile_global_destroy(&g);
    }

    /* Run 2: type 100. */
    {
        vtx_profile_global_t g;
        vtx_typeid_t types[] = {100};
        build_profile_with_callsite(&g, 1, types, 1);
        vtx_ensemble_add_run(&ens, &g, good_meta());
        vtx_profile_global_destroy(&g);
    }

    /* Run 3: type 200 (rare — only 1/3 of runs). */
    {
        vtx_profile_global_t g;
        vtx_typeid_t types[] = {200};
        build_profile_with_callsite(&g, 1, types, 1);
        vtx_ensemble_add_run(&ens, &g, good_meta());
        vtx_profile_global_destroy(&g);
    }

    vtx_profile_global_t *agg = vtx_ensemble_compute_aggregate(&ens);
    VTX_ASSERT_NOT_NULL(agg);

    const vtx_profile_method_t *m = vtx_profile_get_method(agg, 1);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(1u, m->call_site_count);

    /* The callsite should have only type 100 (seen in 2/3 > 50% of runs).
     * Type 200 (seen in 1/3 < 50%) should be excluded. */
    VTX_ASSERT_FALSE(m->call_sites[0].megamorphic);
    VTX_ASSERT_EQUAL(1u, m->call_sites[0].count);
    VTX_ASSERT_EQUAL((vtx_typeid_t)100, m->call_sites[0].types[0]);

    vtx_ensemble_destroy(&ens);
}

/**
 * Shape sets: intersection — only shapes seen in ALL runs.
 *
 * Run 1: shapes {10, 20}
 * Run 2: shapes {10, 30}
 * Intersection = {10}
 *
 * Without ensemble (union), the result would be {10, 20, 30} — 3 shapes,
 * potentially megamorphic. With intersection, it's just {10} — monomorphic.
 */
VTX_TEST(ensemble_shape_intersection_only_common)
{
    vtx_ensemble_t ens;
    VTX_ASSERT_EQUAL(0, vtx_ensemble_init(&ens));

    /* Run 1: shapes {10, 20} at field offset 0. */
    {
        vtx_profile_global_t g;
        vtx_profile_global_init(&g);
        vtx_profile_method_t *m = vtx_profile_add_method(&g, 1);
        m->invocation_count = 1000;
        m->field_accesses = calloc(4, sizeof(vtx_field_profile_t));
        m->field_access_capacity = 4;
        m->field_accesses[0].field_offset = 0;
        m->field_accesses[0].shapes[0] = 10;
        m->field_accesses[0].shapes[1] = 20;
        m->field_accesses[0].count = 2;
        m->field_access_count = 1;
        vtx_ensemble_add_run(&ens, &g, good_meta());
        vtx_profile_global_destroy(&g);
    }

    /* Run 2: shapes {10, 30} at field offset 0. */
    {
        vtx_profile_global_t g;
        vtx_profile_global_init(&g);
        vtx_profile_method_t *m = vtx_profile_add_method(&g, 1);
        m->invocation_count = 1000;
        m->field_accesses = calloc(4, sizeof(vtx_field_profile_t));
        m->field_access_capacity = 4;
        m->field_accesses[0].field_offset = 0;
        m->field_accesses[0].shapes[0] = 10;
        m->field_accesses[0].shapes[1] = 30;
        m->field_accesses[0].count = 2;
        m->field_access_count = 1;
        vtx_ensemble_add_run(&ens, &g, good_meta());
        vtx_profile_global_destroy(&g);
    }

    vtx_profile_global_t *agg = vtx_ensemble_compute_aggregate(&ens);
    VTX_ASSERT_NOT_NULL(agg);

    const vtx_profile_method_t *m = vtx_profile_get_method(agg, 1);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(1u, m->field_access_count);

    /* Intersection of {10,20} and {10,30} = {10}. */
    VTX_ASSERT_FALSE(m->field_accesses[0].megamorphic);
    VTX_ASSERT_EQUAL(1u, m->field_accesses[0].count);
    VTX_ASSERT_EQUAL((vtx_shapeid_t)10, m->field_accesses[0].shapes[0]);

    vtx_ensemble_destroy(&ens);
}

/* ========================================================================== */
/* Sprint 3.3: Quality scoring + demotion                                      */
/* ========================================================================== */

VTX_TEST(ensemble_quality_good_run_not_demoted)
{
    vtx_ensemble_run_meta_t meta = good_meta();
    vtx_ensemble_compute_quality(&meta);
    VTX_ASSERT_FALSE(meta.demoted);
    VTX_ASSERT_TRUE(meta.quality >= VTX_ENSEMBLE_QUALITY_THRESHOLD);
}

VTX_TEST(ensemble_quality_bad_run_demoted)
{
    vtx_ensemble_run_meta_t meta = bad_meta();
    vtx_ensemble_compute_quality(&meta);
    VTX_ASSERT_TRUE(meta.demoted);
    VTX_ASSERT_TRUE(meta.quality < VTX_ENSEMBLE_QUALITY_THRESHOLD);
}

VTX_TEST(ensemble_demoted_run_excluded_from_aggregate)
{
    vtx_ensemble_t ens;
    VTX_ASSERT_EQUAL(0, vtx_ensemble_init(&ens));

    /* 2 good runs: P(taken) = 80%. */
    for (uint32_t i = 0; i < 2; i++) {
        vtx_profile_global_t g;
        build_profile_with_branch(&g, 1, 800, 200);
        vtx_ensemble_add_run(&ens, &g, good_meta());
        vtx_profile_global_destroy(&g);
    }

    /* 1 bad run (demoted): P(taken) = 20%. */
    {
        vtx_profile_global_t g;
        build_profile_with_branch(&g, 1, 200, 800);
        vtx_ensemble_add_run(&ens, &g, bad_meta());
        vtx_profile_global_destroy(&g);
    }

    uint32_t run_count, demoted;
    vtx_ensemble_stats(&ens, &run_count, &demoted, NULL, NULL, NULL);
    VTX_ASSERT_EQUAL(3u, run_count);
    VTX_ASSERT_EQUAL(1u, demoted);

    vtx_profile_global_t *agg = vtx_ensemble_compute_aggregate(&ens);
    VTX_ASSERT_NOT_NULL(agg);

    const vtx_profile_method_t *m = vtx_profile_get_method(agg, 1);
    VTX_ASSERT_NOT_NULL(m);

    /* The aggregate should be ≈ 80% (the demoted 20% run is excluded). */
    uint64_t total = m->branches[0].taken + m->branches[0].not_taken;
    double p_taken = (double)m->branches[0].taken / (double)total;
    VTX_ASSERT_TRUE(p_taken >= 0.75 && p_taken <= 0.85);

    vtx_ensemble_destroy(&ens);
}

/* ========================================================================== */
/* Sprint 3.4: Rollback                                                        */
/* ========================================================================== */

VTX_TEST(ensemble_rollback_on_high_deopt_rate)
{
    vtx_ensemble_t ens;
    VTX_ASSERT_EQUAL(0, vtx_ensemble_init(&ens));

    /* First aggregate: 2 good runs. */
    for (uint32_t i = 0; i < 2; i++) {
        vtx_profile_global_t g;
        build_profile_with_branch(&g, 1, 800, 200);
        vtx_ensemble_add_run(&ens, &g, good_meta());
        vtx_profile_global_destroy(&g);
    }

    vtx_profile_global_t *agg1 = vtx_ensemble_compute_aggregate(&ens);
    VTX_ASSERT_NOT_NULL(agg1);
    VTX_ASSERT_TRUE(vtx_ensemble_is_pending_validation(&ens));

    /* Validate with low deopt rate → should be validated (not rolled back). */
    bool validated = vtx_ensemble_validate(&ens, 0.01);  /* 1% deopt */
    VTX_ASSERT_TRUE(validated);
    VTX_ASSERT_FALSE(vtx_ensemble_is_pending_validation(&ens));

    /* Add 2 more runs and compute a new aggregate. */
    for (uint32_t i = 0; i < 2; i++) {
        vtx_profile_global_t g;
        build_profile_with_branch(&g, 1, 200, 800);  /* different distribution */
        vtx_ensemble_add_run(&ens, &g, good_meta());
        vtx_profile_global_destroy(&g);
    }

    vtx_profile_global_t *agg2 = vtx_ensemble_compute_aggregate(&ens);
    VTX_ASSERT_NOT_NULL(agg2);
    VTX_ASSERT_TRUE(vtx_ensemble_is_pending_validation(&ens));

    /* Validate with HIGH deopt rate → should roll back. */
    bool rolled_back = vtx_ensemble_validate(&ens, 0.20);  /* 20% deopt */
    VTX_ASSERT_FALSE(rolled_back);  /* validate returns false on rollback */

    uint64_t total_rollbacks;
    vtx_ensemble_stats(&ens, NULL, NULL, NULL, &total_rollbacks, NULL);
    VTX_ASSERT_EQUAL(1ull, total_rollbacks);

    /* After rollback, the working profile should be the previous aggregate. */
    vtx_profile_global_t *working = vtx_ensemble_get_working(&ens);
    VTX_ASSERT_NOT_NULL(working);
    /* The working profile should be agg1 (the validated one). */
    VTX_ASSERT_EQUAL(agg1, working);

    vtx_ensemble_destroy(&ens);
}

VTX_TEST(ensemble_rollback_no_previous_returns_false)
{
    vtx_ensemble_t ens;
    VTX_ASSERT_EQUAL(0, vtx_ensemble_init(&ens));

    /* No previous aggregate → rollback should fail. */
    bool ok = vtx_ensemble_rollback(&ens);
    VTX_ASSERT_FALSE(ok);

    vtx_ensemble_destroy(&ens);
}

VTX_TEST(ensemble_validate_low_deopt_keeps_aggregate)
{
    vtx_ensemble_t ens;
    VTX_ASSERT_EQUAL(0, vtx_ensemble_init(&ens));

    for (uint32_t i = 0; i < 2; i++) {
        vtx_profile_global_t g;
        build_profile_with_branch(&g, 1, 800, 200);
        vtx_ensemble_add_run(&ens, &g, good_meta());
        vtx_profile_global_destroy(&g);
    }

    vtx_profile_global_t *agg = vtx_ensemble_compute_aggregate(&ens);
    VTX_ASSERT_TRUE(vtx_ensemble_is_pending_validation(&ens));

    /* Low deopt rate → validate succeeds. */
    bool ok = vtx_ensemble_validate(&ens, 0.02);
    VTX_ASSERT_TRUE(ok);
    VTX_ASSERT_FALSE(vtx_ensemble_is_pending_validation(&ens));

    /* Working profile should still be the same aggregate. */
    VTX_ASSERT_EQUAL(agg, vtx_ensemble_get_working(&ens));

    vtx_ensemble_destroy(&ens);
}

/* ========================================================================== */
/* Sprint 3.1: Statistics                                                      */
/* ========================================================================== */

VTX_TEST(ensemble_stats_track_aggregates_and_demotions)
{
    vtx_ensemble_t ens;
    VTX_ASSERT_EQUAL(0, vtx_ensemble_init(&ens));

    /* Add 1 good + 1 bad run. */
    {
        vtx_profile_global_t g;
        build_profile_with_branch(&g, 1, 100, 50);
        vtx_ensemble_add_run(&ens, &g, good_meta());
        vtx_profile_global_destroy(&g);
    }
    {
        vtx_profile_global_t g;
        build_profile_with_branch(&g, 1, 100, 50);
        vtx_ensemble_add_run(&ens, &g, bad_meta());
        vtx_profile_global_destroy(&g);
    }

    uint32_t run_count, demoted;
    uint64_t total_agg, total_rb, total_dem;
    vtx_ensemble_stats(&ens, &run_count, &demoted, &total_agg, &total_rb, &total_dem);

    VTX_ASSERT_EQUAL(2u, run_count);
    VTX_ASSERT_EQUAL(1u, demoted);
    VTX_ASSERT_TRUE(total_dem >= 1);

    vtx_ensemble_destroy(&ens);
}

/* ========================================================================== */
/* Test runner                                                                 */
/* ========================================================================== */

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\n");
    printf("=== PGO Sprint 3 Ensemble Profiles Tests ===\n");
    printf("  Passed: %u / %u\n", result.pass_count, result.total_count);
    printf("  Failed: %u\n", result.fail_count);
    return (result.fail_count == 0) ? 0 : 1;
}
