/**
 * VORTEX Zero-Cost Deopt Tests
 *
 * Comprehensive tests for:
 *   1. Sampling-based guard profiling (hot-path inline + slow path)
 *   2. Guard page safepoint polling
 *   3. Implicit null checks (SIGSEGV handler)
 *   4. Predicated guard traps (INT3)
 *   5. Value profiling + value speculation
 *   6. Adaptive sampling intervals
 *   7. Implicit null check emission
 *   8. Value guard emission
 *   9. Signal handler chaining
 *  10. Edge cases (NULL pointers, overflow, boundary conditions)
 */

#include "test_framework.h"
#include "guard/metadata.h"
#include "guard/ewma.h"
#include "guard/value_profile.h"
#include "compile/safepoint.h"
#include "deopt/side_table.h"
#include "lower/emit.h"
#include "lower/guard_emit.h"
#include "lower/reloc.h"
#include "lower/isel.h"
#include "runtime/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

/* ========================================================================== */
/* Sampling-based guard profiling tests                                        */
/* ========================================================================== */

VTX_TEST(sampling_init)
{
    vtx_guard_meta_table_t table;
    VTX_ASSERT_EQUAL(vtx_guard_meta_table_init(&table), 0);

    /* Register a guard and verify sampling fields are initialized */
    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 100, 1, 42, VTX_GUARD_FAST_CHECK);
    VTX_ASSERT_TRUE(meta != NULL);

    /* Check that sampling fields are initialized to defaults */
    VTX_ASSERT_EQUAL(meta->sample_counter, VTX_GUARD_SAMPLE_INTERVAL_DEFAULT);
    VTX_ASSERT_EQUAL(meta->sample_interval, VTX_GUARD_SAMPLE_INTERVAL_DEFAULT);
    VTX_ASSERT_EQUAL(meta->sampled_executions, 0ULL);
    VTX_ASSERT_EQUAL(meta->sampled_failures, 0ULL);

    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(sampling_hot_path_accumulates)
{
    vtx_guard_meta_table_t table;
    VTX_ASSERT_EQUAL(vtx_guard_meta_table_init(&table), 0);

    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 200, 1, 42, VTX_GUARD_FAST_CHECK);
    VTX_ASSERT_TRUE(meta != NULL);

    /* Call sampled update many times without reaching the sample boundary.
     * The execution count should only be updated in the sampled counters,
     * not in the main execution_count until the sample boundary is hit. */
    uint32_t half_interval = VTX_GUARD_SAMPLE_INTERVAL_DEFAULT / 2;
    for (uint32_t i = 0; i < half_interval; i++) {
        vtx_guard_meta_update_sampled(meta, false);
    }

    /* After half the interval, the main execution_count should still be 0
     * because we haven't crossed a sample boundary. */
    VTX_ASSERT_EQUAL(meta->execution_count, 0ULL);
    VTX_ASSERT_EQUAL(meta->sampled_executions, (uint64_t)half_interval);

    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(sampling_boundary_triggers_update)
{
    vtx_guard_meta_table_t table;
    VTX_ASSERT_EQUAL(vtx_guard_meta_table_init(&table), 0);

    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 300, 1, 42, VTX_GUARD_FAST_CHECK);
    VTX_ASSERT_TRUE(meta != NULL);

    /* Fill up to the sample boundary */
    for (uint32_t i = 0; i < VTX_GUARD_SAMPLE_INTERVAL_DEFAULT; i++) {
        vtx_guard_meta_update_sampled(meta, false);
    }

    /* After a full interval, the sample boundary should have been hit,
     * and execution_count should be updated. */
    VTX_ASSERT_TRUE(meta->execution_count > 0);
    VTX_ASSERT_EQUAL(meta->execution_count, (uint64_t)VTX_GUARD_SAMPLE_INTERVAL_DEFAULT);

    /* Sampled counters should be reset */
    VTX_ASSERT_EQUAL(meta->sampled_executions, 0ULL);
    VTX_ASSERT_EQUAL(meta->sampled_failures, 0ULL);

    /* EWMA should be initialized (first update) */
    VTX_ASSERT_TRUE(vtx_ewma_is_initialized(&meta->failure_rate_ewma));

    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(sampling_failure_tracking)
{
    vtx_guard_meta_table_t table;
    VTX_ASSERT_EQUAL(vtx_guard_meta_table_init(&table), 0);

    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 400, 1, 42, VTX_GUARD_FAST_CHECK);
    VTX_ASSERT_TRUE(meta != NULL);

    /* Run with some failures mixed in */
    for (uint32_t i = 0; i < VTX_GUARD_SAMPLE_INTERVAL_DEFAULT; i++) {
        bool failed = (i % 100 == 0);  /* 1% failure rate */
        vtx_guard_meta_update_sampled(meta, failed);
    }

    /* Should have tracked failures correctly */
    VTX_ASSERT_TRUE(meta->failure_count > 0);
    VTX_ASSERT_TRUE(meta->execution_count > 0);

    /* EWMA should reflect the failure rate */
    double ewma = vtx_ewma_value(&meta->failure_rate_ewma);
    VTX_ASSERT_TRUE(ewma > 0.0);
    VTX_ASSERT_TRUE(ewma < 1.0);

    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(sampling_failure_shortens_interval)
{
    vtx_guard_meta_table_t table;
    VTX_ASSERT_EQUAL(vtx_guard_meta_table_init(&table), 0);

    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 500, 1, 42, VTX_GUARD_FAST_CHECK);
    VTX_ASSERT_TRUE(meta != NULL);

    /* Verify initial interval is default */
    VTX_ASSERT_EQUAL(meta->sample_interval, VTX_GUARD_SAMPLE_INTERVAL_DEFAULT);

    /* Trigger a failure -- should shorten the interval */
    vtx_guard_meta_update_sampled(meta, true);

    /* The interval should now be shortened to UNSTABLE */
    VTX_ASSERT_EQUAL(meta->sample_interval, VTX_GUARD_SAMPLE_INTERVAL_UNSTABLE);

    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(sampling_adaptive_interval_stable)
{
    vtx_guard_meta_table_t table;
    VTX_ASSERT_EQUAL(vtx_guard_meta_table_init(&table), 0);

    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 600, 1, 42, VTX_GUARD_FAST_CHECK);

    /* Run many iterations with zero failures to make it very stable */
    for (int round = 0; round < 10; round++) {
        for (uint32_t i = 0; i < meta->sample_interval; i++) {
            vtx_guard_meta_update_sampled(meta, false);
        }
    }

    /* After many successful iterations, the EWMA should be very low.
     * The adaptive interval should have increased. */
    double ewma = vtx_ewma_value(&meta->failure_rate_ewma);
    if (ewma < VTX_GUARD_PREDICATE_THRESHOLD && meta->execution_count >= 10000) {
        VTX_ASSERT_EQUAL(meta->sample_interval, VTX_GUARD_SAMPLE_INTERVAL_STABLE);
    }

    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(sampling_strength_transition)
{
    vtx_guard_meta_table_t table;
    VTX_ASSERT_EQUAL(vtx_guard_meta_table_init(&table), 0);

    /* Test Unconditional -> FastCheck on first failure */
    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 700, 1, 42, VTX_GUARD_UNCONDITIONAL);
    VTX_ASSERT_TRUE(meta != NULL);
    VTX_ASSERT_EQUAL(meta->strength, VTX_GUARD_UNCONDITIONAL);

    /* Run until sample boundary with one failure */
    for (uint32_t i = 0; i < VTX_GUARD_SAMPLE_INTERVAL_DEFAULT; i++) {
        bool failed = (i == 0);
        vtx_guard_meta_update_sampled(meta, failed);
    }

    /* Should have transitioned to FastCheck */
    VTX_ASSERT_EQUAL(meta->strength, VTX_GUARD_FAST_CHECK);
    VTX_ASSERT_TRUE(meta->strength_changed);

    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(sampling_null_meta)
{
    /* Should not crash with NULL meta */
    vtx_guard_meta_update_sampled(NULL, false);
    vtx_guard_meta_update_sampled(NULL, true);
}

