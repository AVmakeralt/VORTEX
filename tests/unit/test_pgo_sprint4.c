/**
 * VORTEX PGO Sprint 4 Input-Shape-Keyed Profiles Tests
 *
 * Tests for:
 *   - Sprint 4.1: Input shape signature (size bins, shape composition)
 *   - Sprint 4.2: Per-shape profile storage (different shapes → different profiles)
 *   - Sprint 4.3: Shape dispatch (compile multiple versions, lookup picks right one)
 *
 * The headline test: "different input shapes get different profiles" —
 * a method called with small arrays vs large arrays should have SEPARATE
 * profile data, not merged. This is the novel differentiator that no
 * production JIT does.
 */

#include "test_framework.h"
#include "vortex_config.h"
#include "profile/data.h"
#include "profile/input_shape.h"
#include "profile/shape_dispatch.h"

#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Sprint 4.1: Size binning                                                    */
/* ========================================================================== */

VTX_TEST(size_bin_zero_is_empty)
{
    VTX_ASSERT_EQUAL(VTX_SIZE_BIN_EMPTY, vtx_size_bin(0));
}

VTX_TEST(size_bin_one_is_one)
{
    VTX_ASSERT_EQUAL(VTX_SIZE_BIN_ONE, vtx_size_bin(1));
}

VTX_TEST(size_bin_two_to_four_is_tiny)
{
    VTX_ASSERT_EQUAL(VTX_SIZE_BIN_TINY, vtx_size_bin(2));
    VTX_ASSERT_EQUAL(VTX_SIZE_BIN_TINY, vtx_size_bin(3));
    VTX_ASSERT_EQUAL(VTX_SIZE_BIN_TINY, vtx_size_bin(4));
}

VTX_TEST(size_bin_five_to_sixteen_is_small)
{
    VTX_ASSERT_EQUAL(VTX_SIZE_BIN_SMALL, vtx_size_bin(5));
    VTX_ASSERT_EQUAL(VTX_SIZE_BIN_SMALL, vtx_size_bin(16));
}

VTX_TEST(size_bin_seventeen_to_256_is_medium)
{
    VTX_ASSERT_EQUAL(VTX_SIZE_BIN_MEDIUM, vtx_size_bin(17));
    VTX_ASSERT_EQUAL(VTX_SIZE_BIN_MEDIUM, vtx_size_bin(256));
}

VTX_TEST(size_bin_257_plus_is_large)
{
    VTX_ASSERT_EQUAL(VTX_SIZE_BIN_LARGE, vtx_size_bin(257));
    VTX_ASSERT_EQUAL(VTX_SIZE_BIN_LARGE, vtx_size_bin(1000000));
}

VTX_TEST(size_bin_name_not_null)
{
    VTX_ASSERT_NOT_NULL(vtx_size_bin_name(VTX_SIZE_BIN_EMPTY));
    VTX_ASSERT_NOT_NULL(vtx_size_bin_name(VTX_SIZE_BIN_LARGE));
}

/* ========================================================================== */
/* Sprint 4.1: Shape signature composition                                     */
/* ========================================================================== */

VTX_TEST(shape_make_produces_nonzero)
{
    vtx_input_shape_t s = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_MEDIUM, VTX_SIZE_BIN_SMALL, 3, 0x44);
    VTX_ASSERT_TRUE(s != VTX_INPUT_SHAPE_DEFAULT);
}

VTX_TEST(shape_make_different_sizes_different_shapes)
{
    /* Same args/receiver, but different collection sizes → different shapes. */
    vtx_input_shape_t small = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_TINY, VTX_SIZE_BIN_TINY, 1, 0x44);
    vtx_input_shape_t large = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_LARGE, VTX_SIZE_BIN_LARGE, 1, 0x44);
    VTX_ASSERT_FALSE(vtx_input_shape_equals(small, large));
}

VTX_TEST(shape_make_different_types_different_shapes)
{
    /* Same sizes, different arg types → different shapes. */
    vtx_input_shape_t shape_a = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_MEDIUM, VTX_SIZE_BIN_SMALL, 1, 0x44);
    vtx_input_shape_t shape_b = vtx_input_shape_make(
        0x99, 0x22, VTX_SIZE_BIN_MEDIUM, VTX_SIZE_BIN_SMALL, 1, 0x44);
    VTX_ASSERT_FALSE(vtx_input_shape_equals(shape_a, shape_b));
}

