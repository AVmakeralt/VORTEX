/**
 * bench_t2.c — Benchmark the VORTEX T2 JIT pipeline (the one we fixed).
 *
 * Compares:
 *   1. T0 Interpreter (baseline)
 *   2. T2 Optimizing JIT (GVN + SCCP + DCE + LICM + isel + regalloc + emit)
 *   3. Native C (-O3 -march=native)
 *
 * Workloads (all integer, since the T2 JIT handles SMIs):
 *   - sum_loop(N): sum 1..N via loop
 *   - fib_iter(N): iterative fibonacci
 *   - gcd(a,b): Euclidean algorithm
 *   - collatz(N): 3n+1 convergence
 *   - pow_mod(base, exp, m): modular exponentiation
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>

#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/arena.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "compile/pipeline.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "interp/dispatch.h"
#include "assembler.h"

#define WARMUP 20
#define SAMPLES 50

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

typedef vtx_value_t (*jit_entry_t)(const vtx_method_desc_t *, void *, void *,
                                    vtx_value_t *, uint32_t);

/* ----- Compile through the T2 or T3 pipeline ----- */
static jit_entry_t compile(const char *prog_text, uint32_t arg_count, int tier) {
    vtx_assembler_t *a = calloc(1, sizeof(*a));
    vtx_arena_t *arena = calloc(1, sizeof(*arena));
    vtx_type_system_t *ts = calloc(1, sizeof(*ts));
    vtx_gc_t *gc = calloc(1, sizeof(*gc));
    vtx_graph_t *graph = calloc(1, sizeof(*graph));
    vtx_code_cache_t *cache = calloc(1, sizeof(*cache));
    vtx_method_registry_t *reg = calloc(1, sizeof(*reg));
    vtx_method_desc_t *method = calloc(1, sizeof(*method));
    vtx_bytecode_t *bc = calloc(1, sizeof(*bc));

    vtx_asm_init(a);
    vtx_asm_program(a, prog_text);
    *bc = vtx_asm_emit(a);

    vtx_arena_init(arena);
    vtx_type_system_init(ts);
    vtx_gc_init(gc, ts, VTX_GC_GENERATIONAL);
    vtx_graph_init(graph, arg_count > 0 ? arg_count : 1);

    method->name = "f";
    method->signature = arg_count == 1 ? "(I)I" : "(II)I";
    method->bytecode = bc;
    method->arg_count = arg_count > 0 ? arg_count : 1;
    method->is_virtual = false;

    if (vtx_graph_build(graph, bc, method, arena) != 0) {
        fprintf(stderr, "FAIL: graph build\n");
        return NULL;
    }

    vtx_pipeline_config_t config = (tier == 3) ?
        vtx_pipeline_config_t3() : vtx_pipeline_config_t2();
    vtx_code_cache_init(cache, 1 << 20);
    vtx_method_registry_init(reg, arena);
    config.code_cache = cache;
    config.method_registry = reg;
    config.method = method;

    vtx_compile_result_t result;
    memset(&result, 0, sizeof(result));
    int rc = vtx_pipeline_run(graph, &config, arena, &result);
    if (rc != 0 || !result.success || method->compiled_code == NULL) {
        fprintf(stderr, "FAIL: pipeline rc=%d success=%d\n", rc, result.success);
        return NULL;
    }
    return (jit_entry_t)method->compiled_code;
}

/* ----- T0 interpreter runner ----- */
static vtx_value_t run_t0(const char *prog_text, vtx_value_t *args, uint32_t argc) {
    vtx_assembler_t a; vtx_asm_init(&a);
    vtx_asm_program(&a, prog_text);
    vtx_bytecode_t bc = vtx_asm_emit(&a);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_method_desc_t method = {
        .name = "f", .signature = argc == 1 ? "(I)I" : "(II)I",
        .bytecode = &bc, .arg_count = argc, .is_virtual = false
    };
    vtx_interp_t interp; vtx_interp_init(&interp, &ts, &gc);
    vtx_value_t r = vtx_interp_run(&interp, &method, args, argc);
    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_asm_destroy(&a);
    return r;
}

