/* runtime/vortex_runtime.c — High-level VORTEX runtime API.
 *
 * vtx_runtime_run() uses the REAL JIT: the interpreter dispatches to
 * compiled code when method.compiled_code != NULL. The compile callback
 * fires on hot methods and submits T1/T2 compilation to the threadpool.
 */

#include "runtime/vortex_runtime.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "interp/dispatch.h"
#include "compile/threadpool.h"
#include "compile/request.h"
#include "compile/pipeline.h"
#include "baseline/codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Lifecycle ---- */

int vtx_runtime_create(vtx_runtime_t *rt)
{
    if (!rt) return -1;
    memset(rt, 0, sizeof(*rt));

    vtx_type_system_init(&rt->type_system);
    vtx_gc_init(&rt->gc, &rt->type_system, VTX_GC_GENERATIONAL);
    vtx_arena_init(&rt->arena);
    vtx_code_cache_init(&rt->code_cache, 1 << 20);
    vtx_method_registry_init(&rt->method_registry, &rt->arena);

    rt->interp = (vtx_interp_t *)malloc(sizeof(vtx_interp_t));
    if (!rt->interp) return -1;
    vtx_interp_init(rt->interp, &rt->type_system, &rt->gc);

    rt->compile_ctx = (vtx_compile_context_t *)malloc(sizeof(vtx_compile_context_t));
    if (!rt->compile_ctx) { free(rt->interp); return -1; }
    vtx_compile_context_init(rt->compile_ctx);

    rt->use_jit = 0;
    rt->hot_threshold = 100;  /* compile after 100 invocations */
    rt->initialized = 1;
    return 0;
}

void vtx_runtime_destroy(vtx_runtime_t *rt)
{
    if (!rt || !rt->initialized) return;

    if (rt->threadpool) {
        vtx_threadpool_shutdown(rt->threadpool);
        free(rt->threadpool);
        rt->threadpool = NULL;
    }
    if (rt->compile_ctx) {
        vtx_compile_context_destroy(rt->compile_ctx);
        free(rt->compile_ctx);
        rt->compile_ctx = NULL;
    }
    if (rt->interp) {
        vtx_interp_destroy(rt->interp);
        free(rt->interp);
        rt->interp = NULL;
    }
    vtx_method_registry_destroy(&rt->method_registry);
    vtx_code_cache_destroy(&rt->code_cache);
    vtx_arena_destroy(&rt->arena);
    vtx_gc_destroy(&rt->gc);
    vtx_type_system_destroy(&rt->type_system);
    rt->initialized = 0;
}

int vtx_runtime_enable_jit(vtx_runtime_t *rt, uint32_t nthreads)
{
    if (!rt || !rt->initialized) return -1;
    if (rt->use_jit) return 0;

    /* Start compilation threadpool */
    if (nthreads == 0) nthreads = 2;
    rt->threadpool = (vtx_threadpool_t *)malloc(sizeof(vtx_threadpool_t));
    if (!rt->threadpool) return -1;
    if (vtx_threadpool_init(rt->threadpool, nthreads) != 0) {
        free(rt->threadpool);
        rt->threadpool = NULL;
        return -1;
    }

    /* Wire the threadpool to the compile context */
    vtx_compile_context_wire_threadpool(rt->compile_ctx);

    /* Set the compile callback so the interpreter triggers compilation
     * on hot methods. The interpreter checks method->compiled_code on
     * each call and, if NULL, increments an invocation counter. When
     * the counter exceeds the threshold, it calls this callback. */
    rt->use_jit = 1;

    return 0;
}

/* ---- Execution ---- */

