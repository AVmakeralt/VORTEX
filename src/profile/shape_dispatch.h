/* ============================================================================ *
 * AI-GENERATED CODE
 *
 * This file was written from scratch by an AI assistant (GLM/Z.ai).
 * It is part of the VORTEX JIT compiler project.
 *
 * Human-written code lives in: src/interp/ (dispatch loop), src/baseline/
 * (codegen), src/runtime/ (GC, type system, arena), src/main_new.c.
 *
 * If reviewing, please verify correctness independently.
 * ============================================================================ */

#ifndef VORTEX_PROFILE_SHAPE_DISPATCH_H
#define VORTEX_PROFILE_SHAPE_DISPATCH_H

/**
 * VORTEX Input-Shape-Keyed Dispatch (Sprint 4.3)
 *
 * At compile time: if a method has multiple distinct input shapes (each
 * with enough samples), compile multiple versions of the method — one
 * per shape. At call time: dispatch to the version matching the current
 * input shape.
 *
 * This is the "parameter-sensitive plan caching" that database query
 * planners do, brought to a JIT. No production JIT does this.
 *
 * The dispatch table maps (method_id, input_shape) → compiled_code.
 * The interpreter/callsite checks the table at call time:
 *   1. Compute the current input shape from the call arguments
 *   2. Look up the shape in the dispatch table
 *   3. If found: jump to that version's compiled code
 *   4. If not found: fall back to the default version
 *
 * The table is lock-free for reads (the call path is hot). Writes
 * (installing new shape-versions) go through a mutex.
 */

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "vortex_config.h"
#include "profile/input_shape.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* Configuration                                                               */
/* ========================================================================== */

/**
 * Maximum number of shape-specific compiled versions per method.
 * Matches VTX_INPUT_SHAPE_MAX_PER_METHOD — one compiled version per
 * tracked shape.
 */
#define VTX_SHAPE_DISPATCH_MAX_VERSIONS VTX_INPUT_SHAPE_MAX_PER_METHOD

/* ========================================================================== */
/* Shape-version entry                                                         */
/* ========================================================================== */

/**
 * A single (shape → compiled_code) mapping.
 *
 * The compiled_code pointer is opaque — the dispatch table doesn't know
 * the details of the compiled code, it just stores the entry point.
 */
typedef struct {
    vtx_input_shape_t  shape;          /* the input shape this version is for */
    void              *compiled_code;  /* entry point of the compiled version */
    void              *code_metadata;  /* optional metadata (e.g., deopt info) */
    uint64_t           compile_time_ns;/* when this version was compiled */
    uint64_t           call_count;     /* how many times this version was called */
    bool               valid;          /* false if this slot is unused */
} vtx_shape_version_t;

/* ========================================================================== */
/* Per-method dispatch table                                                   */
/* ========================================================================== */

/**
 * Per-method shape dispatch table.
 *
 * Holds up to VTX_SHAPE_DISPATCH_MAX_VERSIONS shape-specific compiled
 * versions. The first entry (index 0) is always the "default" version
 * (shape == VTX_INPUT_SHAPE_DEFAULT) — the fallback when no shape-
 * specific version matches.
 */
typedef struct {
    uint32_t              method_id;     /* method this table is for */
    vtx_shape_version_t   versions[VTX_SHAPE_DISPATCH_MAX_VERSIONS];
    uint32_t              version_count; /* number of valid entries */
    pthread_mutex_t       mutex;         /* protects writes (installs) */
} vtx_shape_dispatch_t;

/* ========================================================================== */
/* Global dispatch manager                                                     */
/* ========================================================================== */

/**
 * Global shape dispatch manager.
 *
 * Holds per-method dispatch tables. The interpreter queries this at
 * call time to find the right compiled version for the current input shape.
 */
typedef struct {
    vtx_shape_dispatch_t **tables;
    uint32_t               table_count;     /* highest method_id seen + 1 */
    uint32_t               table_capacity;
    pthread_mutex_t        global_mutex;    /* protects table array growth */

    /* Statistics */
    uint64_t               total_dispatches;       /* shape-specific calls */
    uint64_t               total_default_fallbacks; /* no shape match → default */
    uint64_t               total_versions_compiled; /* shape-specific compiles */
} vtx_shape_dispatch_mgr_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the global shape dispatch manager.
 */
int vtx_shape_dispatch_mgr_init(vtx_shape_dispatch_mgr_t *mgr);

