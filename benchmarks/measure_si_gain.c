/**
 * measure_si_gain.c — Measure the superinstruction pre-decode gain
 * by running the same bytecode with and without the pre-decode pass.
 *
 * Builds two versions of each benchmark:
 *   1. Original bytecode (no pre-decode) → T0 baseline
 *   2. Pre-decoded bytecode (with superinstructions) → T0 + SI
 * Compares throughput.
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
#include "runtime/vortex_runtime.h"

/* C++ pre-decode pass (C-callable) */
extern vtx_bytecode_t* vtx_superinstruction_predecode(const vtx_bytecode_t* bc);
extern void vtx_superinstruction_free(vtx_bytecode_t* bc);

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#define SAMPLES 20

static uint64_t median(uint64_t *s, int n) {
    /* simple sort */
    for (int i = 0; i < n-1; i++)
        for (int j = i+1; j < n; j++)
            if (s[j] < s[i]) { uint64_t t = s[i]; s[i] = s[j]; s[j] = t; }
    return s[n/2];
}

/* Run a bytecode module N times and return median ns/call */
static uint64_t run_bench(const vtx_bytecode_t *bc, int64_t N, int iters, int warmup) {
    uint64_t samples[SAMPLES];
    int nsamp = SAMPLES;

    for (int s = 0; s < nsamp; s++) {
        vtx_runtime_t rt;
        vtx_runtime_create(&rt);
        /* No JIT — T0 interpreter only */

        /* warmup */
        for (int i = 0; i < warmup; i++) {
            vtx_value_t args[] = { vtx_make_smi(N + (i % 5)) };
            vtx_runtime_run_with_args(&rt, bc, args, 1);
        }
        /* measure */
        uint64_t t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            vtx_value_t args[] = { vtx_make_smi(N + (i % 5)) };
            vtx_runtime_run_with_args(&rt, bc, args, 1);
        }
        uint64_t t1 = now_ns();
        samples[s] = (t1 - t0) / (uint64_t)iters;
        vtx_runtime_destroy(&rt);
    }
    return median(samples, nsamp);
}

/* loop_sum bytecode: sum 0..N-1 */
static const uint8_t loop_sum_code[] = {
    0x06,0x00,0x00, 0x03,0x00,0x01,  /* LOAD_CONST_INT 0; STORE_LOCAL 1 (sum=0) */
    0x06,0x00,0x00, 0x03,0x00,0x02,  /* LOAD_CONST_INT 0; STORE_LOCAL 2 (i=0) */
    /* loop_top (PC=12) */
    0x02,0x00,0x02, 0x02,0x00,0x00,  /* LOAD_LOCAL 2; LOAD_LOCAL 0 */
    0x1F, 0x2A,0x00,0x1C,           /* ICMP_LT; IF_TRUE 28 */
    0x02,0x00,0x01, 0x30,           /* LOAD_LOCAL 1; RETURN_VALUE */
    /* body (PC=28) */
    0x02,0x00,0x01, 0x02,0x00,0x02, 0x0D, 0x03,0x00,0x01, /* sum += i */
    0x02,0x00,0x02, 0x06,0x00,0x01, 0x0D, 0x03,0x00,0x02, /* i++ */
    0x29,0x00,0x0C,                  /* GOTO 12 */
};

int main(void) {
    printf("================================================================\n");
    printf("  Superinstruction Pre-Decode Gain Measurement\n");
    printf("  Compares T0 interpreter: original vs pre-decoded bytecode\n");
    printf("  %d samples, median reported\n", SAMPLES);
    printf("================================================================\n\n");

    /* Setup constants */
    vtx_value_t consts[] = { vtx_make_smi(0), vtx_make_smi(1) };

    vtx_bytecode_t bc_orig = {
        .code = (uint8_t*)loop_sum_code,
        .length = sizeof(loop_sum_code),
        .constant_pool = consts,
        .constant_count = 2,
        .max_locals = 3,
        .max_stack = 4,
    };

    /* Pre-decode the bytecode */
    vtx_bytecode_t *bc_fused = vtx_superinstruction_predecode(&bc_orig);
    printf("  Original bytecode: %zu bytes\n", sizeof(loop_sum_code));
    if (bc_fused) {
        printf("  Pre-decoded bytecode: %zu bytes (delta=%+ld)\n",
               bc_fused->length,
               (long)bc_fused->length - (long)sizeof(loop_sum_code));
    } else {
        printf("  Pre-decode: NOT linked (libvortex_cpp missing)\n");
        return 1;
    }
    printf("\n");

    /* Benchmark both */
    int64_t N = 10000;
    int iters = 50;
    int warmup = 10;

    printf("  Running loop_sum(%ld) %d iterations x %d samples...\n",
           (long)N, iters, SAMPLES);

    uint64_t orig_ns = run_bench(&bc_orig, N, iters, warmup);
    uint64_t fused_ns = run_bench(bc_fused, N, iters, warmup);

    double orig_ms = (double)orig_ns / 1e6;
    double fused_ms = (double)fused_ns / 1e6;
    double speedup = (double)orig_ns / (double)fused_ns;
    double gain_pct = (1.0 - (double)fused_ns / (double)orig_ns) * 100.0;

    printf("\n  Results:\n");
    printf("    Original (no SI):    %8.3f ms/call  (%lu ns)\n", orig_ms, orig_ns);
    printf("    Pre-decoded (SI):    %8.3f ms/call  (%lu ns)\n", fused_ms, fused_ns);
    printf("    Speedup:             %.2fx\n", speedup);
    printf("    Gain:                %.1f%% faster\n", gain_pct);

    printf("\n  The superinstruction pre-decode pass eliminates one dispatch\n");
    printf("  + one operand read per fused pair. On loop_sum, the LOAD_CONST_INT\n");
    printf("  + IADD and LOAD_LOCAL + LOAD_LOCAL pairs are fused into single\n");
    printf("  superinstructions, reducing interpreter overhead.\n");
    printf("================================================================\n");

    vtx_superinstruction_free(bc_fused);
    return 0;
}
