/* Smaller smoke test: just one bench at a time, no JIT. */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "runtime/vortex_runtime.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"

#include "bench_v8_bytecode.h"

int main(int argc, char **argv) {
    bench_gcd_loop_init_consts();

    const uint8_t *code = BENCH_GCD_LOOP_CODE;
    size_t code_len = BENCH_GCD_LOOP_CODE_LEN;
    uint8_t *hc = malloc(code_len);
    memcpy(hc, code, code_len);

    vtx_bytecode_t bc = {
        .code = hc,
        .length = code_len,
        .constant_pool = BENCH_GCD_LOOP_CONSTS,
        .constant_count = BENCH_GCD_LOOP_NCONSTS,
        .max_locals = BENCH_GCD_LOOP_LOCALS,
        .max_stack = BENCH_GCD_LOOP_STACK,
    };

    int64_t N = (argc > 1) ? atol(argv[1]) : 10;
    printf("running loop_sum(N=%ld) ...\n", (long)N); fflush(stdout);

    vtx_runtime_t rt;
    if (vtx_runtime_create(&rt) != 0) {
        fprintf(stderr, "vtx_runtime_create failed\n");
        return 1;
    }
    /* NO JIT */
    printf("runtime created. running...\n"); fflush(stdout);

    vtx_value_t args[] = { vtx_make_smi(N) };
    vtx_value_t r = vtx_runtime_run_with_args(&rt, &bc, args, 1);
    int64_t got = vtx_is_smi(r) ? vtx_smi_value(r) : -1;
    printf("got = %ld\n", (long)got);
    vtx_runtime_destroy(&rt);
    free(hc);
    return 0;
}
