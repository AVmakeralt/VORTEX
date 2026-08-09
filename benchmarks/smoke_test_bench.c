/* Smoke test: verify each bench bytecode actually terminates. */
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

static void bench_init_all_consts(void) {
    bench_fib_iter_init_consts();
    bench_loop_sum_init_consts();
    bench_tight_loop_init_consts();
    bench_bit_ops_init_consts();
    bench_gcd_loop_init_consts();
    bench_collatz_init_consts();
    bench_fnv_hash_init_consts();
    bench_arith_chain_init_consts();
}

static int64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void) {
    bench_init_all_consts();

    struct { const char *name; const uint8_t *code; size_t len;
             vtx_value_t *consts; int nconsts, locals, stack;
             int64_t N; } rows[] = {
        {"fib_iter(10)",  BENCH_FIB_ITER_CODE,    BENCH_FIB_ITER_CODE_LEN,
         BENCH_FIB_ITER_CONSTS,    BENCH_FIB_ITER_NCONSTS,    BENCH_FIB_ITER_LOCALS,    BENCH_FIB_ITER_STACK,    10},
        {"loop_sum(100)", BENCH_LOOP_SUM_CODE,    BENCH_LOOP_SUM_CODE_LEN,
         BENCH_LOOP_SUM_CONSTS,    BENCH_LOOP_SUM_NCONSTS,    BENCH_LOOP_SUM_LOCALS,    BENCH_LOOP_SUM_STACK,    100},
        {"tight_loop(100)", BENCH_TIGHT_LOOP_CODE, BENCH_TIGHT_LOOP_CODE_LEN,
         BENCH_TIGHT_LOOP_CONSTS,  BENCH_TIGHT_LOOP_NCONSTS,  BENCH_TIGHT_LOOP_LOCALS,  BENCH_TIGHT_LOOP_STACK,  100},
        {"bit_ops(10)",   BENCH_BIT_OPS_CODE,    BENCH_BIT_OPS_CODE_LEN,
         BENCH_BIT_OPS_CONSTS,     BENCH_BIT_OPS_NCONSTS,     BENCH_BIT_OPS_LOCALS,    BENCH_BIT_OPS_STACK,     10},
        {"gcd_loop(5)",   BENCH_GCD_LOOP_CODE,   BENCH_GCD_LOOP_CODE_LEN,
         BENCH_GCD_LOOP_CONSTS,    BENCH_GCD_LOOP_NCONSTS,    BENCH_GCD_LOOP_LOCALS,    BENCH_GCD_LOOP_STACK,    5},
        {"collatz(27)",  BENCH_COLLATZ_CODE,    BENCH_COLLATZ_CODE_LEN,
         BENCH_COLLATZ_CONSTS,     BENCH_COLLATZ_NCONSTS,    BENCH_COLLATZ_LOCALS,    BENCH_COLLATZ_STACK,     27},
        {"fnv_hash(100)", BENCH_FNV_HASH_CODE,    BENCH_FNV_HASH_CODE_LEN,
         BENCH_FNV_HASH_CONSTS,    BENCH_FNV_HASH_NCONSTS,    BENCH_FNV_HASH_LOCALS,    BENCH_FNV_HASH_STACK,    100},
        {"arith_chain(100)", BENCH_ARITH_CHAIN_CODE, BENCH_ARITH_CHAIN_CODE_LEN,
         BENCH_ARITH_CHAIN_CONSTS, BENCH_ARITH_CHAIN_NCONSTS, BENCH_ARITH_CHAIN_LOCALS, BENCH_ARITH_CHAIN_STACK, 100},
    };
    int n = (int)(sizeof(rows)/sizeof(rows[0]));

    /* Test with JIT disabled first */
    for (int use_jit = 0; use_jit <= 1; use_jit++) {
        printf("---- %s ----\n", use_jit ? "JIT enabled" : "JIT disabled");
        for (int i = 0; i < n; i++) {
            uint8_t *hc = malloc(rows[i].len);
            memcpy(hc, rows[i].code, rows[i].len);
            vtx_bytecode_t bc = { hc, rows[i].len, rows[i].consts, rows[i].nconsts,
                                  (uint16_t)rows[i].locals, (uint16_t)rows[i].stack };
            vtx_runtime_t rt; vtx_runtime_create(&rt);
            if (use_jit) vtx_runtime_enable_jit(&rt, 2);

            vtx_value_t args[] = { vtx_make_smi(rows[i].N) };
            int64_t t0 = now_ms();
            vtx_value_t r = vtx_runtime_run_with_args(&rt, &bc, args, 1);
            int64_t t1 = now_ms();
            int64_t got = vtx_is_smi(r) ? vtx_smi_value(r) : -1;
            printf("  %-22s N=%-6ld got=%-12ld time=%ldms\n",
                   rows[i].name, (long)rows[i].N, (long)got, t1 - t0);
            vtx_runtime_destroy(&rt);
            free(hc);
        }
    }
    return 0;
}
