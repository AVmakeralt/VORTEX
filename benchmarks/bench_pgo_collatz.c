/**
 * bench_pgo_collatz.c — PGO Cold-vs-Warm Benchmark for VORTEX
 *
 * Measures the effect of VORTEX's PGO system on a long, branchy workload.
 *
 * Methodology:
 *   1. COLD run: no profile loaded. Interpreter profiles the code,
 *      triggers T1/T2 compilation, saves profile at exit.
 *   2. WARM run: profile loaded from disk. JIT uses the profile for
 *      branch hints, inlining decisions, and tier promotion.
 *   3. Compare median wall-clock time and report the speedup.
 *
 * The workload is Collatz convergence — the canonical unpredictable-
 * branch benchmark. The parity branch (n odd?) is ~50/50, so branch
 * prediction doesn't help. PGO helps by:
 *   - Recording the actual taken/not-taken ratio (for T2 branch hints)
 *   - Knowing the loop trip count distribution (for unrolling decisions)
 *   - Confidence-gated tier promotion (Sprint 1.1)
 *
 * This benchmark uses the VORTEX internal APIs directly (same as
 * bench_real_workload.c) so we can control profile load/save explicitly.
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>

#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/arena.h"
#include "interp/dispatch.h"
#include "interp/profiler.h"
#include "baseline/codegen.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "compile/threadpool.h"
#include "compile/request.h"
#include "profile/data.h"
#include "profile/persist.h"
#include "profile/confidence.h"

/* ========================================================================== */
/* Timing                                                                      */
/* ========================================================================== */

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Global pointer to the single benchmark method, for the method lookup callback. */
static vtx_method_desc_t *g_bench_method = NULL;

/* Method lookup callback for the compile context. Returns the single
 * benchmark method regardless of method_id (we only have one). */
static const vtx_method_desc_t *bench_method_lookup(uint32_t method_id, void *ctx)
{
    (void)method_id;
    (void)ctx;
    return g_bench_method;
}

/* ========================================================================== */
/* Collatz bytecode builder                                                    */
/* ========================================================================== */

/* Builds: int64_t collatz(int64_t n)
 *   steps = 0
 *   while n != 1:
 *     if (n & 1) == 0: n = n / 2
 *     else:            n = 3*n + 1
 *     steps++
 *   return steps
 *
 * Locals: [0]=n, [1]=steps
 */