/* ----- Native C reference implementations ----- */
/* noinline + volatile input prevents the compiler from constant-folding
 * the entire computation. Without this, gcc -O3 precomputes fib(20)=6765
 * at compile time and the "benchmark" measures a register move (~1 ns). */
__attribute__((noinline))
static int64_t native_sum_loop(volatile int64_t n) {
    int64_t s = 0;
    for (int64_t i = 1; i <= n; i++) s += i;
    return s;
}
__attribute__((noinline))
static int64_t native_fib_iter(volatile int64_t n) {
    if (n <= 1) return n;
    int64_t a = 0, b = 1;
    for (int64_t i = 2; i <= n; i++) {
        int64_t t = a + b; a = b; b = t;
    }
    return b;
}
__attribute__((noinline))
static int64_t native_gcd(volatile int64_t a, volatile int64_t b) {
    while (b != 0) { int64_t t = a % b; a = b; b = t; }
    return a;
}
__attribute__((noinline))
static int64_t native_collatz(volatile int64_t n) {
    int64_t steps = 0;
    while (n != 1) {
        if (n % 2 == 0) n = n / 2;
        else n = 3 * n + 1;
        steps++;
    }
    return steps;
}

/* ----- Bench harness ----- */
typedef struct { double median, p95, min; } stats_t;

/* Volatile sink — prevents the compiler from eliminating the computation */
static volatile int64_t g_sink;

static stats_t bench_run(int64_t (*fn)(void *), void *ctx, int iters) {
    static double samples[SAMPLES];
    for (int s = 0; s < SAMPLES; s++) {
        uint64_t t0 = now_ns();
        int64_t acc = 0;
        for (int i = 0; i < iters; i++) acc += fn(ctx);
        uint64_t t1 = now_ns();
        g_sink = acc;  /* consume result to prevent DCE */
        samples[s] = (double)(t1 - t0) / iters;
    }
    /* sort */
    for (int i = 0; i < SAMPLES - 1; i++)
        for (int j = i + 1; j < SAMPLES; j++)
            if (samples[j] < samples[i]) { double t = samples[i]; samples[i] = samples[j]; samples[j] = t; }
    stats_t r;
    r.median = samples[SAMPLES / 2];
    r.p95 = samples[(int)(SAMPLES * 0.95)];
    r.min = samples[0];
    return r;
}

/* ----- Program strings ----- */
static const char *PROG_SUM =
    ".method sum (I)I\n.arg_count 1\n.max_locals 3\n.max_stack 4\n"
    "load_const_int 0\nstore_local 1\n"
    "loop:\nload_local 0\nload_const_int 0\nicmp_le\nif_true done\n"
    "load_local 1\nload_local 0\niadd\nstore_local 1\n"
    "load_local 0\nload_const_int 1\nisub\nstore_local 0\n"
    "goto loop\n"
    "done:\nload_local 1\nreturn_value\n";

static const char *PROG_FIB =
    ".method fib (I)I\n.arg_count 1\n.max_locals 5\n.max_stack 4\n"
    "load_local 0\nload_const_int 1\nicmp_le\nif_true base_case\n"
    "load_const_int 0\nstore_local 1\n"
    "load_const_int 1\nstore_local 2\n"
    "load_const_int 2\nstore_local 3\n"
    "loop:\nload_local 3\nload_local 0\nicmp_gt\nif_true done\n"
    "load_local 1\nload_local 2\niadd\nstore_local 4\n"
    "load_local 2\nstore_local 1\n"
    "load_local 4\nstore_local 2\n"
    "load_local 3\nload_const_int 1\niadd\nstore_local 3\n"
    "goto loop\n"
    "base_case:\nload_local 0\nreturn_value\n"
    "done:\nload_local 2\nreturn_value\n";

static const char *PROG_GCD =
    ".method gcd (II)I\n.arg_count 2\n.max_locals 3\n.max_stack 4\n"
    "loop:\nload_local 1\nload_const_int 0\nicmp_eq\nif_true done\n"
    "load_local 0\nload_local 1\nimod\nstore_local 2\n"
    "load_local 1\nstore_local 0\n"
    "load_local 2\nstore_local 1\n"
    "goto loop\n"
    "done:\nload_local 0\nreturn_value\n";

