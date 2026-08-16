/**
 * bench_t2_vs_v8.c — Direct VORTEX T2 JIT vs V8 comparison.
 *
 * Compiles each kernel through the T2 pipeline and benchmarks the
 * JIT-compiled code directly (no interpreter). Compares against
 * native C and V8 (Node.js, run separately via bench_v8_comparison.js).
 *
 * Build:
 *   cmake --build . --target bench_t2_vs_v8
 *
 * Run:
 *   ./benchmarks/bench_t2_vs_v8
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

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

typedef vtx_value_t (*jit_entry_t)(const vtx_method_desc_t *, void *, void *,
                                    vtx_value_t *, uint32_t);

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---- Compile through T2 pipeline ---- */
static jit_entry_t compile_t2(const char *prog_text, uint32_t arg_count) {
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

    if (vtx_graph_build(graph, bc, method, arena) != 0) return NULL;

    vtx_pipeline_config_t config = vtx_pipeline_config_t2();
    vtx_code_cache_init(cache, 1 << 20);
    vtx_method_registry_init(reg, arena);
    config.code_cache = cache;
    config.method_registry = reg;
    config.method = method;

    vtx_compile_result_t result;
    memset(&result, 0, sizeof(result));
    int rc = vtx_pipeline_run(graph, &config, arena, &result);
    if (rc != 0 || !result.success || method->compiled_code == NULL) return NULL;
    /* ISO C forbids direct object-pointer → function-pointer cast;
     * use a union (the portable, pedantic-clean idiom). */
    union { void *ptr; jit_entry_t fn; } u_e;
    u_e.ptr = method->compiled_code;
    return u_e.fn;
}

/* ---- Programs ---- */
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