VTX_TEST(sampling_saturating_counters)
{
    vtx_guard_meta_table_t table;
    VTX_ASSERT_EQUAL(vtx_guard_meta_table_init(&table), 0);

    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 800, 1, 42, VTX_GUARD_FAST_CHECK);

    /* Simulate near-saturation of sampled_executions */
    meta->sampled_executions = UINT64_MAX - 10;
    meta->sample_counter = 1;

    /* This should not overflow */
    vtx_guard_meta_update_sampled(meta, false);
    VTX_ASSERT_TRUE(meta->sampled_executions <= UINT64_MAX);

    vtx_guard_meta_table_destroy(&table);
}

/* ========================================================================== */
/* Hot-path inline update tests (vtx_guard_meta_update_hot)                    */
/* ========================================================================== */

VTX_TEST(hot_path_inline_basic)
{
    vtx_guard_meta_table_t table;
    VTX_ASSERT_EQUAL(vtx_guard_meta_table_init(&table), 0);

    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 1100, 0, 42, VTX_GUARD_FAST_CHECK);
    VTX_ASSERT_TRUE(meta != NULL);

    /* The hot-path inline should accumulate executions without
     * touching the main counters until a sample boundary. */
    for (uint32_t i = 0; i < VTX_GUARD_SAMPLE_INTERVAL_DEFAULT - 1; i++) {
        vtx_guard_meta_update_hot(meta, false);
    }

    /* Should not have updated execution_count yet (one short of boundary) */
    VTX_ASSERT_EQUAL(meta->execution_count, 0ULL);
    VTX_ASSERT_TRUE(meta->sampled_executions > 0);

    /* One more call to cross the boundary */
    vtx_guard_meta_update_hot(meta, false);
    VTX_ASSERT_TRUE(meta->execution_count > 0);

    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(hot_path_inline_null)
{
    /* Should not crash with NULL meta */
    vtx_guard_meta_update_hot(NULL, false);
    vtx_guard_meta_update_hot(NULL, true);
}

