/**
 * test_jit_e2e.c — End-to-end JIT compilation and execution tests
 *
 * Tests the full JIT pipeline: baseline JIT compilation, code installation,
 * JIT dispatch from the interpreter, and correct execution of compiled code.
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
#include "compile/request.h"
#include "compile/threadpool.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

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

/* Run a method through interpreter (no JIT) to get baseline result */
static vtx_value_t run_interp_only(
    const uint8_t *code, size_t code_len,
    vtx_value_t *consts, uint32_t const_count,
    uint16_t max_locals, uint16_t max_stack,
    uint32_t arg_count, vtx_value_t *args)
{
    vtx_bytecode_t bc = make_bc(code, code_len, consts, const_count, max_locals, max_stack);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_NONE);
    vtx_interp_t interp; vtx_interp_init(&interp, &ts, &gc);

    vtx_method_desc_t method = {
        .name = "test", .signature = "(I)I", .bytecode = &bc,
        .vtable_index = 0xFFFFFFFF, .arg_count = arg_count,
        .is_virtual = false, .compiled_code = NULL
    };

    vtx_value_t result = vtx_interp_run(&interp, &method, args, arg_count);
    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    return result;
}

/* Compile with JIT and run through interpreter (which should dispatch to JIT) */
static vtx_value_t run_jit(
    const uint8_t *code, size_t code_len,
    vtx_value_t *consts, uint32_t const_count,
    uint16_t max_locals, uint16_t max_stack,
    uint32_t arg_count, vtx_value_t *args)
{
    vtx_bytecode_t bc = make_bc(code, code_len, consts, const_count, max_locals, max_stack);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_NONE);
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 20);
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);

    vtx_method_desc_t method = {
        .name = "test", .signature = "(I)I", .bytecode = &bc,
        .vtable_index = 0xFFFFFFFF, .arg_count = arg_count,
        .is_virtual = false, .compiled_code = NULL
    };

    vtx_compiled_code_t *compiled = vtx_baseline_compile(&method, NULL, &arena, &cache, &registry);

    vtx_interp_t interp; vtx_interp_init(&interp, &ts, &gc);
    vtx_value_t result = vtx_interp_run(&interp, &method, args, arg_count);

    vtx_interp_destroy(&interp);
    if (compiled) vtx_compiled_code_destroy(compiled);
    vtx_code_cache_destroy(&cache);
    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    return result;
}

/* ===== Tests ===== */

VTX_TEST(jit_identity)
{
    uint8_t code[] = { VT_OP_LOAD_LOCAL, 0x00, 0x00, VT_OP_RETURN_VALUE };
    vtx_value_t args[] = { vtx_make_smi(42) };
    vtx_value_t interp = run_interp_only(code, sizeof(code), NULL, 0, 1, 8, 1, args);
    vtx_value_t jit = run_jit(code, sizeof(code), NULL, 0, 1, 8, 1, args);
    VTX_ASSERT_TRUE(vtx_is_smi(jit));
    VTX_ASSERT_EQUAL(vtx_smi_value(jit), vtx_smi_value(interp));
}

VTX_TEST(jit_const_return)
{
    vtx_value_t consts[] = { vtx_make_smi(12345) };
    uint8_t code[] = { VT_OP_LOAD_CONST_INT, 0x00, 0x00, VT_OP_RETURN_VALUE };
    vtx_value_t jit = run_jit(code, sizeof(code), consts, 1, 1, 8, 0, NULL);
    VTX_ASSERT_TRUE(vtx_is_smi(jit));
    VTX_ASSERT_EQUAL(vtx_smi_value(jit), (int64_t)12345);
}

VTX_TEST(jit_simple_add)
{
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[] = { vtx_make_smi(3), vtx_make_smi(4) };
    vtx_value_t jit = run_jit(code, sizeof(code), NULL, 0, 2, 8, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(jit));
    VTX_ASSERT_EQUAL(vtx_smi_value(jit), (int64_t)7);
}