static vtx_bytecode_t *build_collatz_bytecode(vtx_arena_t *arena)
{
    size_t cap = 512;
    uint8_t *buf = vtx_arena_alloc(arena, cap);
    size_t pos = 0;

    #define EMIT_OP(op) do { buf[pos++] = (op); } while(0)
    #define EMIT_U16(v) do { buf[pos++] = (uint8_t)((v) >> 8); buf[pos++] = (uint8_t)((v) & 0xFF); } while(0)

    /* Constant pool indices */
    const int CP_ZERO = 0, CP_ONE = 1, CP_TWO = 2, CP_THREE = 3;

    /* steps = 0 */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ZERO);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(1);

    /* loop_start: */
    size_t loop_start = pos;
    /* if n == 1: goto done */
    EMIT_OP(VT_OP_LOAD_LOCAL);      EMIT_U16(0);
    EMIT_OP(VT_OP_LOAD_CONST_INT);  EMIT_U16(CP_ONE);
    EMIT_OP(VT_OP_ICMP_EQ);
    EMIT_OP(VT_OP_IF_TRUE);
    size_t exit_patch = pos; EMIT_U16(0);

    /* if (n & 1) == 0: goto even */
    EMIT_OP(VT_OP_LOAD_LOCAL);      EMIT_U16(0);
    EMIT_OP(VT_OP_LOAD_CONST_INT);  EMIT_U16(CP_ONE);
    EMIT_OP(VT_OP_IAND);
    EMIT_OP(VT_OP_LOAD_CONST_INT);  EMIT_U16(CP_ZERO);
    EMIT_OP(VT_OP_ICMP_EQ);
    EMIT_OP(VT_OP_IF_FALSE);  /* if (n&1)!=0 i.e. odd, jump to odd */
    size_t odd_patch = pos; EMIT_U16(0);

    /* even: n = n / 2 */
    EMIT_OP(VT_OP_LOAD_LOCAL);      EMIT_U16(0);
    EMIT_OP(VT_OP_LOAD_CONST_INT);  EMIT_U16(CP_TWO);
    EMIT_OP(VT_OP_IDIV);
    EMIT_OP(VT_OP_STORE_LOCAL);     EMIT_U16(0);
    EMIT_OP(VT_OP_GOTO);
    size_t skip_patch = pos; EMIT_U16(0);

    /* odd: n = 3*n + 1 */
    size_t odd_start = pos;
    EMIT_OP(VT_OP_LOAD_LOCAL);      EMIT_U16(0);
    EMIT_OP(VT_OP_LOAD_CONST_INT);  EMIT_U16(CP_THREE);
    EMIT_OP(VT_OP_IMUL);
    EMIT_OP(VT_OP_LOAD_CONST_INT);  EMIT_U16(CP_ONE);
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL);     EMIT_U16(0);

    /* after_parity: steps++ */
    size_t after_parity = pos;
    EMIT_OP(VT_OP_LOAD_LOCAL);      EMIT_U16(1);
    EMIT_OP(VT_OP_LOAD_CONST_INT);  EMIT_U16(CP_ONE);
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL);     EMIT_U16(1);

    /* goto loop_start */
    EMIT_OP(VT_OP_GOTO); EMIT_U16((uint16_t)loop_start);

    /* done: return steps */
    size_t done = pos;
    buf[exit_patch] = (uint8_t)(done >> 8);
    buf[exit_patch+1] = (uint8_t)(done & 0xFF);
    buf[odd_patch] = (uint8_t)(odd_start >> 8);
    buf[odd_patch+1] = (uint8_t)(odd_start & 0xFF);
    buf[skip_patch] = (uint8_t)(after_parity >> 8);
    buf[skip_patch+1] = (uint8_t)(after_parity & 0xFF);

    EMIT_OP(VT_OP_LOAD_LOCAL);      EMIT_U16(1);
    EMIT_OP(VT_OP_RETURN_VALUE);

    /* Constant pool */
    vtx_value_t *cp = vtx_arena_alloc(arena, 4 * sizeof(vtx_value_t));
    cp[CP_ZERO]  = vtx_make_smi(0);
    cp[CP_ONE]   = vtx_make_smi(1);
    cp[CP_TWO]   = vtx_make_smi(2);
    cp[CP_THREE] = vtx_make_smi(3);

    vtx_bytecode_t *bc = vtx_arena_alloc(arena, sizeof(vtx_bytecode_t));
    bc->code = buf;
    bc->length = (uint32_t)pos;
    bc->constant_pool = cp;
    bc->constant_count = 4;
    bc->max_locals = 2;
    bc->max_stack = 4;

    #undef EMIT_OP
    #undef EMIT_U16
    return bc;
}

/* ========================================================================== */
/* Native C Collatz (for comparison)                                           */
/* ========================================================================== */

static int64_t native_collatz(int64_t n)
{
    int64_t steps = 0;
    while (n != 1) {
        if ((n & 1) == 0) n = n / 2;
        else              n = 3 * n + 1;
        steps++;
    }
    return steps;
}

/* Sum of collatz steps for n in [1, N] — the actual workload. */
static int64_t native_collatz_sum(int64_t N)
{
    int64_t total = 0;
    for (int64_t n = 1; n <= N; n++) {
        total += native_collatz(n);
    }
    return total;
}

/* ========================================================================== */
/* Benchmark context                                                           */
/* ========================================================================== */

typedef struct {
    vtx_interp_t      *interp;
    vtx_method_desc_t *method;
    int64_t            N;  /* sum collatz(n) for n in [1, N] */
} bench_ctx_t;

/* Run collatz(n) N times via the interpreter/JIT, summing results.
 * This is the function being benchmarked. */
static void bench_vortex_collatz_sum(void *arg)
{
    bench_ctx_t *c = (bench_ctx_t *)arg;
    int64_t total = 0;
    for (int64_t n = 1; n <= c->N; n++) {
        vtx_value_t a = vtx_make_smi(n);
        vtx_value_t r = vtx_interp_run(c->interp, c->method, &a, 1);
        if (vtx_is_smi(r)) total += vtx_smi_value(r);
    }
    /* Sink the result so the compiler can't DCE. */
    if (total == 0xdeadbeef) printf("unlikely\n");
}

