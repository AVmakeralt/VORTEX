/**
 * VORTEX PGO Sprint 2 Phase Partitioning Tests
 *
 * Tests for:
 *   - Sprint 2.1: Phase partition manager (init, transition, get_active)
 *   - Sprint 2.2: Per-phase persistence (save/load each phase)
 *   - Sprint 2.3: Orchestrator wiring (transition + preemptive recompile)
 *
 * The headline test is "10 workloads don't poison each other": record
 * 10 different workloads each into their own phase, then verify that
 * each phase's profile contains only its own data — no leakage.
 */

#include "test_framework.h"
#include "vortex_config.h"
#include "profile/data.h"
#include "profile/phase.h"
#include "profile/phase_partition.h"
#include "profile/phase_persist.h"
#include "profile/persist.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

/* ========================================================================== */
/* Sprint 2.1: Partition manager basics                                        */
/* ========================================================================== */

VTX_TEST(partition_init_creates_default_phase)
{
    vtx_phase_partition_t part;
    VTX_ASSERT_EQUAL(0, vtx_phase_partition_init(&part));

    /* Default phase should exist and be active. */
    VTX_ASSERT_EQUAL(1u, vtx_phase_partition_phase_count(&part));
    VTX_ASSERT_EQUAL(VTX_PHASE_NONE, vtx_phase_partition_active_phase(&part));

    /* Active profile should be non-NULL. */
    VTX_ASSERT_NOT_NULL(vtx_phase_partition_get_active(&part));

    vtx_phase_partition_destroy(&part);
}

VTX_TEST(partition_transition_creates_new_phase)
{
    vtx_phase_partition_t part;
    VTX_ASSERT_EQUAL(0, vtx_phase_partition_init(&part));

    /* Transition to a new phase. */
    vtx_profile_global_t *p = vtx_phase_partition_transition(&part, 100, 0);
    VTX_ASSERT_NOT_NULL(p);
    VTX_ASSERT_EQUAL(100u, vtx_phase_partition_active_phase(&part));
    VTX_ASSERT_EQUAL(2u, vtx_phase_partition_phase_count(&part));

    vtx_phase_partition_destroy(&part);
}

VTX_TEST(partition_transition_same_phase_is_noop)
{
    vtx_phase_partition_t part;
    VTX_ASSERT_EQUAL(0, vtx_phase_partition_init(&part));

    uint64_t transitions_before;
    uint64_t creations;
    uint32_t active;
    vtx_phase_partition_stats(&part, &transitions_before, &creations, &active);

    /* Transition to the same phase (default). */
    vtx_profile_global_t *p = vtx_phase_partition_transition(&part, VTX_PHASE_NONE, 0);
    VTX_ASSERT_NOT_NULL(p);

    uint64_t transitions_after;
    vtx_phase_partition_stats(&part, &transitions_after, NULL, NULL);

    /* No transition recorded (same phase). */
    VTX_ASSERT_EQUAL(transitions_before, transitions_after);

    vtx_phase_partition_destroy(&part);
}

VTX_TEST(partition_get_or_create_does_not_change_active)
{
    vtx_phase_partition_t part;
    VTX_ASSERT_EQUAL(0, vtx_phase_partition_init(&part));

    /* Create phase 5 but don't transition to it. */
    vtx_profile_global_t *p5 = vtx_phase_partition_get_or_create(&part, 5);
    VTX_ASSERT_NOT_NULL(p5);

    /* Active phase should still be default. */
    VTX_ASSERT_EQUAL(VTX_PHASE_NONE, vtx_phase_partition_active_phase(&part));
    VTX_ASSERT_EQUAL(2u, vtx_phase_partition_phase_count(&part));

    /* But we can still query phase 5's profile. */
    VTX_ASSERT_EQUAL(p5, vtx_phase_partition_get_phase(&part, 5));

    vtx_phase_partition_destroy(&part);
}

/* ========================================================================== */
/* Sprint 2.1: The headline test — 10 workloads don't poison each other        */
/* ========================================================================== */

/**
 * Simulate 10 different workloads running in 10 different phases.
 *
 * Each workload records a distinct type at a call site in its phase's
 * profile. After all 10 workloads complete, we verify that:
 *   - Each phase's profile contains ONLY its own type (no leakage)
 *   - The default phase is empty (no workload recorded to it)
 *   - Each phase has exactly 1 call site with 1 type
 */
