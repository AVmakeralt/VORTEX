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
    const char *prog =
        ".method divmod (I)I\n.arg_count 1\n.max_locals 3\n.max_stack 6\n"
        "load_local 0\nload_const_int 7\nidiv\n"
        "load_const_int 7\nimul\nstore_local 1\n"
        "load_local 0\nload_const_int 7\nimod\nstore_local 2\n"
        "load_local 1\nload_local 2\niadd\nreturn_value\n";
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

    /* Print post-regalloc stream */
    vtx_regalloc_result_t *ra = vtx_regalloc_run(stream, &arena);
    if (!ra) { fprintf(stderr,"REGALLOC FAILED\n"); return 1; }
    vtx_regalloc_apply(stream, ra, &arena);

    fprintf(stderr, "=== Post-regalloc stream ===\n");
    for (uint32_t b = 0; b < stream->block_count; b++) {
        fprintf(stderr, "Block %u: %u instructions\n", b, stream->blocks[b].inst_count);
        for (uint32_t i = 0; i < stream->blocks[b].inst_count; i++) {
            vtx_inst_t *inst = &stream->blocks[b].insts[i];
            const char *opname = "?";
            switch (inst->opcode) {
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
                case 25: opname="CQO"; break;
                case 40: opname="RET"; break;
            }
            fprintf(stderr, "  [%u] %s k0=%u op0=%u k1=%u op1=%u flags=0x%x imm=0x%llX\n",
                   i, opname,
                   inst->opnd_kinds[0], inst->operands[0],
                   inst->opnd_kinds[1], inst->operands[1],
                   (unsigned)inst->flags,
                   (unsigned long long)(uint64_t)inst->imm);
        }
    }

    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_asm_destroy(&a);
    return 0;
}