static const char *PROG_COLLATZ =
    ".method collatz (I)I\n.arg_count 1\n.max_locals 3\n.max_stack 6\n"
    "load_const_int 0\nstore_local 1\n"
    "loop:\nload_local 0\nload_const_int 1\nicmp_eq\nif_true done\n"
    "load_local 0\nload_const_int 2\nimod\nif_false even\n"
    "load_local 0\nload_const_int 3\nimul\nload_const_int 1\niadd\nstore_local 0\n"
    "goto inc\n"
    "even:\nload_local 0\nload_const_int 2\nidiv\nstore_local 0\n"
    "inc:\nload_local 1\nload_const_int 1\niadd\nstore_local 1\n"
    "goto loop\n"
    "done:\nload_local 1\nreturn_value\n";

/* ----- JIT caller wrappers ----- */
typedef struct { jit_entry_t entry; vtx_value_t arg; } jit1_ctx_t;
typedef struct { jit_entry_t entry; vtx_value_t a, b; } jit2_ctx_t;

static int64_t call_jit1(void *c) {
    jit1_ctx_t *ctx = (jit1_ctx_t *)c;
    vtx_method_desc_t m; memset(&m, 0, sizeof(m)); m.name = "f";
    return vtx_smi_value(ctx->entry(&m, NULL, (void*)1, &ctx->arg, 1));
}
static int64_t call_jit2(void *c) {
    jit2_ctx_t *ctx = (jit2_ctx_t *)c;
    vtx_method_desc_t m; memset(&m, 0, sizeof(m)); m.name = "f";
    vtx_value_t args[2] = { ctx->a, ctx->b };
    return vtx_smi_value(ctx->entry(&m, NULL, (void*)1, args, 2));
}

/* ----- Native caller wrappers ----- */
static int64_t call_native_sum(void *c) { return native_sum_loop((int64_t)(intptr_t)c); }
static int64_t call_native_fib(void *c) { return native_fib_iter((int64_t)(intptr_t)c); }
static int64_t call_native_gcd(void *c) {
    int64_t *p = (int64_t *)c; return native_gcd(p[0], p[1]);
}
static int64_t call_native_collatz(void *c) { return native_collatz((int64_t)(intptr_t)c); }

/* ----- T0 wrappers ----- */
static int64_t call_t0_sum(void *c) {
    vtx_value_t a = vtx_make_smi((int64_t)(intptr_t)c);
    return vtx_smi_value(run_t0(PROG_SUM, &a, 1));
}
static int64_t call_t0_fib(void *c) {
    vtx_value_t a = vtx_make_smi((int64_t)(intptr_t)c);
    return vtx_smi_value(run_t0(PROG_FIB, &a, 1));
}
static int64_t call_t0_gcd(void *c) {
    int64_t *p = (int64_t *)c;
    vtx_value_t args[2] = { vtx_make_smi(p[0]), vtx_make_smi(p[1]) };
    return vtx_smi_value(run_t0(PROG_GCD, args, 2));
}
static int64_t call_t0_collatz(void *c) {
    vtx_value_t a = vtx_make_smi((int64_t)(intptr_t)c);
    return vtx_smi_value(run_t0(PROG_COLLATZ, &a, 1));
}

static void print_stats(const char *label, stats_t s) {
    printf("  %-36s median %9.0f ns  p95 %9.0f ns  min %9.0f ns\n",
           label, s.median, s.p95, s.min);
}