VTX_TEST(partition_ten_workloads_no_poisoning)
{
    vtx_phase_partition_t part;
    VTX_ASSERT_EQUAL(0, vtx_phase_partition_init(&part));

    /* 10 phases, each recording a distinct type (1000..1009) at
     * callsite 0 in method 1. */
    const uint32_t NUM_PHASES = 10;
    for (uint32_t ph = 0; ph < NUM_PHASES; ph++) {
        uint32_t phase_id = ph + 1;  /* phase IDs 1..10 */
        vtx_profile_global_t *p = vtx_phase_partition_transition(&part, phase_id, 0);
        VTX_ASSERT_NOT_NULL(p);

        /* Record this phase's signature: method 1, callsite 0, type (1000+ph). */
        vtx_profile_record_invocation(p, 1);
        vtx_profile_record_callsite_type(p, 1, 0, 1000 + ph);
    }

    /* Now verify: each phase's profile has ONLY its own type. */
    for (uint32_t ph = 0; ph < NUM_PHASES; ph++) {
        uint32_t phase_id = ph + 1;
        vtx_profile_global_t *p = vtx_phase_partition_get_phase(&part, phase_id);
        VTX_ASSERT_NOT_NULL(p);

        const vtx_profile_method_t *m = vtx_profile_get_method(p, 1);
        VTX_ASSERT_NOT_NULL(m);
        VTX_ASSERT_EQUAL(1u, m->call_site_count);

        const vtx_callsite_profile_t *cs = &m->call_sites[0];
        VTX_ASSERT_FALSE(cs->megamorphic);
        VTX_ASSERT_EQUAL(1u, cs->count);
        VTX_ASSERT_EQUAL((vtx_typeid_t)(1000 + ph), cs->types[0]);
    }

    /* Default phase should have no method data (we never recorded to it). */
    vtx_profile_global_t *def = vtx_phase_partition_get_phase(&part, VTX_PHASE_NONE);
    VTX_ASSERT_NOT_NULL(def);
    VTX_ASSERT_EQUAL(0u, def->method_count);

    /* 11 phases total: default + 10. */
    VTX_ASSERT_EQUAL(NUM_PHASES + 1, vtx_phase_partition_phase_count(&part));

    vtx_phase_partition_destroy(&part);
}

/**
 * Verify that without partitioning (the old behavior), the same 10
 * workloads WOULD poison each other. This test documents the bug that
 * Sprint 2 fixes.
 */
VTX_TEST(partition_old_behavior_poisoned_profiles)
{
    /* Single global profile (the old way). */
    vtx_profile_global_t g;
    VTX_ASSERT_EQUAL(0, vtx_profile_global_init(&g));

    /* Record 10 different types at the same call site. */
    for (uint32_t ph = 0; ph < 10; ph++) {
        vtx_profile_record_invocation(&g, 1);
        vtx_profile_record_callsite_type(&g, 1, 0, 1000 + ph);
    }

    /* Without partitioning, the call site becomes megamorphic
     * (5+ types exceeds VTX_POLY_LIMIT of 4). This is exactly the
     * poisoning that Sprint 2 prevents. */
    const vtx_profile_method_t *m = vtx_profile_get_method(&g, 1);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(1u, m->call_site_count);
    VTX_ASSERT_TRUE(m->call_sites[0].megamorphic);  /* poisoned! */

    vtx_profile_global_destroy(&g);
}

/* ========================================================================== */
/* Sprint 2.1: Branch probability isolation                                    */
/* ========================================================================== */

/**
 * Verify that branch probabilities don't leak across phases.
 *
 * Phase A: branch taken 1000 times, not taken 0 times (P(taken) = 100%)
 * Phase B: branch taken 0 times, not taken 1000 times (P(taken) = 0%)
 *
 * With partitioning, each phase keeps its own probability.
 * Without partitioning, the merged profile would show P(taken) = 50%.
 */