VTX_TEST(shape_make_same_inputs_same_shape)
{
    vtx_input_shape_t s1 = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_MEDIUM, VTX_SIZE_BIN_SMALL, 3, 0x44);
    vtx_input_shape_t s2 = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_MEDIUM, VTX_SIZE_BIN_SMALL, 3, 0x44);
    VTX_ASSERT_TRUE(vtx_input_shape_equals(s1, s2));
}

VTX_TEST(shape_decode_size_bin)
{
    vtx_input_shape_t s = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_LARGE, VTX_SIZE_BIN_TINY, 1, 0x44);
    VTX_ASSERT_EQUAL(VTX_SIZE_BIN_LARGE, vtx_input_shape_size_bin(s));
    VTX_ASSERT_EQUAL(VTX_SIZE_BIN_TINY, vtx_input_shape_trip_bin(s));
}

/* ========================================================================== */
/* Sprint 4.2: Per-shape profile storage                                       */
/* ========================================================================== */

VTX_TEST(shape_table_init_creates_default)
{
    vtx_input_shape_table_t table;
    VTX_ASSERT_EQUAL(0, vtx_input_shape_table_init(&table, 42));

    VTX_ASSERT_EQUAL(1u, vtx_input_shape_table_shape_count(&table));
    VTX_ASSERT_NOT_NULL(vtx_input_shape_table_get_default(&table));

    vtx_input_shape_table_destroy(&table);
}

VTX_TEST(shape_table_get_or_create_new_shape)
{
    vtx_input_shape_table_t table;
    VTX_ASSERT_EQUAL(0, vtx_input_shape_table_init(&table, 1));

    vtx_input_shape_t shape = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_LARGE, VTX_SIZE_BIN_LARGE, 1, 0x44);

    vtx_profile_method_t *m = vtx_input_shape_table_get_or_create(&table, shape, 0);
    VTX_ASSERT_NOT_NULL(m);

    /* Should now have 2 shapes: default + the new one. */
    VTX_ASSERT_EQUAL(2u, vtx_input_shape_table_shape_count(&table));

    vtx_input_shape_table_destroy(&table);
}

VTX_TEST(shape_table_get_existing_shape_returns_same_ptr)
{
    vtx_input_shape_table_t table;
    VTX_ASSERT_EQUAL(0, vtx_input_shape_table_init(&table, 1));

    vtx_input_shape_t shape = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_LARGE, VTX_SIZE_BIN_LARGE, 1, 0x44);

    vtx_profile_method_t *m1 = vtx_input_shape_table_get_or_create(&table, shape, 0);
    vtx_profile_method_t *m2 = vtx_input_shape_table_get_or_create(&table, shape, 0);

    VTX_ASSERT_EQUAL(m1, m2);  /* same pointer — not a new entry */
    VTX_ASSERT_EQUAL(2u, vtx_input_shape_table_shape_count(&table));

    vtx_input_shape_table_destroy(&table);
}

/**
 * THE HEADLINE TEST: different input shapes get different profiles.
 *
 * A method called with small arrays (size bin TINY) vs large arrays
 * (size bin LARGE) should have SEPARATE profile data. The branch
 * probabilities recorded for small arrays should NOT leak into the
 * large-array profile.
 *
 * Without input-shape-keying (the old behavior), these would be merged
 * into one profile, producing wrong specialization.
 */
