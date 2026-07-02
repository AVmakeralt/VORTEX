/* test_deopt.c — Trigger actual deopt and see if it survives */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <setjmp.h>
#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "compile/pipeline.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "interp/dispatch.h"
#include "assembler.h"
#include "deopt/side_table.h"
#include "deopt/frame_state.h"

static void alarm_handler(int sig) { _exit(2); }
static jmp_buf crash_jmp;
static void crash_handler(int sig) { longjmp(crash_jmp, 1); }

typedef vtx_value_t (*entry_t)(const vtx_method_desc_t*,void*,void*,vtx_value_t*,uint32_t);

static entry_t compile(const char *prog, int argc, int tier) {
    vtx_assembler_t *a = calloc(1, sizeof(*a));
    vtx_arena_t *ar = calloc(1, sizeof(*ar));
    vtx_type_system_t *ts = calloc(1, sizeof(*ts));
    vtx_gc_t *gc = calloc(1, sizeof(*gc));
    vtx_graph_t *g = calloc(1, sizeof(*g));
    vtx_code_cache_t *c = calloc(1, sizeof(*c));
    vtx_method_registry_t *r = calloc(1, sizeof(*r));
    vtx_method_desc_t *m = calloc(1, sizeof(*m));
    vtx_bytecode_t *bc = calloc(1, sizeof(*bc));
    vtx_asm_init(a); vtx_asm_program(a, prog); *bc = vtx_asm_emit(a);
    vtx_arena_init(ar); vtx_type_system_init(ts); vtx_gc_init(gc, ts, VTX_GC_GENERATIONAL);
    vtx_graph_init(g, argc);
    m->name = "f"; m->signature = "(I)I"; m->bytecode = bc; m->arg_count = argc; m->is_virtual = false;
    vtx_graph_build(g, bc, m, ar);
    vtx_pipeline_config_t cfg = (tier == 3) ? vtx_pipeline_config_t3() : vtx_pipeline_config_t2();
    vtx_code_cache_init(c, 1<<20); vtx_method_registry_init(r, ar);
    cfg.code_cache = c; cfg.method_registry = r; cfg.method = m;
    vtx_compile_result_t res; memset(&res, 0, sizeof(res));
    int rc = vtx_pipeline_run(g, &cfg, ar, &res);
    if (rc != 0 || !res.success || m->compiled_code == NULL) {
        fprintf(stderr, "compile failed rc=%d success=%d\n", rc, res.success);
        return NULL;
    }
    return (entry_t)m->compiled_code;
}