VTX_TEST(partition_branch_probability_isolation)
{
    vtx_phase_partition_t part;
    VTX_ASSERT_EQUAL(0, vtx_phase_partition_init(&part));

    /* Phase A: branch always taken. */
    vtx_profile_global_t *pa = vtx_phase_partition_transition(&part, 1, 0);
    for (int i = 0; i < 1000; i++) {
        vtx_profile_record_branch(pa, 1, 10, true);
    }

    /* Phase B: branch never taken. */
    vtx_profile_global_t *pb = vtx_phase_partition_transition(&part, 2, 0);
    for (int i = 0; i < 1000; i++) {
        vtx_profile_record_branch(pb, 1, 10, false);
    }

    /* Verify phase A's branch is still 100% taken. */
    pa = vtx_phase_partition_get_phase(&part, 1);
    const vtx_profile_method_t *ma = vtx_profile_get_method(pa, 1);
    VTX_ASSERT_NOT_NULL(ma);
    VTX_ASSERT_EQUAL(1u, ma->branch_count);
    VTX_ASSERT_EQUAL(1000ull, ma->branches[0].taken);
    VTX_ASSERT_EQUAL(0ull, ma->branches[0].not_taken);

    /* Verify phase B's branch is still 0% taken. */
    pb = vtx_phase_partition_get_phase(&part, 2);
    const vtx_profile_method_t *mb = vtx_profile_get_method(pb, 1);
    VTX_ASSERT_NOT_NULL(mb);
    VTX_ASSERT_EQUAL(1u, mb->branch_count);
    VTX_ASSERT_EQUAL(0ull, mb->branches[0].taken);
    VTX_ASSERT_EQUAL(1000ull, mb->branches[0].not_taken);

    vtx_phase_partition_destroy(&part);
}

/* ========================================================================== */
/* Sprint 2.1: Transition statistics                                           */
/* ========================================================================== */

VTX_TEST(partition_transition_stats)
{
    vtx_phase_partition_t part;
    VTX_ASSERT_EQUAL(0, vtx_phase_partition_init(&part));

    /* Transition: default → 1 → 2 → 1 → 2 → 1 (5 transitions). */
    vtx_phase_partition_transition(&part, 1, 0);
    vtx_phase_partition_transition(&part, 2, 0);
    vtx_phase_partition_transition(&part, 1, 0);
    vtx_phase_partition_transition(&part, 2, 0);
    vtx_phase_partition_transition(&part, 1, 0);

    uint64_t transitions, creations;
    uint32_t active;
    vtx_phase_partition_stats(&part, &transitions, &creations, &active);

    VTX_ASSERT_EQUAL(5ull, transitions);    /* 5 explicit transitions */
    /* creations counts ALL phases ever created, including the default
     * phase created by init(). So: default (1) + phase 1 + phase 2 = 3. */
    VTX_ASSERT_EQUAL(3ull, creations);
    VTX_ASSERT_EQUAL(1u, active);            /* currently in phase 1 */

    vtx_phase_partition_destroy(&part);
}

/* ========================================================================== */
/* Sprint 2.2: Per-phase persistence                                           */
/* ========================================================================== */

VTX_TEST(partition_persistence_save_load_roundtrip)
{
    vtx_phase_partition_t part;
    VTX_ASSERT_EQUAL(0, vtx_phase_partition_init(&part));

    /* Record distinct data in 3 phases. */
    for (uint32_t ph = 1; ph <= 3; ph++) {
        vtx_profile_global_t *p = vtx_phase_partition_transition(&part, ph, 0);
        vtx_profile_record_invocation(p, ph);
        vtx_profile_record_callsite_type(p, ph, 0, 5000 + ph);
    }

    /* Save all phases to a temp directory. */
    const char *dir = "/tmp/vortex_sprint2_test";
    mkdir(dir, 0755);

    /* Clean any leftover files. */
    DIR *d = opendir(dir);
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char path[600];
            snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
            remove(path);
        }
        closedir(d);
    }

    uint8_t hash[VTX_PROFILE_HASH_SIZE];
    memset(hash, 0xBB, sizeof(hash));
    char hash_hex[33];
    for (int i = 0; i < 16; i++) {
        snprintf(hash_hex + i * 2, 3, "%02x", hash[i]);
    }
    hash_hex[32] = '\0';

    int saved = vtx_phase_partition_save_all(&part, dir, hash_hex, hash);
    VTX_ASSERT_EQUAL(4, saved);  /* default + 3 phases */

    /* Load into a fresh partition. */
    vtx_phase_partition_t part2;
    VTX_ASSERT_EQUAL(0, vtx_phase_partition_init(&part2));

    int loaded = vtx_phase_partition_load_all(&part2, dir, hash_hex, hash);
    VTX_ASSERT_TRUE(loaded >= 3);  /* at least the 3 explicit phases */

    /* Verify each phase's data survived the round trip. */
    for (uint32_t ph = 1; ph <= 3; ph++) {
        vtx_profile_global_t *p = vtx_phase_partition_get_phase(&part2, ph);
        VTX_ASSERT_NOT_NULL(p);
        const vtx_profile_method_t *m = vtx_profile_get_method(p, ph);
        VTX_ASSERT_NOT_NULL(m);
        VTX_ASSERT_EQUAL(1u, m->call_site_count);
        VTX_ASSERT_EQUAL((vtx_typeid_t)(5000 + ph), m->call_sites[0].types[0]);
    }

    vtx_phase_partition_destroy(&part);
    vtx_phase_partition_destroy(&part2);
}

