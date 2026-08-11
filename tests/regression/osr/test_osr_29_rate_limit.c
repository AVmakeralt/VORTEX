/*
 * VORTEX OSR-29 Regression Test
 *
 * Bug: The dispatch loop set jit_reenter_pending = true on every
 *      failed vtx_osr_up call, re-entering the JIT from method entry
 *      on the next invocation. If the JIT immediately deoptimized
 *      (e.g., a guard failure), the cycle repeated: OSR up → fail →
 *      re-enter → deopt → OSR up → fail → ... an infinite loop with
 *      no rate limiting.
 *
 * Fix: src/deopt/rate_limit.c provides three helper functions that
 *      encapsulate the per-method OSR re-attempt policy:
 *
 *        - vtx_osr_rate_should_attempt: returns true if OSR is
 *          allowed now (failure_count below threshold OR cooldown
 *          has expired).
 *
 *        - vtx_osr_rate_record_failure: bumps the failure counter
 *          and arms the cooldown once the threshold is hit.
 *
 *        - vtx_osr_rate_record_success: resets the failure counter
 *          (a successful OSR clears the cooldown — wired into
 *          vtx_dispatch_jit's return path by the M3 fix).
 *
 *      Constants:
 *        VTX_OSR_MAX_FAILURES         = 5
 *        VTX_OSR_COOLDOWN_INVOCATIONS = 1000
 *
 * Test: Unit-test the three helpers in isolation:
 *
 *   1. should_attempt returns true while failure_count < threshold.
 *   2. record_failure returns true exactly once (when threshold is
 *      first hit) and arms the cooldown.
 *   3. should_attempt returns false during cooldown.
 *   4. should_attempt returns true again once call_count exceeds
 *      the cooldown threshold.
 *   5. record_success resets failure_count to 0 and clears the
 *      cooldown, so should_attempt returns true immediately.
 *   6. Subsequent failures after a success start the counter
 *      fresh (no carryover from the pre-success history).
 *   7. Record_failure during cooldown extends the cooldown.
 */

#include "test_framework.h"
#include "deopt/rate_limit.h"
#include <stdint.h>
#include <stdio.h>

VTX_TEST(osr29_should_attempt_true_when_below_threshold)
{
    /* For any failure_count < VTX_OSR_MAX_FAILURES, OSR is allowed
     * regardless of cooldown state. */
    VTX_ASSERT_TRUE(vtx_osr_rate_should_attempt(
        /*osr_failure_count=*/0,
        /*osr_cooldown_until_call=*/0,
        /*current_call_count=*/0));

    VTX_ASSERT_TRUE(vtx_osr_rate_should_attempt(
        VTX_OSR_MAX_FAILURES - 1,
        0, 0));

    /* Even if a cooldown is armed (osr_cooldown_until_call > 0), as
     * long as we haven't hit the threshold, OSR proceeds. */
    VTX_ASSERT_TRUE(vtx_osr_rate_should_attempt(
        VTX_OSR_MAX_FAILURES - 1,
        /*osr_cooldown_until_call=*/10000,
        /*current_call_count=*/0));
}

VTX_TEST(osr29_record_failure_arms_cooldown_at_threshold)
{
    uint32_t failure_count = 0;
    uint64_t cooldown_until = 0;

    /* Record (VTX_OSR_MAX_FAILURES - 1) failures: no cooldown armed,
     * record_failure returns false (threshold not yet hit). */
    for (uint32_t i = 0; i < VTX_OSR_MAX_FAILURES - 1; i++) {
        bool threshold_hit = vtx_osr_rate_record_failure(
            &failure_count, &cooldown_until, /*current_call_count=*/100 + i);
        VTX_ASSERT_FALSE(threshold_hit);
        VTX_ASSERT_EQUAL(cooldown_until, 0ull);
    }
    VTX_ASSERT_EQUAL(failure_count, VTX_OSR_MAX_FAILURES - 1);

    /* The Nth failure (threshold) returns true and arms the cooldown. */
    bool threshold_hit = vtx_osr_rate_record_failure(
        &failure_count, &cooldown_until, /*current_call_count=*/100 + VTX_OSR_MAX_FAILURES - 1);
    VTX_ASSERT_TRUE(threshold_hit);
    VTX_ASSERT_EQUAL(failure_count, VTX_OSR_MAX_FAILURES);

    /* Cooldown is armed: osr_cooldown_until_call == current_call_count
     * + VTX_OSR_COOLDOWN_INVOCATIONS. */
    VTX_ASSERT_EQUAL(cooldown_until,
                      (uint64_t)(100 + VTX_OSR_MAX_FAILURES - 1) +
                          VTX_OSR_COOLDOWN_INVOCATIONS);
}

