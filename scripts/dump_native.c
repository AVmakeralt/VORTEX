/* Test: dump native code for iftest to see what's actually emitted */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "ir/verify.h"
#include "compile/pipeline.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "assembler.h"

int main(void) {
    vtx_assembler_t a; vtx_asm_init(&a);
    const char *prog =
        ".method iftest (I)I\n.arg_count 1\n"
        "load_local 0\nif_true truthy\n"
        "load_const_int 0\nreturn_value\n"
        "truthy:\nload_const_int 1\nreturn_value\n";
    vtx_asm_program(&a, prog);

    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_graph_t graph; vtx_graph_init(&graph, 1);
    vtx_bytecode_t bc = vtx_asm_emit(&a);
    vtx_method_desc_t method = {
        .name = "iftest", .signature = "(I)I", .bytecode = &bc,
        .compiled_code = NULL, .vtable_index = 0, .arg_count = 1, .is_virtual = false,
    };
    vtx_graph_build(&graph, &bc, &method, &arena);

    vtx_pipeline_config_t config = vtx_pipeline_config_t2();
    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 20);
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);
    config.code_cache = &cache; config.method_registry = &registry; config.method = &method;
    vtx_compile_result_t result; memset(&result, 0, sizeof(result));
    int prc = vtx_pipeline_run(&graph, &config, &arena, &result);
    printf("pipeline rc=%d success=%d code=%p size=%u\n",
           prc, result.success, result.native_code, result.native_size);

    if (method.compiled_code) {
        /* The code is installed in the code cache. We can read it from there
         * since the code cache memory is executable and readable. */
        uint8_t *code = (uint8_t *)method.compiled_code;
        printf("\nNative code at %p:\n", code);
        for (uint32_t i = 0; i < 300; i++) {
            printf("%02X ", code[i]);
            if ((i + 1) % 16 == 0) printf("\n");
        }
        printf("\n");

        typedef vtx_value_t (*entry_t)(const vtx_method_desc_t *, void *, void *, vtx_value_t *, uint32_t);
        entry_t e = (entry_t)method.compiled_code;
        vtx_value_t arg = vtx_make_smi(5);
        vtx_value_t r = e(&method, NULL, (void*)1, &arg, 1);
        printf("\niftest(5) = 0x%016llX (expected 0x%016llX)\n",
               (unsigned long long)r, (unsigned long long)vtx_make_smi(1));

        arg = vtx_make_smi(0);
        r = e(&method, NULL, (void*)1, &arg, 1);
        printf("iftest(0) = 0x%016llX (expected 0x%016llX)\n",
               (unsigned long long)r, (unsigned long long)vtx_make_smi(0));
    }

    vtx_compile_result_destroy(&result);
    vtx_pipeline_config_destroy(&config);
    vtx_code_cache_destroy(&cache);
    vtx_method_registry_destroy(&registry);
    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_asm_destroy(&a);
    return 0;
}
