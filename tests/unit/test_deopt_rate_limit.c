/*
 * VORTEX Deopt Rate Limiter Tests
 *
 * Audit priority #7 (Hardening): validate the deopt storm resilience
 * primitives in src/deopt/rate_limit.c:
 *   1. Per-site rate limiter poisons a site after threshold failures.
 *   2. Per-site limiter resets properly and does NOT poison if failures
 *      are spread out over time (sliding window).
 *   3. Deopt batcher coalesces multiple failures into one flush.
 *   4. Deopt batcher flushes when window elapses OR buffer fills.
 *   5. Global budget suppresses compilation when threshold exceeded.
 *   6. Global budget unsuppresses when rate drops.
 */

#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "deopt/rate_limit.h"

#define NS_PER_SEC 1000000000ULL

VTX_TEST(deopt_rl_site_poisons_after_threshold) {
    vtx_deopt_site_limiter_t sl;
    VTX_ASSERT_TRUE(vtx_deopt_site_init(&sl, 256) == 0);

    /* Record 99 failures (just below threshold of 100). */
    uint64_t now = 0;
    for (uint32_t i = 0; i < VTX_DEOPT_SITE_FAIL_THRESHOLD - 1; i++) {
        bool poisoned = vtx_deopt_site_record_failure(&sl, now);
        VTX_ASSERT_TRUE(!poisoned);
    }
    VTX_ASSERT_TRUE(!vtx_deopt_site_is_poisoned(&sl));

    /* 100th failure should poison. */
    bool poisoned = vtx_deopt_site_record_failure(&sl, now);
    VTX_ASSERT_TRUE(poisoned);
    VTX_ASSERT_TRUE(vtx_deopt_site_is_poisoned(&sl));

    /* Further failures keep it poisoned. */
    VTX_ASSERT_TRUE(vtx_deopt_site_record_failure(&sl, now));

    vtx_deopt_site_destroy(&sl);
}

VTX_TEST(deopt_rl_site_sliding_window_does_not_poison) {
    /* Failures spread out over more than 1 second should NOT poison. */
    vtx_deopt_site_limiter_t sl;
    VTX_ASSERT_TRUE(vtx_deopt_site_init(&sl, 256) == 0);

    /* 200 failures, each 100ms apart = 20 seconds total. At any moment,
     * the 1-second window only contains ~10 failures — well below 100. */
    uint64_t now = 0;
    for (uint32_t i = 0; i < 200; i++) {
        bool poisoned = vtx_deopt_site_record_failure(&sl, now);
        VTX_ASSERT_TRUE(!poisoned);
        now += 100 * 1000000ULL;  /* 100ms */
    }
    VTX_ASSERT_TRUE(!vtx_deopt_site_is_poisoned(&sl));

    vtx_deopt_site_destroy(&sl);
}

VTX_TEST(deopt_rl_site_rate_calculation) {
    vtx_deopt_site_limiter_t sl;
    VTX_ASSERT_TRUE(vtx_deopt_site_init(&sl, 256) == 0);

    /* 50 failures in 0.5 sec → rate = 100/sec (just at threshold). */
    uint64_t now = 0;
    for (uint32_t i = 0; i < 50; i++) {
        vtx_deopt_site_record_failure(&sl, now);
        now += 10 * 1000000ULL;  /* 10ms apart → 50 failures in 500ms */
    }
    double rate = vtx_deopt_site_rate_per_sec(&sl, now);
    printf("[deopt_rl] rate after 50 fails in 500ms: %.1f/sec\n", rate);
    VTX_ASSERT_TRUE(rate >= 50.0);  /* at least 50/sec */

    vtx_deopt_site_destroy(&sl);
}

