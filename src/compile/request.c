/**
 * VORTEX Compilation Request — Implementation
 *
 * Bridges the interpreter's hot-code detection with the compilation
 * thread pool. This is the wiring that was previously missing —
 * the entire JIT pipeline was dead code because vtx_request_compilation
 * was never implemented.
 */

#define _POSIX_C_SOURCE 199309L
#include "compile/request.h"
#include "compile/callee_lookup.h"
#include "compile/threadpool.h"
#include "compile/pipeline.h"
#include "codecache/install.h"
#include "baseline/codegen.h"
#include "ir/graph.h"
#include "runtime/arena.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/* Default tier decision                                                        */
/* ========================================================================== */

static vtx_compile_tier_t default_tier_decision(uint64_t execution_count)
{
    if (execution_count >= VORTEX_T2_THRESHOLD) {
        return VTX_TIER_T2;
    }
    if (execution_count >= VORTEX_T1_THRESHOLD) {
        return VTX_TIER_T1;
    }
    return VTX_TIER_T1; /* always at least T1 when compilation is requested */
}

/* ========================================================================== */
/* Compilation context lifecycle                                                 */
/* ========================================================================== */

int vtx_compile_context_init(vtx_compile_context_t *ctx)
{
    if (ctx == NULL) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->tier_decision = default_tier_decision;
    return 0;
}

void vtx_compile_context_destroy(vtx_compile_context_t *ctx)
{
    if (ctx == NULL) return;
    if (ctx->compilation_requested != NULL) {
        free(ctx->compilation_requested);
        ctx->compilation_requested = NULL;
    }
    ctx->compilation_requested_count = 0;
    ctx->compilation_requested_capacity = 0;
}

void vtx_compile_context_set_method_lookup(
    vtx_compile_context_t *ctx,
    const vtx_method_desc_t *(*lookup)(uint32_t, void *),
    void *context)
{
    if (ctx == NULL) return;
    ctx->method_lookup = lookup;
    ctx->method_lookup_context = context;
}

/* ========================================================================== */
/* Compilation request flag management                                           */
/* ========================================================================== */

static bool ensure_compilation_flag_capacity(vtx_compile_context_t *ctx,
                                               uint32_t method_id)
{
    if (method_id < ctx->compilation_requested_capacity) {
        return true;
    }

    uint32_t new_cap = ctx->compilation_requested_capacity;
    if (new_cap == 0) new_cap = 64;
    while (new_cap <= method_id) {
        uint32_t doubled = new_cap * 2;
        if (doubled <= new_cap) {
            new_cap = method_id + 1;
            break;
        }
        new_cap = doubled;
    }

    bool *new_arr = (bool *)realloc(ctx->compilation_requested,
                                      new_cap * sizeof(bool));
    if (new_arr == NULL) return false;

    /* Initialize new slots to false */
    memset(new_arr + ctx->compilation_requested_capacity, 0,
           (new_cap - ctx->compilation_requested_capacity) * sizeof(bool));

    ctx->compilation_requested = new_arr;
    ctx->compilation_requested_capacity = new_cap;
    return true;
}

bool vtx_is_compilation_requested(const vtx_compile_context_t *ctx,
                                    uint32_t method_id)
{
    if (ctx == NULL || ctx->compilation_requested == NULL) return false;
    if (method_id >= ctx->compilation_requested_capacity) return false;
    return __atomic_load_n(&ctx->compilation_requested[method_id], __ATOMIC_RELAXED);
}

void vtx_clear_compilation_requested(vtx_compile_context_t *ctx,
                                       uint32_t method_id)
{
    if (ctx == NULL || ctx->compilation_requested == NULL) return;
    if (method_id >= ctx->compilation_requested_capacity) return;
    __atomic_store_n(&ctx->compilation_requested[method_id], false, __ATOMIC_RELAXED);
}

/* ========================================================================== */
/* Compilation request                                                          */
/* ========================================================================== */

void vtx_request_compilation(vtx_compile_context_t *ctx,
                              const vtx_method_desc_t *method,
                              uint64_t execution_count)
{
    if (ctx == NULL || method == NULL) return;

    uint32_t method_id = method->vtable_index;

    /* Check if already requested */
    if (vtx_is_compilation_requested(ctx, method_id)) {
        return;
    }

    /* Ensure flag array has space */
    if (!ensure_compilation_flag_capacity(ctx, method_id)) {
        return; /* allocation failure — skip compilation */
    }

    /* Mark as requested */
    __atomic_store_n(&ctx->compilation_requested[method_id], true, __ATOMIC_RELAXED);

    /* Submit to thread pool */
    if (ctx->threadpool != NULL) {
        vtx_compile_task_t task;
        memset(&task, 0, sizeof(task));
        task.method_id = method_id;

        /* Use the tier_decision callback with the REAL execution count
         * from the profiler. No hardcoding — the tier is determined by
         * the actual runtime behavior of the method. */
        if (ctx->tier_decision != NULL) {
            task.tier = ctx->tier_decision(execution_count);
        } else {
            task.tier = VTX_TIER_T1;
        }
        task.priority = VTX_COMPILE_PRIORITY_NORMAL;

        if (vtx_threadpool_submit_task(ctx->threadpool, &task) != 0) {
            /* Submission failed — clear the flag so we can retry later */
            __atomic_store_n(&ctx->compilation_requested[method_id], false, __ATOMIC_RELAXED);
        }
    }
}

