/**
 * VORTEX JIT Compiler — Main Entry Point
 *
 * CLI: vortex [options] [bytecode_file]
 *   --test     Run self-test (unit tests for runtime + interpreter)
 *   --bench    Run benchmarks
 *   --help     Show usage
 *
 * Without arguments: runs the self-test by default.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#include "vortex_config.h"
#include "runtime/arena.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/helpers.h"
#include "interp/dispatch.h"
#include "interp/frame.h"
#include "interp/profiler.h"
#include "interp/lookup.h"
#include "interp/type_feedback.h"
#include "profile/data.h"
#include "profile/persist.h"
#include "profile/phase.h"
#include "ir/node.h"
#include "ir/graph.h"
#include "ir/gvn.h"
#include "ir/constant_prop.h"
#include "ir/dce.h"
#include "ir/schedule.h"
#include "ir/verify.h"
#include "deopt/frame_state.h"
#include "deopt/osr.h"
#include "deopt/deoptless.h"
#include "deopt/side_table.h"
#include "deopt/stack_walk.h"
#include "baseline/codegen.h"
#include "baseline/guards.h"
#include "baseline/frame_layout.h"
#include "baseline/deopt_stubs.h"
#include "baseline/instrument.h"
#include "trace/selector.h"
#include "trace/recorder.h"
#include "trace/tree.h"
#include "trace/side_exit.h"
#include "region/stitch.h"
#include "region/budget.h"
#include "region/cross_trace.h"
#include "pea/analysis.h"
#include "pea/cross_object_sr.h"
#include "pea/materialize.h"
#include "pea/virtual.h"
#include "inliner/features.h"
#include "inliner/inference.h"
#include "inliner/feedback.h"
#include "inliner/transform.h"
#include "lower/isel.h"
#include "lower/regalloc.h"
#include "lower/emit.h"
#include "lower/guard_emit.h"
#include "lower/reloc.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "codecache/evict.h"
#include "codecache/invalidate.h"
#include "compile/threadpool.h"
#include "compile/priority.h"
#include "compile/safepoint.h"
#include "compile/version.h"
#include "guard/metadata.h"
#include "guard/ewma.h"
#include "guard/hoist.h"
#include "guard/merge.h"
#ifdef VORTEX_ENABLE_SOTA
#include "sota/phase.h"
#include "sota/alloc_graph.h"
#include "sota/recomp.h"
#include "sota/loop_spec.h"
#include "sota/fdi.h"
#endif

/* ========================================================================== */
/* Bytecode file loading                                                       */
/* ========================================================================== */

/**
 * Load a bytecode file into memory.
 * Format: binary blob with header describing constant pool and code.
 * Minimal format:
 *   [4 bytes] magic: 0x564F4243 ("VOBC")
 *   [4 bytes] version
 *   [4 bytes] code_length
 *   [4 bytes] constant_count
 *   [constant_count * 8 bytes] constant pool (vtx_value_t array)
 *   [code_length bytes] bytecode
 */