VTX_TEST(jit_simple_sub)
{
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ISUB,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[] = { vtx_make_smi(100), vtx_make_smi(37) };
    vtx_value_t jit = run_jit(code, sizeof(code), NULL, 0, 2, 8, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(jit));
    VTX_ASSERT_EQUAL(vtx_smi_value(jit), (int64_t)63);
}

VTX_TEST(jit_simple_mul)
{
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMUL,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[] = { vtx_make_smi(6), vtx_make_smi(7) };
    vtx_value_t interp = run_interp_only(code, sizeof(code), NULL, 0, 2, 8, 2, args);
    vtx_value_t jit = run_jit(code, sizeof(code), NULL, 0, 2, 8, 2, args);
    printf("  interp mul: %ld, jit mul: %ld\n",
           vtx_is_smi(interp) ? vtx_smi_value(interp) : -999,
           vtx_is_smi(jit) ? vtx_smi_value(jit) : -999);
    VTX_ASSERT_TRUE(vtx_is_smi(jit));
    VTX_ASSERT_EQUAL(vtx_smi_value(jit), vtx_smi_value(interp));
}

VTX_TEST(jit_bitwise_and)
{
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IAND,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[] = { vtx_make_smi(0xFF), vtx_make_smi(0x0F) };
    vtx_value_t jit = run_jit(code, sizeof(code), NULL, 0, 2, 8, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(jit));
    VTX_ASSERT_EQUAL(vtx_smi_value(jit), (int64_t)0x0F);
}

VTX_TEST(jit_comparison_branch)
{
    vtx_value_t consts[] = { vtx_make_smi(0) };
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL,   0x00, 0x00,    /* PC 0 */
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,  /* PC 3 */
        VT_OP_ICMP_GT,                   /* PC 6 */
        VT_OP_IF_TRUE,    0x00, 0x0E,     /* PC 7 -> PC 14 */
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,  /* PC 10 */
        VT_OP_RETURN_VALUE,              /* PC 13 */
        VT_OP_LOAD_LOCAL,  0x00, 0x00,    /* PC 14 */
        VT_OP_RETURN_VALUE               /* PC 17 */
    };

    /* Test: max0(5) should return 5 */
    vtx_value_t args1[] = { vtx_make_smi(5) };
    vtx_value_t r1 = run_jit(code, sizeof(code), consts, 1, 1, 8, 1, args1);
    printf("  max0(5) = %ld\n", vtx_is_smi(r1) ? vtx_smi_value(r1) : -999);
    VTX_ASSERT_TRUE(vtx_is_smi(r1));
    VTX_ASSERT_EQUAL(vtx_smi_value(r1), (int64_t)5);

    /* Test: max0(-3) should return 0 */
    vtx_value_t args2[] = { vtx_make_smi(-3) };
    vtx_value_t r2 = run_jit(code, sizeof(code), consts, 1, 1, 8, 1, args2);
    printf("  max0(-3) = %ld\n", vtx_is_smi(r2) ? vtx_smi_value(r2) : -999);
    VTX_ASSERT_TRUE(vtx_is_smi(r2));
    VTX_ASSERT_EQUAL(vtx_smi_value(r2), (int64_t)0);
}

/* ========================================================================== */
/* Pipeline wiring tests — verify the JIT pipeline is actually connected       */
/* ========================================================================== */

/* Test: vtx_interp_set_compile_ctx should set the compile context */
VTX_TEST(jit_interp_set_compile_ctx)
{
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_NONE);
    vtx_interp_t interp; vtx_interp_init(&interp, &ts, &gc);

    /* compile_ctx should be NULL initially */
    VTX_ASSERT_TRUE(interp.compile_ctx == NULL);

    /* Set it to a non-NULL value */
    vtx_compile_context_t ctx;
    vtx_compile_context_init(&ctx);
    vtx_interp_set_compile_ctx(&interp, &ctx);
    VTX_ASSERT_TRUE(interp.compile_ctx == &ctx);

    /* Set it back to NULL */
    vtx_interp_set_compile_ctx(&interp, NULL);
    VTX_ASSERT_TRUE(interp.compile_ctx == NULL);

    vtx_compile_context_destroy(&ctx);
    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

