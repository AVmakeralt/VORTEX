/**
 * VORTEX JIT Compiler — Standalone Benchmark
 * 
 * Tests the performance of the core runtime + interpreter
 * against native C for comparison.
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>

#include "vortex_config.h"
#include "runtime/arena.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/helpers.h"
#include "interp/frame.h"
#include "interp/profiler.h"
#include "interp/lookup.h"
#include "interp/type_feedback.h"
#include "interp/dispatch.h"

/* ========================================================================== */
/* Timing utilities                                                            */
/* ========================================================================== */

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

/* ========================================================================== */
/* Native C benchmarks (baseline for comparison)                               */
/* ========================================================================== */

static int64_t native_fib_iterative(int64_t n)
{
    int64_t a = 0, b = 1;
    while (n > 0) {
        int64_t temp = a + b;
        a = b;
        b = temp;
        n--;
    }
    return a;
}

static int64_t native_fib_recursive(int64_t n)
{
    if (n < 2) return n;
    return native_fib_recursive(n - 1) + native_fib_recursive(n - 2);
}

static void native_sort(int64_t *arr, int64_t n)
{
    /* Insertion sort */
    for (int64_t i = 1; i < n; i++) {
        int64_t key = arr[i];
        int64_t j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

static int64_t native_sum_array(int64_t *arr, int64_t n)
{
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) {
        sum += arr[i];
    }
    return sum;
}

static int64_t native_nested_loop(int64_t n)
{
    int64_t count = 0;
    for (int64_t i = 0; i < n; i++) {
        for (int64_t j = 0; j < n; j++) {
            count++;
        }
    }
    return count;
}

/* ========================================================================== */
/* Bytecode program builders                                                   */
/* ========================================================================== */

/*
 * Constant pool layout (shared across all programs):
 *   [0] = SMI 0
 *   [1] = SMI 1
 *   [2] = SMI 2
 */
#define CP_ZERO 0
#define CP_ONE  1
#define CP_TWO  2
#define CP_SIZE 3

static vtx_value_t *build_constant_pool(vtx_arena_t *arena)
{
    vtx_value_t *cp = vtx_arena_alloc(arena, CP_SIZE * sizeof(vtx_value_t));
    cp[CP_ZERO] = vtx_make_smi(0);
    cp[CP_ONE]  = vtx_make_smi(1);
    cp[CP_TWO]  = vtx_make_smi(2);
    return cp;
}

/**
 * Build iterative fibonacci bytecode.
 * Locals: [n, a, b, temp]
 * a = 0, b = 1, while n > 0: temp=a+b, a=b, b=temp, n--
 * return a
 */
static vtx_bytecode_t *build_fib_bytecode(vtx_arena_t *arena)
{
    size_t cap = 256;
    uint8_t *buf = vtx_arena_alloc(arena, cap);
    size_t pos = 0;

    #define EMIT_OP(op) do { buf[pos++] = (op); } while(0)
    #define EMIT_U16(v) do { buf[pos++] = (uint8_t)((v) >> 8); buf[pos++] = (uint8_t)((v) & 0xFF); } while(0)

    /* a = 0, b = 1 */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ZERO);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(1);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ONE);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(2);

    /* Lloop: */
    size_t loop_start = pos;
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(0);   /* n */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ZERO);
    EMIT_OP(VT_OP_ICMP_GT);
    EMIT_OP(VT_OP_IF_FALSE);
    size_t if_false_patch = pos;
    EMIT_U16(0); /* placeholder */

    /* temp = a + b */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(1);
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(2);
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(3);

    /* a = b */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(2);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(1);

    /* b = temp */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(3);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(2);

    /* n-- */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(0);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ONE);
    EMIT_OP(VT_OP_ISUB);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(0);

    /* goto Lloop — operand is absolute PC */
    EMIT_OP(VT_OP_GOTO);
    EMIT_U16((uint16_t)loop_start);

    /* Lend: return a */
    size_t loop_end = pos;
    buf[if_false_patch] = (uint8_t)((loop_end) >> 8);
    buf[if_false_patch + 1] = (uint8_t)((loop_end) & 0xFF);

    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(1);
    EMIT_OP(VT_OP_RETURN_VALUE);

    #undef EMIT_OP
    #undef EMIT_U16

    vtx_bytecode_t *bc = vtx_arena_alloc(arena, sizeof(vtx_bytecode_t));
    bc->code = buf;
    bc->length = (uint32_t)pos;
    bc->constant_pool = build_constant_pool(arena);
    bc->constant_count = CP_SIZE;
    bc->max_locals = 4; bc->max_stack = 4;
    return bc;
}

