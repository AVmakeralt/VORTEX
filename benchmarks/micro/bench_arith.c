/**
 * bench_arith.c — Micro-benchmark: arithmetic operations in interpreter
 *
 * Benchmarks iadd, isub, imul, idiv in the VORTEX interpreter (T0)
 * and compares with native C speed.
 */

#include "bench_framework.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "interp/dispatch.h"

#include <stdio.h>
#include <stdlib.h>

/* ========================================================================== */
/* Native C arithmetic benchmark                                                */
/* ========================================================================== */

static int64_t g_arith_result;

static void bench_native_iadd(void *arg)
{
    (void)arg;
    int64_t sum = 0;
    for (int i = 0; i < 10000; i++) {
        sum += (int64_t)i;
    }
    g_arith_result = sum;
}

static void bench_native_isub(void *arg)
{
    (void)arg;
    int64_t diff = 10000000LL;
    for (int i = 0; i < 10000; i++) {
        diff -= (int64_t)i;
    }
    g_arith_result = diff;
}

static void bench_native_imul(void *arg)
{
    (void)arg;
    int64_t prod = 1;
    for (int i = 1; i < 1000; i++) {
        prod *= (int64_t)i;
        /* Reset to prevent overflow */
        if (prod > 1000000000LL || prod == 0) prod = 1;
    }
    g_arith_result = prod;
}

static void bench_native_idiv(void *arg)
{
    (void)arg;
    int64_t quot = 1000000000LL;
    for (int i = 1; i < 1000; i++) {
        quot /= (int64_t)(i + 1);
        if (quot == 0) quot = 1000000000LL;
    }
    g_arith_result = quot;
}

/* ========================================================================== */
/* Interpreter arithmetic benchmark                                             */
/* ========================================================================== */

/* Shared interpreter state for benchmark functions */
typedef struct {
    vtx_interp_t    *interp;
    vtx_method_desc_t method_iadd;
    vtx_method_desc_t method_isub;
    vtx_method_desc_t method_imul;
    vtx_method_desc_t method_idiv;
    vtx_bytecode_t    bc_iadd;
    vtx_bytecode_t    bc_isub;
    vtx_bytecode_t    bc_imul;
    vtx_bytecode_t    bc_idiv;
} bench_interp_state_t;

/* Bytecode for iadd loop:
 * local0 = 0; local1 = iterations;
 * loop: if (local1 == 0) goto end
 *       local0 = local0 + local1
 *       local1 = local1 - 1
 *       goto loop
 * end: return local0
 *
 * We'll keep it simple: just measure a tight iadd loop.
 */

/* Simpler approach: measure a single iadd operation many times.
 * The bytecode just does: load_local 0, load_local 1, iadd, return_value
 * We'll call this 10000 times from the benchmark harness.
 */

/* iadd: load_local 0, load_local 1, iadd, return_value */
static const uint8_t bc_iadd_code[] = {
    VT_OP_LOAD_LOCAL,  0x00, 0x00,
    VT_OP_LOAD_LOCAL,  0x00, 0x01,
    VT_OP_IADD,
    VT_OP_RETURN_VALUE
};

/* isub: load_local 0, load_local 1, isub, return_value */
static const uint8_t bc_isub_code[] = {
    VT_OP_LOAD_LOCAL,  0x00, 0x00,
    VT_OP_LOAD_LOCAL,  0x00, 0x01,
    VT_OP_ISUB,
    VT_OP_RETURN_VALUE
};

/* imul: load_local 0, load_local 1, imul, return_value */
static const uint8_t bc_imul_code[] = {
    VT_OP_LOAD_LOCAL,  0x00, 0x00,
    VT_OP_LOAD_LOCAL,  0x00, 0x01,
    VT_OP_IMUL,
    VT_OP_RETURN_VALUE
};

/* idiv: load_local 0, load_local 1, idiv, return_value */
static const uint8_t bc_idiv_code[] = {
    VT_OP_LOAD_LOCAL,  0x00, 0x00,
    VT_OP_LOAD_LOCAL,  0x00, 0x01,
    VT_OP_IDIV,
    VT_OP_RETURN_VALUE
};

#define ARITH_ITERS 10000

typedef struct {
    vtx_interp_t      *interp;
    vtx_method_desc_t *method;
    vtx_value_t        arg0;
    vtx_value_t        arg1;
} arith_bench_arg_t;

static void bench_interp_iadd(void *arg)
{
    arith_bench_arg_t *ba = (arith_bench_arg_t *)arg;
    vtx_value_t args[] = { ba->arg0, ba->arg1 };
    for (int i = 0; i < ARITH_ITERS; i++) {
        vtx_interp_run(ba->interp, ba->method, args, 2);
    }
}

