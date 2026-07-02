/**
 * test_smi_arith.c — Comprehensive SMI Arithmetic Correctness Tests
 *
 * Tests the correctness of NaN-boxed SMI arithmetic at three levels:
 *   1. Pure C reference: verify SMI encoding/decoding and arithmetic identities
 *   2. Interpreter execution: bytecode-level correctness
 *   3. T2 JIT execution: compare JIT output with interpreter reference
 *
 * The NaN-boxed SMI representation (SMI(a) = HEADER | (a << 3)) makes
 * arithmetic subtle because the header participates in operations.
 * These tests cover edge cases that commonly produce wrong results.
 */

#include "test_framework.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/arena.h"
#include "interp/dispatch.h"
#include "baseline/codegen.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "ir/node.h"
#include "ir/graph.h"
#include "compile/pipeline.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/* Helpers                                                                     */
/* ========================================================================== */

static vtx_bytecode_t make_bc(
    const uint8_t *code, size_t len,
    vtx_value_t *consts, uint32_t const_count,
    uint16_t max_locals, uint16_t max_stack)
{
    vtx_bytecode_t bc;
    bc.code = code; bc.length = len;
    bc.constant_pool = consts; bc.constant_count = const_count;
    bc.max_locals = max_locals; bc.max_stack = max_stack;
    return bc;
}

static vtx_bytecode_t make_bc_simple(
    const uint8_t *code, size_t len,
    uint16_t max_locals, uint16_t max_stack)
{
    return make_bc(code, len, NULL, 0, max_locals, max_stack);
}

/* Run bytecode through interpreter only → reference result */
static vtx_value_t run_interp(
    const uint8_t *code, size_t code_len,
    vtx_value_t *consts, uint32_t const_count,
    uint16_t max_locals, uint16_t max_stack,
    uint32_t arg_count, vtx_value_t *args)
{
    vtx_bytecode_t bc = make_bc(code, code_len, consts, const_count,
                                 max_locals, max_stack);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_NONE);
    vtx_method_desc_t method = {
        .name = "test", .signature = "(I)I", .bytecode = &bc,
        .vtable_index = 0, .arg_count = arg_count,
        .is_virtual = false, .compiled_code = NULL
    };
    vtx_interp_t interp; vtx_interp_init(&interp, &ts, &gc);
    vtx_value_t result = vtx_interp_run(&interp, &method, args, arg_count);
    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    return result;
}

/* Run T2 JIT pipeline and execute, comparing with interpreter reference.
 * Returns true if JIT result matches interpreter result. */
static bool run_t2_and_compare(
    const uint8_t *code, size_t code_len,
    vtx_value_t *consts, uint32_t const_count,
    uint16_t max_locals, uint16_t max_stack,
    uint32_t arg_count, vtx_value_t *args,
    vtx_value_t *out_jit_result)
{
    vtx_bytecode_t bc = make_bc(code, code_len, consts, const_count,
                                 max_locals, max_stack);

    /* Get interpreter reference */
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_NONE);
    vtx_method_desc_t method = {
        .name = "t2test", .signature = "(I)I", .bytecode = &bc,
        .vtable_index = 0, .arg_count = arg_count,
        .is_virtual = false, .compiled_code = NULL
    };
    vtx_interp_t interp; vtx_interp_init(&interp, &ts, &gc);
    vtx_value_t interp_result = vtx_interp_run(&interp, &method, args, arg_count);

    /* Build SoN IR and run T2 pipeline */
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_graph_t graph;
    int rc = vtx_graph_init(&graph, method.arg_count);
    if (rc != 0) {
        vtx_interp_destroy(&interp);
        vtx_arena_destroy(&arena);
        vtx_gc_destroy(&gc);
        vtx_type_system_destroy(&ts);
        *out_jit_result = VTX_VALUE_UNDEFINED;
        return false;
    }

    rc = vtx_graph_build(&graph, &bc, &method, &arena);
    if (rc != 0) {
        vtx_graph_destroy(&graph);
        vtx_interp_destroy(&interp);
        vtx_arena_destroy(&arena);
        vtx_gc_destroy(&gc);
        vtx_type_system_destroy(&ts);
        *out_jit_result = VTX_VALUE_UNDEFINED;
        return false;
    }

    vtx_pipeline_config_t config = vtx_pipeline_config_t2();
    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 20);
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);
    config.code_cache = &cache;
    config.method_registry = &registry;
    config.method = &method;

    vtx_compile_result_t result;
    memset(&result, 0, sizeof(result));
    int prc = vtx_pipeline_run(&graph, &config, &arena, &result);

    bool match = false;
    if (prc == 0 && result.success && method.compiled_code != NULL) {
        typedef vtx_value_t (*vtx_jit_entry_t)(
            const vtx_method_desc_t *, void *, void *,
            vtx_value_t *, uint32_t);
        vtx_jit_entry_t entry = (vtx_jit_entry_t)method.compiled_code;
        vtx_value_t jit_result = entry(&method, NULL, (void*)1, args, arg_count);
        *out_jit_result = jit_result;
        match = (interp_result == jit_result);
    } else {
        *out_jit_result = VTX_VALUE_UNDEFINED;
    }

    vtx_compile_result_destroy(&result);
    vtx_pipeline_config_destroy(&config);
    vtx_code_cache_destroy(&cache);
    vtx_method_registry_destroy(&registry);
    vtx_graph_destroy(&graph);
    vtx_interp_destroy(&interp);
    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    return match;
}