VTX_TEST(deopt_rl_site_reset_clears_poison) {
    vtx_deopt_site_limiter_t sl;
    VTX_ASSERT_TRUE(vtx_deopt_site_init(&sl, 256) == 0);

    /* Poison the site. */
    uint64_t now = 0;
    for (uint32_t i = 0; i < VTX_DEOPT_SITE_FAIL_THRESHOLD; i++) {
        vtx_deopt_site_record_failure(&sl, now);
    }
    VTX_ASSERT_TRUE(vtx_deopt_site_is_poisoned(&sl));

    /* Reset clears the poisoned flag. */
    vtx_deopt_site_reset(&sl);
    VTX_ASSERT_TRUE(!vtx_deopt_site_is_poisoned(&sl));

    /* But total_failures is preserved. */
    VTX_ASSERT_TRUE(sl.total_failures == VTX_DEOPT_SITE_FAIL_THRESHOLD);

    vtx_deopt_site_destroy(&sl);
}

VTX_TEST(deopt_rl_batcher_coalesces) {
    vtx_deopt_batcher_t b;
    VTX_ASSERT_TRUE(vtx_deopt_batcher_init(&b, 256) == 0);

    /* Add 5 pending deopts in rapid succession (well within the 1ms window). */
    uint64_t now = 0;
    for (uint32_t i = 0; i < 5; i++) {
        bool should_flush = vtx_deopt_batcher_add(&b, i, now);
        VTX_ASSERT_TRUE(!should_flush);  /* shouldn't flush yet */
    }
    VTX_ASSERT_TRUE(!vtx_deopt_batcher_should_flush(&b, now));

    /* Now advance time past the 1ms window — should_flush should return true. */
    now += VTX_DEOPT_BATCH_WINDOW_NS + 1;
    VTX_ASSERT_TRUE(vtx_deopt_batcher_should_flush(&b, now));

    /* Flush should return all 5 sites. */
    uint32_t out[16];
    uint32_t n = vtx_deopt_batcher_flush(&b, out, 16, now);
    VTX_ASSERT_TRUE(n == 5);
    for (uint32_t i = 0; i < 5; i++) {
        VTX_ASSERT_TRUE(out[i] == i);
    }

    /* After flush, count is 0. */
    VTX_ASSERT_TRUE(b.count == 0);
    VTX_ASSERT_TRUE(b.total_batches == 1);
    VTX_ASSERT_TRUE(b.total_deopts_coalesced == 5);

    vtx_deopt_batcher_destroy(&b);
}

VTX_TEST(deopt_rl_batcher_fills_capacity) {
    /* Add more than BATCH_MAX_PENDING entries in one window — should
     * signal flush when buffer fills. */
    vtx_deopt_batcher_t b;
    VTX_ASSERT_TRUE(vtx_deopt_batcher_init(&b, 256) == 0);

    uint64_t now = 0;
    bool should_flush = false;
    for (uint32_t i = 0; i < VTX_DEOPT_BATCH_MAX_PENDING; i++) {
        should_flush = vtx_deopt_batcher_add(&b, i, now);
    }
    VTX_ASSERT_TRUE(should_flush);

    uint32_t out[128];
    uint32_t n = vtx_deopt_batcher_flush(&b, out, 128, now);
    VTX_ASSERT_TRUE(n == VTX_DEOPT_BATCH_MAX_PENDING);

    vtx_deopt_batcher_destroy(&b);
}

VTX_TEST(deopt_rl_budget_suppresses) {
    /* Use capacity >= THRESHOLD so all 10000 records fit at the same
     * timestamp without overwriting each other. */
    vtx_deopt_budget_t gb;
    VTX_ASSERT_TRUE(vtx_deopt_budget_init(&gb, VTX_DEOPT_GLOBAL_FAIL_THRESHOLD + 100) == 0);

    /* Below threshold: no suppression. */
    uint64_t now = 1000000000ULL;  /* 1 sec in */
    for (uint32_t i = 0; i < VTX_DEOPT_GLOBAL_FAIL_THRESHOLD - 1; i++) {
        bool exceeded = vtx_deopt_budget_record(&gb, now);
        VTX_ASSERT_TRUE(!exceeded);
    }
    VTX_ASSERT_TRUE(!vtx_deopt_budget_is_suppressed(&gb, now));

    /* At threshold: suppressed. */
    bool exceeded = vtx_deopt_budget_record(&gb, now);
    VTX_ASSERT_TRUE(exceeded);
    VTX_ASSERT_TRUE(vtx_deopt_budget_is_suppressed(&gb, now));

    vtx_deopt_budget_destroy(&gb);
}