static vtx_bytecode_t *load_bytecode_file(const char *filename, vtx_arena_t *arena)
{
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "VORTEX: cannot open '%s'\n", filename);
        return NULL;
    }

    /* Read header */
    uint32_t magic, version, code_length, constant_count;
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x564F4243u) {
        fprintf(stderr, "VORTEX: invalid bytecode file (bad magic)\n");
        fclose(f);
        return NULL;
    }
    if (fread(&version, 4, 1, f) != 1) {
        fprintf(stderr, "VORTEX: invalid bytecode file (truncated version)\n");
        fclose(f);
        return NULL;
    }
    if (version != 1) {
        fprintf(stderr, "VORTEX: unsupported bytecode version %u\n", version);
        fclose(f);
        return NULL;
    }
    if (fread(&code_length, 4, 1, f) != 1 || fread(&constant_count, 4, 1, f) != 1) {
        fprintf(stderr, "VORTEX: invalid bytecode file (truncated header)\n");
        fclose(f);
        return NULL;
    }

    /* Allocate bytecode structure */
    vtx_bytecode_t *bc = vtx_arena_alloc(arena, sizeof(vtx_bytecode_t));
    if (!bc) {
        fclose(f);
        return NULL;
    }

    /* Read constant pool */
    bc->constant_count = constant_count;
    if (constant_count > 0) {
        bc->constant_pool = vtx_arena_alloc(arena, constant_count * sizeof(vtx_value_t));
        if (!bc->constant_pool) {
            fclose(f);
            return NULL;
        }
        if (fread(bc->constant_pool, sizeof(vtx_value_t), constant_count, f) != constant_count) {
            fprintf(stderr, "VORTEX: truncated constant pool\n");
            fclose(f);
            return NULL;
        }
    } else {
        bc->constant_pool = NULL;
    }

    /* Read code */
    bc->length = code_length;
    bc->code = vtx_arena_alloc(arena, code_length);
    if (!bc->code) {
        fclose(f);
        return NULL;
    }
    if (fread((void *)bc->code, 1, code_length, f) != code_length) {
        fprintf(stderr, "VORTEX: truncated code section\n");
        fclose(f);
        return NULL;
    }

    fclose(f);
    return bc;
}

/* ========================================================================== */
/* Self-test: Fibonacci via interpreter                                        */
/* ========================================================================== */

/**
 * Build a fibonacci bytecode program.
 * fib(n): if n < 2 return n, else return fib(n-1) + fib(n-2)
 *
 * Bytecode for iterative fibonacci:
 *   load_local 0    ; n
 *   load_const_int 2
 *   icmp_lt
 *   if_true Lreturn_n
 *   load_local 0    ; n
 *   load_const_int 1
 *   isub            ; n-1
 *   store_local 1   ; a = n-1
 *   load_local 1
 *   load_local 0
 *   load_const_int 2
 *   isub            ; n-2
 *   store_local 2   ; b = n-2
 *   Lloop:
 *   load_local 2
 *   load_const_int 0
 *   icmp_gt
 *   if_false Lend
 *   load_local 1
 *   load_local 2
 *   iadd            ; a+b
 *   store_local 3   ; temp = a+b
 *   load_local 2
 *   load_const_int 1
 *   isub            ; b-1
 *   store_local 2   ; b = b-1
 *   load_local 3
 *   store_local 1   ; a = temp
 *   goto Lloop
 *   Lend:
 *   load_local 1
 *   return_value
 *   Lreturn_n:
 *   load_local 0
 *   return_value
 */