/* ========================================================================== */
/* Part 1: Pure C SMI encoding/decoding tests                                  */
/* ========================================================================== */

VTX_TEST(smi_encode_decode_zero)
{
    vtx_value_t v = vtx_make_smi(0);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), 0);
    /* SMI(0) must NOT be 0 — it's NaN-boxed */
    VTX_ASSERT_NOT_EQUAL(v, (vtx_value_t)0);
    VTX_ASSERT_EQUAL(v, VTX_NAN_BOX_HEADER);
}

VTX_TEST(smi_encode_decode_positive)
{
    for (int64_t i = 1; i <= 100; i++) {
        vtx_value_t v = vtx_make_smi(i);
        VTX_ASSERT_TRUE(vtx_is_smi(v));
        VTX_ASSERT_EQUAL(vtx_smi_value(v), i);
    }
}

VTX_TEST(smi_encode_decode_negative)
{
    for (int64_t i = -100; i <= -1; i++) {
        vtx_value_t v = vtx_make_smi(i);
        VTX_ASSERT_TRUE(vtx_is_smi(v));
        VTX_ASSERT_EQUAL(vtx_smi_value(v), i);
    }
}

VTX_TEST(smi_encode_decode_large_values)
{
    /* Large positive */
    vtx_value_t v = vtx_make_smi(1000000000LL);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), 1000000000LL);

    /* Large negative */
    v = vtx_make_smi(-1000000000LL);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), -1000000000LL);

    /* Near SMI max */
    v = vtx_make_smi(VTX_SMI_MAX);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), VTX_SMI_MAX);

    /* Near SMI min */
    v = vtx_make_smi(VTX_SMI_MIN);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), VTX_SMI_MIN);
}

VTX_TEST(smi_encode_decode_powers_of_2)
{
    /* Test all powers of 2 from 1 to 2^45 (2^46 would exceed SMI_MAX) */
    for (int i = 0; i <= 45; i++) {
        int64_t val = 1LL << i;
        vtx_value_t v = vtx_make_smi(val);
        VTX_ASSERT_TRUE(vtx_is_smi(v));
        VTX_ASSERT_EQUAL(vtx_smi_value(v), val);
    }
}

VTX_TEST(smi_encode_decode_negative_powers_of_2)
{
    for (int i = 0; i <= 45; i++) {
        int64_t val = -(1LL << i);
        vtx_value_t v = vtx_make_smi(val);
        VTX_ASSERT_TRUE(vtx_is_smi(v));
        VTX_ASSERT_EQUAL(vtx_smi_value(v), val);
    }
    /* SMI_MIN = -(2^46) is exactly representable */
    vtx_value_t v = vtx_make_smi(VTX_SMI_MIN);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), VTX_SMI_MIN);
}

/* ========================================================================== */
/* Part 2: SMI arithmetic identity verification (pure C)                       */
/* ========================================================================== */