/* Test: compile context wire_threadpool should set the callback */
VTX_TEST(jit_compile_context_wire_threadpool)
{
    vtx_threadpool_t pool;
    int rc = vtx_threadpool_init(&pool, 1);
    VTX_ASSERT_EQUAL(rc, 0);

    vtx_compile_context_t ctx;
    vtx_compile_context_init(&ctx);
    ctx.threadpool = &pool;

    /* Before wiring, compile_callback should be NULL */
    VTX_ASSERT_TRUE(pool.compile_callback == NULL);

    /* Wire it */
    rc = vtx_compile_context_wire_threadpool(&ctx);
    VTX_ASSERT_EQUAL(rc, 0);

    /* After wiring, compile_callback should be set */
    VTX_ASSERT_TRUE(pool.compile_callback != NULL);

    vtx_compile_context_destroy(&ctx);
    vtx_threadpool_shutdown(&pool);
}

/* Test: compiled code pointer is set after baseline JIT compilation */
VTX_TEST(jit_compiled_code_pointer_set)
{
    uint8_t code[] = { VT_OP_LOAD_LOCAL, 0x00, 0x00, VT_OP_RETURN_VALUE };
    vtx_bytecode_t bc = make_bc_simple(code, sizeof(code), 1, 8);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_NONE);
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 20);
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);

    vtx_method_desc_t method = {
        .name = "test", .signature = "(I)I", .bytecode = &bc,
        .vtable_index = 1, .arg_count = 1,
        .is_virtual = false, .compiled_code = NULL
    };

    /* Before compilation, compiled_code should be NULL */
    VTX_ASSERT_TRUE(method.compiled_code == NULL);

    /* Compile with baseline JIT */
    vtx_compiled_code_t *compiled = vtx_baseline_compile(&method, NULL, &arena, &cache, &registry);
    VTX_ASSERT_TRUE(compiled != NULL);

    /* After compilation with cache+registry, compiled_code should be set */
    VTX_ASSERT_TRUE(method.compiled_code != NULL);

    if (compiled) vtx_compiled_code_destroy(compiled);
    vtx_code_cache_destroy(&cache);
    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

/* Test: void method returns undefined without triggering re-interpretation
 * (verifies the deopt_pending flag fix) */
VTX_TEST(jit_void_method_returns_undefined)
{
    /* A method that returns void (just RETURN, no value) */
    uint8_t code[] = { VT_OP_RETURN };
    vtx_bytecode_t bc = make_bc_simple(code, sizeof(code), 0, 8);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_NONE);
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 20);
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);

    vtx_method_desc_t method = {
        .name = "void_test", .signature = "()V", .bytecode = &bc,
        .vtable_index = 2, .arg_count = 0,
        .is_virtual = false, .compiled_code = NULL
    };

    vtx_compiled_code_t *compiled = vtx_baseline_compile(&method, NULL, &arena, &cache, &registry);
    VTX_ASSERT_TRUE(compiled != NULL);

    /* Run through interpreter — should dispatch to JIT and return undefined
     * WITHOUT falling back to re-interpretation. Previously, the undefined
     * return value triggered re-interpretation, which was wrong. */
    vtx_interp_t interp; vtx_interp_init(&interp, &ts, &gc);
    vtx_value_t result = vtx_interp_run(&interp, &method, NULL, 0);

    /* The result should be undefined (void return), and it should NOT
     * have triggered deopt_pending (which would cause re-interpretation). */
    VTX_ASSERT_TRUE(vtx_is_undefined(result));
    VTX_ASSERT_TRUE(!interp.deopt_pending);

    vtx_interp_destroy(&interp);
    if (compiled) vtx_compiled_code_destroy(compiled);
    vtx_code_cache_destroy(&cache);
    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