/**
 * Build a sum-to-n bytecode.
 * locals: [n, sum, i]
 * sum = 0; i = 0; while i < n: sum = sum + i; i++
 * return sum
 */
static vtx_bytecode_t *build_sum_bytecode(vtx_arena_t *arena)
{
    size_t cap = 256;
    uint8_t *buf = vtx_arena_alloc(arena, cap);
    size_t pos = 0;

    #define EMIT_OP(op) do { buf[pos++] = (op); } while(0)
    #define EMIT_U16(v) do { buf[pos++] = (uint8_t)((v) >> 8); buf[pos++] = (uint8_t)((v) & 0xFF); } while(0)

    /* sum = 0 */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ZERO);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(1);
    /* i = 0 */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ZERO);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(2);

    /* Lloop: i < n? */
    size_t loop_start = pos;
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(2);   /* i */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(0);   /* n */
    EMIT_OP(VT_OP_ICMP_LT);
    EMIT_OP(VT_OP_IF_FALSE);
    size_t if_false_patch = pos;
    EMIT_U16(0);

    /* sum = sum + i */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(1);   /* sum */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(2);   /* i */
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(1);

    /* i++ */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(2);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ONE);
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(2);

    /* goto Lloop — absolute PC */
    EMIT_OP(VT_OP_GOTO);
    EMIT_U16((uint16_t)loop_start);

    /* Lend: return sum */
    size_t loop_end = pos;
    buf[if_false_patch] = (uint8_t)((loop_end) >> 8);
    buf[if_false_patch + 1] = (uint8_t)((loop_end) & 0xFF);

    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(1);
    EMIT_OP(VT_OP_RETURN_VALUE);

    #undef EMIT_OP
    #undef EMIT_U16

    vtx_bytecode_t *bc = vtx_arena_alloc(arena, sizeof(vtx_bytecode_t));
    bc->code = buf;
    bc->length = (uint32_t)pos;
    bc->constant_pool = build_constant_pool(arena);
    bc->constant_count = CP_SIZE;
    bc->max_locals = 4; bc->max_stack = 4;
    return bc;
}

/**
 * Build nested loop bytecode.
 * locals: [n, count, i, j]
 * count = 0; i = 0; while i < n: j = 0; while j < n: count++; j++; i++
 * return count
 */