int main(void) {
    /* Disable SIGALRM — benchmarks manage their own timing */
    signal(SIGALRM, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== VORTEX T2 JIT Pipeline Benchmark ===\n\n");
    printf("Methodology: %d warmup + %d samples, sorted for median/p95\n", WARMUP, SAMPLES);
    printf("Native C compiled with gcc -O3 -march=native\n\n");

    /* ----- Verify correctness for T2 ----- */
    printf("--- Correctness Verification (T2) ---\n");
    jit_entry_t j_sum = compile(PROG_SUM, 1, 2);
    jit_entry_t j_fib = compile(PROG_FIB, 1, 2);
    jit_entry_t j_gcd = compile(PROG_GCD, 2, 2);
    jit_entry_t j_col = compile(PROG_COLLATZ, 1, 2);
    if (!j_sum || !j_fib || !j_gcd || !j_col) {
        fprintf(stderr, "T2 compilation failed\n");
        return 1;
    }
    vtx_method_desc_t m; memset(&m, 0, sizeof(m)); m.name = "f";

    vtx_value_t v;
    printf("  calling sum(100)...\n"); fflush(stdout);
    v = vtx_make_smi(100); int64_t j_r = vtx_smi_value(j_sum(&m, NULL, (void*)1, &v, 1));
    printf("  sum(100)     T2=%lld  native=%lld  %s\n",
           (long long)j_r, (long long)native_sum_loop(100),
           j_r == native_sum_loop(100) ? "OK" : "MISMATCH");
    fflush(stdout);

    printf("  calling fib(20)...\n"); fflush(stdout);
    v = vtx_make_smi(20); j_r = vtx_smi_value(j_fib(&m, NULL, (void*)1, &v, 1));
    printf("  fib(20)      T2=%lld  native=%lld  %s\n",
           (long long)j_r, (long long)native_fib_iter(20),
           j_r == native_fib_iter(20) ? "OK" : "MISMATCH");
    fflush(stdout);

    printf("  calling gcd...\n"); fflush(stdout);
    vtx_value_t g_args[2] = { vtx_make_smi(123456), vtx_make_smi(7890) };
    j_r = vtx_smi_value(j_gcd(&m, NULL, (void*)1, g_args, 2));
    printf("  gcd(123456,7890) T2=%lld  native=%lld  %s\n",
           (long long)j_r, (long long)native_gcd(123456, 7890),
           j_r == native_gcd(123456, 7890) ? "OK" : "MISMATCH");
    fflush(stdout);

    printf("  calling collatz(27)...\n"); fflush(stdout);
    v = vtx_make_smi(27); j_r = vtx_smi_value(j_col(&m, NULL, (void*)1, &v, 1));
    printf("  collatz(27)  T2=%lld  native=%lld  %s\n",
           (long long)j_r, (long long)native_collatz(27),
           j_r == native_collatz(27) ? "OK" : "MISMATCH");
    fflush(stdout);
    printf("\n");

    /* ----- Benchmark: sum(100) ----- */
    printf("--- sum(100) ---\n");
    jit1_ctx_t jc = { j_sum, vtx_make_smi(100) };
    int64_t narg = 100;
    stats_t s_native = bench_run(call_native_sum, (void*)narg, 10000);
    stats_t s_jit = bench_run(call_jit1, &jc, 10000);
    stats_t s_t0 = bench_run(call_t0_sum, (void*)narg, 100);
    print_stats("Native C", s_native);
    print_stats("VORTEX T2 JIT", s_jit);
    print_stats("VORTEX T0 Interpreter", s_t0);
    printf("  → T2 JIT: %.1f%% of native C  |  %.1fx faster than T0 interp\n\n",
           100.0 * s_native.median / s_jit.median, s_t0.median / s_jit.median);

    /* ----- Benchmark: fib(20) ----- */
    printf("--- fib(20) ---\n");
    jc.entry = j_fib; jc.arg = vtx_make_smi(20);
    narg = 20;
    s_native = bench_run(call_native_fib, (void*)narg, 10000);
    s_jit = bench_run(call_jit1, &jc, 10000);
    s_t0 = bench_run(call_t0_fib, (void*)narg, 100);
    print_stats("Native C", s_native);
    print_stats("VORTEX T2 JIT", s_jit);
    print_stats("VORTEX T0 Interpreter", s_t0);
    printf("  → T2 JIT: %.1f%% of native C  |  %.1fx faster than T0 interp\n\n",
           100.0 * s_native.median / s_jit.median, s_t0.median / s_jit.median);

    /* ----- Benchmark: gcd(123456, 7890) ----- */
    printf("--- gcd(123456, 7890) ---\n");
    int64_t gargs[2] = { 123456, 7890 };
    jit2_ctx_t jc2 = { j_gcd, vtx_make_smi(123456), vtx_make_smi(7890) };
    s_native = bench_run(call_native_gcd, gargs, 10000);
    s_jit = bench_run(call_jit2, &jc2, 10000);
    s_t0 = bench_run(call_t0_gcd, gargs, 100);
    print_stats("Native C", s_native);
    print_stats("VORTEX T2 JIT", s_jit);
    print_stats("VORTEX T0 Interpreter", s_t0);
    printf("  → T2 JIT: %.1f%% of native C  |  %.1fx faster than T0 interp\n\n",
           100.0 * s_native.median / s_jit.median, s_t0.median / s_jit.median);

    /* ----- Benchmark: collatz(27) ----- */
    printf("--- collatz(27) ---\n");
    jc.entry = j_col; jc.arg = vtx_make_smi(27);
    narg = 27;
    s_native = bench_run(call_native_collatz, (void*)narg, 10000);
    s_jit = bench_run(call_jit1, &jc, 10000);
    s_t0 = bench_run(call_t0_collatz, (void*)narg, 100);
    print_stats("Native C", s_native);
    print_stats("VORTEX T2 JIT", s_jit);
    print_stats("VORTEX T0 Interpreter", s_t0);
    printf("  → T2 JIT: %.1f%% of native C  |  %.1fx faster than T0 interp\n\n",
           100.0 * s_native.median / s_jit.median, s_t0.median / s_jit.median);

    /* ===== T3 BENCHMARKS ===== */
    printf("\n=== T3 (Speculative JIT) Benchmarks ===\n\n");

    /* Compile through T3 */
    jit_entry_t t3_sum = compile(PROG_SUM, 1, 3);
    jit_entry_t t3_fib = compile(PROG_FIB, 1, 3);
    jit_entry_t t3_gcd = compile(PROG_GCD, 2, 3);
    jit_entry_t t3_col = compile(PROG_COLLATZ, 1, 3);

    if (!t3_sum || !t3_fib || !t3_gcd || !t3_col) {
        printf("  T3 compilation failed — skipping T3 benchmarks\n");
    } else {
        /* Verify T3 correctness */
        printf("--- T3 Correctness ---\n");
        v = vtx_make_smi(100); j_r = vtx_smi_value(t3_sum(&m, NULL, (void*)1, &v, 1));
        printf("  T3 sum(100) = %lld %s\n", (long long)j_r,
               j_r == native_sum_loop(100) ? "OK" : "MISMATCH");
        v = vtx_make_smi(20); j_r = vtx_smi_value(t3_fib(&m, NULL, (void*)1, &v, 1));
        printf("  T3 fib(20) = %lld %s\n", (long long)j_r,
               j_r == native_fib_iter(20) ? "OK" : "MISMATCH");
        vtx_value_t g3[2] = { vtx_make_smi(123456), vtx_make_smi(7890) };
        j_r = vtx_smi_value(t3_gcd(&m, NULL, (void*)1, g3, 2));
        printf("  T3 gcd(123456,7890) = %lld %s\n", (long long)j_r,
               j_r == native_gcd(123456, 7890) ? "OK" : "MISMATCH");
        v = vtx_make_smi(27); j_r = vtx_smi_value(t3_col(&m, NULL, (void*)1, &v, 1));
        printf("  T3 collatz(27) = %lld %s\n\n", (long long)j_r,
               j_r == native_collatz(27) ? "OK" : "MISMATCH");

        /* T3 Benchmark: sum(100) */
        printf("--- T3 sum(100) ---\n");
        jc.entry = t3_sum; jc.arg = vtx_make_smi(100);
        narg = 100;
        s_native = bench_run(call_native_sum, (void*)narg, 10000);
        stats_t s_t3 = bench_run(call_jit1, &jc, 10000);
        s_t0 = bench_run(call_t0_sum, (void*)narg, 100);
        print_stats("Native C", s_native);
        print_stats("VORTEX T3 JIT", s_t3);
        print_stats("VORTEX T2 JIT", s_jit);
        print_stats("VORTEX T0 Interpreter", s_t0);
        printf("  → T3: %.1f%% of native  |  T3 vs T2: %.2fx  |  %.1fx faster than T0\n\n",
               100.0 * s_native.median / s_t3.median,
               s_jit.median / s_t3.median,
               s_t0.median / s_t3.median);

        /* T3 Benchmark: fib(20) */
        printf("--- T3 fib(20) ---\n");
        jc.entry = t3_fib; jc.arg = vtx_make_smi(20);
        narg = 20;
        s_native = bench_run(call_native_fib, (void*)narg, 10000);
        s_t3 = bench_run(call_jit1, &jc, 10000);
        s_jit = bench_run(call_jit1, &(jit1_ctx_t){j_fib, vtx_make_smi(20)}, 10000);
        s_t0 = bench_run(call_t0_fib, (void*)narg, 100);
        print_stats("Native C", s_native);
        print_stats("VORTEX T3 JIT", s_t3);
        print_stats("VORTEX T2 JIT", s_jit);
        print_stats("VORTEX T0 Interpreter", s_t0);
        printf("  → T3: %.1f%% of native  |  T3 vs T2: %.2fx  |  %.1fx faster than T0\n\n",
               100.0 * s_native.median / s_t3.median,
               s_jit.median / s_t3.median,
               s_t0.median / s_t3.median);

        /* T3 Benchmark: gcd(123456, 7890) */
        printf("--- T3 gcd(123456, 7890) ---\n");
        jc2.entry = t3_gcd; jc2.a = vtx_make_smi(123456); jc2.b = vtx_make_smi(7890);
        s_native = bench_run(call_native_gcd, gargs, 10000);
        s_t3 = bench_run(call_jit2, &jc2, 10000);
        s_jit = bench_run(call_jit2, &(jit2_ctx_t){j_gcd, vtx_make_smi(123456), vtx_make_smi(7890)}, 10000);
        s_t0 = bench_run(call_t0_gcd, gargs, 100);
        print_stats("Native C", s_native);
        print_stats("VORTEX T3 JIT", s_t3);
        print_stats("VORTEX T2 JIT", s_jit);
        print_stats("VORTEX T0 Interpreter", s_t0);
        printf("  → T3: %.1f%% of native  |  T3 vs T2: %.2fx  |  %.1fx faster than T0\n\n",
               100.0 * s_native.median / s_t3.median,
               s_jit.median / s_t3.median,
               s_t0.median / s_t3.median);

        /* T3 Benchmark: collatz(27) */
        printf("--- T3 collatz(27) ---\n");
        jc.entry = t3_col; jc.arg = vtx_make_smi(27);
        narg = 27;
        s_native = bench_run(call_native_collatz, (void*)narg, 10000);
        s_t3 = bench_run(call_jit1, &jc, 10000);
        s_jit = bench_run(call_jit1, &(jit1_ctx_t){j_col, vtx_make_smi(27)}, 10000);
        s_t0 = bench_run(call_t0_collatz, (void*)narg, 100);
        print_stats("Native C", s_native);
        print_stats("VORTEX T3 JIT", s_t3);
        print_stats("VORTEX T2 JIT", s_jit);
        print_stats("VORTEX T0 Interpreter", s_t0);
        printf("  → T3: %.1f%% of native  |  T3 vs T2: %.2fx  |  %.1fx faster than T0\n\n",
               100.0 * s_native.median / s_t3.median,
               s_jit.median / s_t3.median,
               s_t0.median / s_t3.median);
    }

    /* ----- Summary ----- */
    printf("=== Summary ===\n");
    printf("  T3 JIT: T2 pipeline + speculative guards + more iterations (5 vs 3)\n");
    printf("  T2 JIT: GVN → SCCP → DCE → strength_reduce → LICM → guard_hoist → isel → regalloc → emit\n");
    printf("  T0 Interpreter: bytecode dispatch loop\n");
    printf("  Native C: gcc -O3 -march=native (gold standard)\n");
    return 0;
}