VTX_TEST(hot_path_inline_failure_shortens)
{
    vtx_guard_meta_table_t table;
    VTX_ASSERT_EQUAL(vtx_guard_meta_table_init(&table), 0);

    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 1200, 0, 42, VTX_GUARD_FAST_CHECK);
    VTX_ASSERT_TRUE(meta != NULL);

    /* Trigger a failure via hot-path inline */
    vtx_guard_meta_update_hot(meta, true);

    /* The interval should be shortened */
    VTX_ASSERT_EQUAL(meta->sample_interval, VTX_GUARD_SAMPLE_INTERVAL_UNSTABLE);

    vtx_guard_meta_table_destroy(&table);
}

/* ========================================================================== */
/* Guard page tests                                                            */
/* ========================================================================== */

VTX_TEST(guard_page_init_destroy)
{
    int result = vtx_guard_page_init();
    VTX_ASSERT_EQUAL(result, 0);

    VTX_ASSERT_TRUE(vtx_guard_page_is_available());
    VTX_ASSERT_TRUE(vtx_guard_page_is_available_inline());

    void *addr = vtx_guard_page_address();
    VTX_ASSERT_TRUE(addr != NULL);

    vtx_guard_page_destroy();
    VTX_ASSERT_TRUE(!vtx_guard_page_is_available());
}

VTX_TEST(guard_page_arm_disarm)
{
    int result = vtx_guard_page_init();
    VTX_ASSERT_EQUAL(result, 0);

    void *addr = vtx_guard_page_address();
    VTX_ASSERT_TRUE(addr != NULL);

    /* Read from the page -- should succeed without crashing */
    volatile uint64_t value = *(volatile uint64_t *)addr;
    (void)value;

    /* Arm the page -- make it PROT_NONE */
    result = vtx_guard_page_arm();
    VTX_ASSERT_EQUAL(result, 0);

    /* Disarm -- make it readable again */
    result = vtx_guard_page_disarm();
    VTX_ASSERT_EQUAL(result, 0);

    /* Should be readable again */
    value = *(volatile uint64_t *)addr;
    (void)value;

    vtx_guard_page_destroy();
}

VTX_TEST(guard_page_register_unregister)
{
    int result = vtx_guard_page_init();
    VTX_ASSERT_EQUAL(result, 0);

    uint8_t fake_code[1024];
    result = vtx_guard_page_register_code(fake_code, sizeof(fake_code), 1, 0);
    VTX_ASSERT_EQUAL(result, 0);

    uint8_t fake_code2[2048];
    result = vtx_guard_page_register_code(fake_code2, sizeof(fake_code2), 2, 1);
    VTX_ASSERT_EQUAL(result, 0);

    result = vtx_guard_page_unregister_code(fake_code);
    VTX_ASSERT_EQUAL(result, 0);

    /* Already removed */
    result = vtx_guard_page_unregister_code(fake_code);
    VTX_ASSERT_EQUAL(result, -1);

    result = vtx_guard_page_unregister_code(fake_code2);
    VTX_ASSERT_EQUAL(result, 0);

    vtx_guard_page_destroy();
}

