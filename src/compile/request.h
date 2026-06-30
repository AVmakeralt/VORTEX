#ifndef VORTEX_COMPILE_REQUEST_H
#define VORTEX_COMPILE_REQUEST_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/type_system.h"  /* for vtx_method_desc_t */
#include "compile/priority.h"    /* for vtx_compile_tier_t */

/* Forward declarations */
struct vtx_threadpool;
struct vtx_code_cache;
struct vtx_method_registry;
struct vtx_arena;
struct vtx_orchestrator;

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
    struct vtx_threadpool        *threadpool;
    struct vtx_code_cache        *code_cache;
    struct vtx_method_registry   *method_registry;
    struct vtx_arena             *global_arena;
    struct vtx_orchestrator      *orchestrator;

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
