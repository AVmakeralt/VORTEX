/* Trace collatz through pipeline: dump IR, schedule, isel, regalloc */
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

int main(void) {
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

    vtx_graph_build(&graph, &bc, &method, &arena);

    /* Print IR */
    printf("=== IR Graph ===\n");
    for (uint32_t i = 0; i < graph.node_table.count; i++) {
        vtx_node_t *n = &graph.node_table.nodes[i];
        if (n->dead) continue;
        printf("  N%u: %s", i, vtx_node_opcode_name(n->opcode));
        if (n->opcode == VTX_OP_Constant && n->constval.kind == VTX_TYPE_Int)
            printf(" val=%lld", (long long)n->constval.as.int_val);
        if (n->opcode == VTX_OP_Phi || n->opcode == VTX_OP_Div || n->opcode == VTX_OP_Mod ||
            n->opcode == VTX_OP_Mul || n->opcode == VTX_OP_Add)
            printf(" -> [");
        else
            printf(" -> [");
        for (uint32_t j = 0; j < n->input_count; j++)
            printf("N%u%s", n->inputs[j], j+1<n->input_count?",":"");
        printf("]\n");
    }

    /* Schedule */
    vtx_schedule_t schedule;
    memset(&schedule, 0, sizeof(schedule));
    vtx_schedule_run(&graph, &arena, &schedule);

    /* ISEL */
    vtx_inst_stream_t *stream = vtx_isel_select(&schedule, &graph, &arena);
    if (!stream) { printf("ISEL FAILED\n"); return 1; }

    /* Print FULL pre-regalloc stream for Block 1 (loop body) */
    printf("\n=== Full pre-regalloc stream (Block 1) ===\n");
    if (stream->block_count > 1) {
        vtx_inst_block_t *blk = &stream->blocks[1];
        printf("Block 1: %u instructions\n", blk->inst_count);
        for (uint32_t i = 0; i < blk->inst_count; i++) {
            vtx_inst_t *ji = &blk->insts[i];
            const char *opname = "?";
            switch (ji->opcode) {
                case 0: opname="NOP"; break;
                case 1: opname="ADD"; break;
                case 2: opname="SUB"; break;
                case 3: opname="IMUL"; break;
                case 4: opname="IDIV"; break;
                case 10: opname="SHL"; break;
                case 11: opname="SHR"; break;
                case 12: opname="SAR"; break;
                case 15: opname="OR"; break;
                case 16: opname="AND"; break;
                case 18: opname="CMP"; break;
                case 20: opname="MOV"; break;
                case 21: opname="MOVZX"; break;
                case 25: opname="CQO"; break;
                case 28: opname="SETCC"; break;
                case 37: opname="JMP"; break;
                case 38: opname="JCC"; break;
                case 40: opname="RET"; break;
            }
            printf("  [%u] %s k0=%u op0=%u k1=%u op1=%u flags=0x%x imm=0x%llX node=N%u\n",
                   i, opname,
                   ji->opnd_kinds[0], ji->operands[0],
                   ji->opnd_kinds[1], ji->operands[1],
                   (unsigned)ji->flags,
                   (unsigned long long)(uint64_t)ji->imm,
                   ji->source_node);
        }
    }

    /* Regalloc */
    vtx_regalloc_result_t *ra = vtx_regalloc_run(stream, &arena);
    if (!ra) { printf("REGALLOC FAILED\n"); return 1; }

    /* Print vreg->phys/spill mapping for Div/Mod related vregs */
    printf("\n=== Regalloc mapping (all vregs) ===\n");
    static const char *reg_names[] = {
        "RAX","RCX","RDX","RBX","RSP","RBP","RSI","RDI",
        "R8","R9","R10","R11","R12","R13","R14","R15"
    };
    for (uint32_t v = 0; v < ra->vreg_to_phys_count; v++) {
        if (ra->vreg_to_phys[v] != 0xFF) {
            printf("  vreg %u -> %s\n", v,
                   ra->vreg_to_phys[v] < 16 ? reg_names[ra->vreg_to_phys[v]] : "?");
        }
        if (ra->vreg_to_spill[v] != 0xFFFFFFFF) {
            printf("  vreg %u -> spill[%u] (rbp-0x%x)\n", v, ra->vreg_to_spill[v],
                   (unsigned)(0x100 - ra->vreg_to_spill[v] * 8));
        }
    }

    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_asm_destroy(&a);
    return 0;
}