/* ========================================================================== */
/* Compile callback — called by threadpool workers                               */
/* ========================================================================== */

/**
 * This is the compile_callback that the threadpool calls when it
 * picks up a compilation task. Previously, this callback was never
 * set, so compilation tasks were silently discarded.
 *
 * The callback:
 *   1. Looks up the method descriptor by method_id
 *   2. Creates a per-compilation arena
 *   3. Runs the baseline JIT (for T1) or the optimizing pipeline (T2+)
 *   4. Installs the compiled code into the code cache
 *   5. Clears the compilation_requested flag
 */
static void compile_callback(uint32_t method_id, vtx_compile_tier_t tier, void *context)
{
    vtx_compile_context_t *ctx = (vtx_compile_context_t *)context;
    if (ctx == NULL) return;

    /* Look up the method descriptor */
    const vtx_method_desc_t *method = NULL;
    if (ctx->method_lookup != NULL) {
        method = ctx->method_lookup(method_id, ctx->method_lookup_context);
    }
    if (method == NULL || method->bytecode == NULL) {
        /* Method not found or has no bytecode — skip */
        vtx_clear_compilation_requested(ctx, method_id);
        return;
    }

    /* Don't compile if already compiled */
    if (__atomic_load_n(&method->compiled_code, __ATOMIC_ACQUIRE) != NULL) {
        vtx_clear_compilation_requested(ctx, method_id);
        return;
    }

    /* Create a per-compilation arena */
    vtx_arena_t compile_arena;
    if (vtx_arena_init(&compile_arena) != 0) {
        vtx_clear_compilation_requested(ctx, method_id);
        return;
    }

    if (tier == VTX_TIER_T1) {
        /* T1: Baseline JIT compilation — fast, minimal optimization.
         * vtx_baseline_compile already handles code installation when
         * cache and registry are provided. */
        vtx_compiled_code_t *compiled = vtx_baseline_compile(
            method, NULL, &compile_arena,
            ctx->code_cache, ctx->method_registry);

        if (compiled != NULL) {
            /* Success — the compiled code has been installed into the
             * code cache and method->compiled_code is set atomically.
             * Destroy the compiled_code wrapper (the actual code lives
             * in the code cache now). */
            vtx_compiled_code_destroy(compiled);
        } else {
            fprintf(stderr, "[compile] T1 compilation failed for method %u\n", method_id);
        }
    } else {
        /* T2+: Optimizing pipeline compilation */
        vtx_graph_t graph;
        if (vtx_graph_init(&graph, method->arg_count) != 0) {
            vtx_arena_destroy(&compile_arena);
            vtx_clear_compilation_requested(ctx, method_id);
            return;
        }
        if (vtx_graph_build(&graph, method->bytecode, method, &compile_arena) != 0) {
            vtx_graph_destroy(&graph);
            vtx_arena_destroy(&compile_arena);
            vtx_clear_compilation_requested(ctx, method_id);
            return;
        }

        vtx_pipeline_config_t config;
        if (tier == VTX_TIER_T2) {
            config = vtx_pipeline_config_t2();
        } else {
            config = vtx_pipeline_config_t3();
        }

        /* Set up code installation so the pipeline installs its output */
        config.code_cache = ctx->code_cache;
        config.method_registry = ctx->method_registry;
        config.method = method;
        config.install_arena = &compile_arena;

        /* Wire the orchestrator so the pipeline can notify it after
         * install. This wakes up the recomp monitor, FDI, and phase-
         * reactive version manager — previously dead code because no
         * one passed the orchestrator to the pipeline. */
        config.orchestrator = ctx->orchestrator;

        /* Wire the callee lookup so the inliner can actually inline.
         * Without this, callee_lookup=NULL and the GBDT model computes
         * scores but never inlines anything.
         * (audit #2: wire callee_lookup) */
        void *callee_ctx = NULL;
        vtx_callee_lookup_fn lookup_fn = vtx_callee_lookup_create(
            ctx->method_registry, NULL, NULL, &callee_ctx);
        config.callee_lookup = lookup_fn;
        config.callee_lookup_context = callee_ctx;

        vtx_compile_result_t result;
        memset(&result, 0, sizeof(result));

        int rc = vtx_pipeline_run(&graph, &config, &compile_arena, &result);

        if (rc == 0 && result.success) {
            /* Pipeline succeeded — code is installed */
        } else {
            fprintf(stderr, "[compile] T%d compilation failed for method %u (rc=%d)\n",
                    tier, method_id, rc);
        }

        vtx_compile_result_destroy(&result);
        vtx_pipeline_config_destroy(&config);
        vtx_callee_lookup_destroy(callee_ctx);
        vtx_graph_destroy(&graph);
    }

    vtx_arena_destroy(&compile_arena);
    vtx_clear_compilation_requested(ctx, method_id);
}

/* ========================================================================== */
/* Wire the compile context to the threadpool                                    */
/* ========================================================================== */

int vtx_compile_context_wire_threadpool(vtx_compile_context_t *ctx)
{
    if (ctx == NULL || ctx->threadpool == NULL) return -1;

    /* Set the compile callback on the threadpool so that when workers
     * pick up a compilation task (with method_id but no task_fn),
     * they call our compile_callback instead of silently discarding
     * the task. */
    vtx_threadpool_set_compile_callback(ctx->threadpool,
                                         compile_callback, ctx);
    return 0;
}
