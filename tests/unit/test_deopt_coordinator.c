/*
 * VORTEX Deopt Coordinator Integration Tests
 *
 * Audit priority #7 (Hardening) — validates the integration of rate_limit.c
 * primitives into the deopt decision path.
 *
 * Tests:
 *   1. Single guard fail → DEOPT_COORD_DEOPTLESS decision
 *   2. 100 fails in 1 sec at same site → DEOPT_COORD_POISONED
 *   3. 10000 fails globally in 1 sec → DEOPT_COORD_SUPPRESSED
 *   4. Batcher flushes trigger recompile callback
 *   5. Poisoned site stays poisoned on subsequent calls
 *   6. Suppression clears when rate drops
 *   7. Stats string format
 */

#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "deopt/coordinator.h"

#define NS_PER_SEC 1000000000ULL

/* Test callback: records how many times it was called and the total sites. */
typedef struct {
    uint32_t call_count;
    uint32_t total_sites;
} test_recompile_state_t;

static void test_recompile_fn(const uint32_t *site_ids, uint32_t count,
                               void *user_data)
{
    test_recompile_state_t *st = (test_recompile_state_t *)user_data;
    st->call_count++;
    st->total_sites += count;
}

VTX_TEST(deopt_coord_single_fail_deoptless) {
    vtx_deopt_coord_t coord;
    VTX_ASSERT_TRUE(vtx_deopt_coord_init(&coord, NULL, NULL) == 0);

    /* A single guard failure should return DEOPTLESS (default decision). */
    vtx_deopt_decision_t d = vtx_deopt_coord_on_guard_fail(&coord, 42, 1000000000ULL);
    VTX_ASSERT_TRUE(d == DEOPT_COORD_DEOPTLESS);
    VTX_ASSERT_TRUE(coord.total_guard_failures == 1);
    VTX_ASSERT_TRUE(coord.total_deoptless == 1);

    vtx_deopt_coord_destroy(&coord);
}

VTX_TEST(deopt_coord_poisons_after_threshold) {
    vtx_deopt_coord_t coord;
    VTX_ASSERT_TRUE(vtx_deopt_coord_init(&coord, NULL, NULL) == 0);

    /* 99 failures → still DEOPTLESS */
    uint64_t now = 1000000000ULL;
    for (uint32_t i = 0; i < VTX_DEOPT_SITE_FAIL_THRESHOLD - 1; i++) {
        vtx_deopt_decision_t d = vtx_deopt_coord_on_guard_fail(&coord, 7, now);
        VTX_ASSERT_TRUE(d == DEOPT_COORD_DEOPTLESS);
    }
    VTX_ASSERT_TRUE(!vtx_deopt_coord_is_poisoned(&coord, 7));

    /* 100th failure → POISONED */
    vtx_deopt_decision_t d = vtx_deopt_coord_on_guard_fail(&coord, 7, now);
    VTX_ASSERT_TRUE(d == DEOPT_COORD_POISONED);
    VTX_ASSERT_TRUE(vtx_deopt_coord_is_poisoned(&coord, 7));
    VTX_ASSERT_TRUE(coord.total_poisoned == 1);

    /* Subsequent failures at same site stay POISONED */
    d = vtx_deopt_coord_on_guard_fail(&coord, 7, now);
    VTX_ASSERT_TRUE(d == DEOPT_COORD_POISONED);

    vtx_deopt_coord_destroy(&coord);
}

VTX_TEST(deopt_coord_global_suppression) {
    vtx_deopt_coord_t coord;
    VTX_ASSERT_TRUE(vtx_deopt_coord_init(&coord, NULL, NULL) == 0);

    /* Spread failures across many sites so no single site poisons.
     * Total > 10000 in 1 sec → global suppression. */
    uint64_t now = 1000000000ULL;
    for (uint32_t i = 0; i < VTX_DEOPT_GLOBAL_FAIL_THRESHOLD + 100; i++) {
        uint32_t site_id = i % 200;  /* spread across 200 sites */
        vtx_deopt_decision_t d = vtx_deopt_coord_on_guard_fail(&coord, site_id, now);
        (void)d;
        if (vtx_deopt_coord_is_suppressed(&coord, now)) break;
    }
    VTX_ASSERT_TRUE(vtx_deopt_coord_is_suppressed(&coord, now));
    VTX_ASSERT_TRUE(coord.total_suppressed > 0);

    /* Once suppressed, new failures return SUPPRESSED. */
    vtx_deopt_decision_t d = vtx_deopt_coord_on_guard_fail(&coord, 999, now);
    VTX_ASSERT_TRUE(d == DEOPT_COORD_SUPPRESSED);

    vtx_deopt_coord_destroy(&coord);
}

VTX_TEST(deopt_coord_batcher_triggers_callback) {
    test_recompile_state_t state = {0};
    vtx_deopt_coord_t coord;
    VTX_ASSERT_TRUE(vtx_deopt_coord_init(&coord, test_recompile_fn, &state) == 0);

    /* Add enough deopts to fill a batch (VTX_DEOPT_BATCH_MAX_PENDING = 64). */
    uint64_t now = 1000000000ULL;
    for (uint32_t i = 0; i < VTX_DEOPT_BATCH_MAX_PENDING; i++) {
        vtx_deopt_coord_on_guard_fail(&coord, i, now);
    }

    /* The batcher should have flushed at least once, triggering the callback. */
    VTX_ASSERT_TRUE(state.call_count >= 1);
    VTX_ASSERT_TRUE(state.total_sites >= VTX_DEOPT_BATCH_MAX_PENDING);
    VTX_ASSERT_TRUE(coord.total_batches_flushed >= 1);

    vtx_deopt_coord_destroy(&coord);
}

