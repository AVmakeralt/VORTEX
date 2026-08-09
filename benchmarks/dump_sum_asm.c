/**
 * dump_sum_asm.c — Dump the generated x86-64 assembly for sum(N)
 * to see exactly what the T2 JIT produces per loop iteration.
 *
 * This tells us precisely where the tag/untag overhead lives.
 */

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

typedef vtx_value_t (*jit_entry_t)(const vtx_method_desc_t *, void *, void *,
                                    vtx_value_t *, uint32_t);

static const char *PROG_SUM =
    ".method sum (I)I\n.arg_count 1\n.max_locals 3\n.max_stack 4\n"
    "load_const_int 0\nstore_local 1\n"
    "loop:\nload_local 0\nload_const_int 0\nicmp_le\nif_true done\n"
    "load_local 1\nload_local 0\niadd\nstore_local 1\n"
    "load_local 0\nload_const_int 1\nisub\nstore_local 0\n"
    "goto loop\n"
    "done:\nload_local 1\nreturn_value\n";

int main(void) {
    vtx_assembler_t *a = calloc(1, sizeof(*a));
    vtx_arena_t *arena = calloc(1, sizeof(*arena));
    vtx_type_system_t *ts = calloc(1, sizeof(*ts));
    vtx_gc_t *gc = calloc(1, sizeof(*gc));
    vtx_graph_t *graph = calloc(1, sizeof(*graph));
    vtx_code_cache_t *cache = calloc(1, sizeof(*cache));
    vtx_method_registry_t *reg = calloc(1, sizeof(*reg));
    vtx_method_desc_t *method = calloc(1, sizeof(*method));
    vtx_bytecode_t *bc = calloc(1, sizeof(*bc));

    vtx_asm_init(a);
    vtx_asm_program(a, PROG_SUM);
    *bc = vtx_asm_emit(a);

    vtx_arena_init(arena);
    vtx_type_system_init(ts);
    vtx_gc_init(gc, ts, VTX_GC_GENERATIONAL);
    vtx_graph_init(graph, 1);

    method->name = "sum";
    method->signature = "(I)I";
    method->bytecode = bc;
    method->arg_count = 1;
    method->is_virtual = false;

    if (vtx_graph_build(graph, bc, method, arena) != 0) {
        fprintf(stderr, "graph build failed\n");
        return 1;
    }

    /* Dump IR BEFORE optimization */
    fprintf(stderr, "=== IR before optimization ===\n");
    vtx_graph_print(graph);
    fprintf(stderr, "\n");

    vtx_pipeline_config_t config = vtx_pipeline_config_t2();
    vtx_code_cache_init(cache, 1 << 20);
    vtx_method_registry_init(reg, arena);
    config.code_cache = cache;
    config.method_registry = reg;
    config.method = method;

    vtx_compile_result_t result;
    memset(&result, 0, sizeof(result));
    int rc = vtx_pipeline_run(graph, &config, arena, &result);
    fprintf(stderr, "compile rc=%d success=%d code=%p\n",
            rc, result.success, method->compiled_code);

    /* Dump IR AFTER optimization */
    fprintf(stderr, "\n=== IR after optimization ===\n");
    vtx_graph_print(graph);
    fprintf(stderr, "\n");

    /* Dump the generated machine code as hex */
    if (method->compiled_code) {
        /* The compiled code is at method->compiled_code.
         * We don't know the exact size, but we can dump the first
         * 512 bytes as hex for inspection. */
        uint8_t *code = (uint8_t *)method->compiled_code;
        fprintf(stderr, "=== Generated x86-64 machine code (first 256 bytes) ===\n");
        for (int i = 0; i < 256; i++) {
            if (i % 16 == 0) fprintf(stderr, "  %04x: ", i);
            fprintf(stderr, "%02x ", code[i]);
            if (i % 16 == 15) fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");

        /* Verify correctness */
        vtx_value_t v = vtx_make_smi(100);
        vtx_value_t r = ((jit_entry_t)method->compiled_code)(
            method, NULL, (void*)1, &v, 1);
        fprintf(stderr, "sum(100) = %ld (expected 5050)\n",
                (long)vtx_smi_value(r));
    }

    return 0;
}
