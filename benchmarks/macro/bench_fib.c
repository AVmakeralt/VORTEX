/**
 * bench_fib.c — Macro benchmark: Fibonacci
 *
 * Compares T0 interpreter vs native C for recursive and iterative Fibonacci.
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
/* Native C Fibonacci                                                          */
/* ========================================================================== */

static int64_t native_fib_iter(int64_t n)
{
    if (n <= 1) return n;
    int64_t a = 0, b = 1;
    for (int64_t i = 2; i <= n; i++) {
        int64_t tmp = a + b;
        a = b;
        b = tmp;
    }
    return b;
}

static int64_t native_fib_rec(int64_t n)
{
    if (n <= 1) return n;
    return native_fib_rec(n - 1) + native_fib_rec(n - 2);
}

static int64_t g_fib_result;

static void bench_native_fib_iter(void *arg)
{
    int64_t n = *(int64_t *)arg;
    g_fib_result = native_fib_iter(n);
}

static void bench_native_fib_rec(void *arg)
{
    int64_t n = *(int64_t *)arg;
    g_fib_result = native_fib_rec(n);
}

/* ========================================================================== */
/* Interpreter Fibonacci (iterative)                                            */
/* ========================================================================== */

/**
 * Iterative Fibonacci bytecode:
 *   local0 = n, local1 = a=0, local2 = b=1, local3 = i=2
 *   loop:
 *     if (i > n) goto end
 *     local4 = local1 + local2  (tmp = a + b)
 *     local1 = local2           (a = b)
 *     local2 = local4           (b = tmp)
 *     local3 = local3 + 1       (i = i + 1)
 *     goto loop
 *   end:
 *     return local2
 *
 * PC  0: load_local 3        ; i
 * PC  3: load_local 0        ; n
 * PC  6: icmp_gt
 * PC  7: if_true 43          ; → end at PC 43
 * PC 10: load_local 1        ; a
 * PC 13: load_local 2        ; b
 * PC 16: iadd                ; a + b
 * PC 17: store_local 4       ; tmp = a + b
 * PC 20: load_local 2        ; b
 * PC 23: store_local 1       ; a = b
 * PC 26: load_local 4        ; tmp
 * PC 29: store_local 2       ; b = tmp
 * PC 32: load_local 3        ; i
 * PC 35: load_const_int 0    ; 1
 * PC 38: iadd                ; i + 1
 * PC 39: store_local 3       ; i = i + 1
 * PC 42: goto 0              ; → loop start
 * PC 45: load_local 2        ; return b
 * PC 48: return_value
 *
 * Hmm, need a const pool for the constant 1. Let me recalculate more carefully:
 *
 * PC  0: load_local 3        (3) → PC 3
 * PC  3: load_local 0        (3) → PC 6
 * PC  6: icmp_gt             (1) → PC 7
 * PC  7: if_true → end       (3) → PC 10
 * PC 10: load_local 1        (3) → PC 13
 * PC 13: load_local 2        (3) → PC 16
 * PC 16: iadd                (1) → PC 17
 * PC 17: store_local 4       (3) → PC 20
 * PC 20: load_local 2        (3) → PC 23
 * PC 23: store_local 1       (3) → PC 26
 * PC 26: load_local 4        (3) → PC 29
 * PC 29: store_local 2       (3) → PC 32
 * PC 32: load_local 3        (3) → PC 35
 * PC 35: load_const_int 1    (3) → PC 38    [const pool index 1 = value 1]
 * PC 38: iadd                (1) → PC 39
 * PC 39: store_local 3       (3) → PC 42
 * PC 42: goto 0              (3) → PC 45
 * PC 45: load_local 2        (3) → PC 48
 * PC 48: return_value        (1) → PC 49
 *
 * So if_true target = 45 (0x002D)
 * goto target = 0 (0x0000)
 */