/**
 * Destroy the global shape dispatch manager.
 */
void vtx_shape_dispatch_mgr_destroy(vtx_shape_dispatch_mgr_t *mgr);

/* ========================================================================== */
/* Version installation (compile-time path)                                    */
/* ========================================================================== */

/**
 * Install a shape-specific compiled version for a method.
 *
 * Called by the JIT compiler after compiling a version specialized for
 * a particular input shape. If the table is full, the LRU shape-version
 * is evicted.
 *
 * @param mgr            Dispatch manager
 * @param method_id      Method ID
 * @param shape          Input shape this version is specialized for
 * @param compiled_code  Entry point of the compiled code
 * @param code_metadata  Optional metadata (may be NULL)
 * @return               0 on success, -1 on failure
 */
int vtx_shape_dispatch_install(vtx_shape_dispatch_mgr_t *mgr,
                                 uint32_t method_id,
                                 vtx_input_shape_t shape,
                                 void *compiled_code,
                                 void *code_metadata);

/**
 * Install the default (shapeless) compiled version for a method.
 *
 * This is the fallback version used when no shape-specific version
 * matches. Every method that participates in shape dispatch must have
 * a default version installed.
 *
 * @param mgr            Dispatch manager
 * @param method_id      Method ID
 * @param compiled_code  Entry point of the compiled code
 * @param code_metadata  Optional metadata (may be NULL)
 * @return               0 on success, -1 on failure
 */
int vtx_shape_dispatch_install_default(vtx_shape_dispatch_mgr_t *mgr,
                                          uint32_t method_id,
                                          void *compiled_code,
                                          void *code_metadata);

/* ========================================================================== */
/* Dispatch (call-time path — lock-free reads)                                 */
/* ========================================================================== */

/**
 * Look up the compiled version for a method + input shape.
 *
 * This is the hot-path call-time query. It's lock-free: the caller
 * reads the versions array without acquiring the mutex. Writes
 * (installs) use atomic swaps to ensure readers never see a torn
 * pointer.
 *
 * If a shape-specific version exists, it's returned. Otherwise, the
 * default version is returned. If no default exists either, NULL is
 * returned (the caller should use the interpreter).
 *
 * @param mgr        Dispatch manager
 * @param method_id  Method ID
 * @param shape      Current input shape
 * @return           Compiled code entry point, or NULL
 */
void *vtx_shape_dispatch_lookup(vtx_shape_dispatch_mgr_t *mgr,
                                  uint32_t method_id,
                                  vtx_input_shape_t shape);

/**
 * Record a dispatch (for statistics).
 *
 * Called by the interpreter after a successful lookup to track whether
 * the shape-specific version or the default was used.
 *
 * @param mgr              Dispatch manager
 * @param shape_specific   true if a shape-specific version was used,
 *                         false if the default was used
 */
void vtx_shape_dispatch_record(vtx_shape_dispatch_mgr_t *mgr,
                                 bool shape_specific);

/* ========================================================================== */
/* Queries                                                                     */
/* ========================================================================== */

/**
 * Check if a method has multiple shape-specific versions installed.
 *
 * Returns the number of shape-specific versions (excluding the default).
 * If >0, the method benefits from shape-keyed dispatch.
 */
uint32_t vtx_shape_dispatch_version_count(vtx_shape_dispatch_mgr_t *mgr,
                                            uint32_t method_id);

/**
 * Get the dispatch table for a method (for introspection).
 * Returns NULL if the method has no dispatch table.
 */
vtx_shape_dispatch_t *vtx_shape_dispatch_get_table(
    vtx_shape_dispatch_mgr_t *mgr,
    uint32_t method_id);

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

/**
 * Get dispatch statistics.
 *
 * @param mgr                    Dispatch manager
 * @param total_dispatches       Out: total shape-specific calls
 * @param total_default_fallbacks Out: calls that fell back to default
 * @param total_versions_compiled Out: shape-specific versions compiled
 * @param table_count            Out: number of methods with dispatch tables
 */
void vtx_shape_dispatch_stats(const vtx_shape_dispatch_mgr_t *mgr,
                                uint64_t *total_dispatches,
                                uint64_t *total_default_fallbacks,
                                uint64_t *total_versions_compiled,
                                uint32_t *table_count);

#ifdef __cplusplus
}
#endif

#endif /* VORTEX_PROFILE_SHAPE_DISPATCH_H */