VTX_TEST(osr29_should_attempt_false_during_cooldown)
{
    /* Setup: failure_count == VTX_OSR_MAX_FAILURES, cooldown armed at
     * call 1000. current_call_count = 500 — well within cooldown. */
    uint32_t failure_count = VTX_OSR_MAX_FAILURES;
    uint64_t cooldown_until = 1000;
    uint64_t current_call_count = 500;

    /* During cooldown: should_attempt returns false. */
    VTX_ASSERT_FALSE(vtx_osr_rate_should_attempt(
        failure_count, cooldown_until, current_call_count));

    /* One invocation before cooldown expires: still false. */
    VTX_ASSERT_FALSE(vtx_osr_rate_should_attempt(
        failure_count, cooldown_until, /*current_call_count=*/999));

    /* Exactly at the cooldown threshold: true (>= check). */
    VTX_ASSERT_TRUE(vtx_osr_rate_should_attempt(
        failure_count, cooldown_until, /*current_call_count=*/1000));

    /* Past the cooldown: true. */
    VTX_ASSERT_TRUE(vtx_osr_rate_should_attempt(
        failure_count, cooldown_until, /*current_call_count=*/1001));
}

VTX_TEST(osr29_record_success_resets_counter_and_cooldown)
{
    /* Setup: failure_count == VTX_OSR_MAX_FAILURES, cooldown armed. */
    uint32_t failure_count = VTX_OSR_MAX_FAILURES;
    uint64_t cooldown_until = 5000;

    /* Sanity: before success, OSR is suppressed. */
    VTX_ASSERT_FALSE(vtx_osr_rate_should_attempt(
        failure_count, cooldown_until, /*current_call_count=*/100));

    /* Record a success. */
    vtx_osr_rate_record_success(&failure_count, &cooldown_until);

    /* Both fields are cleared. */
    VTX_ASSERT_EQUAL(failure_count, 0u);
    VTX_ASSERT_EQUAL(cooldown_until, 0ull);

    /* OSR is allowed immediately, even at call_count = 0. */
    VTX_ASSERT_TRUE(vtx_osr_rate_should_attempt(
        failure_count, cooldown_until, /*current_call_count=*/0));
}

VTX_TEST(osr29_failures_after_success_start_fresh)
{
    /* After a success clears the counter, subsequent failures must
     * start from 0 again — no carryover from pre-success history. */
    uint32_t failure_count = VTX_OSR_MAX_FAILURES;
    uint64_t cooldown_until = 5000;

    vtx_osr_rate_record_success(&failure_count, &cooldown_until);
    VTX_ASSERT_EQUAL(failure_count, 0u);

    /* One failure after success: counter goes to 1, no cooldown. */
    bool threshold_hit = vtx_osr_rate_record_failure(
        &failure_count, &cooldown_until, /*current_call_count=*/10);
    VTX_ASSERT_FALSE(threshold_hit);
    VTX_ASSERT_EQUAL(failure_count, 1u);
    VTX_ASSERT_EQUAL(cooldown_until, 0ull);

    /* OSR is still allowed (count < threshold). */
    VTX_ASSERT_TRUE(vtx_osr_rate_should_attempt(
        failure_count, cooldown_until, /*current_call_count=*/10));
}

VTX_TEST(osr29_record_failure_during_cooldown_extends_cooldown)
{
    /* If failures persist AFTER the cooldown is armed, each new
     * failure re-arms the cooldown (extending it). This prevents the
     * OSR-fail → wait → OSR-fail → wait → ... loop from continuing
     * indefinitely. */
    uint32_t failure_count = VTX_OSR_MAX_FAILURES;
    uint64_t cooldown_until = 1000;  /* armed at call 1000 */

    /* Simulate a failure during cooldown (call_count = 800). */
    bool threshold_hit = vtx_osr_rate_record_failure(
        &failure_count, &cooldown_until, /*current_call_count=*/800);

    /* threshold_hit returns false because we've already hit the
     * threshold previously (failure_count > VTX_OSR_MAX_FAILURES).
     * But the cooldown is extended to 800 + VTX_OSR_COOLDOWN_INVOCATIONS. */
    VTX_ASSERT_FALSE(threshold_hit);
    VTX_ASSERT_EQUAL(failure_count, VTX_OSR_MAX_FAILURES + 1);
    VTX_ASSERT_EQUAL(cooldown_until,
                      (uint64_t)800 + VTX_OSR_COOLDOWN_INVOCATIONS);

    /* Cooldown is now further out — OSR still suppressed at call 1000. */
    VTX_ASSERT_FALSE(vtx_osr_rate_should_attempt(
        failure_count, cooldown_until, /*current_call_count=*/1000));
}

VTX_TEST(osr29_null_inputs_are_safe)
{
    /* Defensive: NULL pointers must not crash. record_failure returns
     * false (no threshold hit) and record_success is a no-op. */
    VTX_ASSERT_FALSE(vtx_osr_rate_record_failure(NULL, NULL, 0));
    vtx_osr_rate_record_success(NULL, NULL);
    VTX_ASSERT_TRUE(vtx_osr_rate_should_attempt(0, 0, 0));
    VTX_ASSERT_TRUE(1);  /* reached here without crashing */
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nOSR-29 regression: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    return (result.fail_count > 0) ? 1 : 0;
}
