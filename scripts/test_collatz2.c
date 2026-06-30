#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
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

static void alarm_handler(int sig) { _exit(2); }

int main(void) {
    signal(SIGALRM, alarm_handler);
    vtx_assembler_t a; vtx_asm_init(&a);
    const char *prog =
        ".method collatz (I)I\n.arg_count 1\n.max_locals 3\n.max_stack 6\n"
        "load_const_int 0\nstore_local 1\n"
        "loop:\nload_local 0\nload_const_int 1\nicmp_eq\nif_true done\n"
        "load_local 0\nload_const_int 2\nimod\nif_false even\n"
        "load_local 0\nload_const_int 3\nimul\nload_const_int 1\niadd\nstore_local 0\n"
        "goto inc\n"
        "even:\nload_local 0\nload_const_int 2\nidiv\nstore_local 0\n"
        "inc:\nload_local 1\nload_const_int 1\niadd\nstore_local 1\n"
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

    /* Build IR and print before optimizations */
    vtx_graph_build(&graph, &bc, &method, &arena);
    fprintf(stderr, "=== IR BEFORE OPTS ===\n");
    for (uint32_t i = 0; i < graph.node_table.count; i++) {
        vtx_node_t *n = &graph.node_table.nodes[i];
        if (n->dead) continue;
        fprintf(stderr, "  N%u: %s", i, vtx_node_opcode_name(n->opcode));
        if (n->opcode == VTX_OP_Constant && n->constval.kind == VTX_TYPE_Int)
            fprintf(stderr, " val=%lld", (long long)n->constval.as.int_val);
        fprintf(stderr, " -> [");
        for (uint32_t j = 0; j < n->input_count; j++)
            fprintf(stderr, "N%u%s", n->inputs[j], j+1<n->input_count?",":"");
        fprintf(stderr, "]\n");
    }

    /* Run pipeline with all opts */
    vtx_pipeline_config_t config = vtx_pipeline_config_t2();
    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1<<20);
    vtx_method_registry_t reg; vtx_method_registry_init(&reg, &arena);
    config.code_cache=&cache; config.method_registry=&reg; config.method=&method;
    vtx_compile_result_t result; memset(&result,0,sizeof(result));
    vtx_pipeline_run(&graph, &config, &arena, &result);

    /* Print IR after optimizations */
    fprintf(stderr, "\n=== IR AFTER OPTS ===\n");
    for (uint32_t i = 0; i < graph.node_table.count; i++) {
        vtx_node_t *n = &graph.node_table.nodes[i];
        if (n->dead) { fprintf(stderr, "  N%u: DEAD\n", i); continue; }
        fprintf(stderr, "  N%u: %s", i, vtx_node_opcode_name(n->opcode));
        if (n->opcode == VTX_OP_Constant && n->constval.kind == VTX_TYPE_Int)
            fprintf(stderr, " val=%lld", (long long)n->constval.as.int_val);
        fprintf(stderr, " -> [");
        for (uint32_t j = 0; j < n->input_count; j++)
            fprintf(stderr, "N%u%s", n->inputs[j], j+1<n->input_count?",":"");
        fprintf(stderr, "]\n");
    }

    fprintf(stderr, "\npipeline rc=%d success=%d code=%p\n", result.success, result.success, method.compiled_code);
    if (method.compiled_code) {
        typedef vtx_value_t (*entry_t)(const vtx_method_desc_t*,void*,void*,vtx_value_t*,uint32_t);
        entry_t e = (entry_t)method.compiled_code;
        vtx_value_t av = vtx_make_smi(6);
        fprintf(stderr, "Executing collatz(6)...\n"); fflush(stderr); alarm(3);
        vtx_value_t r = e(&method, NULL, (void*)1, &av, 1);
        alarm(0); uint8_t *code2 = (uint8_t*)method.compiled_code; fprintf(stderr, "Native:"); for(int i=0;i<2000;i++) fprintf(stderr, " %02X", code2[i]); fprintf(stderr, "\n"); fflush(stderr);
        fprintf(stderr, "collatz(6) = %lld (expected 8)\n", (long long)vtx_smi_value(r));
    }
    vtx_compile_result_destroy(&result);
    vtx_pipeline_config_destroy(&config);
    vtx_code_cache_destroy(&cache);
    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_asm_destroy(&a);
    return 0;
}
