/**
 * test_t1_crash.c — Minimal reproduction of the T1 JIT crash.
 *
 * Compiles a simple loop function through the T1 baseline JIT
 * and runs it, to find where the crash occurs.
 *
 * The function: fib_iter(n) — iterative fibonacci
 *   locals: 0=n, 1=a, 2=b, 3=i, 4=tmp
 *   bytecode: LOAD_CONST_INT/STORE_LOCAL × 3, then loop with
 *             ICMP_LT/IF_TRUE, IADD, GOTO, RETURN_VALUE
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <setjmp.h>

#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/arena.h"
#include "runtime/vortex_runtime.h"
#include "baseline/codegen.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "assembler.h"

typedef vtx_value_t (*jit_entry_t)(const vtx_method_desc_t *, void *, void *,
                                    vtx_value_t *, uint32_t);

static jmp_buf jmpbuf;
static volatile sig_atomic_t got_signal = 0;

static void handler(int sig) {
    got_signal = sig;
    longjmp(jmpbuf, 1);
}

/* Simple loop: count 0..N, return N (tests IF_TRUE/GOTO/RETURN_VALUE) */
static const char *PROG_LOOP =
    ".method count (I)I\n"
    ".arg_count 1\n"
    ".max_locals 3\n"
    ".max_stack 4\n"
    "load_const_int 0\nstore_local 1\n"
    "loop:\nload_local 1\nload_local 0\nicmp_lt\nif_true body\n"
    "load_local 1\nreturn_value\n"
    "body:\nload_local 1\nload_const_int 1\niadd\nstore_local 1\n"
    "goto loop\n";

/* fib_iter: iterative fibonacci */
static const char *PROG_FIB =
    ".method fib (I)I\n"
    ".arg_count 1\n"
    ".max_locals 5\n"
    ".max_stack 4\n"
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

int main(void) {
    signal(SIGSEGV, handler);
    signal(SIGBUS, handler);

    printf("=== T1 JIT Crash Reproduction ===\n\n");

    /* Build bytecode */
    vtx_assembler_t a;
    vtx_asm_init(&a);

    /* Test 1: Simple loop */
    printf("--- Test 1: Simple loop (count) ---\n");
    vtx_asm_program(&a, PROG_LOOP);
    vtx_bytecode_t bc_loop = vtx_asm_emit(&a);

    {
        vtx_arena_t arena; vtx_arena_init(&arena);
        vtx_type_system_t ts; vtx_type_system_init(&ts);
        vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
        vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 20);

        vtx_method_desc_t method = {
            .name = "count",
            .signature = "(I)I",
            .bytecode = &bc_loop,
            .arg_count = 1,
            .is_virtual = false,
            .vtable_index = 1,  /* non-zero, non-sentinel */
        };

        /* Compile through T1 baseline */
        vtx_compiled_code_t *code = vtx_baseline_compile(&method, NULL, &arena, &cache, NULL);
        printf("  compiled_code = %p\n", (void*)code);

        if (code) {
            /* Run it */
            if (setjmp(jmpbuf) != 0) {
                printf("  CRASH! signal=%d\n", got_signal);
                return 1;
            }
            jit_entry_t entry = (jit_entry_t)code;
            vtx_value_t arg = vtx_make_smi(10);
            printf("  calling count(10)...\n");
            vtx_value_t r = entry(&method, NULL, NULL, &arg, 1);
            int64_t got = vtx_is_smi(r) ? vtx_smi_value(r) : -1;
            printf("  count(10) = %ld (expected 10)\n", (long)got);
        } else {
            printf("  compilation FAILED\n");
        }
    }

    /* Test 2: fib_iter */
    printf("\n--- Test 2: fib_iter ---\n");
    vtx_asm_init(&a);
    vtx_asm_program(&a, PROG_FIB);
    vtx_bytecode_t bc_fib = vtx_asm_emit(&a);

    {
        vtx_arena_t arena; vtx_arena_init(&arena);
        vtx_type_system_t ts; vtx_type_system_init(&ts);
        vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
        vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 20);

        vtx_method_desc_t method = {
            .name = "fib",
            .signature = "(I)I",
            .bytecode = &bc_fib,
            .arg_count = 1,
            .is_virtual = false,
            .vtable_index = 2,
        };

        vtx_compiled_code_t *code = vtx_baseline_compile(&method, NULL, &arena, &cache, NULL);
        printf("  compiled_code = %p\n", (void*)code);

        if (code) {
            if (setjmp(jmpbuf) != 0) {
                printf("  CRASH! signal=%d\n", got_signal);
                return 1;
            }
            jit_entry_t entry = (jit_entry_t)code;
            for (int n = 0; n <= 10; n++) {
                vtx_value_t arg = vtx_make_smi(n);
                vtx_value_t r = entry(&method, NULL, NULL, &arg, 1);
                int64_t got = vtx_is_smi(r) ? vtx_smi_value(r) : -1;
                /* Expected: 0,1,1,2,3,5,8,13,21,34,55 */
                int64_t expected[] = {0,1,1,2,3,5,8,13,21,34,55};
                printf("  fib(%d) = %ld (expected %ld) %s\n",
                       n, (long)got, (long)expected[n],
                       got == expected[n] ? "OK" : "MISMATCH");
            }
        } else {
            printf("  compilation FAILED\n");
        }
    }

    printf("\n=== Done ===\n");
    return 0;
}
