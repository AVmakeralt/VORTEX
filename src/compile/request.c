/* ============================================================================ *
 * AI-MODIFIED CODE
 *
 * This file was originally written by a human developer. It has been
 * substantially modified by an AI assistant (GLM/Z.ai) for bug fixes,
 * performance improvements, and feature additions.
 *
 * Original human-written structure is preserved; AI changes are marked
 * with bug fix IDs (B1-B28) or perf notes (Perf 1-10) in comments.
 *
 * If reviewing, please verify AI changes against the original logic.
 * ============================================================================ */

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
#include "interp/profiler.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/* Default tier decision                                                        */
/* ========================================================================== */

/* T3 promotion: a method is promoted to T3 (speculative JIT with
 * SIMD, loop specialization, and aggressive inlining) when its heat
 * exceeds VORTEX_T3_THRESHOLD (10x T2_THRESHOLD = 100,000 by default).
 *
 * The tier decision is stateless — it picks the tier based purely on
 * execution_count. The stateful part (knowing whether a method is
 * already compiled at T2 and needs promotion to T3) is handled by
 * the recompilation path in the orchestrator, which clears the
 * compilation_requested flag after T1/T2 compilation so the method
 * can be re-detected as hot and re-compiled at a higher tier. */
static vtx_compile_tier_t default_tier_decision(uint64_t execution_count)
{
    if (execution_count >= VORTEX_T3_THRESHOLD) {
        return VTX_TIER_T3;
    }
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

    /* Destroy all deoptless tables */
    if (ctx->deoptless_tables != NULL) {
        for (uint32_t i = 0; i < ctx->deoptless_table_count; i++) {
            if (ctx->deoptless_tables[i] != NULL) {
                /* vtx_deoptless_table_destroy is declared in deoptless.h.
                 * We can't include it here (circular dep), so call via
                 * a function pointer or just free the table struct.
                 * The table's internal versions are arena-allocated, so
                 * freeing the table struct is sufficient. */
                free(ctx->deoptless_tables[i]);
                ctx->deoptless_tables[i] = NULL;
            }
        }
        free(ctx->deoptless_tables);
        ctx->deoptless_tables = NULL;
    }
    ctx->deoptless_table_count = 0;
    ctx->deoptless_table_capacity = 0;
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
static int compile_callback(uint32_t method_id, vtx_compile_tier_t tier, void *context)
{
    vtx_compile_context_t *ctx = (vtx_compile_context_t *)context;
    if (ctx == NULL) return -1;

    /* Look up the method descriptor */
    const vtx_method_desc_t *method = NULL;
    if (ctx->method_lookup != NULL) {
        method = ctx->method_lookup(method_id, ctx->method_lookup_context);
    }
    if (method == NULL || method->bytecode == NULL) {
        /* Method not found or has no bytecode — skip */
        vtx_clear_compilation_requested(ctx, method_id);
        return -1;
    }

    /* Don't compile if already compiled — UNLESS this is a tier promotion.
     * When the method is already compiled at T1 and we're now asked to
     * compile at T2 (or T2→T3), we proceed: the new compilation will
     * atomically replace the old compiled_code pointer via
     * vtx_code_cache_install. The old code stays in the cache until
     * versioned reclamation frees it (safe even if other threads are
     * still executing it).
     *
     * Without this exception, T2 and T3 are unreachable: the first
     * compilation (at T1) sets compiled_code, and every subsequent
     * request would bail out here. */
    if (__atomic_load_n(&method->compiled_code, __ATOMIC_ACQUIRE) != NULL) {
        /* Check the current compiled tier. If the new tier is not
         * higher, skip (no point recompiling at the same or lower tier). */
        vtx_compile_tier_t current_tier = VT_TIER_T0;
        if (ctx->profiler != NULL) {
            vtx_profile_data_t *pd = vtx_profiler_get_method_data(
                ctx->profiler, method);
            if (pd != NULL) {
                current_tier = pd->compiled_tier;
            }
        }
        if (tier <= current_tier) {
            vtx_clear_compilation_requested(ctx, method_id);
            return 0;  /* not an error — just no recompilation needed */
        }
        /* Tier promotion: proceed to recompile at the higher tier. */
    }

    /* Create a per-compilation arena */
    vtx_arena_t compile_arena;
    if (vtx_arena_init(&compile_arena) != 0) {
        vtx_clear_compilation_requested(ctx, method_id);
        return -1;
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

            /* Record the compiled tier and reset the tier-up counter
             * so the method can be promoted to T2 when it gets hotter.
             * Without this reset, compilation_requested stays true
             * forever and the method is never re-compiled at a higher
             * tier — T2 and T3 become unreachable. */
            if (ctx->profiler != NULL) {
                vtx_profiler_set_compiled_tier(ctx->profiler, method, tier);
                vtx_profiler_tier_up_reset(ctx->profiler, method, 0);
            }
        } else {
            fprintf(stderr, "[compile] T1 compilation failed for method %u\n", method_id);
        }
    } else {
        /* T2+: Optimizing pipeline compilation */
        vtx_graph_t graph;
        if (vtx_graph_init(&graph, method->arg_count) != 0) {
            vtx_arena_destroy(&compile_arena);
            vtx_clear_compilation_requested(ctx, method_id);
            return -1;
        }
        if (vtx_graph_build(&graph, method->bytecode, method, &compile_arena) != 0) {
            vtx_graph_destroy(&graph);
            vtx_arena_destroy(&compile_arena);
            vtx_clear_compilation_requested(ctx, method_id);
            return -1;
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

        /* Wire the versioned cache so the pipeline can register new
         * versions. This enables N+1 versioning and safe reclamation
         * of old compiled code. */
        config.versioned_cache = ctx->versioned_cache;

        /* Wire the deoptless tables so the pipeline can create per-method
         * continuation tables. The deopt handler uses these to look up
         * pre-compiled continuations when a guard fails. */
        config.deoptless_tables = ctx->deoptless_tables;
        config.deoptless_table_count = ctx->deoptless_table_count;
        config.deoptless_table_capacity = ctx->deoptless_table_capacity;

        /* Wire type feedback so T3 speculative guards can use observed
         * receiver types from the interpreter's inline caches. */
        config.type_feedback = ctx->type_feedback;

        /* Wire the Markov chain so the pipeline can check for predicted
         * phase transitions and proactively compile hot methods. */
        config.markov = ctx->markov;

        /* Wire the profiler so block layout can use branch probability data. */
        config.profiler = ctx->profiler;

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
            /* Pipeline succeeded — code is installed.
             *
             * Tier promotion: after T2 compilation, reset the tier-up
             * counter so the method can be promoted to T3 when its heat
             * crosses VORTEX_T3_THRESHOLD. T3 adds speculative guards,
             * SIMD vectorization, loop specialization, and 5 optimization
             * iterations (vs 3 for T2) — but only fires if the method is
             * very hot, so we need the counter reset to detect that.
             *
             * After T3 compilation, don't reset — T3 is the top tier and
             * recompiling again would just thrash. */
            if (ctx->profiler != NULL && tier < VTX_TIER_T3) {
                vtx_profiler_set_compiled_tier(ctx->profiler, method, tier);
                vtx_profiler_tier_up_reset(ctx->profiler, method, 0);
            } else if (ctx->profiler != NULL) {
                /* T3 — record the tier but don't reset the counter. */
                vtx_profiler_set_compiled_tier(ctx->profiler, method, tier);
            }
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
    return 0;
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