VTX_TEST(shape_table_different_shapes_separate_profiles)
{
    vtx_input_shape_table_t table;
    VTX_ASSERT_EQUAL(0, vtx_input_shape_table_init(&table, 1));

    /* Shape A: small arrays. */
    vtx_input_shape_t shape_small = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_TINY, VTX_SIZE_BIN_TINY, 1, 0x44);
    vtx_profile_method_t *m_small = vtx_input_shape_table_get_or_create(&table, shape_small, 0);
    VTX_ASSERT_NOT_NULL(m_small);

    /* Record branch data for small arrays: 90% taken. */
    for (int i = 0; i < 900; i++) {
        m_small->invocation_count++;
        /* Manually record a branch at PC 10, taken=true. */
        if (m_small->branch_count == 0) {
            m_small->branches = calloc(4, sizeof(vtx_branch_profile_t));
            m_small->branch_capacity = 4;
            m_small->branches[0].bytecode_pc = 10;
            m_small->branch_count = 1;
        }
        m_small->branches[0].taken++;
    }

    /* Shape B: large arrays. */
    vtx_input_shape_t shape_large = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_LARGE, VTX_SIZE_BIN_LARGE, 1, 0x44);
    vtx_profile_method_t *m_large = vtx_input_shape_table_get_or_create(&table, shape_large, 0);
    VTX_ASSERT_NOT_NULL(m_large);

    /* Record branch data for large arrays: 10% taken. */
    for (int i = 0; i < 1000; i++) {
        m_large->invocation_count++;
        if (m_large->branch_count == 0) {
            m_large->branches = calloc(4, sizeof(vtx_branch_profile_t));
            m_large->branch_capacity = 4;
            m_large->branches[0].bytecode_pc = 10;
            m_large->branch_count = 1;
        }
        m_large->branches[0].not_taken++;
    }

    /* Verify: the two shapes have SEPARATE profiles. */
    VTX_ASSERT_EQUAL(3u, vtx_input_shape_table_shape_count(&table));  /* default + 2 */

    /* Small shape: 900 taken, 0 not_taken → 100% taken. */
    VTX_ASSERT_EQUAL(900ull, m_small->branches[0].taken);
    VTX_ASSERT_EQUAL(0ull, m_small->branches[0].not_taken);

    /* Large shape: 0 taken, 1000 not_taken → 0% taken. */
    VTX_ASSERT_EQUAL(0ull, m_large->branches[0].taken);
    VTX_ASSERT_EQUAL(1000ull, m_large->branches[0].not_taken);

    /* The profiles are NOT merged — each shape has its own data. */
    VTX_ASSERT_TRUE(m_small != m_large);

    vtx_input_shape_table_destroy(&table);
}

/**
 * Without shape-keying (the old behavior), the same data would be
 * merged into one profile, producing P(taken) = 900/1900 ≈ 47%.
 * This test documents the bug that Sprint 4 fixes.
 */
VTX_TEST(shape_table_old_behavior_merges_profiles)
{
    /* Single profile (the old way). */
    vtx_profile_global_t g;
    vtx_profile_global_init(&g);
    vtx_profile_method_t *m = vtx_profile_add_method(&g, 1);
    VTX_ASSERT_NOT_NULL(m);

    /* Record 900 taken + 1000 not_taken (merged). */
    for (int i = 0; i < 900; i++) {
        vtx_profile_record_branch(&g, 1, 10, true);
    }
    for (int i = 0; i < 1000; i++) {
        vtx_profile_record_branch(&g, 1, 10, false);
    }

    /* The merged profile shows P(taken) = 900/1900 ≈ 47%. */
    /* Neither the 100% (small) nor the 0% (large) is preserved. */
    const vtx_branch_profile_t *b = vtx_profile_get_branch(&g, 1, 10);
    VTX_ASSERT_NOT_NULL(b);
    VTX_ASSERT_EQUAL(900ull, b->taken);
    VTX_ASSERT_EQUAL(1000ull, b->not_taken);
    /* P(taken) = 47% — WRONG for both shapes. */

    vtx_profile_global_destroy(&g);
}

/* ========================================================================== */
/* Sprint 4.2: Dominant shape and multi-version detection                      */
/* ========================================================================== */

VTX_TEST(shape_table_dominant_shape)
{
    vtx_input_shape_table_t table;
    VTX_ASSERT_EQUAL(0, vtx_input_shape_table_init(&table, 1));

    vtx_input_shape_t shape_a = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_TINY, VTX_SIZE_BIN_TINY, 1, 0x44);
    vtx_input_shape_t shape_b = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_LARGE, VTX_SIZE_BIN_LARGE, 1, 0x44);

    /* Observe shape_a 100 times, shape_b 500 times. */
    for (int i = 0; i < 100; i++) {
        vtx_input_shape_table_record_observation(&table, shape_a, 0);
    }
    for (int i = 0; i < 500; i++) {
        vtx_input_shape_table_record_observation(&table, shape_b, 0);
    }

    /* shape_b should be dominant (more observations). */
    vtx_input_shape_t dominant = vtx_input_shape_table_dominant_shape(&table);
    VTX_ASSERT_TRUE(vtx_input_shape_equals(dominant, shape_b));

    vtx_input_shape_table_destroy(&table);
}

