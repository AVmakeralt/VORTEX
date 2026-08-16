#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/arena.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "compile/pipeline.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "assembler.h"
typedef vtx_value_t (*jit_entry_t)(const vtx_method_desc_t *, void *, void *, vtx_value_t *, uint32_t);
static const char *PROG =
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
int main(void) {
    vtx_assembler_t a; vtx_asm_init(&a); vtx_asm_program(&a, PROG);
    vtx_bytecode_t bc = vtx_asm_emit(&a);
    vtx_arena_t ar; vtx_arena_init(&ar);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_graph_t g; vtx_graph_init(&g, 1);
    vtx_method_desc_t m = {0}; m.name="f"; m.signature="(I)I"; m.bytecode=&bc; m.arg_count=1; m.is_virtual=false;
    vtx_graph_build(&g, &bc, &m, &ar);
    vtx_pipeline_config_t cfg = vtx_pipeline_config_t2();
    vtx_code_cache_t c; vtx_code_cache_init(&c, 1<<20);
    vtx_method_registry_t r; vtx_method_registry_init(&r, &ar);
    cfg.code_cache=&c; cfg.method_registry=&r; cfg.method=&m;
    vtx_compile_result_t res; memset(&res,0,sizeof(res));
    vtx_pipeline_run(&g, &cfg, &ar, &res);
    vtx_graph_print(&g);
    if (m.compiled_code) {
        vtx_value_t v = vtx_make_smi(27);
        /* ISO C forbids direct object-pointer → function-pointer cast;
         * use a union (the portable, pedantic-clean idiom). */
        union { void *ptr; jit_entry_t fn; } u_e;
        u_e.ptr = m.compiled_code;
        vtx_value_t r2 = u_e.fn(&m, NULL, (void*)1, &v, 1);
        printf("\ncollatz(27) = %ld (expected 111)\n", (long)vtx_smi_value(r2));
    }
    return 0;
}