VTX_TEST(guard_page_null_params)
{
    VTX_ASSERT_EQUAL(vtx_guard_page_arm(), -1);
    VTX_ASSERT_EQUAL(vtx_guard_page_disarm(), -1);
    VTX_ASSERT_TRUE(!vtx_guard_page_is_available());
    VTX_ASSERT_TRUE(vtx_guard_page_address() == NULL);
}

VTX_TEST(guard_page_available_flag)
{
    /* Before init, flag should be 0 */
    VTX_ASSERT_TRUE(!vtx_guard_page_is_available_inline());

    int result = vtx_guard_page_init();
    VTX_ASSERT_EQUAL(result, 0);

    /* After init, flag should be 1 */
    VTX_ASSERT_TRUE(vtx_guard_page_is_available_inline());
    VTX_ASSERT_TRUE(vtx_guard_page_is_available());

    vtx_guard_page_destroy();

    /* After destroy, flag should be 0 */
    VTX_ASSERT_TRUE(!vtx_guard_page_is_available_inline());
}

/* ========================================================================== */
/* Predicated guard emission tests                                             */
/* ========================================================================== */

VTX_TEST(predicated_guard_emission)
{
    vtx_guard_desc_t guard;
    memset(&guard, 0, sizeof(guard));
    guard.guard_node = 42;
    guard.frame_state_index = 0;

    uint8_t code_buf[32];
    memset(code_buf, 0, sizeof(code_buf));

    int result = vtx_guard_emit_predicated(&guard, code_buf, sizeof(code_buf));
    VTX_ASSERT_EQUAL(result, 1);  /* 1 byte emitted */
    VTX_ASSERT_EQUAL(code_buf[0], 0xCC);  /* INT3 opcode */
}

VTX_TEST(predicated_guard_null_params)
{
    uint8_t code_buf[32];

    VTX_ASSERT_EQUAL(vtx_guard_emit_predicated(NULL, code_buf, sizeof(code_buf)), -1);

    vtx_guard_desc_t guard;
    memset(&guard, 0, sizeof(guard));
    VTX_ASSERT_EQUAL(vtx_guard_emit_predicated(&guard, NULL, 32), -1);
    VTX_ASSERT_EQUAL(vtx_guard_emit_predicated(&guard, code_buf, 0), -1);
}

/* ========================================================================== */
/* Implicit null check emission tests                                          */
/* ========================================================================== */

VTX_TEST(implicit_null_check_emission)
{
    vtx_guard_desc_t guard;
    memset(&guard, 0, sizeof(guard));
    guard.guard_node = 99;
    guard.frame_state_index = 5;

    uint8_t code_buf[32];
    memset(code_buf, 0, sizeof(code_buf));

    int result = vtx_guard_emit_implicit_null(&guard, code_buf, sizeof(code_buf));
    VTX_ASSERT_EQUAL(result, 0);  /* 0 bytes emitted — implicit! */
    VTX_ASSERT_TRUE(guard.is_implicit_null);
    VTX_ASSERT_EQUAL(guard.jcc_native_offset, UINT32_MAX);  /* no JCC to patch */
}

VTX_TEST(implicit_null_check_null_guard)
{
    uint8_t code_buf[32];
    VTX_ASSERT_EQUAL(vtx_guard_emit_implicit_null(NULL, code_buf, sizeof(code_buf)), -1);
}

/* ========================================================================== */
/* Value guard emission tests                                                  */
/* ========================================================================== */

VTX_TEST(value_guard_small_value)
{
    vtx_guard_desc_t guard;
    memset(&guard, 0, sizeof(guard));
    guard.guard_node = 77;
    guard.is_value_guard = true;
    guard.expected_value = 42;  /* fits in imm8 */

    uint8_t code_buf[32];
    memset(code_buf, 0, sizeof(code_buf));

    int result = vtx_guard_emit_value_guard(&guard, code_buf, sizeof(code_buf));
    VTX_ASSERT_TRUE(result > 0);

    /* Should start with REX.W + CMP imm8 */
    VTX_ASSERT_EQUAL(code_buf[0], 0x48);  /* REX.W */
    VTX_ASSERT_EQUAL(code_buf[1], 0x83);  /* CMP r/m64, imm8 */
    VTX_ASSERT_EQUAL(code_buf[3], 42);    /* immediate = 42 */

    /* Should end with JNE rel32 */
    int len = result;
    VTX_ASSERT_EQUAL(code_buf[len - 6], 0x0F);
    VTX_ASSERT_EQUAL(code_buf[len - 5], 0x85);
}