VTX_TEST(shape_table_needs_multi_version_below_threshold)
{
    vtx_input_shape_table_t table;
    VTX_ASSERT_EQUAL(0, vtx_input_shape_table_init(&table, 1));

    vtx_input_shape_t shape_a = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_TINY, VTX_SIZE_BIN_TINY, 1, 0x44);
    vtx_input_shape_t shape_b = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_LARGE, VTX_SIZE_BIN_LARGE, 1, 0x44);

    /* Both shapes have samples but below the multi-version threshold. */
    for (int i = 0; i < 50; i++) {
        vtx_input_shape_table_record_observation(&table, shape_a, 0);
        vtx_input_shape_table_record_observation(&table, shape_b, 0);
    }

    VTX_ASSERT_FALSE(vtx_input_shape_table_needs_multi_version(&table));

    vtx_input_shape_table_destroy(&table);
}

VTX_TEST(shape_table_needs_multi_version_above_threshold)
{
    vtx_input_shape_table_t table;
    VTX_ASSERT_EQUAL(0, vtx_input_shape_table_init(&table, 1));

    vtx_input_shape_t shape_a = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_TINY, VTX_SIZE_BIN_TINY, 1, 0x44);
    vtx_input_shape_t shape_b = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_LARGE, VTX_SIZE_BIN_LARGE, 1, 0x44);

    /* Both shapes have enough samples for multi-version. */
    for (int i = 0; i < 200; i++) {
        vtx_input_shape_table_record_observation(&table, shape_a, 0);
        vtx_input_shape_table_record_observation(&table, shape_b, 0);
    }

    VTX_ASSERT_TRUE(vtx_input_shape_table_needs_multi_version(&table));

    vtx_input_shape_table_destroy(&table);
}

/* ========================================================================== */
/* Sprint 4.2: LRU eviction                                                    */
/* ========================================================================== */

VTX_TEST(shape_table_evicts_lru_when_full)
{
    vtx_input_shape_table_t table;
    VTX_ASSERT_EQUAL(0, vtx_input_shape_table_init(&table, 1));

    /* Fill the table with VTX_INPUT_SHAPE_MAX_PER_METHOD shapes
     * (including the default at index 0). */
    for (uint32_t i = 1; i < VTX_INPUT_SHAPE_MAX_PER_METHOD; i++) {
        vtx_input_shape_t s = vtx_input_shape_make(
            (uint8_t)i, 0x22, VTX_SIZE_BIN_MEDIUM, VTX_SIZE_BIN_SMALL, 1, 0x44);
        vtx_input_shape_table_get_or_create(&table, s, i * 1000);
    }

    VTX_ASSERT_EQUAL(VTX_INPUT_SHAPE_MAX_PER_METHOD,
                       vtx_input_shape_table_shape_count(&table));

    /* Add one more shape — should evict the LRU (shape 1, earliest timestamp). */
    vtx_input_shape_t new_shape = vtx_input_shape_make(
        0xFF, 0x22, VTX_SIZE_BIN_LARGE, VTX_SIZE_BIN_LARGE, 1, 0x44);
    vtx_profile_method_t *m = vtx_input_shape_table_get_or_create(&table, new_shape, 999999);
    VTX_ASSERT_NOT_NULL(m);

    /* The table should still be at max capacity (eviction made room). */
    VTX_ASSERT_EQUAL(VTX_INPUT_SHAPE_MAX_PER_METHOD,
                       vtx_input_shape_table_shape_count(&table));

    /* The evicted shape (shape 1) should no longer exist. */
    vtx_input_shape_t evicted = vtx_input_shape_make(
        0x01, 0x22, VTX_SIZE_BIN_MEDIUM, VTX_SIZE_BIN_SMALL, 1, 0x44);
    VTX_ASSERT_NULL(vtx_input_shape_table_get(&table, evicted));

    /* Eviction should have been counted. */
    uint32_t sc; uint64_t obs, trans, evict;
    vtx_input_shape_t dom;
    vtx_input_shape_table_stats(&table, &sc, &obs, &trans, &evict, &dom);
    VTX_ASSERT_TRUE(evict >= 1);

    vtx_input_shape_table_destroy(&table);
}

