/* dump_sum_asm.c — placeholder, minimal stub */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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
static const char *PROG_SUM =
    ".method sum (I)I\n.arg_count 1\n.max_locals 3\n.max_stack 4\n"
    "load_const_int 0\nstore_local 1\n"
    "loop:\nload_local 0\nload_const_int 0\nicmp_le\nif_true done\n"
    "load_local 1\nload_local 0\niadd\nstore_local 1\n"
    "load_local 0\nload_const_int 1\nisub\nstore_local 0\n"
    "goto loop\n"
    "done:\nload_local 1\nreturn_value\n";
int main(void) {
    vtx_assembler_t *a=calloc(1,sizeof(*a)); vtx_arena_t *ar=calloc(1,sizeof(*ar));
    vtx_type_system_t *ts=calloc(1,sizeof(*ts)); vtx_gc_t *gc=calloc(1,sizeof(*gc));
    vtx_graph_t *g=calloc(1,sizeof(*g)); vtx_code_cache_t *c=calloc(1,sizeof(*c));
    vtx_method_registry_t *r=calloc(1,sizeof(*r)); vtx_method_desc_t *m=calloc(1,sizeof(*m));
    vtx_bytecode_t *bc=calloc(1,sizeof(*bc));
    vtx_asm_init(a); vtx_asm_program(a, PROG_SUM); *bc=vtx_asm_emit(a);
    vtx_arena_init(ar); vtx_type_system_init(ts); vtx_gc_init(gc,ts,VTX_GC_GENERATIONAL);
    vtx_graph_init(g,1);
    m->name="sum"; m->signature="(I)I"; m->bytecode=bc; m->arg_count=1; m->is_virtual=false;
    if (vtx_graph_build(g,bc,m,ar)!=0){printf("build fail\n");return 1;}
    vtx_pipeline_config_t cfg=vtx_pipeline_config_t2();
    vtx_code_cache_init(c,1<<20); vtx_method_registry_init(r,ar);
    cfg.code_cache=c; cfg.method_registry=r; cfg.method=m;
    vtx_compile_result_t res; memset(&res,0,sizeof(res));
    vtx_pipeline_run(g,&cfg,ar,&res);
    if (m->compiled_code) {
        vtx_value_t v=vtx_make_smi(100);
        vtx_value_t r2=((jit_entry_t)m->compiled_code)(m,NULL,(void*)1,&v,1);
        printf("sum(100) = %ld (expected 5050)\n",(long)vtx_smi_value(r2));
    }
    return 0;
}