VTX_TEST(smi_add_identity)
{
    /* SMI(a) + SMI(b) - HEADER = SMI(a+b) */
    int64_t a = 42, b = 17;
    vtx_value_t sa = vtx_make_smi(a);
    vtx_value_t sb = vtx_make_smi(b);
    vtx_value_t sum_raw = sa + sb;
    vtx_value_t expected = vtx_make_smi(a + b);
    vtx_value_t actual = sum_raw - VTX_NAN_BOX_HEADER;
    VTX_ASSERT_EQUAL(actual, expected);
    VTX_ASSERT_EQUAL(vtx_smi_value(actual), a + b);
}

VTX_TEST(smi_sub_identity)
{
    /* SMI(a) - SMI(b) + HEADER = SMI(a-b) */
    int64_t a = 42, b = 17;
    vtx_value_t sa = vtx_make_smi(a);
    vtx_value_t sb = vtx_make_smi(b);
    vtx_value_t diff_raw = sa - sb;
    vtx_value_t expected = vtx_make_smi(a - b);
    vtx_value_t actual = diff_raw + VTX_NAN_BOX_HEADER;
    VTX_ASSERT_EQUAL(actual, expected);
    VTX_ASSERT_EQUAL(vtx_smi_value(actual), a - b);
}

VTX_TEST(smi_add_identity_zero)
{
    /* SMI(0) + SMI(0) - HEADER = SMI(0) */
    vtx_value_t s0 = vtx_make_smi(0);
    vtx_value_t sum_raw = s0 + s0;
    vtx_value_t actual = sum_raw - VTX_NAN_BOX_HEADER;
    VTX_ASSERT_EQUAL(actual, vtx_make_smi(0));
    VTX_ASSERT_EQUAL(vtx_smi_value(actual), 0);
}

VTX_TEST(smi_add_identity_negative)
{
    /* SMI(a) + SMI(-a) - HEADER does NOT equal SMI(0)!
     * The NaN-boxed add-with-header-adjustment identity is FUNDAMENTALLY
     * WRONG for negative values: carry from data bits corrupts the header.
     * The ONLY correct approach is untag→add→retag, which the interpreter
     * and T2 JIT (after fix) should use. */
    int64_t a = 99;
    vtx_value_t sa = vtx_make_smi(a);
    vtx_value_t sma = vtx_make_smi(-a);
    /* Verify the raw identity is broken: */
    vtx_value_t sum_raw = sa + sma;
    vtx_value_t adjust = sum_raw - VTX_NAN_BOX_HEADER;
    VTX_ASSERT_NOT_EQUAL(adjust, vtx_make_smi(0));
    /* But the logical values are correct when decoded properly: */
    /* (This relies on vtx_smi_value sign-extending correctly, which it does
     * only when the header bits happen to be intact — they aren't here) */
}

VTX_TEST(smi_sub_identity_self)
{
    /* SMI(a) - SMI(a) + HEADER = SMI(0) */
    int64_t a = 12345;
    vtx_value_t sa = vtx_make_smi(a);
    vtx_value_t diff_raw = sa - sa;
    vtx_value_t actual = diff_raw + VTX_NAN_BOX_HEADER;
    VTX_ASSERT_EQUAL(actual, vtx_make_smi(0));
}

VTX_TEST(smi_lea_displacement_identity)
{
    /* SMI(a) + c*8 = HEADER + a*8 + c*8 = HEADER + (a+c)*8 = SMI(a+c)
     * This is the LEA displacement optimization — no header adjustment needed. */
    int64_t a = 10, c = 5;
    vtx_value_t sa = vtx_make_smi(a);
    int64_t c_shifted = c << 3; /* c * 8 */
    vtx_value_t actual = sa + c_shifted;
    vtx_value_t expected = vtx_make_smi(a + c);
    VTX_ASSERT_EQUAL(actual, expected);
}

VTX_TEST(smi_lea_displacement_negative)
{
    /* SMI(a) + (-c)*8 = SMI(a-c) — no header adjustment needed */
    int64_t a = 10, c = 3;
    vtx_value_t sa = vtx_make_smi(a);
    int64_t neg_c_shifted = -(c << 3);
    vtx_value_t actual = sa + neg_c_shifted;
    vtx_value_t expected = vtx_make_smi(a - c);
    VTX_ASSERT_EQUAL(actual, expected);
}

VTX_TEST(smi_inc_dec_identity)
{
    /* INC: SMI(a) + 8 = SMI(a+1)
     * DEC: SMI(a) - 8 = SMI(a-1) */
    int64_t a = 50;
    vtx_value_t sa = vtx_make_smi(a);
    VTX_ASSERT_EQUAL(sa + 8, vtx_make_smi(a + 1));
    VTX_ASSERT_EQUAL(sa - 8, vtx_make_smi(a - 1));
}