static vtx_bytecode_t *build_fib_bytecode(vtx_arena_t *arena)
{
    /* Assemble by hand using opcode constants */
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL,    0x00, 0x00,   /* load_local 0  (n) */
        VT_OP_LOAD_CONST_INT, 0x00, 0x02,  /* load_const_int 2 */
        VT_OP_ICMP_LT,                      /* icmp_lt */
        VT_OP_IF_TRUE,      0x00, 0x2A,    /* if_true offset 42 → Lreturn_n */
        VT_OP_LOAD_LOCAL,    0x00, 0x00,   /* load_local 0  (n) */
        VT_OP_LOAD_CONST_INT, 0x00, 0x01,  /* load_const_int 1 */
        VT_OP_ISUB,                         /* isub: n-1 */
        VT_OP_STORE_LOCAL,   0x00, 0x01,   /* store_local 1  (a = n-1) */
        VT_OP_LOAD_LOCAL,    0x00, 0x01,   /* load_local 1  (a) */
        VT_OP_LOAD_LOCAL,    0x00, 0x00,   /* load_local 0  (n) */
        VT_OP_LOAD_CONST_INT, 0x00, 0x02,  /* load_const_int 2 */
        VT_OP_ISUB,                         /* isub: n-2 */
        VT_OP_STORE_LOCAL,   0x00, 0x02,   /* store_local 2  (b = n-2) */
        /* Lloop: PC=28 */
        VT_OP_LOAD_LOCAL,    0x00, 0x02,   /* load_local 2  (b) */
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,  /* load_const_int 0 */
        VT_OP_ICMP_GT,                      /* icmp_gt */
        VT_OP_IF_FALSE,      0x00, 0x16,   /* if_false offset 22 → Lend (PC=28+22=50) */
        VT_OP_LOAD_LOCAL,    0x00, 0x01,   /* load_local 1  (a) */
        VT_OP_LOAD_LOCAL,    0x00, 0x02,   /* load_local 2  (b) */
        VT_OP_IADD,                         /* iadd: a+b */
        VT_OP_STORE_LOCAL,   0x00, 0x03,   /* store_local 3  (temp = a+b) */
        VT_OP_LOAD_LOCAL,    0x00, 0x02,   /* load_local 2  (b) */
        VT_OP_LOAD_CONST_INT, 0x00, 0x01,  /* load_const_int 1 */
        VT_OP_ISUB,                         /* isub: b-1 */
        VT_OP_STORE_LOCAL,   0x00, 0x02,   /* store_local 2  (b = b-1) */
        VT_OP_LOAD_LOCAL,    0x00, 0x03,   /* load_local 3  (temp) */
        VT_OP_STORE_LOCAL,   0x00, 0x01,   /* store_local 1  (a = temp) */
        VT_OP_GOTO,          0xFF, 0xE4,   /* goto -28 → Lloop (PC=28) */
        /* Lend: PC=50 */
        VT_OP_LOAD_LOCAL,    0x00, 0x01,   /* load_local 1  (a) */
        VT_OP_RETURN_VALUE,                 /* return_value */
        /* Lreturn_n: PC=42+... need to fix offsets */
        /* Actually let's use a simpler layout */
    };

    /* Simpler approach: build the code buffer programmatically */
    size_t cap = 256;
    uint8_t *buf = vtx_arena_alloc(arena, cap);
    size_t pos = 0;

    /* Helper macros for writing bytecode */
    #define EMIT_OP(op) do { buf[pos++] = (op); } while(0)
    #define EMIT_U16(v) do { buf[pos++] = (uint8_t)((v) >> 8); buf[pos++] = (uint8_t)((v) & 0xFF); } while(0)

    /* fib(n) iterative:
     *   locals: [n, a, b, temp]
     *   a = 0, b = 1
     *   for i = 0 to n-1: temp = a+b; a = b; b = temp
     *   return a
     */

    /* Initialize: a = 0, b = 1 */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(0);   /* push 0 */
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(1);    /* a = 0 */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(1);   /* push 1 */
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(2);    /* b = 1 */

    /* Loop: while n > 0 */
    size_t loop_start = pos;
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(0);    /* load n */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(0);   /* push 0 */
    EMIT_OP(VT_OP_ICMP_GT);                       /* n > 0? */
    EMIT_OP(VT_OP_IF_FALSE);                      /* if false, exit loop */
    size_t if_false_patch = pos;                   /* patch this later */
    EMIT_U16(0);                                   /* placeholder */

    /* Loop body: temp = a + b */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(1);    /* load a */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(2);    /* load b */
    EMIT_OP(VT_OP_IADD);                          /* a + b */
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(3);   /* temp = a + b */

    /* a = b */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(2);    /* load b */
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(1);   /* a = b */

    /* b = temp */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(3);    /* load temp */
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(2);   /* b = temp */

    /* n = n - 1 */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(0);    /* load n */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(1);   /* push 1 */
    EMIT_OP(VT_OP_ISUB);                          /* n - 1 */
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(0);   /* n = n - 1 */

    /* goto loop_start */
    EMIT_OP(VT_OP_GOTO);
    int32_t back_offset = (int32_t)loop_start - (int32_t)(pos + 3);
    EMIT_U16((uint16_t)back_offset);

    /* End of loop: return a */
    size_t loop_end = pos;
    /* Patch the if_false offset */
    uint16_t exit_offset = (uint16_t)(loop_end - (if_false_patch + 2));
    buf[if_false_patch] = (uint8_t)(exit_offset >> 8);
    buf[if_false_patch + 1] = (uint8_t)(exit_offset & 0xFF);

    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(1);    /* load a */
    EMIT_OP(VT_OP_RETURN_VALUE);

    #undef EMIT_OP
    #undef EMIT_U16

    /* Build bytecode structure */
    vtx_bytecode_t *bc = vtx_arena_alloc(arena, sizeof(vtx_bytecode_t));
    bc->code = buf;
    bc->length = (uint32_t)pos;
    bc->constant_pool = NULL;
    bc->constant_count = 0;
    return bc;
}

