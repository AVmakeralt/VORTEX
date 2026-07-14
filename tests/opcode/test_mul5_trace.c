/* Test mul5 via T2 pipeline — trace the bug */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vortex_config.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/arena.h"
#include "interp/dispatch.h"
#include "compile/pipeline.h"
#include "codecache/install.h"

/* Build mul5 bytecode */
static vtx_bytecode_t *build_mul5(vtx_arena_t *arena) {
    uint8_t code[] = {
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,  /* const[0]=0, result=0 */
        VT_OP_STORE_LOCAL,    0x00, 0x01,  /* store_local 1 */
        VT_OP_LOAD_CONST_INT, 0x00, 0x01,  /* const[1]=5, counter=5 */
        VT_OP_STORE_LOCAL,    0x00, 0x02,  /* store_local 2 */
        /* loop: pc=12 */
        VT_OP_LOAD_LOCAL,     0x00, 0x02,  /* load_local 2 (counter) */
        VT_OP_IF_FALSE,       0x00, 0x00,  /* if_false done (patch later) */
        VT_OP_LOAD_LOCAL,     0x00, 0x01,  /* load_local 1 (result) */
        VT_OP_LOAD_LOCAL,     0x00, 0x00,  /* load_local 0 (a) */
        VT_OP_IADD,                         /* result + a */
        VT_OP_STORE_LOCAL,    0x00, 0x01,  /* store_local 1 */
        VT_OP_LOAD_LOCAL,     0x00, 0x02,  /* load_local 2 (counter) */
        VT_OP_LOAD_CONST_INT, 0x00, 0x02,  /* const[2]=1 */
        VT_OP_ISUB,                         /* counter - 1 */
        VT_OP_STORE_LOCAL,    0x00, 0x02,  /* store_local 2 */
        VT_OP_GOTO,           0x00, 0x0C,  /* goto loop (pc=12) */
        /* done: pc=38 */
        VT_OP_LOAD_LOCAL,     0x00, 0x01,  /* load_local 1 (result) */
        VT_OP_RETURN_VALUE,
    };
    /* Patch if_false target to done (pc=38) */
    code[15] = 0x00; code[16] = 0x26; /* 0x26 = 38 */

    vtx_value_t *consts = vtx_arena_alloc(arena, 3 * sizeof(vtx_value_t));
    consts[0] = vtx_make_smi(0);
    consts[1] = vtx_make_smi(5);
    consts[2] = vtx_make_smi(1);

    vtx_bytecode_t *bc = vtx_arena_alloc(arena, sizeof(vtx_bytecode_t));
    bc->code = vtx_arena_alloc(arena, sizeof(code));
    memcpy((void*)bc->code, code, sizeof(code));
    bc->length = sizeof(code);
    bc->constant_pool = consts;
    bc->constant_count = 3;
    bc->max_locals = 3;
    bc->max_stack = 4;
    return bc;
}

int main(void) {
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);
    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_NONE);

    vtx_bytecode_t *bc = build_mul5(&arena);
    vtx_method_desc_t method = {
        .name = "mul5", .signature = "(I)I",
        .bytecode = bc, .compiled_code = NULL,
        .vtable_index = 1, .arg_count = 1, .is_virtual = false
    };

    /* Run in interpreter first */
    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);
    vtx_value_t arg = vtx_make_smi(7);
    vtx_value_t r_interp = vtx_interp_run(&interp, &method, &arg, 1);
    vtx_interp_destroy(&interp);

    printf("interp mul5(7) raw=0x%lX is_smi=%d\n",
           (unsigned long)r_interp, vtx_is_smi(r_interp));

    /* Now compile with T2 and run */
    vtx_code_cache_t cache;
    vtx_code_cache_init(&cache, 1 << 20);
    vtx_method_registry_t registry;
    vtx_method_registry_init(&registry, &arena);

    vtx_graph_t graph;
    vtx_graph_init(&graph, 1);
    int brc = vtx_graph_build(&graph, bc, &method, &arena);
    printf("graph_build rc=%d\n", brc);

    vtx_pipeline_config_t config = vtx_pipeline_config_t2();
    config.code_cache = &cache;
    config.method_registry = &registry;
    config.method = &method;

    vtx_compile_result_t result;
    memset(&result, 0, sizeof(result));

    int rc = vtx_pipeline_run(&graph, &config, &arena, &result);
    printf("pipeline rc=%d success=%d compiled_code=%p\n", rc, result.success, (void*)method.compiled_code);

    if (result.success && method.compiled_code != NULL) {
        typedef vtx_value_t (*vtx_jit_entry_t)(
            const vtx_method_desc_t *, void *, void *,
            vtx_value_t *, uint32_t);
        vtx_jit_entry_t entry = (vtx_jit_entry_t)method.compiled_code;
        vtx_value_t arg2 = vtx_make_smi(7);
        vtx_value_t r_jit = entry(&method, NULL, (void*)1, &arg2, 1);

        printf("JIT mul5(7) raw=0x%lX is_smi=%d\n",
               (unsigned long)r_jit, vtx_is_smi(r_jit));
    }

    vtx_compile_result_destroy(&result);
    vtx_method_registry_destroy(&registry);
    vtx_code_cache_destroy(&cache);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);
    return 0;
}
