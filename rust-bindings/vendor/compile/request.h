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

#ifndef VORTEX_COMPILE_REQUEST_H
#define VORTEX_COMPILE_REQUEST_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/type_system.h"  /* for vtx_method_desc_t */
#include "compile/priority.h"    /* for vtx_compile_tier_t */
#include "compile/threadpool.h"  /* for vtx_threadpool_t */
#include "interp/type_feedback.h" /* for vtx_type_feedback_t */
#include "interp/profiler.h"     /* for vtx_profiler_t, vtx_profile_data_t */
#include "sota/markov.h"         /* for vtx_markov_t */

/* Forward declarations — use typedefs from the actual headers.
 * The old code used `struct vtx_foo *` forward declarations, but the
 * actual types are `typedef struct { ... } vtx_foo_t;` (anonymous struct
 * tags). This made `struct vtx_foo *` and `vtx_foo_t *` DIFFERENT types,
 * hidden by -Wno-incompatible-pointer-types. Fix: include the headers
 * so we use the real typedef'd pointer types. */
#include "codecache/cache.h"          /* vtx_code_cache_t */
#include "codecache/install.h"        /* vtx_method_registry_t */
#include "runtime/arena.h"            /* vtx_arena_t */
#include "compile/orchestrator.h"     /* vtx_orchestrator_t */
#include "compile/spec_versioning.h"  /* vtx_spec_version_manager_t */
#include "deopt/coordinator.h"        /* vtx_deopt_coord_t */
#include "codecache/versioned.h"      /* vtx_versioned_cache_t */
#include "runtime/safepoint_manager.h" /* vtx_safepoint_manager_t */

/**
 * VORTEX Compilation Request
 *
 * Bridges the interpreter's hot-code detection with the compilation
 * thread pool. When the interpreter detects that a method has exceeded
 * its tier-up threshold (via vtx_profiler_tier_up_check), it calls
 * vtx_request_compilation() to queue the method for background
 * compilation.
 *
 * This is the critical wiring that was previously missing — the
 * interpreter had TODO comments where this function should be called.
 * Without it, the entire JIT pipeline was dead code.
 */

/* ========================================================================== */
/* Compilation context                                                          */
/* ========================================================================== */

/**
 * Global compilation context shared between the interpreter and
 * the compilation thread pool. Initialized once at startup.
 */
typedef struct {
    vtx_threadpool_t             *threadpool;
    vtx_code_cache_t             *code_cache;
    vtx_method_registry_t        *method_registry;
    vtx_arena_t                  *global_arena;
    vtx_orchestrator_t           *orchestrator;
    vtx_spec_version_manager_t   *spec_version_mgr;  /* argument-type specialization */
    vtx_deopt_coord_t            *deopt_coord;  /* deopt rate limiting / batching */
    vtx_versioned_cache_t        *versioned_cache;  /* N+1 versioning + safe reclamation */
    vtx_safepoint_manager_t      *safepoint_mgr;  /* multi-threaded safepoint manager */

    /* Deoptless continuation tables: per-method array indexed by method_id.
     * Each entry tracks continuation versions for a method. When a guard
     * fails, the deopt handler checks if a deoptless continuation exists
     * and jumps to it instead of deoptimizing to the interpreter. */
    vtx_deoptless_table_t       **deoptless_tables;
    uint32_t                      deoptless_table_count;
    uint32_t                      deoptless_table_capacity;

    /* Type feedback from the interpreter. Forwarded to the pipeline config
     * so T3 speculative guards can use observed receiver types. */
    const vtx_type_feedback_t    *type_feedback;

    /* Markov chain for predictive compilation. Forwarded to the pipeline
     * config so the pipeline can check for predicted phase transitions
     * and proactively compile methods that will be hot in the next phase. */
    vtx_markov_t                  *markov;

    /* Profiler — used by the compile callback to record which tier a
     * method was compiled at (vtx_profiler_set_compiled_tier) and to
     * reset the tier-up counter after T1/T2 compilation so the method
     * can be promoted to a higher tier when it gets hotter.
     * Without this, every method compiles exactly once at whatever
     * tier it first crossed the threshold for, and T3 is never reached. */
    vtx_profiler_t                *profiler;

    /* Method lookup: given a method_id, returns the method descriptor.
     * This is needed by the threadpool worker to find the method's
     * bytecode when compiling. */
    const vtx_method_desc_t *(*method_lookup)(uint32_t method_id, void *context);
    void                     *method_lookup_context;

    /* Track which methods have been submitted for compilation to
     * avoid re-queueing the same method multiple times. */
    bool     *compilation_requested;  /* per-method_id flag */
    uint32_t  compilation_requested_count;
    uint32_t  compilation_requested_capacity;

    /* Tier decision: which tier to compile at for a given method.
     * Based on execution count from the profiler. */
    vtx_compile_tier_t (*tier_decision)(uint64_t execution_count);
} vtx_compile_context_t;

/**
 * Initialize the compilation context.
 * All pointers are stored but NOT owned.
 * Returns 0 on success, -1 on failure.
 */
int vtx_compile_context_init(vtx_compile_context_t *ctx);

/**
 * Destroy the compilation context and free internal resources.
 */
void vtx_compile_context_destroy(vtx_compile_context_t *ctx);

/**
 * Set the method lookup callback.
 * The callback takes a method_id and returns the method descriptor.
 */
void vtx_compile_context_set_method_lookup(
    vtx_compile_context_t *ctx,
    const vtx_method_desc_t *(*lookup)(uint32_t, void *),
    void *context);

/* ========================================================================== */
/* Compilation request                                                          */
/* ========================================================================== */

/**
 * Request compilation of a method.
 *
 * Called from the interpreter's dispatch loop when a method's
 * execution count exceeds the tier-up threshold. Submits the
 * method for background compilation on the thread pool.
 *
 * If the method has already been submitted (compilation_requested
 * flag is set), this is a no-op.
 *
 * @param ctx      Compilation context
 * @param method   Method that needs compilation
 */
void vtx_request_compilation(vtx_compile_context_t *ctx,
                              const vtx_method_desc_t *method,
                              uint64_t execution_count);

/**
 * Check if a method has been submitted for compilation.
 */
bool vtx_is_compilation_requested(const vtx_compile_context_t *ctx,
                                    uint32_t method_id);

/**
 * Clear the compilation-requested flag for a method.
 * Called after compilation completes or fails.
 */
void vtx_clear_compilation_requested(vtx_compile_context_t *ctx,
                                       uint32_t method_id);

/**
 * Wire the compile context to its threadpool.
 *
 * This sets the threadpool's compile_callback to a function that
 * looks up the method by ID, compiles it (T1 baseline or T2+ pipeline),
 * and installs the result in the code cache.
 *
 * Must be called after setting ctx->threadpool, ctx->code_cache,
 * ctx->method_registry, and ctx->method_lookup.
 *
 * Returns 0 on success, -1 if ctx or ctx->threadpool is NULL.
 */
int vtx_compile_context_wire_threadpool(vtx_compile_context_t *ctx);

#endif /* VORTEX_COMPILE_REQUEST_H */
