/* Test: does T2 correctly return a constant? */
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

static vtx_value_t run_jit(const char *prog, vtx_value_t arg) {
    vtx_assembler_t a; vtx_asm_init(&a);
    vtx_asm_program(&a, prog);
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_graph_t graph; vtx_graph_init(&graph, 1);
    vtx_bytecode_t bc = vtx_asm_emit(&a);
    vtx_method_desc_t method = {
        .name = "test", .signature = "(I)I", .bytecode = &bc,
        .compiled_code = NULL, .vtable_index = 0, .arg_count = 1, .is_virtual = false,
    };
    vtx_graph_build(&graph, &bc, &method, &arena);
    vtx_pipeline_config_t config = vtx_pipeline_config_t2();
    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 20);
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);
    config.code_cache = &cache; config.method_registry = &registry; config.method = &method;
    vtx_compile_result_t result; memset(&result, 0, sizeof(result));
    vtx_pipeline_run(&graph, &config, &arena, &result);
    vtx_value_t r = vtx_make_smi(0xDEAD);
    if (result.success && method.compiled_code) {
        typedef vtx_value_t (*entry_t)(const vtx_method_desc_t *, void *, void *, vtx_value_t *, uint32_t);
        entry_t e = (entry_t)method.compiled_code;
        r = e(&method, NULL, (void*)1, &arg, 1);
    }
    vtx_compile_result_destroy(&result);
    vtx_pipeline_config_destroy(&config);
    vtx_code_cache_destroy(&cache);
    vtx_method_registry_destroy(&registry);
    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_asm_destroy(&a);
    return r;
}

int main(void) {
    /* Test 1: return constant 42 */
    const char *ret_42 =
        ".method ret42 (I)I\n.arg_count 1\nload_const_int 42\nreturn_value\n";
    vtx_value_t r1 = run_jit(ret_42, vtx_make_smi(0));
    printf("ret42 = 0x%016llX (expected 0x%016llX) %s\n",
           (unsigned long long)r1, (unsigned long long)vtx_make_smi(42),
           r1 == vtx_make_smi(42) ? "PASS" : "FAIL");

    /* Test 2: return constant 0 */
    const char *ret_0 =
        ".method ret0 (I)I\n.arg_count 1\nload_const_int 0\nreturn_value\n";
    vtx_value_t r2 = run_jit(ret_0, vtx_make_smi(0));
    printf("ret0  = 0x%016llX (expected 0x%016llX) %s\n",
           (unsigned long long)r2, (unsigned long long)vtx_make_smi(0),
           r2 == vtx_make_smi(0) ? "PASS" : "FAIL");

    /* Test 3: return constant 1 */
    const char *ret_1 =
        ".method ret1 (I)I\n.arg_count 1\nload_const_int 1\nreturn_value\n";
    vtx_value_t r3 = run_jit(ret_1, vtx_make_smi(0));
    printf("ret1  = 0x%016llX (expected 0x%016llX) %s\n",
           (unsigned long long)r3, (unsigned long long)vtx_make_smi(1),
           r3 == vtx_make_smi(1) ? "PASS" : "FAIL");

    /* Test 4: if_true returning constants */
    const char *if_test =
        ".method iftest (I)I\n.arg_count 1\n"
        "load_local 0\nif_true truthy\n"
        "load_const_int 0\nreturn_value\n"
        "truthy:\nload_const_int 1\nreturn_value\n";
    vtx_value_t r4a = run_jit(if_test, vtx_make_smi(5));
    printf("iftest(5)  = 0x%016llX (expected 0x%016llX) %s\n",
           (unsigned long long)r4a, (unsigned long long)vtx_make_smi(1),
           r4a == vtx_make_smi(1) ? "PASS" : "FAIL");
    vtx_value_t r4b = run_jit(if_test, vtx_make_smi(0));
    printf("iftest(0)  = 0x%016llX (expected 0x%016llX) %s\n",
           (unsigned long long)r4b, (unsigned long long)vtx_make_smi(0),
           r4b == vtx_make_smi(0) ? "PASS" : "FAIL");

    return 0;
}