VTX_TEST(deopt_rl_budget_unsuppresses_when_rate_drops) {
    vtx_deopt_budget_t gb;
    VTX_ASSERT_TRUE(vtx_deopt_budget_init(&gb, VTX_DEOPT_GLOBAL_FAIL_THRESHOLD + 100) == 0);

    /* Trigger suppression by recording 10000 deopts. */
    uint64_t now = 1000000000ULL;
    for (uint32_t i = 0; i < VTX_DEOPT_GLOBAL_FAIL_THRESHOLD; i++) {
        vtx_deopt_budget_record(&gb, now);
    }
    VTX_ASSERT_TRUE(vtx_deopt_budget_is_suppressed(&gb, now));

    /* Wait 2 seconds — old entries should expire, rate drops to ~0. */
    now += 2 * NS_PER_SEC;
    /* Record one new deopt at the new time. The eviction should clear
     * the old entries; count drops to 1, well below threshold/2 = 5000. */
    vtx_deopt_budget_record(&gb, now);
    VTX_ASSERT_TRUE(!vtx_deopt_budget_is_suppressed(&gb, now));

    vtx_deopt_budget_destroy(&gb);
}

VTX_TEST(deopt_rl_budget_rate_calculation) {
    vtx_deopt_budget_t gb;
    VTX_ASSERT_TRUE(vtx_deopt_budget_init(&gb, 256 * 1024) == 0);

    /* 5000 deopts in 0.5 sec → rate = 10000/sec (at threshold). */
    uint64_t now = 1000000000ULL;
    for (uint32_t i = 0; i < 5000; i++) {
        vtx_deopt_budget_record(&gb, now);
        now += 100000ULL;  /* 0.1ms apart → 5000 in 500ms */
    }
    double rate = vtx_deopt_budget_rate_per_sec(&gb, now);
    printf("[deopt_rl] global rate after 5000 deopts in 500ms: %.0f/sec\n", rate);
    VTX_ASSERT_TRUE(rate >= 4999.0);  /* allow off-by-one */

    vtx_deopt_budget_destroy(&gb);
}

VTX_TEST(deopt_rl_integration_site_and_budget) {
    /* Integration: a per-site limiter that hits threshold also records
     * into the global budget. Verify both fire correctly. */
    vtx_deopt_site_limiter_t sl;
    vtx_deopt_budget_t gb;
    VTX_ASSERT_TRUE(vtx_deopt_site_init(&sl, 256) == 0);
    VTX_ASSERT_TRUE(vtx_deopt_budget_init(&gb, VTX_DEOPT_GLOBAL_FAIL_THRESHOLD + 100) == 0);

    /* 100 failures at one site within 1 sec. */
    uint64_t now = 1000000000ULL;
    for (uint32_t i = 0; i < VTX_DEOPT_SITE_FAIL_THRESHOLD; i++) {
        bool site_poisoned = vtx_deopt_site_record_failure(&sl, now);
        bool budget_exceeded = vtx_deopt_budget_record(&gb, now);
        (void)site_poisoned; (void)budget_exceeded;
    }
    VTX_ASSERT_TRUE(vtx_deopt_site_is_poisoned(&sl));
    /* Global budget should NOT be exceeded (100 < 10000). */
    VTX_ASSERT_TRUE(!vtx_deopt_budget_is_suppressed(&gb, now));

    vtx_deopt_site_destroy(&sl);
    vtx_deopt_budget_destroy(&gb);
}

int main(void) {
    printf("=== VORTEX Deopt Rate Limiter Tests ===\n\n");
    vtx_test_run_all();
    return 0;
}