/* Test: JIT division produces correct results */
VTX_TEST(jit_division)
{
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IDIV,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[] = { vtx_make_smi(100), vtx_make_smi(7) };
    vtx_value_t interp = run_interp_only(code, sizeof(code), NULL, 0, 2, 8, 2, args);
    vtx_value_t jit = run_jit(code, sizeof(code), NULL, 0, 2, 8, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(jit));
    VTX_ASSERT_EQUAL(vtx_smi_value(jit), vtx_smi_value(interp));
}

/* Test: JIT left shift produces correct results */
VTX_TEST(jit_left_shift)
{
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_ISHL,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[] = { vtx_make_smi(1), vtx_make_smi(10) };
    vtx_value_t interp = run_interp_only(code, sizeof(code), NULL, 0, 2, 8, 2, args);
    vtx_value_t jit = run_jit(code, sizeof(code), NULL, 0, 2, 8, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(jit));
    VTX_ASSERT_EQUAL(vtx_smi_value(jit), vtx_smi_value(interp));
}

/* Test: JIT bitwise OR */
VTX_TEST(jit_bitwise_or)
{
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IOR,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[] = { vtx_make_smi(0xF0), vtx_make_smi(0x0F) };
    vtx_value_t jit = run_jit(code, sizeof(code), NULL, 0, 2, 8, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(jit));
    VTX_ASSERT_EQUAL(vtx_smi_value(jit), (int64_t)0xFF);
}

/* Test: JIT modulo */
VTX_TEST(jit_modulo)
{
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IMOD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t args[] = { vtx_make_smi(17), vtx_make_smi(5) };
    vtx_value_t interp = run_interp_only(code, sizeof(code), NULL, 0, 2, 8, 2, args);
    vtx_value_t jit = run_jit(code, sizeof(code), NULL, 0, 2, 8, 2, args);
    VTX_ASSERT_TRUE(vtx_is_smi(jit));
    VTX_ASSERT_EQUAL(vtx_smi_value(jit), vtx_smi_value(interp));
}

/* Test: multiple sequential JIT calls produce consistent results */
VTX_TEST(jit_sequential_calls)
{
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,
        VT_OP_LOAD_LOCAL, 0x00, 0x01,
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_bytecode_t bc = make_bc_simple(code, sizeof(code), 2, 8);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_NONE);
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 20);
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);

    vtx_method_desc_t method = {
        .name = "add", .signature = "(II)I", .bytecode = &bc,
        .vtable_index = 3, .arg_count = 2,
        .is_virtual = false, .compiled_code = NULL
    };

    vtx_compiled_code_t *compiled = vtx_baseline_compile(&method, NULL, &arena, &cache, &registry);
    VTX_ASSERT_TRUE(compiled != NULL);

    vtx_interp_t interp; vtx_interp_init(&interp, &ts, &gc);

    /* Call the same compiled method multiple times */
    for (int i = 0; i < 5; i++) {
        vtx_value_t args[] = { vtx_make_smi(i * 10), vtx_make_smi(i * 3) };
        vtx_value_t result = vtx_interp_run(&interp, &method, args, 2);
        VTX_ASSERT_TRUE(vtx_is_smi(result));
        VTX_ASSERT_EQUAL(vtx_smi_value(result), (int64_t)(i * 13));
    }

    vtx_interp_destroy(&interp);
    if (compiled) vtx_compiled_code_destroy(compiled);
    vtx_code_cache_destroy(&cache);
    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\n========================================\n");
    printf("Results: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    printf("========================================\n");
    return result.fail_count > 0 ? 1 : 0;
}