VTX_TEST(partition_persistence_filename_format)
{
    char buf[600];

    /* Default phase. */
    VTX_ASSERT_EQUAL(0, vtx_phase_partition_filename(
        "/tmp/profs", "abcdef0123456789", VTX_PHASE_NONE, buf, sizeof(buf)));
    VTX_ASSERT_TRUE(strcmp(buf, "/tmp/profs/abcdef0123456789.default.prof") == 0);

    /* Numeric phase. */
    VTX_ASSERT_EQUAL(0, vtx_phase_partition_filename(
        "/tmp/profs", "abcdef0123456789", 42, buf, sizeof(buf)));
    VTX_ASSERT_TRUE(strcmp(buf, "/tmp/profs/abcdef0123456789.42.prof") == 0);

    /* Buffer too small. */
    VTX_ASSERT_EQUAL(-1, vtx_phase_partition_filename(
        "/tmp/profs", "abcdef0123456789", 42, buf, 10));
}

VTX_TEST(partition_persistence_load_missing_dir_returns_zero)
{
    vtx_phase_partition_t part;
    VTX_ASSERT_EQUAL(0, vtx_phase_partition_init(&part));

    /* A directory that doesn't exist should return 0 (no phases loaded),
     * not an error. The default phase still exists from init. */
    int loaded = vtx_phase_partition_load_all(&part, "/nonexistent/dir/xyz",
                                                "abcdef0123456789", NULL);
    VTX_ASSERT_EQUAL(0, loaded);
    VTX_ASSERT_EQUAL(1u, vtx_phase_partition_phase_count(&part));  /* just default */

    vtx_phase_partition_destroy(&part);
}

/* ========================================================================== */
/* Sprint 2.1: Hot methods helper                                              */
/* ========================================================================== */

/**
 * Verify that hot_methods_for_phase returns the right methods in
 * heat order. We can't easily build a full phase graph in a unit test,
 * so this test focuses on the partition's part of the logic.
 */
VTX_TEST(partition_hot_methods_empty_phase_returns_zero)
{
    vtx_phase_partition_t part;
    VTX_ASSERT_EQUAL(0, vtx_phase_partition_init(&part));

    /* Phase 99 has no profile data — should return 0 methods. */
    uint32_t methods[8];
    /* Pass NULL graph since we just want to test the empty-profile path.
     * The function will early-return because phase 99 doesn't exist
     * in the partition. */
    uint32_t n = vtx_phase_partition_hot_methods_for_phase(
        &part, NULL, 99, methods, 8);
    VTX_ASSERT_EQUAL(0u, n);

    vtx_phase_partition_destroy(&part);
}

/* ========================================================================== */
/* Test runner                                                                 */
/* ========================================================================== */

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\n");
    printf("=== PGO Sprint 2 Phase Partitioning Tests ===\n");
    printf("  Passed: %u / %u\n", result.pass_count, result.total_count);
    printf("  Failed: %u\n", result.fail_count);
    return (result.fail_count == 0) ? 0 : 1;
}
