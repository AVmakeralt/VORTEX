#include <stdio.h>
#include <string.h>
#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "ir/schedule.h"
#include "lower/isel.h"
#include "lower/regalloc.h"
#include "assembler.h"

int main(void) {
    vtx_assembler_t a; vtx_asm_init(&a);
    /* Simple test: if_false with a constant condition */
    const char *prog =
        ".method f (I)I\n.arg_count 1\n.max_locals 1\n.max_stack 4\n"
        "load_local 0\nif_false done\n"
        "load_const_int 1\nreturn_value\n"
        "done:\nload_const_int 0\nreturn_value\n";
    vtx_asm_program(&a, prog);
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_graph_t graph; vtx_graph_init(&graph, 1);
    vtx_bytecode_t bc = vtx_asm_emit(&a);
    vtx_method_desc_t method = {.name="f",.signature="(I)I",.bytecode=&bc,
        .compiled_code=NULL,.vtable_index=0,.arg_count=1,.is_virtual=false};
    vtx_graph_build(&graph, &bc, &method, &arena);
    vtx_schedule_t schedule;
    memset(&schedule, 0, sizeof(schedule));
    vtx_schedule_run(&graph, &arena, &schedule);
    vtx_inst_stream_t *stream = vtx_isel_select(&schedule, &graph, &arena);
    if (!stream) { fprintf(stderr,"ISEL FAILED\n"); return 1; }

    /* Print the instruction stream with native_offset and vreg assignments */
    fprintf(stderr, "=== Instruction stream (If block) ===\n");
    for (uint32_t b = 0; b < stream->block_count; b++) {
        for (uint32_t i = 0; i < stream->blocks[b].inst_count; i++) {
            vtx_inst_t *inst = &stream->blocks[b].insts[i];
            /* Only print MOV imm and CMP and JCC */
            if (inst->opcode == 20 /*MOV*/ || inst->opcode == 18 /*CMP*/ || inst->opcode == 38 /*JCC*/) {
                const char *opname = inst->opcode == 20 ? "MOV" : inst->opcode == 18 ? "CMP" : "JCC";
                fprintf(stderr, "  Block %u [%u] pos=%u %s k0=%u op0=%u k1=%u op1=%u flags=0x%x imm=0x%llX\n",
                       b, i, inst->native_offset, opname,
                       inst->opnd_kinds[0], inst->operands[0],
                       inst->opnd_kinds[1], inst->operands[1],
                       (unsigned)inst->flags,
                       (unsigned long long)(uint64_t)inst->imm);
            }
        }
    }

    /* Run regalloc and print intervals */
    vtx_regalloc_result_t *ra = vtx_regalloc_run(stream, &arena);
    if (ra) {
        fprintf(stderr, "\n=== Live intervals (key vregs) ===\n");
        for (uint32_t v = 0; v < ra->interval_count; v++) {
            vtx_live_interval_t *iv = &ra->intervals[v];
            fprintf(stderr, "  vreg %u: start=%u end=%u phys=%u spill=%u\n",
                   iv->vreg, iv->start, iv->end,
                   iv->phys_reg, iv->spill_slot);
        }
        fprintf(stderr, "\n=== Phys assignments ===\n");
        for (uint32_t v = 0; v < ra->vreg_to_phys_count; v++) {
            if (ra->vreg_to_phys[v] != 0xFF || ra->vreg_to_spill[v] != 0xFFFFFFFF) {
                fprintf(stderr, "  vreg %u: phys=%u spill=%u\n",
                       v, ra->vreg_to_phys[v], ra->vreg_to_spill[v]);
            }
        }
    }
    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_asm_destroy(&a);
    return 0;
}