/* ========================================================================== */
/* Sprint 4.2: Global shape manager                                            */
/* ========================================================================== */

VTX_TEST(shape_manager_init_destroy)
{
    vtx_input_shape_manager_t mgr;
    VTX_ASSERT_EQUAL(0, vtx_input_shape_manager_init(&mgr));

    uint64_t obs, mv;
    uint32_t tc;
    vtx_input_shape_manager_stats(&mgr, &obs, &mv, &tc);
    VTX_ASSERT_EQUAL(0ull, obs);
    VTX_ASSERT_EQUAL(0u, tc);

    vtx_input_shape_manager_destroy(&mgr);
}

VTX_TEST(shape_manager_get_or_create_table)
{
    vtx_input_shape_manager_t mgr;
    VTX_ASSERT_EQUAL(0, vtx_input_shape_manager_init(&mgr));

    vtx_input_shape_table_t *t1 = vtx_input_shape_manager_get_or_create(&mgr, 5);
    VTX_ASSERT_NOT_NULL(t1);
    vtx_input_shape_table_t *t2 = vtx_input_shape_manager_get_or_create(&mgr, 5);
    VTX_ASSERT_EQUAL(t1, t2);  /* same table returned */

    uint32_t tc;
    vtx_input_shape_manager_stats(&mgr, NULL, NULL, &tc);
    VTX_ASSERT_TRUE(tc >= 1);

    vtx_input_shape_manager_destroy(&mgr);
}

VTX_TEST(shape_manager_get_profile_falls_back_to_default)
{
    vtx_input_shape_manager_t mgr;
    VTX_ASSERT_EQUAL(0, vtx_input_shape_manager_init(&mgr));

    /* Create a table for method 1, but only record to the default shape. */
    vtx_input_shape_table_t *table = vtx_input_shape_manager_get_or_create(&mgr, 1);
    VTX_ASSERT_NOT_NULL(table);
    vtx_profile_method_t *def = vtx_input_shape_table_get_default(table);
    VTX_ASSERT_NOT_NULL(def);
    def->invocation_count = 42;

    /* Query a shape that doesn't exist — should fall back to default. */
    vtx_input_shape_t unknown_shape = vtx_input_shape_make(
        0x99, 0x88, VTX_SIZE_BIN_LARGE, VTX_SIZE_BIN_LARGE, 1, 0x77);
    vtx_profile_method_t *m = vtx_input_shape_manager_get_profile(&mgr, 1, unknown_shape);
    VTX_ASSERT_EQUAL(def, m);  /* got the default */
    VTX_ASSERT_EQUAL(42ull, m->invocation_count);

    vtx_input_shape_manager_destroy(&mgr);
}

/* ========================================================================== */
/* Sprint 4.3: Shape dispatch                                                  */
/* ========================================================================== */

VTX_TEST(dispatch_init_destroy)
{
    vtx_shape_dispatch_mgr_t mgr;
    VTX_ASSERT_EQUAL(0, vtx_shape_dispatch_mgr_init(&mgr));

    uint64_t disp, fb, vc;
    uint32_t tc;
    vtx_shape_dispatch_stats(&mgr, &disp, &fb, &vc, &tc);
    VTX_ASSERT_EQUAL(0ull, disp);
    VTX_ASSERT_EQUAL(0ull, fb);
    VTX_ASSERT_EQUAL(0ull, vc);
    VTX_ASSERT_EQUAL(0u, tc);

    vtx_shape_dispatch_mgr_destroy(&mgr);
}

VTX_TEST(dispatch_install_default_lookup)
{
    vtx_shape_dispatch_mgr_t mgr;
    VTX_ASSERT_EQUAL(0, vtx_shape_dispatch_mgr_init(&mgr));

    /* Install a default version for method 1. */
    void *fake_code = (void *)0x1000;
    VTX_ASSERT_EQUAL(0, vtx_shape_dispatch_install_default(&mgr, 1, fake_code, NULL));

    /* Look up with any shape — should return the default. */
    vtx_input_shape_t any_shape = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_LARGE, VTX_SIZE_BIN_LARGE, 1, 0x44);
    void *code = vtx_shape_dispatch_lookup(&mgr, 1, any_shape);
    VTX_ASSERT_EQUAL(fake_code, code);

    vtx_shape_dispatch_mgr_destroy(&mgr);
}