VTX_TEST(smi_zero_is_not_zero)
{
    /* SMI(0) != 0 — this is the fundamental property that breaks CMP→TEST */
    vtx_value_t s0 = vtx_make_smi(0);
    VTX_ASSERT_NOT_EQUAL(s0, (vtx_value_t)0);
    /* TEST reg, reg on SMI(0) would give ZF=0 (non-zero), but the
     * intent is to check if the logical value is zero. TEST is wrong here. */
}

VTX_TEST(smi_negative_appears_positive)
{
    /* SMI(-1) as a raw 64-bit value appears positive (high bits are 0x7FF8...)
     * This means unsigned comparison gives wrong results for negative SMIs.
     * CMP on NaN-boxed SMI values needs sign-aware comparison. */
    vtx_value_t sm1 = vtx_make_smi(-1);
    vtx_value_t s0 = vtx_make_smi(0);
    /* As raw uint64, SMI(-1) > SMI(0) — but logically -1 < 0 */
    VTX_ASSERT_TRUE(sm1 > s0); /* raw comparison: wrong order */
}

/* ========================================================================== */
/* Part 3: Interpreter arithmetic correctness                                   */
/* ========================================================================== */

VTX_TEST(interp_add_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,  /* push local[0] */
        VT_OP_LOAD_LOCAL, 0x00, 0x01,  /* push local[1] */
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(10), vtx_make_smi(20) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 30);
}

VTX_TEST(interp_add_negative)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(-10), vtx_make_smi(20) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 10);
}

VTX_TEST(interp_add_zero)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(42), vtx_make_smi(0) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 42);
}

VTX_TEST(interp_sub_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ISUB,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(50), vtx_make_smi(20) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 30);
}

VTX_TEST(interp_sub_negative_result)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ISUB,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(10), vtx_make_smi(20) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), -10);
}

VTX_TEST(interp_mul_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMUL,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(6), vtx_make_smi(7) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 42);
}

VTX_TEST(interp_mul_by_zero)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMUL,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(42), vtx_make_smi(0) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 0);
}

VTX_TEST(interp_mul_negative)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMUL,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(-6), vtx_make_smi(7) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), -42);
}

VTX_TEST(interp_mul_negative_negative)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMUL,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(-6), vtx_make_smi(-7) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 42);
}

VTX_TEST(interp_div_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(42), vtx_make_smi(7) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 6);
}

VTX_TEST(interp_div_negative_dividend)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(-42), vtx_make_smi(7) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), -6);
}

VTX_TEST(interp_div_negative_divisor)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(42), vtx_make_smi(-7) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), -6);
}

VTX_TEST(interp_div_truncates_toward_zero)
{
    /* 7 / 2 = 3 (truncation toward zero) */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(7), vtx_make_smi(2) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 3);
}

VTX_TEST(interp_div_negative_truncation)
{
    /* -7 / 2 = -3 (truncation toward zero, NOT -4) */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(-7), vtx_make_smi(2) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), -3);
}

VTX_TEST(interp_mod_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMOD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(17), vtx_make_smi(5) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 2);
}

VTX_TEST(interp_mod_negative)
{
    /* C semantics: -17 % 5 = -2 */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMOD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(-17), vtx_make_smi(5) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), -2);
}

VTX_TEST(interp_neg)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_INEG,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t arg = vtx_make_smi(42);
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 1, 2, 1, &arg);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), -42);
}

VTX_TEST(interp_neg_negative)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_INEG,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t arg = vtx_make_smi(-42);
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 1, 2, 1, &arg);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 42);
}

VTX_TEST(interp_neg_zero)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_INEG,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t arg = vtx_make_smi(0);
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 1, 2, 1, &arg);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 0);
}

VTX_TEST(interp_shl_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ISHL,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(1), vtx_make_smi(10) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 1024);
}

VTX_TEST(interp_shr_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ISHR,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(1024), vtx_make_smi(10) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 1);
}

VTX_TEST(interp_and_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IAND,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(0xFF), vtx_make_smi(0x0F) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 0x0F);
}

VTX_TEST(interp_or_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IOR,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(0xF0), vtx_make_smi(0x0F) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 0xFF);
}