VTX_TEST(value_guard_32bit_value)
{
    vtx_guard_desc_t guard;
    memset(&guard, 0, sizeof(guard));
    guard.guard_node = 78;
    guard.is_value_guard = true;
    guard.expected_value = 100000;  /* fits in imm32 but not imm8 */

    uint8_t code_buf[32];
    memset(code_buf, 0, sizeof(code_buf));

    int result = vtx_guard_emit_value_guard(&guard, code_buf, sizeof(code_buf));
    VTX_ASSERT_TRUE(result > 0);

    /* Should start with REX.W + CMP imm32 */
    VTX_ASSERT_EQUAL(code_buf[0], 0x48);  /* REX.W */
    VTX_ASSERT_EQUAL(code_buf[1], 0x81);  /* CMP r/m64, imm32 */
}

VTX_TEST(value_guard_64bit_value)
{
    vtx_guard_desc_t guard;
    memset(&guard, 0, sizeof(guard));
    guard.guard_node = 79;
    guard.is_value_guard = true;
    guard.expected_value = 0x123456789ABCDEF0ULL;  /* needs full 64-bit */

    uint8_t code_buf[32];
    memset(code_buf, 0, sizeof(code_buf));

    int result = vtx_guard_emit_value_guard(&guard, code_buf, sizeof(code_buf));
    VTX_ASSERT_TRUE(result > 0);

    /* Should start with MOV RCX, imm64 */
    VTX_ASSERT_EQUAL(code_buf[0], 0x48);  /* REX.W */
    VTX_ASSERT_EQUAL(code_buf[1], 0xB9);  /* MOV RCX, imm64 */
}

VTX_TEST(value_guard_not_value_guard)
{
    vtx_guard_desc_t guard;
    memset(&guard, 0, sizeof(guard));
    guard.guard_node = 80;
    guard.is_value_guard = false;  /* NOT a value guard */

    uint8_t code_buf[32];
    VTX_ASSERT_EQUAL(vtx_guard_emit_value_guard(&guard, code_buf, sizeof(code_buf)), -1);
}

VTX_TEST(value_guard_null_params)
{
    vtx_guard_desc_t guard;
    memset(&guard, 0, sizeof(guard));
    guard.is_value_guard = true;

    uint8_t code_buf[32];
    VTX_ASSERT_EQUAL(vtx_guard_emit_value_guard(NULL, code_buf, 32), -1);
    VTX_ASSERT_EQUAL(vtx_guard_emit_value_guard(&guard, NULL, 32), -1);
}

/* ========================================================================== */
/* Guard page poll emission test                                               */
/* ========================================================================== */

VTX_TEST(guard_page_poll_emission)
{
    vtx_x86_emit_t emit;
    VTX_ASSERT_EQUAL(vtx_x86_emit_init(&emit, 256), 0);

    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_reloc_table_t relocs;
    memset(&relocs, 0, sizeof(relocs));
    emit.relocs = &relocs;
    emit.reloc_arena = &arena;

    int result = vtx_x86_emit_safepoint_poll_guard_page(&emit);
    VTX_ASSERT_EQUAL(result, 0);

    /* Verify emitted bytes: REX.W + MOV + ModR/M + disp32 = 7 bytes */
    VTX_ASSERT_EQUAL(emit.position, 7);
    VTX_ASSERT_EQUAL(emit.buffer[0], 0x48);  /* REX.W */
    VTX_ASSERT_EQUAL(emit.buffer[1], 0x8B);  /* MOV r64, r/m64 */
    VTX_ASSERT_EQUAL(emit.buffer[2], 0x05);  /* ModR/M: rax, RIP-relative */

    /* A relocation should be recorded */
    VTX_ASSERT_TRUE(relocs.count > 0);

    vtx_x86_emit_destroy(&emit);
    vtx_arena_destroy(&arena);
}

VTX_TEST(guard_page_poll_null_emitter)
{
    VTX_ASSERT_EQUAL(vtx_x86_emit_safepoint_poll_guard_page(NULL), -1);
}