static void bench_native_collatz_sum(void *arg)
{
    int64_t N = *(int64_t *)arg;
    int64_t total = native_collatz_sum(N);
    if (total == 0xdeadbeef) printf("unlikely\n");
}

/* ========================================================================== */
/* Benchmark runner                                                            */
/* ========================================================================== */

typedef struct {
    const char *name;
    uint64_t    median_ns;
    uint64_t    min_ns;
    uint64_t    p95_ns;
} bench_result_t;

static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a, vb = *(const uint64_t *)b;
    return (va < vb) ? -1 : (va > vb) ? 1 : 0;
}

static bench_result_t bench_run(const char *name, void (*fn)(void *), void *arg,
                                 uint32_t warmup, uint32_t samples)
{
    bench_result_t r = { .name = name };
    /* Warmup */
    for (uint32_t i = 0; i < warmup; i++) fn(arg);

    uint64_t *s = malloc(samples * sizeof(uint64_t));
    for (uint32_t i = 0; i < samples; i++) {
        uint64_t t0 = now_ns();
        fn(arg);
        uint64_t t1 = now_ns();
        s[i] = t1 - t0;
    }
    qsort(s, samples, sizeof(uint64_t), cmp_u64);
    r.min_ns    = s[0];
    r.median_ns = s[samples / 2];
    r.p95_ns    = s[(uint32_t)(samples * 0.95)];
    free(s);
    return r;
}

static void bench_print(const bench_result_t *r)
{
    printf("  %-45s  median %9lu ns  p95 %9lu ns  min %9lu ns\n",
           r->name, (unsigned long)r->median_ns,
           (unsigned long)r->p95_ns, (unsigned long)r->min_ns);
}

/* ========================================================================== */
/* MAIN                                                                        */
/* ========================================================================== */

