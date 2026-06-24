/*
 * Deep dive: trace a simple loop through the entire T2 pipeline.
 * Program: sum(3) = 3+2+1 = 6
 *
 *   load_const_int 0; store_local 1     // sum = 0
 *   loop:
 *     load_local 0; if_false done        // if n==0, exit
 *     load_local 1; load_local 0; iadd; store_local 1  // sum += n
 *     load_local 0; load_const_int 1; isub; store_local 0  // n--
 *     goto loop
 *   done:
 *     load_local 1; return_value
 */
#include <stdio.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <string.h>
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

static sigjmp_buf jmpbuf;
static void handler(int s) { siglongjmp(jmpbuf, 1); }

int main(void) {
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

    /* Step 1: Build IR */
    printf("=== STEP 1: IR ===\n");
    vtx_graph_build(&graph, &bc, &method, &arena);
    vtx_graph_print(&graph);
    printf("\nverify: %d\n", vtx_verify_graph(&graph));

    /* Step 2: Schedule */
    printf("\n=== STEP 2: Schedule ===\n");
    vtx_schedule_t schedule;
    memset(&schedule, 0, sizeof(schedule));
    vtx_schedule_run(&graph, &arena, &schedule);
    for (uint32_t b = 0; b < schedule.count; b++) {
        printf("Block %u: succ=[", b);
        for (uint32_t s = 0; s < schedule.blocks[b].succ_count; s++)
            printf("%u ", schedule.blocks[b].succ_blocks[s]);
        printf("] pred=[");
        for (uint32_t p = 0; p < schedule.blocks[b].pred_count; p++)
            printf("%u ", schedule.blocks[b].pred_blocks[p]);
        printf("] loop_header=%d nodes:", schedule.blocks[b].is_loop_header);
        for (uint32_t n = 0; n < schedule.blocks[b].node_count; n++) {
            vtx_nodeid_t nid = schedule.blocks[b].nodes[n];
            const vtx_node_t *nd = vtx_node_get_const(&graph.node_table, nid);
            printf(" N%u(%s)", nid, nd ? vtx_node_opcode_name(nd->opcode) : "?");
        }
        printf("\n");
    }

    /* Step 3: Instruction Selection */
    printf("\n=== STEP 3: Instruction Selection ===\n");
    vtx_inst_stream_t *stream = vtx_isel_select(&schedule, &graph, &arena);
    if (!stream) { printf("ISEL FAILED\n"); return 1; }
    for (uint32_t b = 0; b < stream->block_count; b++) {
        printf("Block %u: %u instructions\n", b, stream->blocks[b].inst_count);
        for (uint32_t i = 0; i < stream->blocks[b].inst_count; i++) {
            vtx_inst_t *inst = &stream->blocks[b].insts[i];
            printf("  [%u] op=%u(%s) k0=%u op0=%u k1=%u op1=%u flags=0x%x imm=0x%llX\n",
                   i, inst->opcode,
                   inst->opcode == 0 ? "NOP" : inst->opcode == 18 ? "CMP" :
                   inst->opcode == 20 ? "MOV" : inst->opcode == 38 ? "JCC" :
                   inst->opcode == 39 ? "JMP" : inst->opcode == 40 ? "RET" :
                   inst->opcode == 1 ? "ADD" : inst->opcode == 2 ? "SUB" : "?",
                   inst->opnd_kinds[0], inst->operands[0],
                   inst->opnd_kinds[1], inst->operands[1],
                   (unsigned)inst->flags, (unsigned long long)(uint64_t)inst->imm);
        }
    }

    /* Step 4: Register Allocation */
    printf("\n=== STEP 4: Register Allocation ===\n");
    vtx_regalloc_result_t *ra = vtx_regalloc_run(stream, &arena);
    if (!ra) { printf("REGALLOC FAILED\n"); return 1; }
    vtx_regalloc_apply(stream, ra, &arena);

    /* Step 5: Post-regalloc stream */
    printf("\n=== STEP 5: Post-Regalloc Stream ===\n");
    for (uint32_t b = 0; b < stream->block_count; b++) {
        printf("Block %u: %u instructions\n", b, stream->blocks[b].inst_count);
        for (uint32_t i = 0; i < stream->blocks[b].inst_count; i++) {
            vtx_inst_t *inst = &stream->blocks[b].insts[i];
            printf("  [%u] op=%u k0=%u op0=%u k1=%u op1=%u flags=0x%x imm=0x%llX\n",
                   i, inst->opcode,
                   inst->opnd_kinds[0], inst->operands[0],
                   inst->opnd_kinds[1], inst->operands[1],
                   (unsigned)inst->flags, (unsigned long long)(uint64_t)inst->imm);
        }
    }

    /* Step 6: Peephole */
    printf("\n=== STEP 6: Post-Peephole ===\n");
    vtx_peephole_optimize(stream, ra);
    for (uint32_t b = 0; b < stream->block_count; b++) {
        printf("Block %u: %u instructions\n", b, stream->blocks[b].inst_count);
        for (uint32_t i = 0; i < stream->blocks[b].inst_count; i++) {
            vtx_inst_t *inst = &stream->blocks[b].insts[i];
            printf("  [%u] op=%u k0=%u op0=%u k1=%u op1=%u flags=0x%x imm=0x%llX\n",
                   i, inst->opcode,
                   inst->opnd_kinds[0], inst->operands[0],
                   inst->opnd_kinds[1], inst->operands[1],
                   (unsigned)inst->flags, (unsigned long long)(uint64_t)inst->imm);
        }
    }

    /* Step 7: Emit */
    printf("\n=== STEP 7: Native Code ===\n");
    vtx_pipeline_config_t config = vtx_pipeline_config_t2();
    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1<<20);
    vtx_method_registry_t reg; vtx_method_registry_init(&reg, &arena);
    config.code_cache=&cache; config.method_registry=&reg; config.method=&method;
    vtx_compile_result_t result; memset(&result,0,sizeof(result));
    vtx_pipeline_run(&graph, &config, &arena, &result);

    if (method.compiled_code) {
        uint8_t *code = (uint8_t*)method.compiled_code;
        printf("Native code (300 bytes):\n");
        for (int i=0; i<300; i++) {
            printf("%02X ", code[i]);
            if((i+1)%16==0) printf("\n");
        }
        printf("\n\n");

        typedef vtx_value_t (*entry_t)(const vtx_method_desc_t*,void*,void*,vtx_value_t*,uint32_t);
        entry_t e = (entry_t)method.compiled_code;
        vtx_value_t av = vtx_make_smi(3);
        signal(SIGALRM, handler);
        if (sigsetjmp(jmpbuf, 1) == 0) {
            alarm(2);
            vtx_value_t r = e(&method, NULL, (void*)1, &av, 1);
            alarm(0);
            printf("sum(3) = %lld (expected 6) %s\n", (long long)vtx_smi_value(r),
                   vtx_smi_value(r)==6?"PASS":"FAIL");
        } else {
            printf("sum(3) TIMEOUT\n");
        }
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
