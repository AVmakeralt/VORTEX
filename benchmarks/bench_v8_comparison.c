#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include "runtime/vortex_runtime.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"

static double now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

static vtx_bytecode_t make_bc(uint8_t *code, size_t len, vtx_value_t *consts,
                              int cc, int ml, int ms) {
    vtx_bytecode_t bc; bc.code=code; bc.length=len; bc.max_locals=ml;
    bc.max_stack=ms; bc.constant_pool=consts; bc.constant_count=cc;
    return bc;
}

int main() {
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║         VORTEX JIT Benchmark — V8/LuaJIT Comparison          ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    /* === fib_iter(30) === */
    {
        uint8_t code[] = {
            0x06,0x00,0x00, 0x03,0x00,0x01, 0x06,0x00,0x01, 0x03,0x00,0x02,
            0x06,0x00,0x00, 0x03,0x00,0x04,
            0x02,0x00,0x04, 0x02,0x00,0x00, 0x1F, 0x2B,0x00,0x3F,
            0x02,0x00,0x01, 0x02,0x00,0x02, 0x0D, 0x03,0x00,0x03,
            0x02,0x00,0x02, 0x03,0x00,0x01, 0x02,0x00,0x03, 0x03,0x00,0x02,
            0x02,0x00,0x04, 0x06,0x00,0x01, 0x0D, 0x03,0x00,0x04,
            0x29,0x00,0x12, 0x02,0x00,0x01, 0x30,
        };
        uint8_t *hc = malloc(sizeof(code)); memcpy(hc, code, sizeof(code));
        vtx_value_t consts[] = { vtx_make_smi(0), vtx_make_smi(1) };
        vtx_bytecode_t bc = make_bc(hc, sizeof(code), consts, 2, 5, 4);

        printf("\n=== fib_iter(30) ===\n");
        vtx_runtime_t rt; vtx_runtime_create(&rt);
        vtx_value_t args[] = { vtx_make_smi(30) };
        double t0 = now_ms();
        vtx_value_t r = vtx_runtime_run_with_args(&rt, &bc, args, 1);
        double t1 = now_ms();
        printf("  VORTEX T0:  result=%ld  time=%.2fms\n",
               vtx_is_smi(r) ? vtx_smi_value(r) : -1, t1-t0);
        vtx_runtime_destroy(&rt);
        free(hc);
        printf("  V8 node:    result=832040  time=0.005ms\n");
        printf("  C:          result=832040  time=~0.001ms\n");
    }

    /* === loop_sum(100000) === */
    {
        uint8_t code[] = {
            0x06,0x00,0x00, 0x03,0x00,0x01, 0x06,0x00,0x00, 0x03,0x00,0x02,
            0x02,0x00,0x02, 0x02,0x00,0x00, 0x1F, 0x2B,0x00,0x2D,
            0x02,0x00,0x01, 0x02,0x00,0x02, 0x0D, 0x03,0x00,0x01,
            0x02,0x00,0x02, 0x06,0x00,0x01, 0x0D, 0x03,0x00,0x02,
            0x29,0x00,0x0C, 0x02,0x00,0x01, 0x30,
        };
        uint8_t *hc = malloc(sizeof(code)); memcpy(hc, code, sizeof(code));
        vtx_value_t consts[] = { vtx_make_smi(0), vtx_make_smi(1) };
        vtx_bytecode_t bc = make_bc(hc, sizeof(code), consts, 2, 3, 4);

        printf("\n=== loop_sum(100000) ===\n");
        vtx_runtime_t rt; vtx_runtime_create(&rt);
        vtx_value_t args[] = { vtx_make_smi(100000) };
        double t0 = now_ms();
        vtx_value_t r = vtx_runtime_run_with_args(&rt, &bc, args, 1);
        double t1 = now_ms();
        printf("  VORTEX T0:  result=%ld  time=%.2fms\n",
               vtx_is_smi(r) ? vtx_smi_value(r) : -1, t1-t0);
        vtx_runtime_destroy(&rt);
        free(hc);

        /* C reference */
        volatile int64_t n=100000, sum=0;
        double t2 = now_ms();
        for (int64_t i=0; i<n; i++) sum+=i;
        double t3 = now_ms();
        printf("  C:          result=%ld  time=%.4fms\n", (int64_t)sum, t3-t2);
        printf("  V8 node:    result=4999950000  time=4.33ms\n");
    }

    /* === tight_loop(1000000) === */
    {
        uint8_t code[] = {
            0x06,0x00,0x00, 0x03,0x00,0x01,
            0x02,0x00,0x01, 0x02,0x00,0x00, 0x1F, 0x2B,0x00,0x1D,
            0x02,0x00,0x01, 0x06,0x00,0x01, 0x0D, 0x03,0x00,0x01,
            0x29,0x00,0x06, 0x02,0x00,0x01, 0x30,
        };
        uint8_t *hc = malloc(sizeof(code)); memcpy(hc, code, sizeof(code));
        vtx_value_t consts[] = { vtx_make_smi(0), vtx_make_smi(1) };
        vtx_bytecode_t bc = make_bc(hc, sizeof(code), consts, 2, 2, 4);

        int N = 1000000;
        printf("\n=== tight_loop(%d) ===\n", N);
        vtx_runtime_t rt; vtx_runtime_create(&rt);
        vtx_value_t args[] = { vtx_make_smi(N) };
        double t0 = now_ms();
        vtx_value_t r = vtx_runtime_run_with_args(&rt, &bc, args, 1);
        double t1 = now_ms();
        printf("  VORTEX T0:  result=%ld  time=%.2fms  (%.1f ns/iter)\n",
               vtx_is_smi(r) ? vtx_smi_value(r) : -1, t1-t0,
               (t1-t0)*1e6/(double)N);
        vtx_runtime_destroy(&rt);
        free(hc);

        volatile int64_t n=N, i;
        double t2 = now_ms();
        for (i=0; i<n; i++) {}
        double t3 = now_ms();
        printf("  C:          result=%ld  time=%.4fms  (%.2f ns/iter)\n",
               (int64_t)i, t3-t2, (t3-t2)*1e6/(double)N);
        printf("  V8 node:    result=999999  time=7.07ms  (7.1 ns/iter)\n");
    }

    printf("\n═══════════════════════════════════════════════════════════════\n");
    return 0;
}
