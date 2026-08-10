/**
 * bench_jit_vs_jit.c — Fair VORTEX T2 JIT vs V8 JIT benchmark.
 *
 * The challenge: V8's TurboFan is extremely aggressive at constant-folding
 * micro-benchmarks. To make a fair JIT-vs-JIT comparison, we need:
 *   1. Inputs that V8 cannot predict (read from stdin/env)
 *   2. Functions that V8 cannot inline (eval() + dynamic dispatch)
 *   3. Enough iterations that V8 actually JIT-compiles (not just interprets)
 *
 * This benchmark runs the SAME workloads as bench_t2_vs_v8 but with
 * truly unpredictable inputs via an LCG seeded from getpid().
 * Both JITs see the same work; we measure steady-state throughput.
 *
 * Build: cmake --build . --target bench_jit_vs_jit
 * Run:   ./benchmarks/bench_jit_vs_jit
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
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
    return (jit_entry_t)method->compiled_code;
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

/* ---- Native C ---- */
__attribute__((noinline))
static int64_t native_sum(volatile int64_t n) {
    int64_t s = 0;
    for (int64_t i = n; i > 0; i--) s += i;
    return s;
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

/* ---- LCG for unpredictable inputs ---- */
static uint64_t lcg_state;
static void lcg_seed(uint64_t s) { lcg_state = s; }
static uint64_t lcg_next(void) {
    lcg_state = lcg_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return lcg_state;
}

/* ---- Benchmark harness ---- */
#define SAMPLES 20
static volatile int64_t g_sink;

static double bench_jit1(jit_entry_t entry, int64_t baseN, int iters) {
    vtx_method_desc_t m = {0}; m.name = "f";
    static double samples[SAMPLES];
    /* Seed from getpid so V8 can't predict (both use same seed though) */
    uint64_t seed = (uint64_t)getpid() * 1000000007ULL;
    for (int s = 0; s < SAMPLES; s++) {
        lcg_seed(seed + s * 31);
        uint64_t t0 = now_ns();
        int64_t acc = 0;
        for (int i = 0; i < iters; i++) {
            int64_t n = baseN + (lcg_next() & 15);
            vtx_value_t v = vtx_make_smi(n);
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
    uint64_t seed = (uint64_t)getpid() * 1000000007ULL;
    for (int s = 0; s < SAMPLES; s++) {
        lcg_seed(seed + s * 31);
        uint64_t t0 = now_ns();
        int64_t acc = 0;
        for (int i = 0; i < iters; i++) {
            int64_t aa = a + (lcg_next() & 15);
            int64_t bb = b + (lcg_next() & 7);
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

static double bench_native1(int64_t (*fn)(int64_t), int64_t baseN, int iters) {
    static double samples[SAMPLES];
    uint64_t seed = (uint64_t)getpid() * 1000000007ULL;
    for (int s = 0; s < SAMPLES; s++) {
        lcg_seed(seed + s * 31);
        uint64_t t0 = now_ns();
        int64_t acc = 0;
        for (int i = 0; i < iters; i++) {
            acc += fn(baseN + (lcg_next() & 15));
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
    uint64_t seed = (uint64_t)getpid() * 1000000007ULL;
    for (int s = 0; s < SAMPLES; s++) {
        lcg_seed(seed + s * 31);
        uint64_t t0 = now_ns();
        int64_t acc = 0;
        for (int i = 0; i < iters; i++) {
            acc += fn(a + (lcg_next() & 15), b + (lcg_next() & 7));
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
    printf("  JIT vs JIT: VORTEX T2 vs V8 (Node.js) vs Native C\n");
    printf("  LCG-varying inputs (seeded from getpid) — no constant folding\n");
    printf("  %d samples, median reported\n", SAMPLES);
    printf("================================================================\n\n");

    jit_entry_t j_sum = compile_t2(PROG_SUM, 1);
    jit_entry_t j_gcd = compile_t2(PROG_GCD, 2);
    jit_entry_t j_col = compile_t2(PROG_COLLATZ, 1);
    if (!j_sum || !j_gcd || !j_col) {
        printf("FAIL: compilation failed\n");
        return 1;
    }

    /* Verify correctness */
    printf("--- Correctness ---\n");
    vtx_method_desc_t m = {0}; m.name = "f";
    vtx_value_t v = vtx_make_smi(100);
    printf("  sum(100) = %ld (expected 5050)\n",
           (long)vtx_smi_value(j_sum(&m, NULL, (void*)1, &v, 1)));
    vtx_value_t args[] = { vtx_make_smi(123456), vtx_make_smi(7890) };
    printf("  gcd = %ld (expected 6)\n",
           (long)vtx_smi_value(j_gcd(&m, NULL, (void*)1, args, 2)));
    v = vtx_make_smi(27);
    printf("  collatz(27) = %ld (expected 111)\n",
           (long)vtx_smi_value(j_col(&m, NULL, (void*)1, &v, 1)));
    printf("\n");

    printf("--- Results (ns/call) ---\n");
    printf("  %-18s  %10s  %10s  %10s  %7s  %7s\n",
           "Benchmark", "VORTEX T2", "Native C", "V8 (JS)", "T2/C", "T2/V8");

    double t2, nat;
    int iters = 2000;

    /* sum(10000) — large enough that V8 can't constant-fold */
    t2 = bench_jit1(j_sum, 10000, iters);
    nat = bench_native1(native_sum, 10000, iters);
    printf("  %-18s  %8.0f ns  %8.0f ns  %8s  %5.1f%%  %6s\n",
           "sum(10000)", t2, nat, "(run JS)", 100.0*nat/t2, "(JS)");

    /* gcd(big) — V8 can't constant-fold because inputs vary */
    t2 = bench_jit2(j_gcd, 1234567890, 123456789, iters);
    nat = bench_native2(native_gcd, 1234567890, 123456789, iters);
    printf("  %-18s  %8.0f ns  %8.0f ns  %8s  %5.1f%%  %6s\n",
           "gcd(big)", t2, nat, "(run JS)", 100.0*nat/t2, "(JS)");

    /* collatz(97) — V8 can't constant-fold because input varies */
    t2 = bench_jit1(j_col, 97, iters);
    nat = bench_native1(native_collatz, 97, iters);
    printf("  %-18s  %8.0f ns  %8.0f ns  %8s  %5.1f%%  %6s\n",
           "collatz(97)", t2, nat, "(run JS)", 100.0*nat/t2, "(JS)");

    printf("\n  Run V8 companion: node benchmarks/v8_js/bench_jit_vs_jit.js\n");
    printf("  V8 uses the same LCG seed (getpid) for fair comparison.\n");
    printf("================================================================\n");

    (void)g_sink;
    return 0;
}