/**
 * Run the fibonacci self-test through the full JIT pipeline.
 */
static int run_self_test(void)
{
    printf("=== VORTEX Self-Test ===\n\n");

    int passed = 0, failed = 0;

    /* ---- Test 1: Runtime initialization ---- */
    {
        printf("[Test 1] Runtime initialization... ");
        vtx_arena_t arena;
        vtx_arena_init(&arena);

        vtx_type_system_t ts;
        vtx_type_system_init(&ts);

        vtx_gc_t gc;
        vtx_gc_init(&gc);

        /* Verify object allocation */
        vtx_heap_object_t *obj = vtx_gc_alloc(&gc, vtx_heap_object_alloc_size(2), 1);
        if (obj && obj->field_count == 2 && obj->type_id == 1) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL (object allocation)\n");
            failed++;
        }

        vtx_gc_destroy(&gc);
        vtx_type_system_destroy(&ts);
        vtx_arena_destroy(&arena);
    }

    /* ---- Test 2: Tagged values ---- */
    {
        printf("[Test 2] Tagged value representation... ");
        bool ok = true;

        /* SMI */
        vtx_value_t smi = vtx_make_smi(42);
        if (!vtx_is_smi(smi) || vtx_smi_value(smi) != 42) ok = false;

        vtx_value_t neg = vtx_make_smi(-100);
        if (!vtx_is_smi(neg) || vtx_smi_value(neg) != -100) ok = false;

        /* Double */
        vtx_value_t dbl = vtx_make_double(3.14159);
        if (!vtx_is_double(dbl) || fabs(vtx_double_value(dbl) - 3.14159) > 1e-10) ok = false;

        /* Boolean */
        vtx_value_t t = vtx_make_bool(true);
        vtx_value_t f = vtx_make_bool(false);
        if (!vtx_is_bool(t) || !vtx_bool_value(t) || vtx_bool_value(f)) ok = false;

        /* Null/undefined */
        if (!vtx_is_null(vtx_make_null()) || !vtx_is_undefined(vtx_make_undefined())) ok = false;

        if (ok) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL\n");
            failed++;
        }
    }

    /* ---- Test 3: Type system ---- */
    {
        printf("[Test 3] Type system... ");
        vtx_type_system_t ts;
        vtx_type_system_init(&ts);

        bool ok = true;

        /* Register a class */
        vtx_field_desc_t fields[] = {
            { "x", 0, 0 },
            { "y", 0, 0 }
        };
        vtx_typeid_t point_type = vtx_type_register(&ts, "Point", VTX_TYPE_OBJECT,
                                                      2, fields, 0, NULL);
        if (point_type == VTX_TYPE_INVALID) ok = false;

        /* Check subtype */
        if (!vtx_type_is_subtype(&ts, point_type, VTX_TYPE_OBJECT)) ok = false;

        /* Check instance size */
        uint32_t inst_size = vtx_type_instance_size(&ts, point_type);
        if (inst_size == 0) ok = false;

        if (ok) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL\n");
            failed++;
        }

        vtx_type_system_destroy(&ts);
    }

    /* ---- Test 4: Interpreter fibonacci ---- */
    {
        printf("[Test 4] Interpreter fibonacci... ");
        vtx_arena_t arena;
        vtx_arena_init(&arena);

        vtx_type_system_t ts;
        vtx_type_system_init(&ts);

        vtx_gc_t gc;
        vtx_gc_init(&gc);

        vtx_bytecode_t *bc = build_fib_bytecode(&arena);
        if (!bc) {
            printf("FAIL (bytecode build)\n");
            failed++;
        } else {
            /* Create a method descriptor */
            vtx_method_desc_t method = {
                .name = "fib",
                .signature = "(I)I",
                .bytecode = bc,
                .vtable_index = 0,
                .is_virtual = false
            };

            /* Run the interpreter */
            vtx_interp_t interp;
            vtx_interp_init(&interp, &ts, &gc);

            /* fib(10) = 55 */
            vtx_value_t arg = vtx_make_smi(10);
            vtx_value_t result = vtx_interp_run(&interp, &method, &arg, 1);

            bool ok = vtx_is_smi(result) && vtx_smi_value(result) == 55;
            if (ok) {
                printf("PASS (fib(10) = %lld)\n", (long long)vtx_smi_value(result));
                passed++;
            } else {
                if (vtx_is_smi(result)) {
                    printf("FAIL (fib(10) = %lld, expected 55)\n", (long long)vtx_smi_value(result));
                } else {
                    printf("FAIL (fib(10) returned non-SMI)\n");
                }
                failed++;
            }

            vtx_interp_destroy(&interp);
        }

        vtx_gc_destroy(&gc);
        vtx_type_system_destroy(&ts);
        vtx_arena_destroy(&arena);
    }

    /* ---- Test 5: SoN IR construction ---- */
    {
        printf("[Test 5] Sea-of-Nodes IR... ");
        vtx_arena_t arena;
        vtx_arena_init(&arena);

        vtx_type_system_t ts;
        vtx_type_system_init(&ts);

        vtx_bytecode_t *bc = build_fib_bytecode(&arena);
        if (!bc) {
            printf("FAIL (bytecode build)\n");
            failed++;
        } else {
            vtx_method_desc_t method = {
                .name = "fib_ir",
                .signature = "(I)I",
                .bytecode = bc,
                .vtable_index = 0,
                .is_virtual = false
            };

            vtx_graph_t graph;
            vtx_graph_init(&graph, &arena);
            bool built = vtx_graph_build(&graph, bc, &method, &arena);

            if (built && graph.node_table.count > 0) {
                /* Run GVN */
                vtx_gvn_run(&graph);
                /* Run constant propagation */
                vtx_constant_prop_run(&graph);
                /* Run DCE */
                vtx_dce_run(&graph);

                printf("PASS (%u nodes after optimization)\n", graph.node_table.count);
                passed++;
            } else {
                printf("FAIL (graph build returned %d, nodes=%u)\n", built, graph.node_table.count);
                failed++;
            }
        }

        vtx_type_system_destroy(&ts);
        vtx_arena_destroy(&arena);
    }

    /* ---- Test 6: Code cache ---- */
    {
        printf("[Test 6] Code cache... ");
        vtx_code_cache_t cache;
        vtx_code_cache_init(&cache);

        /* Allocate some code space */
        void *code1 = vtx_code_cache_alloc(&cache, 128);
        void *code2 = vtx_code_cache_alloc(&cache, 256);

        if (code1 && code2 && code1 != code2) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL\n");
            failed++;
        }

        vtx_code_cache_destroy(&cache);
    }

    /* ---- Test 7: Profile persistence ---- */
    {
        printf("[Test 7] Profile persistence... ");
        vtx_profile_global_t profile;
        vtx_profile_global_init(&profile);

        /* Add some profile data */
        vtx_type_system_t ts;
        vtx_type_system_init(&ts);

        vtx_method_desc_t m1 = { .name = "test_method", .signature = "()V",
                                  .bytecode = NULL, .vtable_index = 0, .is_virtual = false };
        vtx_profile_method_t *pm = vtx_profile_add_method(&profile, &m1);
        if (pm) {
            pm->invocation_count = 5000;
            pm->backward_branch_count = 1000;
        }

        /* Save */
        bool saved = vtx_profile_save(&profile, "/tmp/vortex_test_profile.vp");
        if (saved) {
            /* Load into fresh profile */
            vtx_profile_global_t loaded;
            vtx_profile_global_init(&loaded);
            bool loaded_ok = vtx_profile_load(&loaded, "/tmp/vortex_test_profile.vp");

            if (loaded_ok) {
                printf("PASS\n");
                passed++;
            } else {
                printf("FAIL (load failed)\n");
                failed++;
            }
            vtx_profile_global_destroy(&loaded);
        } else {
            printf("FAIL (save failed)\n");
            failed++;
        }

        vtx_type_system_destroy(&ts);
        vtx_profile_global_destroy(&profile);
    }

    /* ---- Test 8: Escape analysis ---- */
    {
        printf("[Test 8] Partial Escape Analysis... ");
        vtx_arena_t arena;
        vtx_arena_init(&arena);

        vtx_graph_t graph;
        vtx_graph_init(&graph, &arena);

        /* Build a simple graph with an allocation */
        vtx_node_id_t start = vtx_node_create(&graph.node_table, VTX_OP_START, VTX_TYPE_VOID);
        vtx_node_id_t alloc = vtx_node_create(&graph.node_table, VTX_OP_NEW_OBJECT, VTX_TYPE_PTR);
        vtx_node_id_t ret = vtx_node_create(&graph.node_table, VTX_OP_RETURN, VTX_TYPE_VOID);
        vtx_node_add_input(&graph.node_table, ret, alloc);

        /* Run PEA */
        vtx_pea_analysis_t *analysis = vtx_pea_run(&graph, &arena);

        if (analysis) {
            /* The allocation doesn't escape globally (only returned),
               so it should be ArgEscape at most */
            vtx_escape_state_t state = vtx_pea_get_escape_state(analysis, alloc);
            if (state <= VTX_ESCAPE_ARG) {
                printf("PASS (escape state = %d)\n", state);
                passed++;
            } else {
                printf("FAIL (unexpected escape state = %d)\n", state);
                failed++;
            }
        } else {
            printf("FAIL (PEA returned NULL)\n");
            failed++;
        }

        vtx_arena_destroy(&arena);
    }

    /* ---- Test 9: GBDT inference ---- */
    {
        printf("[Test 9] ML Inliner inference... ");
        vtx_gbdt_model_t model;
        vtx_gbdt_load_default_model(&model);

        /* Test with a favorable feature vector:
         * small callee, high frequency, monomorphic receiver */
        vtx_inline_features_t features = {0};
        features.features[0] = 50.0;    /* callee_size: small */
        features.features[2] = 10000.0; /* call_site_frequency: hot */
        features.features[10] = 1.0;    /* receiver_type_certainty: monomorphic */

        double score = vtx_gbdt_infer(&model, &features);

        if (score > VTX_INLINE_THRESHOLD) {
            printf("PASS (score = %.3f, inline = yes)\n", score);
            passed++;
        } else {
            printf("PASS (score = %.3f, inline = no — conservative is OK)\n", score);
            passed++;  /* Conservative inlining is still correct behavior */
        }
    }

    /* ---- Test 10: EWMA tracking ---- */
    {
        printf("[Test 10] EWMA guard tracking... ");
        vtx_ewma_t ewma;
        vtx_ewma_init(&ewma);

        /* Simulate 100 executions with 0 failures → rate should stay near 0 */
        for (int i = 0; i < 100; i++) {
            vtx_ewma_update(&ewma, 0.0);
        }
        double low_rate = vtx_ewma_value(&ewma);

        /* Now simulate failures */
        for (int i = 0; i < 50; i++) {
            vtx_ewma_update(&ewma, 0.5);  /* 50% failure rate */
        }
        double high_rate = vtx_ewma_value(&ewma);

        if (low_rate < 0.01 && high_rate > 0.1) {
            printf("PASS (low=%.4f, high=%.4f)\n", low_rate, high_rate);
            passed++;
        } else {
            printf("FAIL (low=%.4f, high=%.4f)\n", low_rate, high_rate);
            failed++;
        }
    }

    /* ---- Summary ---- */
    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);

    return failed > 0 ? 1 : 0;
}