static vtx_bytecode_t *build_nested_loop_bytecode(vtx_arena_t *arena)
{
    size_t cap = 512;
    uint8_t *buf = vtx_arena_alloc(arena, cap);
    size_t pos = 0;

    #define EMIT_OP(op) do { buf[pos++] = (op); } while(0)
    #define EMIT_U16(v) do { buf[pos++] = (uint8_t)((v) >> 8); buf[pos++] = (uint8_t)((v) & 0xFF); } while(0)

    /* count = 0, i = 0 */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ZERO);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(1);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ZERO);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(2);

    /* Outer loop: i < n? */
    size_t outer_start = pos;
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(2);   /* i */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(0);   /* n */
    EMIT_OP(VT_OP_ICMP_LT);
    EMIT_OP(VT_OP_IF_FALSE);
    size_t outer_exit_patch = pos;
    EMIT_U16(0);

    /* j = 0 */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ZERO);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(3);

    /* Inner loop: j < n? */
    size_t inner_start = pos;
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(3);   /* j */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(0);   /* n */
    EMIT_OP(VT_OP_ICMP_LT);
    EMIT_OP(VT_OP_IF_FALSE);
    size_t inner_exit_patch = pos;
    EMIT_U16(0);

    /* count++ */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(1);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ONE);
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(1);

    /* j++ */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(3);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ONE);
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(3);

    /* goto inner_start — absolute PC */
    EMIT_OP(VT_OP_GOTO);
    EMIT_U16((uint16_t)inner_start);

    /* Inner loop exit */
    size_t inner_end = pos;
    buf[inner_exit_patch] = (uint8_t)((inner_end) >> 8);
    buf[inner_exit_patch + 1] = (uint8_t)((inner_end) & 0xFF);

    /* i++ */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(2);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ONE);
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(2);

    /* goto outer_start — absolute PC */
    EMIT_OP(VT_OP_GOTO);
    EMIT_U16((uint16_t)outer_start);

    /* Outer loop exit: return count */
    size_t outer_end = pos;
    buf[outer_exit_patch] = (uint8_t)((outer_end) >> 8);
    buf[outer_exit_patch + 1] = (uint8_t)((outer_end) & 0xFF);

    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(1);
    EMIT_OP(VT_OP_RETURN_VALUE);

    #undef EMIT_OP
    #undef EMIT_U16

    vtx_bytecode_t *bc = vtx_arena_alloc(arena, sizeof(vtx_bytecode_t));
    bc->code = buf;
    bc->length = (uint32_t)pos;
    bc->constant_pool = build_constant_pool(arena);
    bc->constant_count = CP_SIZE;
    bc->max_locals = 4; bc->max_stack = 4;
    return bc;
}

/* ========================================================================== */
/* Benchmark runner                                                            */
/* ========================================================================== */

typedef struct {
    const char *name;
    double min_ns;
    double median_ns;
    double p95_ns;
    double mean_ns;
    int64_t iterations;
} bench_result_t;

