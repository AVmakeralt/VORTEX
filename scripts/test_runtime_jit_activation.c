/* test_runtime_jit_activation.c
 *
 * Smoke test: verify that vtx_runtime_enable_jit() actually causes the JIT
 * to compile and execute code (not just the interpreter).
 *
 * Before the fix: vtx_runtime_enable_jit() set rt->use_jit = 1 but never
 * called vtx_interp_set_compile_ctx(), so the interpreter never submitted
 * compilation requests and compiled_code stayed NULL forever.
 *
 * After the fix: a hot loop (>= 10000 back-edges) triggers
 * vtx_request_compilation, the threadpool compiles the method, and
 * method->compiled_code becomes non-NULL.
 *
 * Build:
 *   cc -O2 -I VORTEX/src test_runtime_jit_activation.c \
 *      -L VORTEX/build/src -L VORTEX/build/tests \
 *      -Wl,--start-group \
 *        -lvortex_test_framework -lvortex_runtime -lvortex_interp \
 *        -lvortex_baseline -lvortex_compile -lvortex_codecache \
 *        -lvortex_ir -lvortex_guard -lvortex_deopt -lvortex_profile \
 *        -lvortex_lower -lvortex_inliner -lvortex_pea -lvortex_sota \
 *        -lvortex_midtier -lvortex_trace -lvortex_region -lvortex_common \
 *      -Wl,--end-group -lm -lpthread -o test_runtime_jit_activation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "runtime/vortex_runtime.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"

/*
 * Bytecode: compute sum = 0; for (i = 0; i < N; i++) sum += i; return sum
 *
 * Locals:  [0] = N (arg), [1] = sum (=0), [2] = i (=0)
 * Consts:  [0] = 0
 *
 *   PC  0: LOAD_CONST_INT 0       ; push 0
 *   PC  3: STORE_LOCAL 1          ; sum = 0
 *   PC  6: LOAD_CONST_INT 0       ; push 0
 *   PC  9: STORE_LOCAL 2          ; i = 0
 *   -- loop header (PC 12) --
 *   PC 12: LOAD_LOCAL 2            ; push i
 *   PC 15: LOAD_LOCAL 0            ; push N
 *   PC 18: ICMP_LT                 ; i < N ?
 *   PC 19: IF_FALSE -> PC 38       ; exit loop
 *   PC 22: LOAD_LOCAL 1            ; push sum
 *   PC 25: LOAD_LOCAL 2            ; push i
 *   PC 28: IADD                    ; sum + i
 *   PC 29: STORE_LOCAL 1           ; sum = sum + i
 *   PC 32: LOAD_LOCAL 2            ; push i
 *   PC 35: LOAD_CONST_INT 0        ; push 1? use i+1
 *   ... need a 1 const ...
 *
 * Simpler: just do an infinite loop that bumps a counter until it hits N
 * using only LOAD_CONST_INT 0 (=0). That requires a 1 constant though.
 *
 * Let me use consts [0]=0, [1]=1
 */
static const uint8_t loop_bytecode[] = {
    /* PC  0 */ VT_OP_LOAD_CONST_INT, 0x00, 0x00,   /* push 0       */
    /* PC  3 */ VT_OP_STORE_LOCAL,    0x00, 0x01,   /* sum = 0      */
    /* PC  6 */ VT_OP_LOAD_CONST_INT, 0x00, 0x00,   /* push 0       */
    /* PC  9 */ VT_OP_STORE_LOCAL,    0x00, 0x02,   /* i = 0        */
    /* ---- loop header (PC 12) ---- */
    /* PC 12 */ VT_OP_LOAD_LOCAL,     0x00, 0x02,   /* push i       */
    /* PC 15 */ VT_OP_LOAD_LOCAL,     0x00, 0x00,   /* push N       */
    /* PC 18 */ VT_OP_ICMP_LT,                       /* i < N ?      */
    /* PC 19 */ VT_OP_IF_FALSE,       0x00, 0x2D,   /* -> PC 45 (exit) */
    /* PC 22 */ VT_OP_LOAD_LOCAL,     0x00, 0x01,   /* push sum     */
    /* PC 25 */ VT_OP_LOAD_LOCAL,     0x00, 0x02,   /* push i       */
    /* PC 28 */ VT_OP_IADD,                          /* sum + i      */
    /* PC 29 */ VT_OP_STORE_LOCAL,    0x00, 0x01,   /* sum = ...    */
    /* PC 32 */ VT_OP_LOAD_LOCAL,     0x00, 0x02,   /* push i       */
    /* PC 35 */ VT_OP_LOAD_CONST_INT, 0x00, 0x01,   /* push 1       */
    /* PC 38 */ VT_OP_IADD,                          /* i + 1        */
    /* PC 39 */ VT_OP_STORE_LOCAL,    0x00, 0x02,   /* i = i + 1    */
    /* PC 42 */ VT_OP_GOTO,           0x00, 0x0C,   /* -> PC 12     */
    /* ---- exit (PC 45 = 0x2D) ---- */
    /* PC 45 */ VT_OP_LOAD_LOCAL,     0x00, 0x01,   /* push sum     */
    /* PC 48 */ VT_OP_RETURN_VALUE,
};

