/* Test loop_sum with JIT enabled. */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include "runtime/vortex_runtime.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"

#include "bench_v8_bytecode.h"

int main(int argc, char **argv) {
    bench_loop_sum_init_consts();

    uint8_t *hc = malloc(BENCH_LOOP_SUM_CODE_LEN);
    memcpy(hc, BENCH_LOOP_SUM_CODE, BENCH_LOOP_SUM_CODE_LEN);

    vtx_bytecode_t bc = {
        .code = hc,
        .length = BENCH_LOOP_SUM_CODE_LEN,
        .constant_pool = BENCH_LOOP_SUM_CONSTS,
        .constant_count = BENCH_LOOP_SUM_NCONSTS,
        .max_locals = BENCH_LOOP_SUM_LOCALS,
        .max_stack = BENCH_LOOP_SUM_STACK,
    };

    int64_t N = (argc > 1) ? atol(argv[1]) : 100;
    printf("running loop_sum(N=%ld) with JIT enabled ...\n", (long)N); fflush(stdout);

    vtx_runtime_t rt;
    if (vtx_runtime_create(&rt) != 0) { fprintf(stderr, "create failed\n"); return 1; }
    if (vtx_runtime_enable_jit(&rt, 2) != 0) { fprintf(stderr, "enable_jit failed\n"); return 1; }
    printf("runtime+JIT created.\n"); fflush(stdout);

    /* First few runs (warmup + JIT trigger) */
    for (int i = 0; i < 5; i++) {
        printf("  run %d ... ", i); fflush(stdout);
        vtx_value_t args[] = { vtx_make_smi(N) };
        vtx_value_t r = vtx_runtime_run_with_args(&rt, &bc, args, 1);
        int64_t got = vtx_is_smi(r) ? vtx_smi_value(r) : -1;
        printf("got=%ld\n", (long)got); fflush(stdout);
    }
    vtx_runtime_destroy(&rt);
    free(hc);
    return 0;
}