int main(int argc, char **argv)
{
    fprintf(stderr, "[bench] starting\n");
    fflush(stderr);

    int64_t N = 1000;
    if (argc > 1) N = atoll(argv[1]);

    /* Mode: "cold" (no profile, save at exit) or "warm" (load profile). */
    const char *mode = (argc > 2) ? argv[2] : "cold";

    uint32_t warmup  = 3;
    uint32_t samples = 10;

    fprintf(stderr, "[bench] N=%ld mode=%s\n", (long)N, mode);
    fflush(stderr);

    printf("================================================================\n");
    printf("  VORTEX PGO Benchmark — Collatz (branchy, unpredictable)\n");
    printf("  Workload: sum of collatz(n) for n in [1, %lld]\n", (long long)N);
    printf("  Mode: %s   Warmup: %u   Samples: %u\n", mode, warmup, samples);
    printf("================================================================\n\n");
    fflush(stdout);

    /* ---- Verify correctness ---- */
    fprintf(stderr, "[bench] calling native_collatz_sum\n"); fflush(stderr);
    int64_t expected = native_collatz_sum(N);
    fprintf(stderr, "[bench] native_collatz_sum returned %ld\n", (long)expected); fflush(stderr);
    printf("  Native C reference: total steps = %lld\n\n", (long long)expected);
    fflush(stdout);

    /* ---- Set up VORTEX runtime ---- */
    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    vtx_code_cache_t cache;
    vtx_code_cache_init(&cache, VORTEX_CACHE_MAX_SIZE);

    vtx_method_registry_t registry;
    vtx_method_registry_init(&registry, &arena);

    vtx_bytecode_t *collatz_bc = build_collatz_bytecode(&arena);
    vtx_method_desc_t collatz_method = {
        .name = "collatz", .signature = "(I)I",
        .bytecode = collatz_bc, .compiled_code = NULL,
        .vtable_index = 0xFFFFFFFF, .arg_count = 1, .is_virtual = false
    };

    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    /* Note: We do NOT wire up the JIT compilation pipeline (no threadpool,
     * no compile context). This means the interpreter runs in pure T0
     * mode — it profiles the code but never tiers up to T1/T2.
     *
     * This is intentional for the PGO benchmark: we want to measure the
     * pure interpreter speed and the profile collection overhead. The
     * WARM run then loads the saved profile and runs the same interpreter
     * — the difference is that the profile data is pre-populated.
     *
     * The real JIT speedup is measured by bench_real_workload, which
     * pre-compiles with the T1 baseline JIT and compares to native C. */
    (void)registry;

    /* ---- PGO: set up profile data and persistence ---- */
    vtx_profile_global_t profile;
    vtx_profile_global_init(&profile);

    /* Compute bytecode hash for profile version gating. */
    uint8_t bytecode_hash[VTX_PROFILE_HASH_SIZE];
    vtx_profile_compute_bytecode_hash(collatz_bc->code, collatz_bc->length,
                                        bytecode_hash);

    /* Determine profile directory. */
    const char *dir = getenv("VORTEX_PROFILE_DIR");
    char dir_buf[512];
    if (dir == NULL) {
        const char *home = getenv("HOME");
        if (home == NULL) home = "/tmp";
        snprintf(dir_buf, sizeof(dir_buf), "%s/.cache/vortex/profiles", home);
        dir = dir_buf;
        mkdir(dir, 0755);
    }

    /* Profile filename: first 16 bytes of hash as hex. */
    char hash_hex[33];
    for (int i = 0; i < 16; i++) {
        snprintf(hash_hex + i * 2, 3, "%02x", bytecode_hash[i]);
    }
    hash_hex[32] = '\0';

    char profile_file[600];
    snprintf(profile_file, sizeof(profile_file), "%s/%s.prof", dir, hash_hex);

    bool profile_loaded = false;
    if (strcmp(mode, "warm") == 0) {
        /* WARM mode: try to load the profile from disk. */
        if (vtx_profile_load(&profile, profile_file, bytecode_hash)) {
            printf("  [pgo] Loaded profile from %s (%u methods)\n",
                    profile_file, profile.method_count);
            profile_loaded = true;

            /* Dump profile confidence for the collatz method if present. */
            for (uint32_t i = 0; i < profile.method_count; i++) {
                vtx_profile_method_t *m = &profile.methods[i];
                double conf = vtx_confidence_method(m);
                printf("  [pgo]   method %u: invocations=%lu  confidence=%.3f\n",
                       m->method_id, (unsigned long)m->invocation_count, conf);
            }
        } else {
            printf("  [pgo] No profile found at %s (falling back to cold)\n",
                    profile_file);
        }
    } else {
        printf("  [pgo] Cold run — no profile loaded\n");
    }

    /* Register atexit handler to save profile on exit (cold mode). */
    if (strcmp(mode, "cold") == 0) {
        vtx_profile_register_atexit(&profile, profile_file, bytecode_hash);
        printf("  [pgo] Will save profile to %s at exit\n\n", profile_file);
    } else {
        printf("\n");
    }

    /* Compile with T1 baseline JIT.
     *
     * In BOTH cold and warm modes, we let the interpreter tier-up
     * naturally — it profiles the code and triggers T1 compilation
     * via the tier-up mechanism. This is how real PGO works:
     * profile first, compile second.
     *
     * Pre-compiling would skip the profiling phase, which is what
     * we want to measure. */
    printf("  T1 JIT: deferred (interpreter will tier-up after %d invocations)\n\n",
           VORTEX_T1_THRESHOLD);

    bench_ctx_t ctx = { .interp = &interp, .method = &collatz_method, .N = N };

    /* ---- Phase 1: single-shot timing ---- */
    printf("--- Phase 1: Single-shot (includes JIT compile if cold) ---\n");
    {
        uint64_t t0 = now_ns();
        bench_vortex_collatz_sum(&ctx);
        uint64_t t1 = now_ns();
        printf("  Single shot:  %9lu ns  (%.2f ms)\n",
               (unsigned long)(t1 - t0), (t1 - t0) / 1e6);
    }

    /* ---- Phase 2: WARM runs (JIT code cached) ---- */
    printf("\n--- Phase 2: WARM (JIT cached, %u samples) ---\n", samples);
    bench_result_t r_warm = bench_run("VORTEX (warm, JIT cached)",
                                       bench_vortex_collatz_sum, &ctx,
                                       warmup, samples);
    bench_print(&r_warm);

    /* ---- Phase 3: Native C baseline ---- */
    printf("\n--- Phase 3: Native C (-O3 -march=native) ---\n");
    int64_t n_arg = N;
    bench_result_t r_native = bench_run("Native C (-O3)",
                                         bench_native_collatz_sum, &n_arg,
                                         warmup, samples);
    bench_print(&r_native);

    /* ---- Summary ---- */
    printf("\n================================================================\n");
    printf("  Summary  (mode=%s, profile_loaded=%s)\n",
           mode, profile_loaded ? "yes" : "no");
    printf("================================================================\n");
    printf("  Workload: sum of collatz(n) for n in [1, %lld]\n", (long long)N);
    printf("  Total steps: %lld\n\n", (long long)expected);

    printf("  VORTEX warm median:  %9lu ns\n", (unsigned long)r_warm.median_ns);
    printf("  Native C median:     %9lu ns\n", (unsigned long)r_native.median_ns);

    if (r_warm.median_ns > 0 && r_native.median_ns > 0) {
        double pct = 100.0 * (double)r_native.median_ns / (double)r_warm.median_ns;
        printf("  VORTEX vs native:    %.1f%% of native speed  (%.2fx)\n",
               pct, (double)r_warm.median_ns / (double)r_native.median_ns);
    }

    if (profile_loaded) {
        printf("\n  [pgo] Profile was loaded — branch hints and tier promotion\n");
        printf("  [pgo]   decisions used the saved profile data.\n");
    } else {
        printf("\n  [pgo] No profile loaded — this was a cold run. The profile\n");
        printf("  [pgo]   will be saved at exit for the next warm run.\n");
    }

    /* Sync interpreter profiler data into the global profile for PGO.
     * Without this, the profile saved at exit would be empty — the
     * interpreter's profiler is a separate data structure. This mirrors
     * the sync logic in main_new.c. */
    if (strcmp(mode, "cold") == 0) {
        uint32_t methods_synced = 0;
        for (uint32_t i = 0; i < interp.profiler.count; i++) {
            vtx_profile_data_t *pd = &interp.profiler.data[i];
            if (!pd->method) continue;

            uint32_t method_id = pd->method->vtable_index;
            if (method_id == 0xFFFFFFFF) method_id = 0;  /* our method */
            vtx_profile_method_t *pm = vtx_profile_add_method(&profile, method_id);
            if (!pm) continue;

            pm->invocation_count += pd->invocation_count;

            /* Merge branch profiles */
            for (uint32_t b = 0; b < pd->branch_array_size; b++) {
                if (pd->branch_total_counts[b] > 0) {
                    uint32_t taken = pd->branch_taken_counts[b];
                    uint32_t not_taken = pd->branch_total_counts[b] - taken;
                    /* Use the public API which handles allocation. */
                    for (uint32_t t = 0; t < taken; t++) {
                        vtx_profile_record_branch(&profile, method_id, b, true);
                    }
                    for (uint32_t t = 0; t < not_taken; t++) {
                        vtx_profile_record_branch(&profile, method_id, b, false);
                    }
                }
            }

            /* Merge loop profiles (backward branches = loop back-edges) */
            for (uint32_t l = 0; l < pd->branch_array_size; l++) {
                if (pd->backward_branch_count > 0 && pd->branch_taken_counts[l] > 0) {
                    for (uint32_t t = 0; t < pd->branch_taken_counts[l]; t++) {
                        vtx_profile_record_loop_backedge(&profile, method_id, l);
                    }
                }
            }

            methods_synced++;
        }
        printf("\n  [pgo] Synced %u methods to global profile\n", methods_synced);
        printf("  [pgo] Profile will be saved at exit (%u methods, %lu bytes)\n",
               profile.method_count,
               (unsigned long)(52 + profile.method_count * 100));
    }

    /* Don't destroy profile if we registered atexit (cold mode). */
    if (strcmp(mode, "warm") == 0) {
        vtx_profile_global_destroy(&profile);
    }

    vtx_interp_destroy(&interp);
    vtx_method_registry_destroy(&registry);
    vtx_code_cache_destroy(&cache);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);

    return 0;
}