VTX_TEST(interp_xor_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IXOR,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(0xFF), vtx_make_smi(0x0F) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), 0xF0);
}

/* ========================================================================== */
/* Part 4: Interpreter comparison correctness                                   */
/* ========================================================================== */

VTX_TEST(interp_cmp_eq_true)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ICMP_EQ,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(42), vtx_make_smi(42) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_bool(result));
    VTX_ASSERT_TRUE(vtx_bool_value(result));
}

VTX_TEST(interp_cmp_eq_false)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ICMP_EQ,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(42), vtx_make_smi(43) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_bool(result));
    VTX_ASSERT_FALSE(vtx_bool_value(result));
}

VTX_TEST(interp_cmp_lt_negative)
{
    /* -3 < 5 should be true */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ICMP_LT,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(-3), vtx_make_smi(5) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_bool(result));
    VTX_ASSERT_TRUE(vtx_bool_value(result));
}

VTX_TEST(interp_cmp_gt_negative)
{
    /* -1 > 5 should be false */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ICMP_GT,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(-1), vtx_make_smi(5) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_bool(result));
    VTX_ASSERT_FALSE(vtx_bool_value(result));
}

VTX_TEST(interp_cmp_zero_vs_zero)
{
    /* SMI(0) == SMI(0) — the tricky case for CMP→TEST */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ICMP_EQ,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(0), vtx_make_smi(0) };
    vtx_value_t result = run_interp(code, sizeof(code), NULL, 0, 2, 2, 2, args);
    VTX_ASSERT_TRUE(vtx_is_bool(result));
    VTX_ASSERT_TRUE(vtx_bool_value(result));
}

VTX_TEST(interp_conditional_max)
{
    /* if (arg0 > 0) return arg0 else return 0 */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,      /* push arg0 */
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,   /* push 0 */
        VT_OP_ICMP_GT,                       /* arg0 > 0? */
        VT_OP_IF_TRUE, 0x00, 0x0E,          /* if true, goto PC 14 (return_arg) */
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,   /* push 0 */
        VT_OP_RETURN_VALUE,                  /* return 0 */
        VT_OP_LOAD_LOCAL, 0x00, 0x00,       /* return_arg: push arg0 */
        VT_OP_RETURN_VALUE                   /* return arg0 */
    };
    vtx_value_t consts[] = { vtx_make_smi(0) };
    vtx_bytecode_t bc = make_bc(code, sizeof(code), consts, 1, 1, 3);

    /* Test positive */
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_NONE);
    vtx_method_desc_t method = {
        .name = "max0", .signature = "(I)I", .bytecode = &bc,
        .vtable_index = 0, .arg_count = 1,
        .is_virtual = false, .compiled_code = NULL
    };
    vtx_interp_t interp; vtx_interp_init(&interp, &ts, &gc);
    vtx_value_t arg_pos = vtx_make_smi(5);
    vtx_value_t result_pos = vtx_interp_run(&interp, &method, &arg_pos, 1);
    VTX_ASSERT_TRUE(vtx_is_smi(result_pos));
    VTX_ASSERT_EQUAL(vtx_smi_value(result_pos), 5);

    /* Test negative */
    vtx_value_t arg_neg = vtx_make_smi(-3);
    vtx_value_t result_neg = vtx_interp_run(&interp, &method, &arg_neg, 1);
    VTX_ASSERT_TRUE(vtx_is_smi(result_neg));
    VTX_ASSERT_EQUAL(vtx_smi_value(result_neg), 0);

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Part 5: T2 JIT numerical correctness tests                                   */
/* ========================================================================== */

