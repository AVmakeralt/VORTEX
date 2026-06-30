#include <stdio.h>
#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "assembler.h"

int main(void) {
    vtx_assembler_t a; vtx_asm_init(&a);
    const char *prog =
        ".method collatz (I)I\n.arg_count 1\n.max_locals 3\n.max_stack 6\n"
        "load_const_int 0\nstore_local 1\n"
        "loop:\nload_local 0\nload_const_int 1\nicmp_eq\nif_true done\n"
        "load_local 0\nload_const_int 2\nimod\n"
        "load_const_int 1\nicmp_eq\nif_false even\n"
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
    for (uint32_t i = 0; i < graph.node_table.count; i++) {
        vtx_node_t *n = &graph.node_table.nodes[i];
        if (n->dead) continue;
        printf("N%u: %s -> [", i, vtx_node_opcode_name(n->opcode));
        for (uint32_t j = 0; j < n->input_count; j++)
            printf("N%u%s", n->inputs[j], j+1<n->input_count?",":"");
        printf("]\n");
    }
    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_asm_destroy(&a);
    return 0;
}
