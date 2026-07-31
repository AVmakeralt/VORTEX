/* runtime/vortex_runtime.c — High-level VORTEX runtime API implementation.
 *
 * Implements the simplified C API declared in vortex_runtime.h.
 */

#include "runtime/vortex_runtime.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "interp/dispatch.h"
#include "compile/threadpool.h"
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
    vtx_code_cache_init(&rt->code_cache, 1 << 20);  /* 1 MB */
    vtx_method_registry_init(&rt->method_registry, &rt->arena);

    rt->interp = (vtx_interp_t *)malloc(sizeof(vtx_interp_t));
    if (!rt->interp) return -1;
    vtx_interp_init(rt->interp, &rt->type_system, &rt->gc);

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

/* ---- Execution ---- */

vtx_value_t vtx_runtime_run(vtx_runtime_t *rt, const vtx_bytecode_t *bc)
{
    if (!rt || !bc) return VTX_VALUE_UNDEFINED;

    vtx_method_desc_t method = {
        .name = "main",
        .signature = "()I",
        .bytecode = (vtx_bytecode_t *)bc,
        .compiled_code = NULL,
        .vtable_index = 0,
        .arg_count = 0,
        .is_virtual = false,
    };

    return vtx_interp_run(rt->interp, &method, NULL, 0);
}

vtx_value_t vtx_runtime_run_with_args(vtx_runtime_t *rt,
                                       const vtx_bytecode_t *bc,
                                       const vtx_value_t *args,
                                       uint32_t arg_count)
{
    if (!rt || !bc) return VTX_VALUE_UNDEFINED;

    vtx_method_desc_t method = {
        .name = "main",
        .signature = "(I)I",
        .bytecode = (vtx_bytecode_t *)bc,
        .compiled_code = NULL,
        .vtable_index = 0,
        .arg_count = arg_count,
        .is_virtual = false,
    };

    return vtx_interp_run(rt->interp, &method, (vtx_value_t *)args, arg_count);
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
    if (!rt) return NULL;
    static vtx_compile_context_t ctx;
    static int ctx_init = 0;
    if (!ctx_init) {
        vtx_compile_context_init(&ctx);
        ctx_init = 1;
    }
    return &ctx;
}

/* ---- Threadpool ---- */

int vtx_runtime_start_threadpool(vtx_runtime_t *rt, uint32_t nthreads)
{
    if (!rt) return -1;
    if (rt->threadpool) return 0;  /* already running */

    rt->threadpool = (vtx_threadpool_t *)malloc(sizeof(vtx_threadpool_t));
    if (!rt->threadpool) return -1;

    int rc = vtx_threadpool_init(rt->threadpool, nthreads);
    if (rc != 0) {
        free(rt->threadpool);
        rt->threadpool = NULL;
        return -1;
    }
    return 0;
}

void vtx_runtime_stop_threadpool(vtx_runtime_t *rt)
{
    if (!rt || !rt->threadpool) return;
    vtx_threadpool_shutdown(rt->threadpool);
    free(rt->threadpool);
    rt->threadpool = NULL;
}

/* ---- Bytecode loading ---- */

vtx_bytecode_t *vtx_bytecode_load(const char *path, vtx_arena_t *arena)
{
    (void)arena;
    if (!path) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    /* Read header: magic(4) + version(4) + code_length(4) + max_locals(2) + max_stack(2) + const_count(4) */
    uint32_t magic, version, code_length;
    uint16_t max_locals, max_stack;
    uint32_t const_count;

    if (fread(&magic, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&version, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&code_length, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&max_locals, 2, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&max_stack, 2, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&const_count, 4, 1, f) != 1) { fclose(f); return NULL; }

    /* Allocate bytecode struct */
    vtx_bytecode_t *bc = (vtx_bytecode_t *)malloc(sizeof(vtx_bytecode_t));
    if (!bc) { fclose(f); return NULL; }

    /* Read code */
    uint8_t *code = (uint8_t *)malloc(code_length);
    if (!code) { free(bc); fclose(f); return NULL; }
    if (fread(code, 1, code_length, f) != code_length) {
        free(code); free(bc); fclose(f); return NULL;
    }

    bc->code = code;
    bc->length = code_length;
    bc->max_locals = max_locals;
    bc->max_stack = max_stack;

    /* Read constants */
    if (const_count > 0) {
        vtx_value_t *consts = (vtx_value_t *)malloc(const_count * sizeof(vtx_value_t));
        if (consts) {
            if (fread(consts, sizeof(vtx_value_t), const_count, f) != const_count) {
                free(consts);
                bc->constant_pool = NULL;
                bc->constant_count = 0;
            } else {
                bc->constant_pool = consts;
                bc->constant_count = const_count;
            }
        } else {
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