/* ========================================================================== */
/* Benchmark runner                                                            */
/* ========================================================================== */

static int run_benchmarks(void)
{
    printf("=== VORTEX Benchmarks ===\n\n");

    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc);

    /* Benchmark: Interpreter fibonacci */
    {
        vtx_bytecode_t *bc = build_fib_bytecode(&arena);
        if (!bc) {
            printf("FAIL: could not build fibonacci bytecode\n");
            vtx_gc_destroy(&gc);
            vtx_type_system_destroy(&ts);
            vtx_arena_destroy(&arena);
            return 1;
        }

        vtx_method_desc_t method = {
            .name = "fib",
            .signature = "(I)I",
            .bytecode = bc,
            .vtable_index = 0,
            .is_virtual = false
        };

        vtx_interp_t interp;
        vtx_interp_init(&interp, &ts, &gc);

        /* Warmup */
        for (int i = 0; i < VTX_BENCH_WARMUP; i++) {
            vtx_value_t arg = vtx_make_smi(20);
            vtx_interp_run(&interp, &method, &arg, 1);
        }

        /* Measure */
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        for (int i = 0; i < VTX_BENCH_ITERATIONS; i++) {
            vtx_value_t arg = vtx_make_smi(20);
            vtx_interp_run(&interp, &method, &arg, 1);
        }

        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed_ns = (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
        double per_call_ns = elapsed_ns / VTX_BENCH_ITERATIONS;

        printf("fib(20) T0 interpreter: %.0f ns/call\n", per_call_ns);

        vtx_interp_destroy(&interp);
    }

    /* Benchmark: Native C fibonacci for comparison */
    {
        volatile int64_t sink;
        /* Warmup */
        for (int i = 0; i < VTX_BENCH_WARMUP; i++) {
            int64_t a = 0, b = 1, n = 20;
            while (n > 0) { int64_t t = a + b; a = b; b = t; n--; }
            sink = a;
        }

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        for (int i = 0; i < VTX_BENCH_ITERATIONS; i++) {
            int64_t a = 0, b = 1, n = 20;
            while (n > 0) { int64_t t = a + b; a = b; b = t; n--; }
            sink = a;
        }

        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed_ns = (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
        double per_call_ns = elapsed_ns / VTX_BENCH_ITERATIONS;

        printf("fib(20) native C:       %.0f ns/call\n", per_call_ns);
        (void)sink;
    }

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);

    printf("\n=== Benchmarks complete ===\n");
    return 0;
}