static int compare_doubles(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static bench_result_t run_benchmark(const char *name, int64_t iters,
                                     void (*fn)(void *), void *ctx)
{
    bench_result_t r;
    r.name = name;
    r.iterations = iters;

    /* Warmup */
    for (int i = 0; i < 3; i++) fn(ctx);

    /* Collect samples */
    int num_samples = 50;
    double *samples = malloc(num_samples * sizeof(double));

    for (int i = 0; i < num_samples; i++) {
        double t0 = now_ns();
        for (int64_t j = 0; j < iters; j++) fn(ctx);
        double t1 = now_ns();
        samples[i] = (t1 - t0) / iters;
    }

    qsort(samples, num_samples, sizeof(double), compare_doubles);
    r.min_ns = samples[0];
    r.median_ns = samples[num_samples / 2];
    r.p95_ns = samples[(int)(num_samples * 0.95)];
    r.mean_ns = 0;
    for (int i = 0; i < num_samples; i++) r.mean_ns += samples[i];
    r.mean_ns /= num_samples;

    free(samples);
    return r;
}

static void print_result(bench_result_t r)
{
    printf("  %-30s  %8.0f ns  %8.0f ns  %8.0f ns\n",
           r.name, r.median_ns, r.p95_ns, r.min_ns);
}

/* ========================================================================== */
/* Benchmark contexts and functions                                            */
/* ========================================================================== */

typedef struct {
    vtx_interp_t *interp;
    vtx_method_desc_t *method;
    int64_t n;
} interp_bench_ctx_t;

static void bench_interp_fib(void *ctx)
{
    interp_bench_ctx_t *c = (interp_bench_ctx_t *)ctx;
    vtx_value_t arg = vtx_make_smi(c->n);
    vtx_interp_run(c->interp, c->method, &arg, 1);
}

typedef struct {
    int64_t n;
    int64_t result;
} native_bench_ctx_t;

static void bench_native_fib_iter(void *ctx)
{
    native_bench_ctx_t *c = (native_bench_ctx_t *)ctx;
    c->result = native_fib_iterative(c->n);
}

static void bench_native_fib_recur(void *ctx)
{
    native_bench_ctx_t *c = (native_bench_ctx_t *)ctx;
    c->result = native_fib_recursive(c->n);
}

static void bench_interp_sum(void *ctx)
{
    interp_bench_ctx_t *c = (interp_bench_ctx_t *)ctx;
    vtx_value_t arg = vtx_make_smi(c->n);
    vtx_interp_run(c->interp, c->method, &arg, 1);
}

static void bench_native_sum(void *ctx)
{
    native_bench_ctx_t *c = (native_bench_ctx_t *)ctx;
    int64_t sum = 0;
    for (int64_t i = 0; i < c->n; i++) sum += i;
    c->result = sum;
}

static void bench_interp_nested(void *ctx)
{
    interp_bench_ctx_t *c = (interp_bench_ctx_t *)ctx;
    vtx_value_t arg = vtx_make_smi(c->n);
    vtx_interp_run(c->interp, c->method, &arg, 1);
}

static void bench_native_nested(void *ctx)
{
    native_bench_ctx_t *c = (native_bench_ctx_t *)ctx;
    c->result = native_nested_loop(c->n);
}

/* Tagged value micro-benchmarks */
static void bench_smi_create(void *ctx)
{
    (void)ctx;
    volatile vtx_value_t v;
    for (int i = 0; i < 1000; i++) {
        v = vtx_make_smi(i);
    }
    (void)v;
}

static void bench_smi_add(void *ctx)
{
    (void)ctx;
    vtx_value_t a = vtx_make_smi(42);
    vtx_value_t b = vtx_make_smi(58);
    volatile vtx_value_t result;
    for (int i = 0; i < 1000; i++) {
        int64_t va = vtx_smi_value(a);
        int64_t vb = vtx_smi_value(b);
        result = vtx_make_smi(va + vb);
    }
    (void)result;
}

static void bench_double_create(void *ctx)
{
    (void)ctx;
    volatile vtx_value_t v;
    for (int i = 0; i < 1000; i++) {
        v = vtx_make_double((double)i * 0.5);
    }
    (void)v;
}

static void bench_double_add(void *ctx)
{
    (void)ctx;
    vtx_value_t a = vtx_make_double(3.14);
    vtx_value_t b = vtx_make_double(2.72);
    volatile vtx_value_t result;
    for (int i = 0; i < 1000; i++) {
        double va = vtx_double_value(a);
        double vb = vtx_double_value(b);
        result = vtx_make_double(va + vb);
    }
    (void)result;
}

static void bench_native_int_add(void *ctx)
{
    (void)ctx;
    volatile int64_t result;
    for (int i = 0; i < 1000; i++) {
        result = 42 + 58;
    }
    (void)result;
}

static void bench_native_double_add(void *ctx)
{
    (void)ctx;
    volatile double result;
    for (int i = 0; i < 1000; i++) {
        result = 3.14 + 2.72;
    }
    (void)result;
}

/* ========================================================================== */
/* Main benchmark entry                                                        */
/* ========================================================================== */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║            VORTEX JIT Compiler — Performance Report        ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    /* Initialize runtime */
    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    /* Build bytecode programs */
    vtx_bytecode_t *fib_bc = build_fib_bytecode(&arena);
    vtx_method_desc_t fib_method = {
        .name = "fib", .signature = "(I)I",
        .bytecode = fib_bc, .vtable_index = 0, .is_virtual = false
    };

    vtx_bytecode_t *sum_bc = build_sum_bytecode(&arena);
    vtx_method_desc_t sum_method = {
        .name = "sum", .signature = "(I)I",
        .bytecode = sum_bc, .vtable_index = 0, .is_virtual = false
    };

    vtx_bytecode_t *nested_bc = build_nested_loop_bytecode(&arena);
    vtx_method_desc_t nested_method = {
        .name = "nested", .signature = "(I)I",
        .bytecode = nested_bc, .vtable_index = 0, .is_virtual = false
    };

    /* ================================================================ */
    /* Correctness check first */
    /* ================================================================ */
    printf("── Correctness Verification ──────────────────────────────────\n\n");

    vtx_value_t arg10 = vtx_make_smi(10);
    vtx_value_t fib10 = vtx_interp_run(&interp, &fib_method, &arg10, 1);
    int64_t native_fib10 = native_fib_iterative(10);
    bool fib_ok = vtx_is_smi(fib10) && vtx_smi_value(fib10) == native_fib10;
    printf("  fib(10): interp=%lld, native=%lld  %s\n",
           vtx_is_smi(fib10) ? (long long)vtx_smi_value(fib10) : -1,
           (long long)native_fib10,
           fib_ok ? "✓ PASS" : "✗ FAIL");

    vtx_value_t arg100 = vtx_make_smi(100);
    vtx_value_t sum100 = vtx_interp_run(&interp, &sum_method, &arg100, 1);
    int64_t native_sum100 = 0;
    for (int64_t i = 0; i < 100; i++) native_sum100 += i;
    bool sum_ok = vtx_is_smi(sum100) && vtx_smi_value(sum100) == native_sum100;
    printf("  sum(100): interp=%lld, native=%lld  %s\n",
           vtx_is_smi(sum100) ? (long long)vtx_smi_value(sum100) : -1,
           (long long)native_sum100,
           sum_ok ? "✓ PASS" : "✗ FAIL");

    vtx_value_t arg50 = vtx_make_smi(50);
    vtx_value_t nested50 = vtx_interp_run(&interp, &nested_method, &arg50, 1);
    int64_t native_nested50 = native_nested_loop(50);
    bool nested_ok = vtx_is_smi(nested50) && vtx_smi_value(nested50) == native_nested50;
    printf("  nested(50): interp=%lld, native=%lld  %s\n\n",
           vtx_is_smi(nested50) ? (long long)vtx_smi_value(nested50) : -1,
           (long long)native_nested50,
           nested_ok ? "✓ PASS" : "✗ FAIL");

    /* ================================================================ */
    /* Micro-benchmarks: Tagged value overhead */
    /* ================================================================ */
    printf("── Micro-Benchmarks: Tagged Value Overhead ──────────────────\n\n");
    printf("  %-30s  %9s  %9s  %9s\n", "Benchmark", "Median", "P95", "Min");
    printf("  %-30s  %9s  %9s  %9s\n", "──────────────────────────────", "─────────", "─────────", "─────────");

    bench_result_t r;
    int64_t micro_iters = 100000;

    r = run_benchmark("SMI create (×1000)", micro_iters, bench_smi_create, NULL);
    print_result(r);

    r = run_benchmark("SMI add (×1000)", micro_iters, bench_smi_add, NULL);
    print_result(r);

    r = run_benchmark("native int add (×1000)", micro_iters, bench_native_int_add, NULL);
    print_result(r);

    r = run_benchmark("Double create (×1000)", micro_iters, bench_double_create, NULL);
    print_result(r);

    r = run_benchmark("Double add (×1000)", micro_iters, bench_double_add, NULL);
    print_result(r);

    r = run_benchmark("native double add (×1000)", micro_iters, bench_native_double_add, NULL);
    print_result(r);

    /* ================================================================ */
    /* Macro-benchmarks: Interpreter vs Native */
    /* ================================================================ */
    printf("\n── Macro-Benchmarks: Interpreter (T0) vs Native C ───────────\n\n");
    printf("  %-30s  %9s  %9s  %9s\n", "Benchmark", "Median", "P95", "Min");
    printf("  %-30s  %9s  %9s  %9s\n", "──────────────────────────────", "─────────", "─────────", "─────────");

    /* Fibonacci iterative */
    interp_bench_ctx_t fib_ctx = { &interp, &fib_method, 20 };
    native_bench_ctx_t nfib_ctx = { 20, 0 };
    r = run_benchmark("fib(20) — T0 interpreter", 1000, bench_interp_fib, &fib_ctx);
    print_result(r);
    double interp_fib20 = r.median_ns;

    r = run_benchmark("fib(20) — native C", 100000, bench_native_fib_iter, &nfib_ctx);
    print_result(r);
    double native_fib20 = r.median_ns;

    /* Fibonacci larger */
    fib_ctx.n = 30;
    nfib_ctx.n = 30;
    r = run_benchmark("fib(30) — T0 interpreter", 100, bench_interp_fib, &fib_ctx);
    print_result(r);

    r = run_benchmark("fib(30) — native C", 10000, bench_native_fib_iter, &nfib_ctx);
    print_result(r);

    /* Fibonacci recursive (native only, too slow for interpreter) */
    nfib_ctx.n = 30;
    r = run_benchmark("fib(30) — native recursive", 1000, bench_native_fib_recur, &nfib_ctx);
    print_result(r);

    /* Sum loop */
    interp_bench_ctx_t sum_ctx = { &interp, &sum_method, 1000 };
    native_bench_ctx_t nsum_ctx = { 1000, 0 };
    r = run_benchmark("sum(1000) — T0 interpreter", 1000, bench_interp_sum, &sum_ctx);
    print_result(r);
    double interp_sum1000 = r.median_ns;

    r = run_benchmark("sum(1000) — native C", 100000, bench_native_sum, &nsum_ctx);
    print_result(r);
    double native_sum1000 = r.median_ns;

    /* Sum loop larger */
    sum_ctx.n = 10000;
    nsum_ctx.n = 10000;
    r = run_benchmark("sum(10000) — T0 interpreter", 100, bench_interp_sum, &sum_ctx);
    print_result(r);

    r = run_benchmark("sum(10000) — native C", 100000, bench_native_sum, &nsum_ctx);
    print_result(r);

    /* Nested loop */
    interp_bench_ctx_t nest_ctx = { &interp, &nested_method, 100 };
    native_bench_ctx_t nnest_ctx = { 100, 0 };
    r = run_benchmark("nested(100) — T0 interpreter", 100, bench_interp_nested, &nest_ctx);
    print_result(r);
    double interp_nest100 = r.median_ns;

    r = run_benchmark("nested(100) — native C", 10000, bench_native_nested, &nnest_ctx);
    print_result(r);
    double native_nest100 = r.median_ns;

    /* ================================================================ */
    /* Summary */
    /* ================================================================ */
    printf("\n── Performance Summary ───────────────────────────────────────\n\n");

    printf("  ┌─────────────────────┬──────────────┬──────────────┬───────────┐\n");
    printf("  │ Benchmark           │ T0 (interp)  │ Native C     │ Slowdown  │\n");
    printf("  ├─────────────────────┼──────────────┼──────────────┼───────────┤\n");

    if (native_fib20 > 0)
        printf("  │ fib(20)             │ %8.0f ns  │ %8.0f ns  │ %7.1fx   │\n",
               interp_fib20, native_fib20, interp_fib20 / native_fib20);
    if (native_sum1000 > 0)
        printf("  │ sum(1000)           │ %8.0f ns  │ %8.0f ns  │ %7.1fx   │\n",
               interp_sum1000, native_sum1000, interp_sum1000 / native_sum1000);
    if (native_nest100 > 0)
        printf("  │ nested(100)         │ %8.0f ns  │ %8.0f ns  │ %7.1fx   │\n",
               interp_nest100, native_nest100, interp_nest100 / native_nest100);

    printf("  └─────────────────────┴──────────────┴──────────────┴───────────┘\n\n");

    printf("  T0 interpreter overhead is expected (dispatch + tagged values + profiling).\n");
    printf("  T1 baseline JIT target: 2x+ faster than T0.\n");
    printf("  T2 optimizing JIT target: 5x+ faster than T0 (with PEA + inlining).\n");
    printf("  T3 speculative JIT target: 10x+ faster than T0 (with SIMD + adaptive guards).\n\n");

    /* Cleanup */
    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);

    return 0;
}
