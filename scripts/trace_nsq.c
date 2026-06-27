#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "ir/schedule.h"
#include "lower/isel.h"
#include "assembler.h"

int main(void) {
    vtx_assembler_t a; vtx_asm_init(&a);
    const char *prog =
        ".method n_sq (I)I\n.arg_count 1\n.max_locals 4\n.max_stack 4\n"
        "load_const_int 0\nstore_local 1\n"
        "load_const_int 0\nstore_local 2\n"
        "outer:\nload_local 2\nload_local 0\nicmp_ge\nif_true outer_done\n"
        "load_const_int 0\nstore_local 3\n"
        "inner:\nload_local 3\nload_local 0\nicmp_ge\nif_true inner_done\n"
        "load_local 1\nload_const_int 1\niadd\nstore_local 1\n"
        "load_local 3\nload_const_int 1\niadd\nstore_local 3\n"
        "goto inner\n"
        "inner_done:\nload_local 2\nload_const_int 1\niadd\nstore_local 2\n"
        "goto outer\n"
        "outer_done:\nload_local 1\nreturn_value\n";
    vtx_asm_program(&a, prog);
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_graph_t graph; vtx_graph_init(&graph, 1);
    vtx_bytecode_t bc = vtx_asm_emit(&a);
    vtx_method_desc_t method = {.name="f",.signature="(I)I",.bytecode=&bc,
        .compiled_code=NULL,.vtable_index=0,.arg_count=1,.is_virtual=false};
    vtx_graph_build(&graph, &bc, &method, &arena);
    printf("=== IR Graph (Phis only) ===\n");
    for (uint32_t i = 0; i < graph.node_table.count; i++) {
        vtx_node_t *n = &graph.node_table.nodes[i];
        if (n->dead) continue;
        if (n->opcode != VTX_OP_Phi) continue;
        printf("  N%u: Phi -> [", i);
        for (uint32_t j = 0; j < n->input_count; j++)
            printf("N%u%s", n->inputs[j], j+1<n->input_count?",":"");
        printf("]\n");
    }
    vtx_schedule_t schedule;
    memset(&schedule, 0, sizeof(schedule));
    vtx_schedule_run(&graph, &arena, &schedule);
    printf("\n=== Schedule ===\n");
    for (uint32_t b = 0; b < schedule.count; b++) {
        printf("Block %u:", b);
        for (uint32_t n = 0; n < schedule.blocks[b].node_count; n++)
            printf(" N%u", schedule.blocks[b].nodes[n]);
        printf("\n");
    }
    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_asm_destroy(&a);
    return 0;
}
