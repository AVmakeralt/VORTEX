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
    vtx_graph_print(&graph);
    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_asm_destroy(&a);
    return 0;
}