static void bench_interp_isub(void *arg)
{
    arith_bench_arg_t *ba = (arith_bench_arg_t *)arg;
    vtx_value_t args[] = { ba->arg0, ba->arg1 };
    for (int i = 0; i < ARITH_ITERS; i++) {
        vtx_interp_run(ba->interp, ba->method, args, 2);
    }
}

static void bench_interp_imul(void *arg)
{
    arith_bench_arg_t *ba = (arith_bench_arg_t *)arg;
    vtx_value_t args[] = { ba->arg0, ba->arg1 };
    for (int i = 0; i < ARITH_ITERS; i++) {
        vtx_interp_run(ba->interp, ba->method, args, 2);
    }
}

static void bench_interp_idiv(void *arg)
{
    arith_bench_arg_t *ba = (arith_bench_arg_t *)arg;
    vtx_value_t args[] = { ba->arg0, ba->arg1 };
    for (int i = 0; i < ARITH_ITERS; i++) {
        vtx_interp_run(ba->interp, ba->method, args, 2);
    }
}

/* ========================================================================== */
/* Main                                                                         */
/* ========================================================================== */

int main(void)
{
    printf("=== VORTEX Arithmetic Micro-Benchmarks ===\n\n");

    /* --- Native C benchmarks --- */
    printf("--- Native C ---\n");
    vtx_bench_result_t native_iadd = vtx_bench_run("native iadd (10k iters)", bench_native_iadd, NULL);
    vtx_bench_report(&native_iadd);

    vtx_bench_result_t native_isub = vtx_bench_run("native isub (10k iters)", bench_native_isub, NULL);
    vtx_bench_report(&native_isub);

    vtx_bench_result_t native_imul = vtx_bench_run("native imul (1k iters)", bench_native_imul, NULL);
    vtx_bench_report(&native_imul);

    vtx_bench_result_t native_idiv = vtx_bench_run("native idiv (1k iters)", bench_native_idiv, NULL);
    vtx_bench_report(&native_idiv);

    /* --- Interpreter (T0) benchmarks --- */
    printf("\n--- Interpreter T0 ---\n");

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    /* iadd */
    {
        vtx_bytecode_t bc = { bc_iadd_code, sizeof(bc_iadd_code), NULL, 0, 2, 4 };
        vtx_method_desc_t method = {
            .name = "iadd", .signature = "(II)I", .bytecode = &bc,
            .vtable_index = 0xFFFFFFFF, .is_virtual = false
        };
        arith_bench_arg_t ba = { &interp, &method, vtx_make_smi(42), vtx_make_smi(17) };
        vtx_bench_result_t result = vtx_bench_run("T0 iadd (10k iters)", bench_interp_iadd, &ba);
        vtx_bench_report(&result);
        vtx_bench_compare("T0 iadd", &result, "native iadd", &native_iadd);
    }

    /* isub */
    {
        vtx_bytecode_t bc = { bc_isub_code, sizeof(bc_isub_code), NULL, 0, 2, 4 };
        vtx_method_desc_t method = {
            .name = "isub", .signature = "(II)I", .bytecode = &bc,
            .vtable_index = 0xFFFFFFFF, .is_virtual = false
        };
        arith_bench_arg_t ba = { &interp, &method, vtx_make_smi(100), vtx_make_smi(37) };
        vtx_bench_result_t result = vtx_bench_run("T0 isub (10k iters)", bench_interp_isub, &ba);
        vtx_bench_report(&result);
        vtx_bench_compare("T0 isub", &result, "native isub", &native_isub);
    }

    /* imul */
    {
        vtx_bytecode_t bc = { bc_imul_code, sizeof(bc_imul_code), NULL, 0, 2, 4 };
        vtx_method_desc_t method = {
            .name = "imul", .signature = "(II)I", .bytecode = &bc,
            .vtable_index = 0xFFFFFFFF, .is_virtual = false
        };
        arith_bench_arg_t ba = { &interp, &method, vtx_make_smi(6), vtx_make_smi(7) };
        vtx_bench_result_t result = vtx_bench_run("T0 imul (10k iters)", bench_interp_imul, &ba);
        vtx_bench_report(&result);
        vtx_bench_compare("T0 imul", &result, "native imul", &native_imul);
    }

    /* idiv */
    {
        vtx_bytecode_t bc = { bc_idiv_code, sizeof(bc_idiv_code), NULL, 0, 2, 4 };
        vtx_method_desc_t method = {
            .name = "idiv", .signature = "(II)I", .bytecode = &bc,
            .vtable_index = 0xFFFFFFFF, .is_virtual = false
        };
        arith_bench_arg_t ba = { &interp, &method, vtx_make_smi(100), vtx_make_smi(3) };
        vtx_bench_result_t result = vtx_bench_run("T0 idiv (10k iters)", bench_interp_idiv, &ba);
        vtx_bench_report(&result);
        vtx_bench_compare("T0 idiv", &result, "native idiv", &native_idiv);
    }

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);

    (void)g_arith_result; /* prevent optimization */
    return 0;
}