VTX_TEST(t2_add_positive)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(10), vtx_make_smi(20) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_add_negative)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(-10), vtx_make_smi(20) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_add_zero)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(0), vtx_make_smi(0) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_add_large_values)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(1000000000LL), vtx_make_smi(2000000000LL) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_sub_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ISUB,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(50), vtx_make_smi(20) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_sub_negative_result)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ISUB,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(10), vtx_make_smi(20) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_mul_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMUL,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(6), vtx_make_smi(7) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_mul_negative)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMUL,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(-6), vtx_make_smi(7) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_mul_by_zero)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMUL,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(42), vtx_make_smi(0) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_mul_both_negative)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMUL,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(-6), vtx_make_smi(-7) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_div_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(42), vtx_make_smi(7) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_div_negative)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(-42), vtx_make_smi(7) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_div_truncation)
{
    /* 7 / 2 = 3 (truncation toward zero) */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(7), vtx_make_smi(2) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

/* ---- Magic-number Div tests (Fix #15) ----
 * Divisor is a non-power-of-2 constant loaded via LOAD_CONST_INT.
 * The isel fast path uses compute_magic_number + one-operand IMUL
 * instead of IDIV. The result must match C99 truncating division
 * for both positive and negative dividends. */

VTX_TEST(t2_div_magic_3_positive)
{
    /* 100 / 3 = 33 */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(3) };
    vtx_value_t args[1] = { vtx_make_smi(100) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_div_magic_3_negative)
{
    /* -100 / 3 = -33 (C99 truncation) */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(3) };
    vtx_value_t args[1] = { vtx_make_smi(-100) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_div_magic_7_various)
{
    /* Multiple cases for divisor 7 — exercises M = ceil(2^64/7). */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(7) };
    int64_t cases[] = { 0, 1, 6, 7, 8, 14, 100, -1, -6, -7, -8, -14, -100 };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        vtx_value_t args[1] = { vtx_make_smi(cases[i]) };
        vtx_value_t jit_result;
        bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
        if (!match) {
            fprintf(stderr, "FAIL: t2_div_magic_7_various case %zu (n=%lld)\n",
                    i, (long long)cases[i]);
        }
        VTX_ASSERT_TRUE(match);
    }
}

VTX_TEST(t2_div_magic_10)
{
    /* 12345 / 10 = 1234 — divisor 10 = 2 * 5, not power of 2 */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(10) };
    vtx_value_t args[1] = { vtx_make_smi(12345) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_div_magic_10_negative)
{
    /* -12345 / 10 = -1234 (C99 truncation) */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(10) };
    vtx_value_t args[1] = { vtx_make_smi(-12345) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_div_magic_negative_divisor)
{
    /* 100 / -3 = -33 (negative divisor triggers the NEG correction) */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(-3) };
    vtx_value_t args[1] = { vtx_make_smi(100) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_div_magic_negative_divisor_negative_dividend)
{
    /* -100 / -3 = 33 */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(-3) };
    vtx_value_t args[1] = { vtx_make_smi(-100) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_div_magic_large_divisor)
{
    /* 1000000 / 999 = 1001 — large non-power-of-2 divisor */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(999) };
    vtx_value_t args[1] = { vtx_make_smi(1000000) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_div_magic_small_dividend)
{
    /* 5 / 1000 = 0 (dividend smaller than divisor) */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(1000) };
    vtx_value_t args[1] = { vtx_make_smi(5) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_mod_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMOD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(17), vtx_make_smi(5) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_neg_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_INEG,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t arg = vtx_make_smi(42);
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 1, 2, 1, &arg, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_neg_negative)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_INEG,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t arg = vtx_make_smi(-42);
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 1, 2, 1, &arg, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_shl_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ISHL,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(1), vtx_make_smi(10) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_shr_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ISHR,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(1024), vtx_make_smi(10) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_and_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IAND,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(0xFF), vtx_make_smi(0x0F) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_or_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IOR,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(0xF0), vtx_make_smi(0x0F) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_xor_basic)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IXOR,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(0xFF), vtx_make_smi(0x0F) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_conditional_positive)
{
    /* if (arg0 > 0) return arg0 else return 0 */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_ICMP_GT,
        VT_OP_IF_TRUE, 0x00, 0x0E,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_RETURN_VALUE,
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(0) };
    vtx_value_t arg = vtx_make_smi(5);
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 3, 1, &arg, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_conditional_negative)
{
    /* Same bytecode, but with negative arg */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_ICMP_GT,
        VT_OP_IF_TRUE, 0x00, 0x0E,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_RETURN_VALUE,
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(0) };
    vtx_value_t arg = vtx_make_smi(-3);
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 3, 1, &arg, &jit_result);
    VTX_ASSERT_TRUE(match);
}

/* ========================================================================== */
/* Part 6: Edge case T2 JIT tests                                              */
/* ========================================================================== */

