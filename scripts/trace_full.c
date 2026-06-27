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
    if (!stream) { fprintf(stderr,"ISEL FAILED\n"); return 1; }
    vtx_regalloc_result_t *ra = vtx_regalloc_run(stream, &arena);
    if (!ra) { fprintf(stderr,"REGALLOC FAILED\n"); return 1; }

    /* Print ALL vreg assignments */
    fprintf(stderr, "=== ALL vreg assignments ===\n");
    for (uint32_t v = 0; v < ra->vreg_to_phys_count; v++) {
        uint8_t phys = ra->vreg_to_phys[v];
        uint32_t spill = ra->vreg_to_spill[v];
        if (phys != 0xFF) {
            fprintf(stderr, "  vreg %u -> phys %u\n", v, phys);
        }
        if (spill != 0xFFFFFFFF) {
            fprintf(stderr, "  vreg %u -> spill %u ([rbp-0x%x])\n", v, spill, 8*(spill+1));
        }
    }

    /* Print vreg to node mapping */
    fprintf(stderr, "\n=== Vreg to Node mapping ===\n");
    for (uint32_t i = 0; i < graph.node_table.count; i++) {
        vtx_node_t *n = &graph.node_table.nodes[i];
        if (n->dead) continue;
        uint32_t vreg = vtx_isel_node_vreg(stream, i);
        if (vreg != VTX_VREG_INVALID) {
            fprintf(stderr, "  vreg %u -> N%u (%s)\n", vreg, i,
                   vtx_node_opcode_name(n->opcode));
        }
    }

    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_asm_destroy(&a);
    return 0;
}