static uint8_t fib_iter_code[] = {
    VT_OP_LOAD_LOCAL,     0x00, 0x03,  /* PC  0: load i */
    VT_OP_LOAD_LOCAL,     0x00, 0x00,  /* PC  3: load n */
    VT_OP_ICMP_GT,                      /* PC  6: i > n ? */
    VT_OP_IF_TRUE,        0x00, 0x2D,  /* PC  7: → PC 45 */
    VT_OP_LOAD_LOCAL,     0x00, 0x01,  /* PC 10: load a */
    VT_OP_LOAD_LOCAL,     0x00, 0x02,  /* PC 13: load b */
    VT_OP_IADD,                         /* PC 16: a + b */
    VT_OP_STORE_LOCAL,    0x00, 0x04,  /* PC 17: store tmp */
    VT_OP_LOAD_LOCAL,     0x00, 0x02,  /* PC 20: load b */
    VT_OP_STORE_LOCAL,    0x00, 0x01,  /* PC 23: a = b */
    VT_OP_LOAD_LOCAL,     0x00, 0x04,  /* PC 26: load tmp */
    VT_OP_STORE_LOCAL,    0x00, 0x02,  /* PC 29: b = tmp */
    VT_OP_LOAD_LOCAL,     0x00, 0x03,  /* PC 32: load i */
    VT_OP_LOAD_CONST_INT, 0x00, 0x01,  /* PC 35: load const 1 */
    VT_OP_IADD,                         /* PC 38: i + 1 */
    VT_OP_STORE_LOCAL,    0x00, 0x03,  /* PC 39: i = i+1 */
    VT_OP_GOTO,           0x00, 0x00,  /* PC 42: → PC 0 */
    VT_OP_LOAD_LOCAL,     0x00, 0x02,  /* PC 45: load b */
    VT_OP_RETURN_VALUE                  /* PC 48: return b */
};

static vtx_value_t fib_consts[2];

static void fib_consts_init(void)
{
    fib_consts[0] = vtx_make_smi(0);
    fib_consts[1] = vtx_make_smi(1);
}

#define FIB_N 30

typedef struct {
    vtx_interp_t      *interp;
    vtx_method_desc_t *method;
    vtx_value_t       *args;
    uint32_t           arg_count;
} fib_bench_arg_t;

static void bench_interp_fib_iter(void *arg)
{
    fib_bench_arg_t *ba = (fib_bench_arg_t *)arg;
    vtx_interp_run(ba->interp, ba->method, ba->args, ba->arg_count);
}

/* ========================================================================== */
/* Main                                                                         */
/* ========================================================================== */

int main(void)
{
    printf("=== VORTEX Fibonacci Macro-Benchmark ===\n\n");

    fib_consts_init();
    int64_t fib_n = FIB_N;

    /* --- Native iterative Fibonacci --- */
    printf("--- Native C ---\n");
    vtx_bench_result_t native_iter = vtx_bench_run("native fib_iter(30)",
                                                     bench_native_fib_iter, &fib_n);
    vtx_bench_report(&native_iter);

    /* Verify correctness */
    int64_t expected = native_fib_iter(FIB_N);
    printf("  Expected fib(%d) = %ld\n\n", FIB_N, (long)expected);

    /* --- Native recursive Fibonacci (smaller N to avoid long runtime) --- */
    int64_t fib_n_rec = 20;
    vtx_bench_result_t native_rec = vtx_bench_run("native fib_rec(20)",
                                                    bench_native_fib_rec, &fib_n_rec);
    vtx_bench_report(&native_rec);

    /* --- Interpreter T0 iterative Fibonacci --- */
    printf("\n--- Interpreter T0 ---\n");

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts);

    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    vtx_bytecode_t fib_bc = {
        .code = fib_iter_code,
        .length = sizeof(fib_iter_code),
        .constant_pool = fib_consts,
        .constant_count = 2,
        .max_locals = 5,
        .max_stack = 4
    };

    vtx_method_desc_t fib_method = {
        .name = "fib_iter", .signature = "(IIIII)I",
        .bytecode = &fib_bc,
        .vtable_index = 0xFFFFFFFF, .is_virtual = false
    };

    /* Args: n=FIB_N, a=0, b=1, i=2, tmp=0 */
    vtx_value_t fib_args[] = {
        vtx_make_smi(FIB_N),   /* local 0: n */
        vtx_make_smi(0),       /* local 1: a */
        vtx_make_smi(1),       /* local 2: b */
        vtx_make_smi(2),       /* local 3: i */
        vtx_make_smi(0)        /* local 4: tmp */
    };

    fib_bench_arg_t fib_ba = { &interp, &fib_method, fib_args, 5 };
    vtx_bench_result_t t0_iter = vtx_bench_run("T0 fib_iter(30)",
                                                 bench_interp_fib_iter, &fib_ba);
    vtx_bench_report(&t0_iter);

    /* Verify T0 result */
    vtx_value_t result = vtx_interp_run(&interp, &fib_method, fib_args, 5);
    if (vtx_is_smi(result)) {
        printf("  T0 fib(%d) = %ld  %s\n", FIB_N, (long)vtx_smi_value(result),
               vtx_smi_value(result) == expected ? "✓ CORRECT" : "✗ WRONG");
    }

    /* --- Compare --- */
    vtx_bench_compare("T0 fib_iter", &t0_iter, "native fib_iter", &native_iter);

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);

    (void)g_fib_result;
    return 0;
}