VTX_TEST(t2_add_both_negative)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(-10), vtx_make_smi(-20) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_sub_zero_from_zero)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ISUB,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(0), vtx_make_smi(0) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_mul_power_of_two)
{
    /* Multiplication by power of 2 uses shift optimization */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMUL,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(3), vtx_make_smi(8) };  /* 3 * 8 = 24 */
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_mul_by_one)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMUL,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(42), vtx_make_smi(1) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_mul_by_minus_one)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMUL,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(42), vtx_make_smi(-1) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_div_negative_by_negative)
{
    /* (-42) / (-7) = 6 */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(-42), vtx_make_smi(-7) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_mod_negative)
{
    /* -17 % 5 = -2 */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMOD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[2] = { vtx_make_smi(-17), vtx_make_smi(5) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

/* ---- Mod(x, 2^k) strength reduction tests (Fix #2) ----
 * These exercise the isel fast path that replaces IDIV (~25 cycles)
 * with a SAR+AND+ADD+SAR+SHL+SUB sequence (~6 cycles). The formula
 * must match C99 % truncation-toward-zero semantics exactly.
 * Note: the bytecode uses a runtime variable as the divisor so SCCP
 * can't fold it — but the *IR Constant node* for the divisor (created
 * by LOAD_CONST_INT) is what try_get_const_int() looks at, so the
 * isel fast path still fires. */

VTX_TEST(t2_mod_power_of_2_positive)
{
    /* 17 % 4 = 1 (4 = 2^2, exercises k=2 path) */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IMOD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(4) };
    vtx_value_t args[1] = { vtx_make_smi(17) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_mod_power_of_2_negative_dividend)
{
    /* -7 % 4 = -3 (C99 truncation; bitwise AND would give 1) */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IMOD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(4) };
    vtx_value_t args[1] = { vtx_make_smi(-7) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_mod_power_of_2_k1)
{
    /* n % 2 = 0 if even, ±1 if odd — the collatz hot path.
     * Tests k=1 specifically (mask=1). */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IMOD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(2) };
    int64_t cases[] = { 0, 1, -1, 2, -2, 7, -7, 100, -100 };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        vtx_value_t args[1] = { vtx_make_smi(cases[i]) };
        vtx_value_t jit_result;
        bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
        if (!match) {
            fprintf(stderr, "FAIL: t2_mod_power_of_2_k1 case %zu (n=%lld)\n",
                    i, (long long)cases[i]);
        }
        VTX_ASSERT_TRUE(match);
    }
}

VTX_TEST(t2_mod_power_of_2_k3)
{
    /* 100 % 8 = 4 (k=3) */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IMOD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(8) };
    vtx_value_t args[1] = { vtx_make_smi(100) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_mod_power_of_2_negative_k3)
{
    /* -100 % 8 = -4 (C99: sign of dividend) */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IMOD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(8) };
    vtx_value_t args[1] = { vtx_make_smi(-100) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_mod_power_of_2_exact_divisor)
{
    /* 16 % 8 = 0 (no remainder) */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IMOD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(8) };
    vtx_value_t args[1] = { vtx_make_smi(16) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_mod_power_of_2_negative_exact_divisor)
{
    /* -16 % 8 = 0 (still zero remainder) */
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_IMOD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(8) };
    vtx_value_t args[1] = { vtx_make_smi(-16) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 1, 2, 1, args, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_const_return)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(42) };
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), consts, 1, 0, 1, 0, NULL, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_identity_return)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t arg = vtx_make_smi(99);
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 1, 1, 1, &arg, &jit_result);
    VTX_ASSERT_TRUE(match);
}

VTX_TEST(t2_neg_zero)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_INEG,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t arg = vtx_make_smi(0);
    vtx_value_t jit_result;
    bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 1, 2, 1, &arg, &jit_result);
    VTX_ASSERT_TRUE(match);
}

/* ========================================================================== */
/* Part 7: Stress tests — many values through JIT                              */
/* ========================================================================== */

VTX_TEST(t2_add_stress)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    /* Test many pairs */
    int64_t test_values[] = {
        0, 1, -1, 2, -2, 42, -42, 100, -100,
        1000, -1000, 1000000, -1000000,
        1000000000LL, -1000000000LL
    };
    int n = sizeof(test_values) / sizeof(test_values[0]);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            vtx_value_t args[2] = { vtx_make_smi(test_values[i]), vtx_make_smi(test_values[j]) };
            vtx_value_t jit_result;
            bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
            if (!match) {
                char msg[128];
                snprintf(msg, sizeof(msg), "add(%lld, %lld) mismatch",
                         (long long)test_values[i], (long long)test_values[j]);
                vtx_test_record_failure(__FILE__, __LINE__, msg);
                return; /* One failure per test is enough to flag the bug */
            }
        }
    }
}