/**
 * THE HEADLINE DISPATCH TEST: different shapes dispatch to different
 * compiled versions.
 *
 * Install two shape-specific versions + a default. Look up each shape
 * and verify the correct version is returned.
 */
VTX_TEST(dispatch_different_shapes_different_versions)
{
    vtx_shape_dispatch_mgr_t mgr;
    VTX_ASSERT_EQUAL(0, vtx_shape_dispatch_mgr_init(&mgr));

    /* Install default version. */
    void *default_code = (void *)0x1000;
    VTX_ASSERT_EQUAL(0, vtx_shape_dispatch_install_default(&mgr, 1, default_code, NULL));

    /* Install shape-specific version for "small arrays". */
    vtx_input_shape_t shape_small = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_TINY, VTX_SIZE_BIN_TINY, 1, 0x44);
    void *small_code = (void *)0x2000;
    VTX_ASSERT_EQUAL(0, vtx_shape_dispatch_install(&mgr, 1, shape_small, small_code, NULL));

    /* Install shape-specific version for "large arrays". */
    vtx_input_shape_t shape_large = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_LARGE, VTX_SIZE_BIN_LARGE, 1, 0x44);
    void *large_code = (void *)0x3000;
    VTX_ASSERT_EQUAL(0, vtx_shape_dispatch_install(&mgr, 1, shape_large, large_code, NULL));

    /* Look up shape_small → should get small_code. */
    void *code1 = vtx_shape_dispatch_lookup(&mgr, 1, shape_small);
    VTX_ASSERT_EQUAL(small_code, code1);

    /* Look up shape_large → should get large_code. */
    void *code2 = vtx_shape_dispatch_lookup(&mgr, 1, shape_large);
    VTX_ASSERT_EQUAL(large_code, code2);

    /* Look up an unknown shape → should fall back to default_code. */
    vtx_input_shape_t unknown_shape = vtx_input_shape_make(
        0x99, 0x88, VTX_SIZE_BIN_MEDIUM, VTX_SIZE_BIN_MEDIUM, 1, 0x77);
    void *code3 = vtx_shape_dispatch_lookup(&mgr, 1, unknown_shape);
    VTX_ASSERT_EQUAL(default_code, code3);

    /* Verify version count (2 shape-specific, excluding default). */
    VTX_ASSERT_EQUAL(2u, vtx_shape_dispatch_version_count(&mgr, 1));

    vtx_shape_dispatch_mgr_destroy(&mgr);
}

VTX_TEST(dispatch_lookup_unknown_method_returns_null)
{
    vtx_shape_dispatch_mgr_t mgr;
    VTX_ASSERT_EQUAL(0, vtx_shape_dispatch_mgr_init(&mgr));

    vtx_input_shape_t shape = vtx_input_shape_make(
        0x11, 0x22, VTX_SIZE_BIN_LARGE, VTX_SIZE_BIN_LARGE, 1, 0x44);
    void *code = vtx_shape_dispatch_lookup(&mgr, 999, shape);
    VTX_ASSERT_NULL(code);

    vtx_shape_dispatch_mgr_destroy(&mgr);
}

VTX_TEST(dispatch_record_stats)
{
    vtx_shape_dispatch_mgr_t mgr;
    VTX_ASSERT_EQUAL(0, vtx_shape_dispatch_mgr_init(&mgr));

    vtx_shape_dispatch_record(&mgr, true);   /* shape-specific */
    vtx_shape_dispatch_record(&mgr, true);
    vtx_shape_dispatch_record(&mgr, false);  /* default fallback */

    uint64_t disp, fb, vc;
    uint32_t tc;
    vtx_shape_dispatch_stats(&mgr, &disp, &fb, &vc, &tc);
    VTX_ASSERT_EQUAL(2ull, disp);
    VTX_ASSERT_EQUAL(1ull, fb);

    vtx_shape_dispatch_mgr_destroy(&mgr);
}

/* ========================================================================== */
/* Test runner                                                                 */
/* ========================================================================== */

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\n");
    printf("=== PGO Sprint 4 Input-Shape-Keyed Profiles Tests ===\n");
    printf("  Passed: %u / %u\n", result.pass_count, result.total_count);
    printf("  Failed: %u\n", result.fail_count);
    return (result.fail_count == 0) ? 0 : 1;
}