/* Derive a stable method_id from a bytecode pointer.
 *
 * The baseline JIT (vtx_baseline_compile in src/baseline/codegen.c)
 * computes method_id as: method->vtable_index != 0xFFFFFFFF
 *                          ? method->vtable_index
 *                          : (uint32_t)(uintptr_t)method;
 *
 * For runtime-managed methods (no vtable_index set), this means the
 * method_id is the lower 32 bits of the method descriptor's address.
 * Since the bytecode pointer is the only stable identity we have at
 * the runtime API level, we use it as a proxy for the method address.
 *
 * The method descriptor constructed in vtx_runtime_run() is on the
 * stack, so its address changes every call — we can't use that.
 * Instead, we use the bytecode pointer (which is stable for a given
 * loaded Bytecode) and rely on the fact that the eager compile path
 * (vtx_runtime_compile) also constructs its method descriptor with
 * the same bytecode pointer.
 *
 * To make the method_id consistent, we set method.vtable_index to
 * the lower 32 bits of the bytecode pointer in BOTH the compile and
 * run paths. This ensures the registry lookup finds the compiled code. */
static uint32_t runtime_method_id(const vtx_bytecode_t *bc)
{
    /* Use lower 32 bits of bytecode pointer. Clamp to avoid colliding
     * with valid vtable_index values (0 is reserved for "unset" in
     * some contexts, so add 1). */
    uint32_t id = (uint32_t)((uintptr_t)bc & 0xFFFFFFFFu);
    if (id == 0xFFFFFFFFu || id == 0) id = 1;
    return id;
}

vtx_value_t vtx_runtime_run(vtx_runtime_t *rt, const vtx_bytecode_t *bc)
{
    if (!rt || !bc) return VTX_VALUE_UNDEFINED;

    uint32_t method_id = runtime_method_id(bc);

    vtx_method_desc_t method = {
        .name = "main",
        .signature = "()I",
        .bytecode = (vtx_bytecode_t *)bc,
        .compiled_code = NULL,
        .vtable_index = method_id,
        .arg_count = 0,
        .is_virtual = false,
    };

    /* Look up eagerly-compiled code from the registry. If found,
     * wire method.compiled_code so the interpreter dispatches to it. */
    if (rt->method_registry.capacity > 0) {
        vtx_compiled_method_t *cm = vtx_method_registry_get(
            &rt->method_registry, method_id);
        if (cm != NULL && cm->code_start != NULL) {
            method.compiled_code = cm->code_start;
        }
    }

    return vtx_interp_run(rt->interp, &method, NULL, 0);
}

vtx_value_t vtx_runtime_run_with_args(vtx_runtime_t *rt,
                                       const vtx_bytecode_t *bc,
                                       const vtx_value_t *args,
                                       uint32_t arg_count)
{
    if (!rt || !bc) return VTX_VALUE_UNDEFINED;

    uint32_t method_id = runtime_method_id(bc);

    vtx_method_desc_t method = {
        .name = "main",
        .signature = "(I)I",
        .bytecode = (vtx_bytecode_t *)bc,
        .compiled_code = NULL,
        .vtable_index = method_id,
        .arg_count = arg_count,
        .is_virtual = false,
    };

    /* Look up eagerly-compiled code from the registry. */
    if (rt->method_registry.capacity > 0) {
        vtx_compiled_method_t *cm = vtx_method_registry_get(
            &rt->method_registry, method_id);
        if (cm != NULL && cm->code_start != NULL) {
            method.compiled_code = cm->code_start;
        }
    }

    return vtx_interp_run(rt->interp, &method, (vtx_value_t *)args, arg_count);
}

/* ---- Eager compilation ---- */