VTX_TEST(t2_sub_stress)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ISUB,
        VT_OP_RETURN_VALUE
    };
    int64_t test_values[] = {
        0, 1, -1, 2, -2, 42, -42, 100, -100,
        1000, -1000, 1000000, -1000000
    };
    int n = sizeof(test_values) / sizeof(test_values[0]);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            vtx_value_t args[2] = { vtx_make_smi(test_values[i]), vtx_make_smi(test_values[j]) };
            vtx_value_t jit_result;
            bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
            if (!match) {
                char msg[128];
                snprintf(msg, sizeof(msg), "sub(%lld, %lld) mismatch",
                         (long long)test_values[i], (long long)test_values[j]);
                vtx_test_record_failure(__FILE__, __LINE__, msg);
                return;
            }
        }
    }
}

VTX_TEST(t2_mul_stress)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMUL,
        VT_OP_RETURN_VALUE
    };
    int64_t test_values[] = {
        0, 1, -1, 2, -2, 3, -3, 4, -4, 5, -5,
        7, -7, 8, -8, 16, -16, 42, -42
    };
    int n = sizeof(test_values) / sizeof(test_values[0]);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            /* Skip products that overflow 48-bit SMI range */
            int64_t product = test_values[i] * test_values[j];
            if (product > VTX_SMI_MAX || product < VTX_SMI_MIN) continue;
            vtx_value_t args[2] = { vtx_make_smi(test_values[i]), vtx_make_smi(test_values[j]) };
            vtx_value_t jit_result;
            bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
            if (!match) {
                char msg[128];
                snprintf(msg, sizeof(msg), "mul(%lld, %lld) mismatch (product=%lld)",
                         (long long)test_values[i], (long long)test_values[j],
                         (long long)product);
                vtx_test_record_failure(__FILE__, __LINE__, msg);
                return;
            }
        }
    }
}

VTX_TEST(t2_div_stress)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    int64_t dividends[] = { 0, 1, -1, 7, -7, 42, -42, 100, -100, 1000000 };
    int64_t divisors[] = { 1, -1, 2, -2, 3, -3, 7, -7 };
    int nd = sizeof(dividends) / sizeof(dividends[0]);
    int ndv = sizeof(divisors) / sizeof(divisors[0]);
    for (int i = 0; i < nd; i++) {
        for (int j = 0; j < ndv; j++) {
            vtx_value_t args[2] = { vtx_make_smi(dividends[i]), vtx_make_smi(divisors[j]) };
            vtx_value_t jit_result;
            bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 2, 2, 2, args, &jit_result);
            if (!match) {
                char msg[128];
                snprintf(msg, sizeof(msg), "div(%lld, %lld) mismatch",
                         (long long)dividends[i], (long long)divisors[j]);
                vtx_test_record_failure(__FILE__, __LINE__, msg);
                return;
            }
        }
    }
}

VTX_TEST(t2_neg_stress)
{
    static const uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_INEG,
        VT_OP_RETURN_VALUE
    };
    int64_t test_values[] = {
        0, 1, -1, 2, -2, 42, -42, 100, -100,
        1000, -1000, 1000000, -1000000
    };
    int n = sizeof(test_values) / sizeof(test_values[0]);
    for (int i = 0; i < n; i++) {
        vtx_value_t arg = vtx_make_smi(test_values[i]);
        vtx_value_t jit_result;
        bool match = run_t2_and_compare(code, sizeof(code), NULL, 0, 1, 2, 1, &arg, &jit_result);
        if (!match) {
            char msg[128];
            snprintf(msg, sizeof(msg), "neg(%lld) mismatch", (long long)test_values[i]);
            vtx_test_record_failure(__FILE__, __LINE__, msg);
            return;
        }
    }
}

/* ========================================================================== */
/* Main                                                                        */
/* ========================================================================== */

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\n========================================\n");
    printf("SMI Arithmetic Tests: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    printf("========================================\n");
    return result.fail_count > 0 ? 1 : 0;
}
