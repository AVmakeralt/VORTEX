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
#include "compile/threadpool.h"
#include "compile/pipeline.h"
#include "codecache/install.h"
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
    return ctx->compilation_requested[method_id];
}

void vtx_clear_compilation_requested(vtx_compile_context_t *ctx,
                                       uint32_t method_id)
{
    if (ctx == NULL || ctx->compilation_requested == NULL) return;
    if (method_id >= ctx->compilation_requested_capacity) return;
    ctx->compilation_requested[method_id] = false;
}

/* ========================================================================== */
/* Compilation request                                                          */
/* ========================================================================== */

void vtx_request_compilation(vtx_compile_context_t *ctx,
                              const vtx_method_desc_t *method)
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
    ctx->compilation_requested[method_id] = true;

    /* Submit to thread pool */
    if (ctx->threadpool != NULL) {
        vtx_compile_task_t task;
        memset(&task, 0, sizeof(task));
        task.method_id = method_id;
        task.tier = VTX_TIER_T1;
        task.priority = VTX_COMPILE_PRIORITY_NORMAL;

        if (vtx_threadpool_submit_task(ctx->threadpool, &task) != 0) {
            /* Submission failed — clear the flag so we can retry later */
            ctx->compilation_requested[method_id] = false;
        }
    }
}