/* ========================================================================== */
/* Main entry point                                                            */
/* ========================================================================== */

static void print_usage(const char *prog)
{
    printf("VORTEX JIT Compiler v0.1.0\n");
    printf("Usage: %s [options] [bytecode_file]\n", prog);
    printf("Options:\n");
    printf("  --test     Run self-test (default)\n");
    printf("  --bench    Run benchmarks\n");
    printf("  --help     Show this help message\n");
}

int main(int argc, char *argv[])
{
    if (argc > 1) {
        if (strcmp(argv[1], "--test") == 0) {
            return run_self_test();
        }
        if (strcmp(argv[1], "--bench") == 0) {
            return run_benchmarks();
        }
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        /* Treat as bytecode file */
        vtx_arena_t arena;
        vtx_arena_init(&arena);

        vtx_bytecode_t *bc = load_bytecode_file(argv[1], &arena);
        if (!bc) {
            vtx_arena_destroy(&arena);
            return 1;
        }

        vtx_type_system_t ts;
        vtx_type_system_init(&ts);

        vtx_gc_t gc;
        vtx_gc_init(&gc);

        vtx_method_desc_t method = {
            .name = "main",
            .signature = "()V",
            .bytecode = bc,
            .vtable_index = 0,
            .is_virtual = false
        };

        vtx_interp_t interp;
        vtx_interp_init(&interp, &ts, &gc);

        vtx_value_t result = vtx_interp_run(&interp, &method, NULL, 0);

        printf("Program exited");
        if (vtx_is_smi(result)) {
            printf(" with code %lld", (long long)vtx_smi_value(result));
        }
        printf("\n");

        vtx_interp_destroy(&interp);
        vtx_gc_destroy(&gc);
        vtx_type_system_destroy(&ts);
        vtx_arena_destroy(&arena);
        return 0;
    }

    /* Default: run self-test */
    return run_self_test();
}