int main(void) {
    signal(SIGALRM, alarm_handler);
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);

    /* Simple loop: sum 1..n. This compiles fine and has no guards
     * that can fail — but let's see what happens when we run it. */
    const char *prog =
        ".method sum (I)I\n.arg_count 1\n.max_locals 3\n.max_stack 4\n"
        "load_const_int 0\nstore_local 1\n"
        "loop:\nload_local 0\nload_const_int 0\nicmp_le\nif_true done\n"
        "load_local 1\nload_local 0\niadd\nstore_local 1\n"
        "load_local 0\nload_const_int 1\nisub\nstore_local 0\n"
        "goto loop\ndone:\nload_local 1\nreturn_value\n";

    printf("=== Deopt Survival Test ===\n\n");
    fflush(stdout);

    entry_t e = compile(prog, 1, 2);
    if (!e) { printf("FAIL: compile\n"); return 1; }

    vtx_method_desc_t m; memset(&m, 0, sizeof(m)); m.name = "f";

    /* Test 1: Normal execution — should work */
    printf("Test 1: Normal execution sum(100)\n");
    fflush(stdout);
    {
        vtx_value_t v = vtx_make_smi(100);
        alarm(3);
        vtx_value_t r = e(&m, NULL, (void*)1, &v, 1);
        alarm(0);
        printf("  sum(100) = %lld (expected 5050) — %s\n",
               (long long)vtx_smi_value(r),
               vtx_smi_value(r) == 5050 ? "OK" : "WRONG");
    }

    /* Test 2: Run many times to check for state corruption */
    printf("\nTest 2: Repeated execution (1000 calls)\n");
    fflush(stdout);
    {
        int ok = 0, fail = 0;
        alarm(10);
        for (int i = 0; i < 1000; i++) {
            vtx_value_t v = vtx_make_smi(50);
            vtx_value_t r = e(&m, NULL, (void*)1, &v, 1);
            if (vtx_smi_value(r) == 1275) ok++;
            else fail++;
        }
        alarm(0);
        printf("  %d OK, %d FAIL out of 1000 calls\n", ok, fail);
    }

    /* Test 3: Call with different args each time (varying inputs) */
    printf("\nTest 3: Varying inputs (100 calls, different N)\n");
    fflush(stdout);
    {
        int ok = 0, fail = 0;
        alarm(10);
        for (int i = 1; i <= 100; i++) {
            vtx_value_t v = vtx_make_smi(i);
            vtx_value_t r = e(&m, NULL, (void*)1, &v, 1);
            int64_t expected = (int64_t)i * (i + 1) / 2;
            if (vtx_smi_value(r) == expected) ok++;
            else fail++;
        }
        alarm(0);
        printf("  %d OK, %d FAIL\n", ok, fail);
    }

    /* Test 4: Edge cases — zero, negative, large */
    printf("\nTest 4: Edge cases\n");
    fflush(stdout);
    {
        struct { int64_t input; int64_t expected; const char *desc; } cases[] = {
            { 0, 0, "sum(0)" },
            { 1, 1, "sum(1)" },
            { -1, 0, "sum(-1) — negative input" },
            { 1000, 500500, "sum(1000)" },
        };
        for (int i = 0; i < 4; i++) {
            vtx_value_t v = vtx_make_smi(cases[i].input);
            if (setjmp(crash_jmp) == 0) {
                alarm(3);
                vtx_value_t r = e(&m, NULL, (void*)1, &v, 1);
                alarm(0);
                int64_t got = vtx_smi_value(r);
                printf("  %s = %lld (expected %lld) — %s\n",
                       cases[i].desc, (long long)got,
                       (long long)cases[i].expected,
                       got == cases[i].expected ? "OK" : "WRONG");
            } else {
                printf("  %s — CRASHED (SIGSEGV/SIGABRT)\n", cases[i].desc);
            }
        }
    }

    /* Test 5: Heavy load — 100K calls to stress the JIT */
    printf("\nTest 5: Heavy load (100K calls)\n");
    fflush(stdout);
    {
        int ok = 0;
        alarm(30);
        for (int i = 0; i < 100000; i++) {
            vtx_value_t v = vtx_make_smi(10);
            vtx_value_t r = e(&m, NULL, (void*)1, &v, 1);
            if (vtx_smi_value(r) == 55) ok++;
        }
        alarm(0);
        printf("  %d/100000 calls returned correct result\n", ok);
    }

    /* Test 6: Now try a program with actual branching that could
     * potentially trigger guard-like behavior */
    printf("\nTest 6: Branchy program (collatz) — 1000 calls\n");
    fflush(stdout);
    {
        const char *col_prog =
            ".method collatz (I)I\n.arg_count 1\n.max_locals 3\n.max_stack 6\n"
            "load_const_int 0\nstore_local 1\n"
            "loop:\nload_local 0\nload_const_int 1\nicmp_eq\nif_true done\n"
            "load_local 0\nload_const_int 2\nimod\nif_false even\n"
            "load_local 0\nload_const_int 3\nimul\nload_const_int 1\niadd\nstore_local 0\n"
            "goto inc\neven:\nload_local 0\nload_const_int 2\nidiv\nstore_local 0\n"
            "inc:\nload_local 1\nload_const_int 1\niadd\nstore_local 1\n"
            "goto loop\ndone:\nload_local 1\nreturn_value\n";
        entry_t col = compile(col_prog, 1, 2);
        if (!col) { printf("  FAIL: compile collatz\n"); }
        else {
            int ok = 0;
            alarm(30);
            for (int i = 1; i <= 1000; i++) {
                vtx_value_t v = vtx_make_smi(i);
                vtx_value_t r = col(&m, NULL, (void*)1, &v, 1);
                /* Just check it doesn't crash — collatz(n) is hard to verify inline */
                if (vtx_is_smi(r)) ok++;
            }
            alarm(0);
            printf("  %d/1000 calls survived (returned valid SMI)\n", ok);
        }
    }

    printf("\n=== Summary ===\n");
    printf("If all tests pass, the JIT is stable for normal execution.\n");
    printf("Note: This tests execution stability, not deopt. VORTEX's deopt\n");
    printf("handler can find FrameState but the interpreter frame reconstruction\n");
    printf("is a stub — actual guard-failure deopt would need that completed.\n");
    return 0;
}