/* ---- Native C references ---- */
__attribute__((noinline))
static int64_t native_sum(volatile int64_t n) {
    int64_t s = 0;
    for (int64_t i = n; i > 0; i--) s += i;
    return s;
}
__attribute__((noinline))
static int64_t native_fib(volatile int64_t n) {
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

/* ---- Benchmark harness ---- */
#define SAMPLES 20

static volatile int64_t g_sink;

static double bench_jit1(jit_entry_t entry, int64_t n, int iters) {
    vtx_method_desc_t m = {0}; m.name = "f";
    static double samples[SAMPLES];
    for (int s = 0; s < SAMPLES; s++) {
        uint64_t lcg = 12345u + (uint64_t)s;
        uint64_t t0 = now_ns();
        int64_t acc = 0;
        for (int i = 0; i < iters; i++) {
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            int64_t nn = n + (int64_t)(lcg & 15);
            vtx_value_t v = vtx_make_smi(nn);
            vtx_value_t r = entry(&m, NULL, (void*)1, &v, 1);
            acc += vtx_smi_value(r);
        }
        uint64_t t1 = now_ns();
        g_sink = acc;
        samples[s] = (double)(t1 - t0) / iters;
    }
    for (int i = 0; i < SAMPLES - 1; i++)
        for (int j = i + 1; j < SAMPLES; j++)
            if (samples[j] < samples[i]) { double t = samples[i]; samples[i] = samples[j]; samples[j] = t; }
    return samples[SAMPLES / 2];
}

static double bench_jit2(jit_entry_t entry, int64_t a, int64_t b, int iters) {
    vtx_method_desc_t m = {0}; m.name = "f";
    static double samples[SAMPLES];
    for (int s = 0; s < SAMPLES; s++) {
        uint64_t lcg = 12345u + (uint64_t)s;
        uint64_t t0 = now_ns();
        int64_t acc = 0;
        for (int i = 0; i < iters; i++) {
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            int64_t aa = a + (int64_t)(lcg & 15);
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            int64_t bb = b + (int64_t)(lcg & 7);
            vtx_value_t args[2] = { vtx_make_smi(aa), vtx_make_smi(bb) };
            vtx_value_t r = entry(&m, NULL, (void*)1, args, 2);
            acc += vtx_smi_value(r);
        }
        uint64_t t1 = now_ns();
        g_sink = acc;
        samples[s] = (double)(t1 - t0) / iters;
    }
    for (int i = 0; i < SAMPLES - 1; i++)
        for (int j = i + 1; j < SAMPLES; j++)
            if (samples[j] < samples[i]) { double t = samples[i]; samples[j] = t; }
    return samples[SAMPLES / 2];
}

static double bench_native1(int64_t (*fn)(int64_t), int64_t n, int iters) {
    static double samples[SAMPLES];
    for (int s = 0; s < SAMPLES; s++) {
        uint64_t lcg = 12345u + (uint64_t)s;
        uint64_t t0 = now_ns();
        int64_t acc = 0;
        for (int i = 0; i < iters; i++) {
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            acc += fn(n + (int64_t)(lcg & 15));
        }
        uint64_t t1 = now_ns();
        g_sink = acc;
        samples[s] = (double)(t1 - t0) / iters;
    }
    for (int i = 0; i < SAMPLES - 1; i++)
        for (int j = i + 1; j < SAMPLES; j++)
            if (samples[j] < samples[i]) { double t = samples[i]; samples[j] = t; }
    return samples[SAMPLES / 2];
}

static double bench_native2(int64_t (*fn)(int64_t, int64_t), int64_t a, int64_t b, int iters) {
    static double samples[SAMPLES];
    for (int s = 0; s < SAMPLES; s++) {
        uint64_t lcg = 12345u + (uint64_t)s;
        uint64_t t0 = now_ns();
        int64_t acc = 0;
        for (int i = 0; i < iters; i++) {
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            int64_t aa = a + (int64_t)(lcg & 15);
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            int64_t bb = b + (int64_t)(lcg & 7);
            acc += fn(aa, bb);
        }
        uint64_t t1 = now_ns();
        g_sink = acc;
        samples[s] = (double)(t1 - t0) / iters;
    }
    for (int i = 0; i < SAMPLES - 1; i++)
        for (int j = i + 1; j < SAMPLES; j++)
            if (samples[j] < samples[i]) { double t = samples[i]; samples[j] = t; }
    return samples[SAMPLES / 2];
}

int main(void) {
    printf("================================================================\n");
    printf("  VORTEX T2 JIT vs Native C vs V8 (Node.js)\n");
    printf("  Same inputs as bench_t2_vs_v8.js (large N, varying input)\n");
    printf("  %d samples per benchmark, median reported\n", SAMPLES);
    printf("================================================================\n\n");

    /* Compile all kernels through T2 */
    jit_entry_t j_sum = compile_t2(PROG_SUM, 1);
    jit_entry_t j_fib = compile_t2(PROG_FIB, 1);
    jit_entry_t j_gcd = compile_t2(PROG_GCD, 2);
    jit_entry_t j_col = compile_t2(PROG_COLLATZ, 1);

    if (!j_sum || !j_fib || !j_gcd || !j_col) {
        printf("FAIL: compilation failed\n");
        return 1;
    }

    /* Verify correctness */
    printf("--- Correctness ---\n");
    vtx_method_desc_t m = {0}; m.name = "f";
    vtx_value_t v;
    v = vtx_make_smi(100); printf("  sum(100) = %ld\n", (long)vtx_smi_value(j_sum(&m, NULL, (void*)1, &v, 1)));
    v = vtx_make_smi(20);  printf("  fib(20) = %ld\n", (long)vtx_smi_value(j_fib(&m, NULL, (void*)1, &v, 1)));
    vtx_value_t args[] = { vtx_make_smi(123456), vtx_make_smi(7890) };
    printf("  gcd = %ld\n", (long)vtx_smi_value(j_gcd(&m, NULL, (void*)1, args, 2)));
    v = vtx_make_smi(27);  printf("  collatz(27) = %ld\n", (long)vtx_smi_value(j_col(&m, NULL, (void*)1, &v, 1)));

    /* Benchmark — use large N with varying input to prevent constant folding */
    printf("\n--- Results ---\n");
    printf("  %-20s  %12s  %12s  %8s  %8s\n",
           "Benchmark", "T2 JIT (ns)", "Native (ns)", "V8 (ns)", "T2/C");

    double t2, nat;
    int iters = 2000;

    t2 = bench_jit1(j_sum, 10000, iters);
    nat = bench_native1(native_sum, 10000, iters);
    printf("  %-20s  %12.1f  %12.1f  %8s  %7.1f%%\n",
           "sum(10000)", t2, nat, "(run JS)", 100.0 * nat / t2);

    t2 = bench_jit1(j_fib, 30, iters);
    nat = bench_native1(native_fib, 30, iters);
    printf("  %-20s  %12.1f  %12.1f  %8s  %7.1f%%\n",
           "fib(30)", t2, nat, "(run JS)", 100.0 * nat / t2);

    t2 = bench_jit2(j_gcd, 1234567890, 123456789, iters);
    nat = bench_native2(native_gcd, 1234567890, 123456789, iters);
    printf("  %-20s  %12.1f  %12.1f  %8s  %7.1f%%\n",
           "gcd(big)", t2, nat, "(run JS)", 100.0 * nat / t2);

    t2 = bench_jit1(j_col, 97, iters);
    nat = bench_native1(native_collatz, 97, iters);
    printf("  %-20s  %12.1f  %12.1f  %8s  %7.1f%%\n",
           "collatz(97)", t2, nat, "(run JS)", 100.0 * nat / t2);

    printf("\n  V8 numbers: run `node benchmarks/v8_js/bench_t2_vs_v8.js`\n");
    printf("================================================================\n");

    (void)g_sink;
    return 0;
}
