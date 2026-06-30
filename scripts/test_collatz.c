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
    fflush(stderr);
    _exit(1);
}

static void alarm_handler(int sig) {
    fflush(stdout);
    fprintf(stderr, "\n=== ALARM: TIMEOUT ===\n");
    fflush(stderr);
    _exit(2);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segfault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    signal(SIGALRM, alarm_handler);

    vtx_assembler_t a; vtx_asm_init(&a);
    const char *prog =
        ".method popcount (I)I\n.arg_count 1\n.max_locals 3\n.max_stack 6\n"
        "load_const_int 0;store_local 1\n"
        "loop:;load_local 0;if_false done\n"
        "load_local 0;load_const_int 2;imod;load_const_int 1;icmp_eq;if_false skip\n"
        "load_local 1;load_const_int 1;iadd;store_local 1\n"
        "\n"
        "skip:;load_local 0;load_const_int 2;idiv;store_local 0\n"
        "goto loop\n"
        "goto loop\n"
        "done:\nload_local 1\nreturn_value\n";
    vtx_asm_program(&a, prog);

    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_graph_t graph; vtx_graph_init(&graph, 1);
    vtx_bytecode_t bc = vtx_asm_emit(&a);
    vtx_method_desc_t method = {.name="f",.signature="(I)I",.bytecode=&bc,
        .compiled_code=NULL,.vtable_index=0,.arg_count=1,.is_virtual=false};

    vtx_graph_build(&graph, &bc, &method, &arena);
    vtx_pipeline_config_t config = vtx_pipeline_config_t2();
    
    
    
    
    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1<<20);
    vtx_method_registry_t reg; vtx_method_registry_init(&reg, &arena);
    config.code_cache=&cache; config.method_registry=&reg; config.method=&method;
    vtx_compile_result_t result; memset(&result,0,sizeof(result));
    int prc = vtx_pipeline_run(&graph, &config, &arena, &result);
    printf("pipeline rc=%d success=%d code=%p\n", prc, result.success, method.compiled_code);

    if (method.compiled_code) {
        printf("Executing popcount(6)...\n"); fflush(stdout); uint8_t *code = (uint8_t*)method.compiled_code; printf("Native code:"); for(int i=0;i<2000;i++){printf(" %02X",code[i]);} printf("\n"); fflush(stdout);
        fflush(stdout);
        typedef vtx_value_t (*entry_t)(const vtx_method_desc_t*,void*,void*,vtx_value_t*,uint32_t);
        entry_t e = (entry_t)method.compiled_code;
        vtx_value_t av = vtx_make_smi(6);
        alarm(3);
        vtx_value_t r = e(&method, NULL, (void*)1, &av, 1);
        alarm(0);
        printf("popcount(6) = %lld (expected 8)\n", (long long)vtx_smi_value(r));
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