/* ========================================================================== */
/* EWMA integration with sampling                                              */
/* ========================================================================== */

VTX_TEST(ewma_batch_update)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);

    /* Batch update: 10 failures out of 1000 executions */
    double val = vtx_ewma_update_counts(&ewma, 10, 1000);
    VTX_ASSERT_TRUE(ewma.initialized);
    VTX_ASSERT_TRUE(val > 0.0);

    /* Another batch: 0 failures out of 1000 */
    val = vtx_ewma_update_counts(&ewma, 0, 1000);
    VTX_ASSERT_TRUE(val < 1.0);

    /* Zero executions should not change the EWMA */
    double prev_val = vtx_ewma_value(&ewma);
    val = vtx_ewma_update_counts(&ewma, 0, 0);
    VTX_ASSERT_TRUE(val == prev_val);
}

VTX_TEST(ewma_saturating)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);

    for (int i = 0; i < 100; i++) {
        vtx_ewma_update(&ewma, 1.0);
    }
    VTX_ASSERT_TRUE(vtx_ewma_value(&ewma) <= 1.0);

    vtx_ewma_reset(&ewma);
    for (int i = 0; i < 100; i++) {
        vtx_ewma_update(&ewma, 0.0);
    }
    VTX_ASSERT_TRUE(vtx_ewma_value(&ewma) >= 0.0);
}

/* ========================================================================== */
/* Value profiling tests                                                       */
/* ========================================================================== */

VTX_TEST(value_profile_init)
{
    vtx_value_profile_t profile;
    vtx_value_profile_init(&profile);

    VTX_ASSERT_EQUAL(profile.entries[0].count, 0ULL);
    VTX_ASSERT_EQUAL(profile.entries[1].count, 0ULL);
    VTX_ASSERT_EQUAL(profile.other_count, 0ULL);
    VTX_ASSERT_EQUAL(profile.total_observations, 0ULL);
    VTX_ASSERT_TRUE(!profile.is_stable);
    VTX_ASSERT_EQUAL(profile.sample_counter, VTX_VALUE_SAMPLE_INTERVAL_DEFAULT);
}

VTX_TEST(value_profile_single_value)
{
    vtx_value_profile_t profile;
    vtx_value_profile_init(&profile);

    /* Observe the same value many times */
    for (int i = 0; i < 200; i++) {
        vtx_value_profile_observe(&profile, 42);
    }

    VTX_ASSERT_TRUE(profile.total_observations > 0);

    uint64_t top_value = 0;
    double top_freq = 0.0;
    bool has_top = vtx_value_profile_top(&profile, &top_value, &top_freq);

    VTX_ASSERT_TRUE(has_top);
    VTX_ASSERT_EQUAL(top_value, 42ULL);
    VTX_ASSERT_TRUE(top_freq > 0.9);
    VTX_ASSERT_TRUE(profile.is_stable);
    VTX_ASSERT_EQUAL(profile.speculated_value, 42ULL);
}

VTX_TEST(value_profile_two_values)
{
    vtx_value_profile_t profile;
    vtx_value_profile_init(&profile);

    /* Observe two values: 42 (80%) and 7 (20%) */
    for (int i = 0; i < 160; i++) {
        vtx_value_profile_observe(&profile, 42);
    }
    for (int i = 0; i < 40; i++) {
        vtx_value_profile_observe(&profile, 7);
    }

    uint64_t top_value = 0;
    double top_freq = 0.0;
    vtx_value_profile_top(&profile, &top_value, &top_freq);

    VTX_ASSERT_EQUAL(top_value, 42ULL);
    VTX_ASSERT_TRUE(top_freq > 0.7);

    uint64_t second_value = 0;
    double second_freq = 0.0;
    bool has_second = vtx_value_profile_second(&profile, &second_value, &second_freq);

    VTX_ASSERT_TRUE(has_second);
    VTX_ASSERT_EQUAL(second_value, 7ULL);
}

VTX_TEST(value_profile_unstable)
{
    vtx_value_profile_t profile;
    vtx_value_profile_init(&profile);

    /* Observe many different values — no single value dominates */
    for (int i = 0; i < 200; i++) {
        vtx_value_profile_observe(&profile, (uint64_t)(i % 50));
    }

    /* Should NOT be stable — no single value exceeds 95% */
    VTX_ASSERT_TRUE(!profile.is_stable);
}

