/* Test rep_infer on a simple program: sum(n) = sum of 0..n-1
 * Trace the IR before and after rep_infer to find the BoxInt bug. */
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
#include "ir/graph.h"
#include "ir/node.h"
#include "ir/rep_infer.h"
#include "interp/dispatch.h"
#include "compile/pipeline.h"
#include "codecache/install.h"

static void print_graph(const char *label, vtx_graph_t *graph) {
    fprintf(stderr, "=== %s ===\n", label);
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        vtx_node_t *n = &graph->node_table.nodes[i];
        if (n->dead) continue;
        fprintf(stderr, "  N%u: %s", i, vtx_node_opcode_name(n->opcode));
        if (vtx_nf_has(n->flags, VTX_NF_RAW_INT)) fprintf(stderr, " [RAW]");
        fprintf(stderr, " inputs:");
        for (uint32_t j = 0; j < n->input_count; j++) {
            fprintf(stderr, " N%u", n->inputs[j]);
        }
        fprintf(stderr, " uses:");
        for (uint32_t u = 0; u < n->use_count; u++) {
            fprintf(stderr, " N%u[%u]", n->uses[u].user_id, n->uses[u].input_index);
        }
        fprintf(stderr, "\n");
    }
}

int main(void) {
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_NONE);

    /* sum(n): locals=[n, sum, i], const=[0, 1]
     *   sum = 0; i = 0;
     *   while (i < n) { sum += i; i++; }
     *   return sum; */
    uint8_t code[] = {
        6,0,0, 3,0,1,   /* sum=0 */
        6,0,0, 3,0,2,   /* i=0 */
        /* loop@12 */
        2,0,2, 2,0,0, 31, 43,0,38,  /* if i>=n goto done@38 */
        2,0,1, 2,0,2, 13, 3,0,1,   /* sum += i */
        2,0,2, 6,0,1, 13, 3,0,2,   /* i++ */
        41,0,12,                       /* goto loop */
        /* done@38 */
        2,0,1, 52,                    /* return sum */
    };
    vtx_value_t consts[2] = { vtx_make_smi(0), vtx_make_smi(1) };
    vtx_bytecode_t bc = { code, sizeof(code), consts, 2, 3, 4 };
    vtx_method_desc_t m = { "sum","(I)I",&bc,NULL,1,1,false };

    /* Build graph */
    vtx_graph_t g; vtx_graph_init(&g, 1);
    vtx_graph_build(&g, &bc, &m, &arena);

    /* Run T2 pipeline up to (but not including) rep_infer */
    vtx_pipeline_config_t cfg = vtx_pipeline_config_t2();
    cfg.run_rep_infer = false;
    cfg.run_block_layout = false;
    vtx_code_cache_t cc; vtx_code_cache_init(&cc, 1<<20);
    vtx_method_registry_t mr; vtx_method_registry_init(&mr, &arena);
    cfg.code_cache = &cc; cfg.method_registry = &mr; cfg.method = &m;

    /* Compile to get the graph through GVN/SCCP/DCE/LICM */
    vtx_compile_result_t r; memset(&r, 0, sizeof(r));
    vtx_pipeline_run(&g, &cfg, &arena, &r);
    vtx_compile_result_destroy(&r);

    /* Now run rep_infer manually and trace */
    print_graph("Before rep_infer", &g);

    uint32_t inserted = vtx_rep_infer_run(&g, &arena);
    fprintf(stderr, "\nInserted %u UnboxInt/BoxInt nodes\n\n", inserted);

    print_graph("After rep_infer", &g);

    /* Test correctness */
    m.compiled_code = NULL;
    vtx_graph_t g2; vtx_graph_init(&g2, 1);
    vtx_graph_build(&g2, &bc, &m, &arena);

    vtx_pipeline_config_t cfg2 = vtx_pipeline_config_t2();
    cfg2.run_rep_infer = true;
    cfg2.run_block_layout = false;
    vtx_code_cache_t cc2; vtx_code_cache_init(&cc2, 1<<20);
    vtx_method_registry_t mr2; vtx_method_registry_init(&mr2, &arena);
    cfg2.code_cache = &cc2; cfg2.method_registry = &mr2; cfg2.method = &m;
    vtx_compile_result_t r2; memset(&r2, 0, sizeof(r2));
    int rc = vtx_pipeline_run(&g2, &cfg2, &arena, &r2);
    vtx_compile_result_destroy(&r2);

    if (rc == 0 && r2.success && m.compiled_code) {
        typedef vtx_value_t(*E)(const vtx_method_desc_t*,void*,void*,vtx_value_t*,uint32_t);
        E jit = (E)m.compiled_code;
        vtx_value_t arg = vtx_make_smi(100);
        vtx_value_t r = jit(&m, NULL, (void*)1, &arg, 1);
        printf("JIT sum(100) = %lld (expected 4950)\n",
               vtx_is_smi(r) ? (long long)vtx_smi_value(r) : -999);
    } else {
        printf("Compilation failed (rc=%d success=%d)\n", rc, r2.success);
    }

    vtx_method_registry_destroy(&mr); vtx_code_cache_destroy(&cc);
    vtx_method_registry_destroy(&mr2); vtx_code_cache_destroy(&cc2);
    vtx_gc_destroy(&gc); vtx_type_system_destroy(&ts); vtx_arena_destroy(&arena);
    return 0;
}
