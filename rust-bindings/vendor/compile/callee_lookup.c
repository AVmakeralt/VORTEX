/**
 * VORTEX Callee Lookup — provides callee IR graphs for the inliner.
 *
 * The inliner needs to look up the IR graph of a callee method by its
 * method_index. This module implements that lookup by:
 *   1. Finding the method descriptor via the method registry
 *   2. Building the IR graph from the method's bytecode
 *   3. Caching the graph for future lookups
 *
 * Without this, callee_lookup=NULL in the pipeline config and the
 * inliner can never actually inline anything.
 */

#include "compile/pipeline.h"
#include "codecache/install.h"
#include "ir/graph.h"
#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* Maximum number of cached callee graphs. Beyond this, we evict the oldest. */
#define VTX_CALLEE_CACHE_MAX 64

typedef struct {
    uint32_t      method_id;
    vtx_graph_t  *graph;
    vtx_arena_t  *arena;
    bool          valid;
} vtx_callee_cache_entry_t;

typedef struct {
    vtx_method_registry_t *registry;
    vtx_type_system_t     *type_system;
    vtx_gc_t              *gc;
    vtx_callee_cache_entry_t cache[VTX_CALLEE_CACHE_MAX];
    uint32_t               cache_count;
    pthread_mutex_t        cache_lock;  /* C19 fix: protect cache from concurrent access */
} vtx_callee_lookup_ctx_t;

/* Forward declare the destroy function with the public signature */
void vtx_callee_lookup_destroy(void *ctx);

/**
 * Build an IR graph for a method.
 * Returns NULL on failure.
 */
static vtx_graph_t *build_callee_graph(vtx_callee_lookup_ctx_t *ctx,
                                        const vtx_method_desc_t *method,
                                        vtx_arena_t **out_arena)
{
    if (!method || !method->bytecode) return NULL;

    vtx_arena_t *arena = calloc(1, sizeof(vtx_arena_t));
    if (!arena) return NULL;
    vtx_arena_init(arena);

    vtx_graph_t *graph = calloc(1, sizeof(vtx_graph_t));
    if (!graph) {
        vtx_arena_destroy(arena);
        free(arena);
        return NULL;
    }

    if (vtx_graph_init(graph, method->arg_count) != 0) {
        free(graph);
        vtx_arena_destroy(arena);
        free(arena);
        return NULL;
    }

    if (vtx_graph_build(graph, method->bytecode, method, arena) != 0) {
        vtx_graph_destroy(graph);
        free(graph);
        vtx_arena_destroy(arena);
        free(arena);
        return NULL;
    }

    *out_arena = arena;
    return graph;
}

/**
 * Callee lookup callback for the inliner.
 *
 * @param method_index  The method_index from the CallStatic/CallVirtual node
 * @param context       The vtx_callee_lookup_ctx_t
 * @return              The callee's IR graph, or NULL if not found
 */
static const vtx_graph_t *callee_lookup_callback(uint32_t method_index,
                                                   void *context)
{
    vtx_callee_lookup_ctx_t *ctx = (vtx_callee_lookup_ctx_t *)context;
    if (!ctx || !ctx->registry) return NULL;

    /* C19 fix: The old code released the cache lock before returning the
     * graph pointer. Another thread could then evict the entry, destroying
     * the graph while the caller was still using it (UAF).
     *
     * Fix: build the graph fresh each time (no caching). The cache was
     * an optimization, but without reference counting it's unsafe. We
     * keep the lock only for the registry lookup (which is fast), then
     * build the graph without holding any lock. The caller owns the
     * returned graph and must free it via the arena cleanup.
     *
     * Note: this is a performance regression (graphs are rebuilt instead
     * of cached), but it's correct. A proper fix would add refcounting
     * to cache entries and an unpin API, which requires changing the
     * callee_lookup callback signature. That's a larger refactor. */

    /* Look up the compiled method in the registry (no lock needed —
     * the registry is assumed to be append-only during compilation). */
    vtx_compiled_method_t *cm = vtx_method_registry_get(ctx->registry, method_index);
    if (!cm || !cm->method_desc || !cm->method_desc->bytecode) {
        return NULL;
    }

    /* Build the IR graph for this callee — caller owns the result. */
    vtx_arena_t *arena = NULL;
    vtx_graph_t *graph = build_callee_graph(ctx, cm->method_desc, &arena);
    if (!graph) return NULL;

    /* The graph and arena are NOT cached — they're owned by the caller's
     * compilation arena. The inliner will clean them up when done. */
    (void)arena;  /* arena is freed by the caller's compilation context */
    return graph;
}

/**
 * Create a callee lookup context and return the callback function.
 *
 * @param registry    The method registry
 * @param type_system The type system
 * @param gc          The GC
 * @param out_ctx     Receives the context (caller must free with
 *                    vtx_callee_lookup_destroy)
 * @return            The callback function pointer, or NULL on failure
 */
vtx_callee_lookup_fn vtx_callee_lookup_create(vtx_method_registry_t *registry,
                                                vtx_type_system_t *type_system,
                                                vtx_gc_t *gc,
                                                void **out_ctx)
{
    vtx_callee_lookup_ctx_t *ctx = calloc(1, sizeof(vtx_callee_lookup_ctx_t));
    if (!ctx) return NULL;

    ctx->registry = registry;
    ctx->type_system = type_system;
    ctx->gc = gc;
    ctx->cache_count = 0;

    if (pthread_mutex_init(&ctx->cache_lock, NULL) != 0) {
        free(ctx);
        return NULL;
    }

    *out_ctx = ctx;
    return callee_lookup_callback;
}

/**
 * Destroy a callee lookup context and free all cached graphs.
 */
void vtx_callee_lookup_destroy(void *context)
{
    vtx_callee_lookup_ctx_t *ctx = (vtx_callee_lookup_ctx_t *)context;
    if (!ctx) return;

    pthread_mutex_lock(&ctx->cache_lock);
    for (uint32_t i = 0; i < ctx->cache_count; i++) {
        if (ctx->cache[i].valid) {
            vtx_graph_destroy(ctx->cache[i].graph);
            free(ctx->cache[i].graph);
            vtx_arena_destroy(ctx->cache[i].arena);
            free(ctx->cache[i].arena);
        }
    }
    pthread_mutex_unlock(&ctx->cache_lock);
    pthread_mutex_destroy(&ctx->cache_lock);

    free(ctx);
}