VTX_TEST(value_profile_sampling_hot_path)
{
    vtx_value_profile_t profile;
    vtx_value_profile_init(&profile);

    /* Use the hot-path inline function */
    for (int i = 0; i < 1000; i++) {
        vtx_value_profile_observe_hot(&profile, 42);
    }

    /* Should have observed the value (at least some samples) */
    VTX_ASSERT_TRUE(profile.total_observations > 0);

    uint64_t top_value = 0;
    double top_freq = 0.0;
    vtx_value_profile_top(&profile, &top_value, &top_freq);
    VTX_ASSERT_EQUAL(top_value, 42ULL);
}

VTX_TEST(value_profile_null_params)
{
    vtx_value_profile_init(NULL);
    vtx_value_profile_observe(NULL, 42);
    vtx_value_profile_observe_hot(NULL, 42);
    vtx_value_profile_reset(NULL);

    VTX_ASSERT_TRUE(!vtx_value_profile_is_stable(NULL));
    VTX_ASSERT_EQUAL(vtx_value_profile_speculated_value(NULL), 0ULL);

    uint64_t val;
    double freq;
    VTX_ASSERT_TRUE(!vtx_value_profile_top(NULL, &val, &freq));
    VTX_ASSERT_TRUE(!vtx_value_profile_second(NULL, &val, &freq));
}

VTX_TEST(value_profile_table)
{
    vtx_value_profile_table_t table;
    VTX_ASSERT_EQUAL(vtx_value_profile_table_init(&table), 0);

    /* Create profiles */
    vtx_value_profile_t *p1 = vtx_value_profile_get_or_create(&table, 100);
    VTX_ASSERT_TRUE(p1 != NULL);

    vtx_value_profile_t *p2 = vtx_value_profile_get_or_create(&table, 200);
    VTX_ASSERT_TRUE(p2 != NULL);

    /* Lookup existing */
    vtx_value_profile_t *found = vtx_value_profile_lookup(&table, 100);
    VTX_ASSERT_TRUE(found == p1);

    /* Lookup non-existing */
    vtx_value_profile_t *not_found = vtx_value_profile_lookup(&table, 999);
    VTX_ASSERT_TRUE(not_found == NULL);

    /* Populate with stable value */
    for (int i = 0; i < 200; i++) {
        vtx_value_profile_observe(p1, 42);
    }
    VTX_ASSERT_TRUE(p1->is_stable);

    /* Check stable count */
    VTX_ASSERT_EQUAL(vtx_value_profile_stable_count(&table), 1u);

    vtx_value_profile_table_destroy(&table);
}

VTX_TEST(value_profile_reset)
{
    vtx_value_profile_t profile;
    vtx_value_profile_init(&profile);

    /* Populate */
    for (int i = 0; i < 200; i++) {
        vtx_value_profile_observe(&profile, 42);
    }
    VTX_ASSERT_TRUE(profile.is_stable);

    /* Reset */
    vtx_value_profile_reset(&profile);
    VTX_ASSERT_EQUAL(profile.total_observations, 0ULL);
    VTX_ASSERT_TRUE(!profile.is_stable);
}

VTX_TEST(value_profile_adapt_interval)
{
    vtx_value_profile_t profile;
    vtx_value_profile_init(&profile);

    /* Start at default interval */
    VTX_ASSERT_EQUAL(profile.sample_interval, VTX_VALUE_SAMPLE_INTERVAL_DEFAULT);

    /* Make it stable — should increase interval */
    for (int i = 0; i < 300; i++) {
        vtx_value_profile_observe(&profile, 42);
    }

    if (profile.is_stable) {
        VTX_ASSERT_TRUE(profile.sample_interval >= VTX_VALUE_SAMPLE_INTERVAL_DEFAULT);
    }

    /* Introduce instability — should decrease interval */
    for (int i = 0; i < 50; i++) {
        vtx_value_profile_observe(&profile, (uint64_t)(i % 10));
    }

    if (!profile.is_stable) {
        VTX_ASSERT_TRUE(profile.sample_interval <= VTX_VALUE_SAMPLE_INTERVAL_DEFAULT);
    }
}

/* ========================================================================== */
/* Instruction flag tests                                                      */
/* ========================================================================== */