VTX_TEST(deopt_coord_batcher_flushes_on_window) {
    test_recompile_state_t state = {0};
    vtx_deopt_coord_t coord;
    VTX_ASSERT_TRUE(vtx_deopt_coord_init(&coord, test_recompile_fn, &state) == 0);

    /* Add a few deopts, then advance time past the batch window. */
    uint64_t now = 1000000000ULL;
    for (uint32_t i = 0; i < 5; i++) {
        vtx_deopt_coord_on_guard_fail(&coord, i, now);
    }
    VTX_ASSERT_TRUE(state.call_count == 0);  /* not flushed yet */

    /* Advance past the 1ms window. */
    now += VTX_DEOPT_BATCH_WINDOW_NS + 1;

    /* Next deopt should trigger a flush of the old batch + new entry. */
    vtx_deopt_coord_on_guard_fail(&coord, 99, now);
    VTX_ASSERT_TRUE(state.call_count >= 1);
    VTX_ASSERT_TRUE(state.total_sites >= 5);

    vtx_deopt_coord_destroy(&coord);
}

VTX_TEST(deopt_coord_suppression_clears_when_rate_drops) {
    vtx_deopt_coord_t coord;
    VTX_ASSERT_TRUE(vtx_deopt_coord_init(&coord, NULL, NULL) == 0);

    /* Trigger suppression. */
    uint64_t now = 1000000000ULL;
    for (uint32_t i = 0; i < VTX_DEOPT_GLOBAL_FAIL_THRESHOLD; i++) {
        vtx_deopt_coord_on_guard_fail(&coord, i % 200, now);
    }
    VTX_ASSERT_TRUE(vtx_deopt_coord_is_suppressed(&coord, now));

    /* Wait 2 seconds — old entries expire, rate drops. */
    now += 2 * NS_PER_SEC;
    vtx_deopt_coord_on_guard_fail(&coord, 1, now);
    VTX_ASSERT_TRUE(!vtx_deopt_coord_is_suppressed(&coord, now));

    vtx_deopt_coord_destroy(&coord);
}

VTX_TEST(deopt_coord_stats_str) {
    vtx_deopt_coord_t coord;
    VTX_ASSERT_TRUE(vtx_deopt_coord_init(&coord, NULL, NULL) == 0);

    /* Generate some activity. */
    uint64_t now = 1000000000ULL;
    for (uint32_t i = 0; i < 10; i++) {
        vtx_deopt_coord_on_guard_fail(&coord, i, now);
    }

    char buf[512];
    vtx_deopt_coord_stats_str(&coord, buf, sizeof(buf));
    printf("[coord] stats: %s\n", buf);
    VTX_ASSERT_TRUE(strstr(buf, "guard_fails=10") != NULL);
    VTX_ASSERT_TRUE(strstr(buf, "deoptless=10") != NULL);

    vtx_deopt_coord_destroy(&coord);
}

VTX_TEST(deopt_coord_distinct_sites_dont_interfere) {
    vtx_deopt_coord_t coord;
    VTX_ASSERT_TRUE(vtx_deopt_coord_init(&coord, NULL, NULL) == 0);

    /* 50 failures at site 1, 50 at site 2 — neither should poison. */
    uint64_t now = 1000000000ULL;
    for (uint32_t i = 0; i < 50; i++) {
        vtx_deopt_coord_on_guard_fail(&coord, 1, now);
        vtx_deopt_coord_on_guard_fail(&coord, 2, now);
    }
    VTX_ASSERT_TRUE(!vtx_deopt_coord_is_poisoned(&coord, 1));
    VTX_ASSERT_TRUE(!vtx_deopt_coord_is_poisoned(&coord, 2));

    /* 50 more at site 1 → poisons (100 total at site 1). */
    for (uint32_t i = 0; i < 50; i++) {
        vtx_deopt_coord_on_guard_fail(&coord, 1, now);
    }
    VTX_ASSERT_TRUE(vtx_deopt_coord_is_poisoned(&coord, 1));
    VTX_ASSERT_TRUE(!vtx_deopt_coord_is_poisoned(&coord, 2));

    vtx_deopt_coord_destroy(&coord);
}

VTX_TEST(deopt_coord_force_flush) {
    test_recompile_state_t state = {0};
    vtx_deopt_coord_t coord;
    VTX_ASSERT_TRUE(vtx_deopt_coord_init(&coord, test_recompile_fn, &state) == 0);

    /* Add 3 deopts (not enough to auto-flush). */
    uint64_t now = 1000000000ULL;
    for (uint32_t i = 0; i < 3; i++) {
        vtx_deopt_coord_on_guard_fail(&coord, i, now);
    }
    VTX_ASSERT_TRUE(state.call_count == 0);

    /* Force flush — should trigger callback with 3 sites. */
    uint32_t n = vtx_deopt_coord_flush(&coord, now);
    VTX_ASSERT_TRUE(n == 3);
    VTX_ASSERT_TRUE(state.call_count == 1);
    VTX_ASSERT_TRUE(state.total_sites == 3);

    vtx_deopt_coord_destroy(&coord);
}

int main(void) {
    printf("=== VORTEX Deopt Coordinator Integration Tests ===\n\n");
    vtx_test_run_all();
    return 0;
}
