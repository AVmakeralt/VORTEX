/**
 * bench_scaling.c — Scaling diagnostic: sum(N) for N = 100, 1K, 10K, 100K, 1M
 *
 * This single experiment distinguishes:
 *   - Per-iteration cost problem (linear scaling)
 *   - Fixed overhead problem (constant + linear)
 *
 * If times are ~300ns, ~3us, ~27us, ~270us → per-iteration cost
 * If times are ~20us, ~21us, ~27us, ~35us → fixed overhead
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

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

__attribute__((noinline))
static int64_t native_sum(volatile int64_t n) {
    int64_t s = 0;
    for (int64_t i = n; i > 0; i--) s += i;
    return s;
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

static volatile int64_t g_sink;

int main(void) {
    printf("================================================================\n");
    printf("  Scaling Diagnostic: sum(N) for N = 100..1M\n");
    printf("  Distinguishes per-iteration cost vs fixed overhead\n");
    printf("================================================================\n\n");

    jit_entry_t j_sum = compile_t2(PROG_SUM, 1);
    jit_entry_t j_col = compile_t2(PROG_COLLATZ, 1);
    if (!j_sum || !j_col) {
        printf("FAIL: compilation failed\n");
        return 1;
    }

    vtx_method_desc_t m = {0}; m.name = "f";

    printf("  %-10s  %12s  %12s  %12s  %10s\n",
           "N", "T2 JIT (ns)", "Native (ns)", "T2/C", "ns/iter");
    printf("  %-10s  %12s  %12s  %12s  %10s\n",
           "----------", "------------", "------------", "--------", "-------");

    /* sum scaling */
    int64_t sum_Ns[] = {100, 1000, 10000, 100000, 1000000};
    int n_sums = sizeof(sum_Ns)/sizeof(sum_Ns[0]);

    for (int i = 0; i < n_sums; i++) {
        int64_t N = sum_Ns[i];
        int iters = (N >= 100000) ? 20 : 200;

        /* JIT */
        uint64_t t0 = now_ns();
        int64_t acc = 0;
        for (int j = 0; j < iters; j++) {
            vtx_value_t v = vtx_make_smi(N);
            vtx_value_t r = j_sum(&m, NULL, (void*)1, &v, 1);
            acc += vtx_smi_value(r);
        }
        uint64_t t1 = now_ns();
        g_sink = acc;
        double t2_ns = (double)(t1 - t0) / iters;

        /* Native */
        t0 = now_ns();
        acc = 0;
        for (int j = 0; j < iters; j++) acc += native_sum(N);
        t1 = now_ns();
        g_sink = acc;
        double nat_ns = (double)(t1 - t0) / iters;

        double ratio = 100.0 * nat_ns / t2_ns;
        double per_iter = t2_ns / N;

        printf("  %-10ld  %10.0f ns  %10.0f ns  %9.1f%%  %8.2f\n",
               (long)N, t2_ns, nat_ns, ratio, per_iter);
    }

    printf("\n  --- Collatz scaling ---\n");
    printf("  %-10s  %12s  %12s  %12s  %10s\n",
           "start", "T2 JIT (ns)", "Native (ns)", "T2/C", "ns/step");
    printf("  %-10s  %12s  %12s  %12s  %10s\n",
           "----------", "------------", "------------", "--------", "-------");

    /* collatz scaling — different start values give different step counts */
    int64_t col_Ns[] = {27, 97, 871, 6171, 77031};
    int n_cols = sizeof(col_Ns)/sizeof(col_Ns[0]);

    for (int i = 0; i < n_cols; i++) {
        int64_t N = col_Ns[i];
        int iters = 2000;

        uint64_t t0 = now_ns();
        int64_t acc = 0;
        for (int j = 0; j < iters; j++) {
            vtx_value_t v = vtx_make_smi(N);
            vtx_value_t r = j_col(&m, NULL, (void*)1, &v, 1);
            acc += vtx_smi_value(r);
        }
        uint64_t t1 = now_ns();
        g_sink = acc;
        double t2_ns = (double)(t1 - t0) / iters;

        t0 = now_ns();
        acc = 0;
        for (int j = 0; j < iters; j++) acc += native_collatz(N);
        t1 = now_ns();
        g_sink = acc;
        double nat_ns = (double)(t1 - t0) / iters;

        double ratio = 100.0 * nat_ns / t2_ns;
        /* collatz(27) = 111 steps, collatz(97) = 118, etc. */
        int64_t steps = native_collatz(N);
        double per_step = t2_ns / steps;

        printf("  %-10ld  %10.0f ns  %10.0f ns  %9.1f%%  %8.2f\n",
               (long)N, t2_ns, nat_ns, ratio, per_step);
    }

    printf("\n  --- GCD (single call, no loop scaling) ---\n");
    jit_entry_t j_gcd = compile_t2(PROG_GCD, 2);
    if (j_gcd) {
        int iters = 20000;
        uint64_t t0 = now_ns();
        int64_t acc = 0;
        for (int j = 0; j < iters; j++) {
            vtx_value_t args[2] = { vtx_make_smi(1234567890), vtx_make_smi(123456789) };
            vtx_value_t r = j_gcd(&m, NULL, (void*)1, args, 2);
            acc += vtx_smi_value(r);
        }
        uint64_t t1 = now_ns();
        g_sink = acc;
        double t2_ns = (double)(t1 - t0) / iters;
        printf("  gcd(big)   %10.0f ns/call  (T2)\n", t2_ns);
    }

    printf("\n================================================================\n");
    printf("  Interpretation:\n");
    printf("  - If ns/iter is constant across N → per-iteration cost problem\n");
    printf("  - If ns/iter decreases as N grows → fixed overhead component\n");
    printf("  - If T2/C ratio is constant → loop body is the bottleneck\n");
    printf("  - If T2/C ratio improves with N → fixed overhead dominates\n");
    printf("================================================================\n");

    (void)g_sink;
    return 0;
}