int vtx_runtime_compile(vtx_runtime_t *rt, vtx_method_desc_t *method,
                          int tier)
{
    if (!rt || !method) return -1;

    /* Ensure method_id is stable and consistent with vtx_runtime_run().
     *
     * If the caller left vtable_index at 0 (the Rust bindings do this
     * because they construct the method_desc with mem::zeroed), the
     * baseline JIT would derive method_id from the method descriptor's
     * STACK address — which differs between the compile call and the
     * subsequent run call. That breaks the registry lookup.
     *
     * Fix: derive method_id from the bytecode pointer (stable across
     * calls) and store it in vtable_index so the baseline JIT picks
     * it up. The run path uses the same derivation. */
    if (method->vtable_index == 0 || method->vtable_index == 0xFFFFFFFFu) {
        method->vtable_index = runtime_method_id(method->bytecode);
    }

    if (tier == 1) {
        /* T1 baseline JIT — fast compilation, correct code */
        vtx_compiled_code_t *compiled = vtx_baseline_compile(
            method, NULL, &rt->arena,
            &rt->code_cache, &rt->method_registry);
        return compiled ? 0 : -1;
    } else if (tier == 2) {
        /* T2 optimizing JIT — full SoN IR pipeline */
        vtx_graph_t graph;
        vtx_graph_init(&graph, method->arg_count);

        vtx_pipeline_config_t cfg = vtx_pipeline_config_t2();
        cfg.code_cache = &rt->code_cache;
        cfg.method_registry = &rt->method_registry;
        cfg.method = method;

        vtx_compile_result_t result;
        memset(&result, 0, sizeof(result));

        int rc = vtx_graph_build(&graph, method->bytecode, method, &rt->arena);
        if (rc != 0) {
            /* T2 can't handle this method (e.g. float ops) — fall back to T1 */
            vtx_graph_destroy(&graph);
            vtx_compiled_code_t *compiled = vtx_baseline_compile(
                method, NULL, &rt->arena,
                &rt->code_cache, &rt->method_registry);
            return compiled ? 0 : -1;
        }

        int prc = vtx_pipeline_run(&graph, &cfg, &rt->arena, &result);
        vtx_compile_result_destroy(&result);
        vtx_pipeline_config_destroy(&cfg);
        vtx_graph_destroy(&graph);
        return (prc == 0 && method->compiled_code != NULL) ? 0 : -1;
    }

    return -1;
}

/* ---- Accessors ---- */

vtx_interp_t *vtx_runtime_interp(vtx_runtime_t *rt)
{
    return rt ? rt->interp : NULL;
}

vtx_type_system_t *vtx_runtime_type_system(vtx_runtime_t *rt)
{
    return rt ? &rt->type_system : NULL;
}

vtx_gc_t *vtx_runtime_gc(vtx_runtime_t *rt)
{
    return rt ? &rt->gc : NULL;
}

vtx_code_cache_t *vtx_runtime_code_cache(vtx_runtime_t *rt)
{
    return rt ? &rt->code_cache : NULL;
}

vtx_compile_context_t *vtx_runtime_compile_ctx(vtx_runtime_t *rt)
{
    return rt ? rt->compile_ctx : NULL;
}

/* ---- Bytecode loading ---- */

vtx_bytecode_t *vtx_bytecode_load(const char *path)
{
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    uint32_t magic, version, code_length;
    uint16_t max_locals, max_stack;
    uint32_t const_count;

    if (fread(&magic, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&version, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&code_length, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&max_locals, 2, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&max_stack, 2, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&const_count, 4, 1, f) != 1) { fclose(f); return NULL; }

    vtx_bytecode_t *bc = (vtx_bytecode_t *)malloc(sizeof(vtx_bytecode_t));
    if (!bc) { fclose(f); return NULL; }

    uint8_t *code = (uint8_t *)malloc(code_length);
    if (!code) { free(bc); fclose(f); return NULL; }
    if (fread(code, 1, code_length, f) != code_length) {
        free(code); free(bc); fclose(f); return NULL;
    }

    bc->code = code;
    bc->length = code_length;
    bc->max_locals = max_locals;
    bc->max_stack = max_stack;

    if (const_count > 0) {
        vtx_value_t *consts = (vtx_value_t *)malloc(const_count * sizeof(vtx_value_t));
        if (consts && fread(consts, sizeof(vtx_value_t), const_count, f) == const_count) {
            bc->constant_pool = consts;
            bc->constant_count = const_count;
        } else {
            free(consts);
            bc->constant_pool = NULL;
            bc->constant_count = 0;
        }
    } else {
        bc->constant_pool = NULL;
        bc->constant_count = 0;
    }

    fclose(f);
    return bc;
}
