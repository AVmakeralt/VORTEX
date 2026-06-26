/*
 * Crash diagnostic: trace a simple loop (sum(3)) to find where it crashes.
 * Uses SIGSEGV handler to catch the crash and dump register state.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <ucontext.h>

#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "ir/verify.h"
#include "ir/schedule.h"
#include "lower/isel.h"
#include "lower/regalloc.h"
#include "lower/emit.h"
#include "compile/pipeline.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "assembler.h"

static void segfault_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    fflush(stdout);
    fprintf(stderr, "\n=== SEGFAULT ===\n");
    fprintf(stderr, "Fault address: %p\n", si->si_addr);
    fprintf(stderr, "RIP: 0x%llx\n", (unsigned long long)uc->uc_mcontext.gregs[REG_RIP]);
    fprintf(stderr, "RAX: 0x%llx  RBX: 0x%llx  RCX: 0x%llx\n",
           (unsigned long long)uc->uc_mcontext.gregs[REG_RAX],
           (unsigned long long)uc->uc_mcontext.gregs[REG_RBX],
           (unsigned long long)uc->uc_mcontext.gregs[REG_RCX]);
    fprintf(stderr, "RDX: 0x%llx  RSI: 0x%llx  RDI: 0x%llx\n",
           (unsigned long long)uc->uc_mcontext.gregs[REG_RDX],
           (unsigned long long)uc->uc_mcontext.gregs[REG_RSI],
           (unsigned long long)uc->uc_mcontext.gregs[REG_RDI]);
    fprintf(stderr, "R8:  0x%llx  R9:  0x%llx  R10: 0x%llx  R11: 0x%llx\n",
           (unsigned long long)uc->uc_mcontext.gregs[REG_R8],
           (unsigned long long)uc->uc_mcontext.gregs[REG_R9],
           (unsigned long long)uc->uc_mcontext.gregs[REG_R10],
           (unsigned long long)uc->uc_mcontext.gregs[REG_R11]);
    fprintf(stderr, "R12: 0x%llx  R13: 0x%llx  R14: 0x%llx  R15: 0x%llx\n",
           (unsigned long long)uc->uc_mcontext.gregs[REG_R12],
           (unsigned long long)uc->uc_mcontext.gregs[REG_R13],
           (unsigned long long)uc->uc_mcontext.gregs[REG_R14],
           (unsigned long long)uc->uc_mcontext.gregs[REG_R15]);
    fprintf(stderr, "RSP: 0x%llx  RBP: 0x%llx\n",
           (unsigned long long)uc->uc_mcontext.gregs[REG_RSP],
           (unsigned long long)uc->uc_mcontext.gregs[REG_RBP]);
    fflush(stderr);
    _exit(1);
}

static void alarm_handler(int sig) {
    fflush(stdout);
    fprintf(stderr, "\n=== ALARM: TIMEOUT (infinite loop) ===\n");
    fflush(stderr);
    _exit(2);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segfault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);

    /* Set up alarm handler for timeout detection */
    signal(SIGALRM, alarm_handler);

    vtx_assembler_t a; vtx_asm_init(&a);
    const char *prog =
        ".method f (I)I\n.arg_count 1\n.max_locals 2\n.max_stack 4\n"
        "load_const_int 0\nstore_local 1\n"
        "loop:\nload_local 0\nif_false done\n"
        "load_local 1\nload_local 0\niadd\nstore_local 1\n"
        "load_local 0\nload_const_int 1\nisub\nstore_local 0\n"
        "goto loop\ndone:\nload_local 1\nreturn_value\n";
    vtx_asm_program(&a, prog);

    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_graph_t graph; vtx_graph_init(&graph, 1);
    vtx_bytecode_t bc = vtx_asm_emit(&a);
    vtx_method_desc_t method = {.name="f",.signature="(I)I",.bytecode=&bc,
        .compiled_code=NULL,.vtable_index=0,.arg_count=1,.is_virtual=false};

    vtx_graph_build(&graph, &bc, &method, &arena);
    vtx_graph_print(&graph);

    /* Schedule + isel + regalloc manually to get the stream */
    vtx_schedule_t schedule;
    memset(&schedule, 0, sizeof(schedule));
    vtx_schedule_run(&graph, &arena, &schedule);

    vtx_inst_stream_t *stream = vtx_isel_select(&schedule, &graph, &arena);
    if (!stream) { printf("ISEL FAILED\n"); return 1; }

    vtx_regalloc_result_t *ra = vtx_regalloc_run(stream, &arena);
    if (!ra) { printf("REGALLOC FAILED\n"); return 1; }

    /* Print vreg→phys mapping */
    printf("=== Regalloc mapping ===\n");
    for (uint32_t v = 0; v < ra->vreg_to_phys_count; v++) {
        if (ra->vreg_to_phys[v] != 0xFF) {
            printf("  vreg %u → phys %u\n", v, ra->vreg_to_phys[v]);
        }
        if (ra->vreg_to_spill[v] != 0xFFFFFFFF) {
            printf("  vreg %u → spill %u\n", v, ra->vreg_to_spill[v]);
        }
    }

    vtx_regalloc_apply(stream, ra, &arena);

    /* Print post-regalloc instruction stream */
    printf("\n=== Post-regalloc instruction stream ===\n");
    for (uint32_t b = 0; b < stream->block_count; b++) {
        printf("Block %u: %u instructions\n", b, stream->blocks[b].inst_count);
        for (uint32_t i = 0; i < stream->blocks[b].inst_count; i++) {
            vtx_inst_t *inst = &stream->blocks[b].insts[i];
            printf("  [%u] op=%u k0=%u op0=%u k1=%u op1=%u flags=0x%x imm=0x%llX\n",
                   i, inst->opcode,
                   inst->opnd_kinds[0], inst->operands[0],
                   inst->opnd_kinds[1], inst->operands[1],
                   (unsigned)inst->flags,
                   (unsigned long long)(uint64_t)inst->imm);
        }
    }

    /* Now compile via pipeline and execute */
    vtx_pipeline_config_t config = vtx_pipeline_config_t2();
    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1<<20);
    vtx_method_registry_t reg; vtx_method_registry_init(&reg, &arena);
    config.code_cache=&cache; config.method_registry=&reg; config.method=&method;
    vtx_compile_result_t result; memset(&result,0,sizeof(result));
    int prc = vtx_pipeline_run(&graph, &config, &arena, &result);
    printf("\npipeline rc=%d success=%d code=%p\n", prc, result.success, method.compiled_code);

    if (method.compiled_code) {
        uint8_t *code = (uint8_t*)method.compiled_code;
        printf("\n=== Native code (400 bytes) ===\n");
        for (int i=0; i<400; i++) {
            printf("%02X ", code[i]);
            if((i+1)%16==0) printf("\n");
        }
        printf("\n\n");

        printf("Executing sum(3)...\n");
        fflush(stdout);
        typedef vtx_value_t (*entry_t)(const vtx_method_desc_t*,void*,void*,vtx_value_t*,uint32_t);
        entry_t e = (entry_t)method.compiled_code;
        vtx_value_t av = vtx_make_smi(3);

        /* Set a 3-second alarm to catch infinite loops */
        alarm(3);
        vtx_value_t r = e(&method, NULL, (void*)1, &av, 1);
        alarm(0);
        fflush(stdout);
        printf("Function returned! raw=0x%llX\n", (unsigned long long)r);
        fflush(stdout);
        printf("sum(3) = %lld (expected 6)\n", (long long)vtx_smi_value(r));
        fflush(stdout);
    }

    vtx_compile_result_destroy(&result);
    vtx_pipeline_config_destroy(&config);
    vtx_code_cache_destroy(&cache);
    vtx_method_registry_destroy(&reg);
    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_asm_destroy(&a);
    return 0;
}