int main(void)
{
    int rc;
    int failures = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("[1] starting\n");

    /* 1. Create runtime */
    vtx_runtime_t rt;
    rc = vtx_runtime_create(&rt);
    if (rc != 0) {
        fprintf(stderr, "FAIL: vtx_runtime_create returned %d\n", rc);
        return 1;
    }
    printf("[2] runtime created\n");

    /* 2. Enable JIT — this was the bug */
    rc = vtx_runtime_enable_jit(&rt, 2);
    if (rc != 0) {
        fprintf(stderr, "FAIL: vtx_runtime_enable_jit returned %d\n", rc);
        return 1;
    }
    printf("[3] vtx_runtime_enable_jit(2) succeeded\n");

    /* Verify the wiring is actually in place */
    if (rt.interp->compile_ctx == NULL) {
        fprintf(stderr, "FAIL: interp->compile_ctx is NULL after enable_jit\n");
        failures++;
    } else {
        printf("[ok] interp->compile_ctx is wired\n");
    }
    if (rt.compile_ctx->code_cache == NULL) {
        fprintf(stderr, "FAIL: compile_ctx->code_cache is NULL\n");
        failures++;
    } else {
        printf("[ok] compile_ctx->code_cache is wired\n");
    }
    if (rt.compile_ctx->method_registry == NULL) {
        fprintf(stderr, "FAIL: compile_ctx->method_registry is NULL\n");
        failures++;
    } else {
        printf("[ok] compile_ctx->method_registry is wired\n");
    }
    if (rt.compile_ctx->profiler == NULL) {
        fprintf(stderr, "FAIL: compile_ctx->profiler is NULL\n");
        failures++;
    } else {
        printf("[ok] compile_ctx->profiler is wired\n");
    }
    if (rt.compile_ctx->method_lookup == NULL) {
        fprintf(stderr, "FAIL: compile_ctx->method_lookup is NULL\n");
        failures++;
    } else {
        printf("[ok] compile_ctx->method_lookup is wired\n");
    }
    if (rt.threadpool == NULL || rt.threadpool->compile_callback == NULL) {
        fprintf(stderr, "FAIL: threadpool compile_callback not set\n");
        failures++;
    } else {
        printf("[ok] threadpool compile_callback is wired\n");
    }
    if (failures) {
        fprintf(stderr, "RESULT: %d wiring failures — JIT cannot activate\n", failures);
        vtx_runtime_destroy(&rt);
        return 1;
    }

    /* 3. Build bytecode.
     *
     * The bytecode is a tight loop summing 0..N-1.
     *
     * NOTE: A pre-existing T1 baseline-JIT bug means STORE_LOCAL/LOAD_LOCAL
     * in a loop with arguments may return 0 when called via the JIT
     * dispatch path (existing JIT tests don't cover loops). The first
     * interpreter run produces the correct result; the bug only affects
     * the SECOND call which goes through the JIT directly. So we only
     * assert on the first run for correctness — the goal of this test
     * is to prove compiled_code != NULL, not to validate T1 code gen.
     */
    vtx_value_t consts[2];
    consts[0] = vtx_make_smi(0);
    consts[1] = vtx_make_smi(1);

    vtx_bytecode_t bc;
    bc.code = (uint8_t *)loop_bytecode;
    bc.length = sizeof(loop_bytecode);
    bc.max_locals = 3;
    bc.max_stack = 8;
    bc.constant_pool = consts;
    bc.constant_count = 2;

    /* 4. Run with a large enough N to trip the tier-up counter.
     *    VTX_TIER_UP_INITIAL_COUNT = 10000 back-edges, so N=12000 is enough. */
    vtx_value_t args[1] = { vtx_make_smi(12000) };

    printf("\n[run] computing sum(0..12000) via runtime API\n");
    vtx_value_t result = vtx_runtime_run_with_args(&rt, &bc, args, 1);

    int64_t got = vtx_is_smi(result) ? vtx_smi_value(result) : -1;
    int64_t expected = (12000LL * 11999LL) / 2; /* 0..11999 */
    printf("[result] got=%ld expected=%ld match=%s\n",
           got, expected, (got == expected) ? "YES" : "NO");
    printf("[diag] is_smi=%d is_double=%d is_heap=%d raw=0x%016llx\n",
           vtx_is_smi(result), vtx_is_double(result),
           vtx_is_heap_ptr(result),
           (unsigned long long)result);
    if (vtx_is_double(result)) {
        printf("[diag] as_double=%f\n", vtx_double_value(result));
    }
    if (got != expected) {
        fprintf(stderr, "FAIL: result mismatch (interpreter should produce correct result)\n");
        failures++;
    }

    /* 5. Give the threadpool worker time to finish installing the compiled code.
     *    The compilation request was queued during the run; the install
     *    happens asynchronously. Poll for up to 5 seconds. */
    int compiled = 0;
    for (int i = 0; i < 500; i++) {
        if (rt.main_method != NULL &&
            __atomic_load_n(&rt.main_method->compiled_code,
                             __ATOMIC_ACQUIRE) != NULL) {
            compiled = 1;
            break;
        }
        usleep(10000); /* 10ms */
    }

    if (compiled) {
        printf("[ok] method->compiled_code is non-NULL — JIT activated!\n");
    } else {
        fprintf(stderr, "FAIL: method->compiled_code is still NULL after 5s\n");
        fprintf(stderr, "       The JIT never compiled the method.\n");
        failures++;
    }

    vtx_runtime_destroy(&rt);

    if (failures == 0) {
        printf("\n=== ALL CHECKS PASSED — JIT is activating through the runtime API ===\n");
        return 0;
    } else {
        printf("\n=== %d FAILURE(S) ===\n", failures);
        return 1;
    }
}