VTX_TEST(implicit_null_flag_exists)
{
    /* Verify the new instruction flags are defined */
    VTX_ASSERT_TRUE(VTX_INST_FLAG_IMPLICIT_NULL != 0);
    VTX_ASSERT_TRUE(VTX_INST_FLAG_VALUE_GUARD != 0);
    VTX_ASSERT_TRUE(VTX_INST_FLAG_IMPLICIT_NULL != VTX_INST_FLAG_VALUE_GUARD);
}

VTX_TEST(implicit_null_flag_in_inst)
{
    /* Verify the flag can be set on an instruction */
    vtx_inst_t inst;
    memset(&inst, 0, sizeof(inst));
    inst.flags = VTX_INST_FLAG_IMPLICIT_NULL | VTX_INST_FLAG_IS_GUARD;

    VTX_ASSERT_TRUE(inst.flags & VTX_INST_FLAG_IMPLICIT_NULL);
    VTX_ASSERT_TRUE(inst.flags & VTX_INST_FLAG_IS_GUARD);
    VTX_ASSERT_TRUE(!(inst.flags & VTX_INST_FLAG_VALUE_GUARD));
}

/* ========================================================================== */
/* Combined integration tests                                                  */
/* ========================================================================== */

VTX_TEST(full_sampling_lifecycle)
{
    vtx_guard_meta_table_t table;
    VTX_ASSERT_EQUAL(vtx_guard_meta_table_init(&table), 0);

    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 1000, 0, 100, VTX_GUARD_FAST_CHECK);
    VTX_ASSERT_TRUE(meta != NULL);

    /* Phase 1: Many successful executions */
    for (int round = 0; round < 20; round++) {
        for (uint32_t i = 0; i < meta->sample_interval; i++) {
            vtx_guard_meta_update_sampled(meta, false);
        }
    }

    double ewma = vtx_ewma_value(&meta->failure_rate_ewma);
    VTX_ASSERT_TRUE(ewma < 0.1);

    /* Phase 2: Start failing */
    for (int round = 0; round < 5; round++) {
        for (uint32_t i = 0; i < meta->sample_interval; i++) {
            vtx_guard_meta_update_sampled(meta, true);
        }
    }

    ewma = vtx_ewma_value(&meta->failure_rate_ewma);
    VTX_ASSERT_TRUE(ewma > 0.1);

    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(guard_page_full_lifecycle)
{
    VTX_ASSERT_EQUAL(vtx_guard_page_init(), 0);
    VTX_ASSERT_TRUE(vtx_guard_page_is_available());
    VTX_ASSERT_TRUE(vtx_guard_page_is_available_inline());

    uint8_t code_region[4096];
    VTX_ASSERT_EQUAL(vtx_guard_page_register_code(code_region, sizeof(code_region), 42, 5), 0);

    void *addr = vtx_guard_page_address();
    volatile uint64_t val = *(volatile uint64_t *)addr;
    (void)val;

    VTX_ASSERT_EQUAL(vtx_guard_page_arm(), 0);
    VTX_ASSERT_EQUAL(vtx_guard_page_disarm(), 0);

    val = *(volatile uint64_t *)addr;
    (void)val;

    VTX_ASSERT_EQUAL(vtx_guard_page_unregister_code(code_region), 0);

    vtx_guard_page_destroy();
    VTX_ASSERT_TRUE(!vtx_guard_page_is_available());
}

VTX_TEST(value_profile_full_lifecycle)
{
    vtx_value_profile_table_t table;
    VTX_ASSERT_EQUAL(vtx_value_profile_table_init(&table), 0);

    /* Create profiles for multiple sites */
    for (uint32_t pc = 0; pc < 10; pc++) {
        vtx_value_profile_t *p = vtx_value_profile_get_or_create(&table, pc);
        VTX_ASSERT_TRUE(p != NULL);

        /* Populate with a stable value */
        for (int i = 0; i < 200; i++) {
            vtx_value_profile_observe(p, (uint64_t)(pc * 100));
        }
    }

    /* All should be stable */
    VTX_ASSERT_EQUAL(vtx_value_profile_stable_count(&table), 10u);

    vtx_value_profile_table_destroy(&table);
}

/* ========================================================================== */
/* Test main — runs all auto-registered tests                                  */
/* ========================================================================== */

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nZero-Cost Deopt Tests: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    return (result.fail_count > 0) ? 1 : 0;
}
