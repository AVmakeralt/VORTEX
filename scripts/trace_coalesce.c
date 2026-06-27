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
        ".method popcount (I)I\n.arg_count 1\n.max_locals 3\n.max_stack 4\n"
        "load_const_int 0\nstore_local 1\n"
        "loop:\nload_local 0\nif_false done\n"
        "load_local 0\nload_const_int 2\nimod\n"
        "load_const_int 1\nicmp_eq\nif_false skip\n"
        "load_local 1\nload_const_int 1\niadd\nstore_local 1\n"
        "skip:\nload_local 0\nload_const_int 2\nidiv\nstore_local 0\n"
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
    vtx_schedule_t schedule;
    memset(&schedule, 0, sizeof(schedule));
    vtx_schedule_run(&graph, &arena, &schedule);
    vtx_inst_stream_t *stream = vtx_isel_select(&schedule, &graph, &arena);
    if (!stream) { printf("ISEL FAILED\n"); return 1; }

    /* Print pre-coalescing vreg references for PHI_COPY instructions */
    printf("=== Pre-coalescing PHI_COPY vregs ===\n");
    for (uint32_t b = 0; b < stream->block_count; b++) {
        for (uint32_t i = 0; i < stream->blocks[b].inst_count; i++) {
            vtx_inst_t *inst = &stream->blocks[b].insts[i];
            if (inst->flags & 0x10000) {
                printf("  Block %u [%u]: MOV vreg%u <- vreg%u (node=N%u)\n",
                       b, i, inst->operands[0], inst->operands[1], inst->source_node);
            }
        }
    }

    /* Run regalloc (which includes coalescing) */
    vtx_regalloc_result_t *ra = vtx_regalloc_run(stream, &arena);

    /* Print post-coalescing vreg references */
    printf("\n=== Post-coalescing PHI_COPY vregs ===\n");
    for (uint32_t b = 0; b < stream->block_count; b++) {
        for (uint32_t i = 0; i < stream->blocks[b].inst_count; i++) {
            vtx_inst_t *inst = &stream->blocks[b].insts[i];
            if (inst->flags & 0x10000) {
                printf("  Block %u [%u]: MOV k0=%u op0=%u k1=%u op1=%u (node=N%u)\n",
                       b, i, inst->opnd_kinds[0], inst->operands[0],
                       inst->opnd_kinds[1], inst->operands[1], inst->source_node);
            }
        }
    }

    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_asm_destroy(&a);
    return 0;
}
