/* ============================================================================ *
 * Dynamic code test: exercise the deopt system by feeding different types
 * to a JIT-compiled method.
 *
 * The method add_one(x) = x + 1 is compiled with T2. The first call uses
 * an SMI (fast path). The second call uses a non-SMI value (a heap object)
 * which should trigger a guard failure → deopt → interpreter resume.
 *
 * If deopt works, the second call returns a correct result (or a type error).
 * If deopt is broken, the second call segfaults.
 * ============================================================================ */

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
#include "interp/dispatch.h"
#include "compile/pipeline.h"
#include "codecache/install.h"

int main(void) {
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_NONE);

    /* add_one(x) = x + 1
     * locals: [x]
     * const: [1] */
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0, 0,        /* load x */
        VT_OP_LOAD_CONST_INT, 0, 0,    /* push 1 */
        VT_OP_IADD,                    /* x + 1 */
        VT_OP_RETURN_VALUE,
    };
    vtx_value_t consts[1] = { vtx_make_smi(1) };
    vtx_bytecode_t bc = { code, sizeof(code), consts, 1, 1, 4 };
    vtx_method_desc_t m = { "add_one","(I)I",&bc,NULL,1,1,false };

    /* Compile with T2 */
    vtx_graph_t g; vtx_graph_init(&g, 1);
    vtx_graph_build(&g, &bc, &m, &arena);
    vtx_pipeline_config_t cfg = vtx_pipeline_config_t2();
    vtx_code_cache_t cc; vtx_code_cache_init(&cc, 1<<20);
    vtx_method_registry_t mr; vtx_method_registry_init(&mr, &arena);
    cfg.code_cache = &cc; cfg.method_registry = &mr; cfg.method = &m;
    vtx_compile_result_t r; memset(&r, 0, sizeof(r));
    int rc = vtx_pipeline_run(&g, &cfg, &arena, &r);
    bool ok = r.success;
    void *code_ptr = m.compiled_code;
    vtx_compile_result_destroy(&r);

    if (!ok || !code_ptr) {
        printf("Compile failed (rc=%d)\n", rc);
        return 1;
    }

    typedef vtx_value_t(*E)(const vtx_method_desc_t*,void*,void*,vtx_value_t*,uint32_t);
    E jit = (E)code_ptr;

    printf("=== Dynamic Code / Deopt Test ===\n\n");

    /* Test 1: SMI input (fast path) */
    {
        vtx_value_t arg = vtx_make_smi(41);
        vtx_value_t result = jit(&m, NULL, (void*)1, &arg, 1);
        int64_t val = vtx_is_smi(result) ? vtx_smi_value(result) : -999;
        printf("Test 1: add_one(41) = %lld (expected 42) %s\n",
               (long long)val, val == 42 ? "PASS" : "FAIL");
    }

    /* Test 2: SMI input again (should still work) */
    {
        vtx_value_t arg = vtx_make_smi(99);
        vtx_value_t result = jit(&m, NULL, (void*)1, &arg, 1);
        int64_t val = vtx_is_smi(result) ? vtx_smi_value(result) : -999;
        printf("Test 2: add_one(99) = %lld (expected 100) %s\n",
               (long long)val, val == 100 ? "PASS" : "FAIL");
    }

    /* Test 3: Negative SMI (tests SMI encoding edge case) */
    {
        vtx_value_t arg = vtx_make_smi(-5);
        vtx_value_t result = jit(&m, NULL, (void*)1, &arg, 1);
        int64_t val = vtx_is_smi(result) ? vtx_smi_value(result) : -999;
        printf("Test 3: add_one(-5) = %lld (expected -4) %s\n",
               (long long)val, val == -4 ? "PASS" : "FAIL");
    }

    /* Test 4: Large SMI (well within range) */
    {
        vtx_value_t arg = vtx_make_smi(1000000);
        vtx_value_t result = jit(&m, NULL, (void*)1, &arg, 1);
        int64_t val = vtx_is_smi(result) ? vtx_smi_value(result) : -999;
        printf("Test 4: add_one(1000000) = %lld (expected 1000001) %s\n",
               (long long)val, val == 1000001 ? "PASS" : "FAIL");
    }

    /* Test 5: Non-SMI input (null) — should trigger deopt or type error
     * This is the critical deopt test. The JIT code assumes SMI input.
     * If deopt works, it falls back to interpreter.
     * If deopt is broken, it segfaults. */
    {
        printf("\nTest 5: add_one(NULL) — deopt test...\n");
        vtx_value_t arg = VTX_VALUE_NULL;
        printf("  Calling JIT with NULL...\n");
        fflush(stdout);
        vtx_value_t result = jit(&m, NULL, (void*)1, &arg, 1);
        printf("  JIT returned (no crash!)\n");
        if (vtx_is_smi(result)) {
            printf("  Result: %lld (interpreter handled it)\n",
                   (long long)vtx_smi_value(result));
        } else if (vtx_is_null(result)) {
            printf("  Result: null (type error handled)\n");
        } else {
            printf("  Result: unknown type\n");
        }
    }

    /* Test 6: Interpreter fallback comparison */
    printf("\nTest 6: Interpreter comparison...\n");
    {
        vtx_interp_t interp; vtx_interp_init(&interp, &ts, &gc);
        vtx_value_t arg = vtx_make_smi(41);
        vtx_value_t result = vtx_interp_run(&interp, &m, &arg, 1);
        vtx_interp_destroy(&interp);
        int64_t val = vtx_is_smi(result) ? vtx_smi_value(result) : -999;
        printf("  interp add_one(41) = %lld %s\n",
               (long long)val, val == 42 ? "PASS" : "FAIL");
    }

    /* Test 7: Loop with deopt potential
     * sum_loop(n) = sum of 0..n-1
     * Run with SMI arg first, then with NULL */
    {
        uint8_t loop_code[] = {
            VT_OP_LOAD_CONST_INT, 0, 0,  /* sum=0 */
            VT_OP_STORE_LOCAL, 0, 1,
            VT_OP_LOAD_CONST_INT, 0, 0,  /* i=0 */
            VT_OP_STORE_LOCAL, 0, 2,
            /* loop@12 */
            VT_OP_LOAD_LOCAL, 0, 2,      /* i */
            VT_OP_LOAD_LOCAL, 0, 0,      /* n */
            VT_OP_ICMP_LT,
            VT_OP_IF_FALSE, 0, 38,       /* done@38 */
            VT_OP_LOAD_LOCAL, 0, 1,      /* sum */
            VT_OP_LOAD_LOCAL, 0, 2,      /* i */
            VT_OP_IADD,
            VT_OP_STORE_LOCAL, 0, 1,     /* sum += i */
            VT_OP_LOAD_LOCAL, 0, 2,      /* i */
            VT_OP_LOAD_CONST_INT, 0, 0,  /* 0 — wait need const 1 */
            /* This won't work — const[0]=0, need const[1]=1 */
            /* Let's fix: use const[0]=0 for sum/i init, const[1]=1 for increment */
            0, 0, 0, /* padding */
        };
        /* Actually let me build this properly */
        uint8_t lc[] = {
            6,0,0, 3,0,1,   /* sum=0 */
            6,0,0, 3,0,2,   /* i=0 */
            /* loop@12 */
            2,0,2, 2,0,0, 31, 43,0,45,
            2,0,1, 2,0,2, 13, 3,0,1,
            2,0,2, 6,0,1, 13, 3,0,2,
            41,0,12,
            /* done@45 */
            2,0,1, 48,
        };
        vtx_value_t lconsts[2] = { vtx_make_smi(0), vtx_make_smi(1) };
        vtx_bytecode_t lbc = { lc, sizeof(lc), lconsts, 2, 3, 4 };
        vtx_method_desc_t lm = { "sum_loop","(I)I",&lbc,NULL,2,1,false };

        /* Compile */
        vtx_graph_t lg; vtx_graph_init(&lg, 1);
        vtx_graph_build(&lg, &lbc, &lm, &arena);
        vtx_pipeline_config_t lcfg = vtx_pipeline_config_t2();
        vtx_code_cache_t lcc; vtx_code_cache_init(&lcc, 1<<20);
        vtx_method_registry_t lmr; vtx_method_registry_init(&lmr, &arena);
        lcfg.code_cache = &lcc; lcfg.method_registry = &lmr; lcfg.method = &lm;
        vtx_compile_result_t lr; memset(&lr, 0, sizeof(lr));
        vtx_pipeline_run(&lg, &lcfg, &arena, &lr);
        bool lok = lr.success;
        void *lcode = lm.compiled_code;
        vtx_compile_result_destroy(&lr);

        if (lok && lcode) {
            typedef vtx_value_t(*E2)(const vtx_method_desc_t*,void*,void*,vtx_value_t*,uint32_t);
            E2 ljit = (E2)lcode;

            printf("\nTest 7: sum_loop(1000)...\n");
            vtx_value_t arg = vtx_make_smi(1000);
            vtx_value_t result = ljit(&lm, NULL, (void*)1, &arg, 1);
            int64_t val = vtx_is_smi(result) ? vtx_smi_value(result) : -999;
            printf("  JIT sum_loop(1000) = %lld (expected 499500) %s\n",
                   (long long)val, val == 499500 ? "PASS" : "FAIL");

            printf("Test 8: sum_loop(100000)...\n");
            arg = vtx_make_smi(100000);
            result = ljit(&lm, NULL, (void*)1, &arg, 1);
            val = vtx_is_smi(result) ? vtx_smi_value(result) : -999;
            printf("  JIT sum_loop(100K) = %lld (expected 4999950000) %s\n",
                   (long long)val, val == 4999950000LL ? "PASS" : "FAIL");
        } else {
            printf("sum_loop compilation failed\n");
        }

        vtx_method_registry_destroy(&lmr);
        vtx_code_cache_destroy(&lcc);
    }

    vtx_method_registry_destroy(&mr);
    vtx_code_cache_destroy(&cc);
    vtx_gc_destroy(&gc); vtx_type_system_destroy(&ts); vtx_arena_destroy(&arena);
    return 0;
}
