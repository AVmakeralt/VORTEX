/* runtime/vortex_runtime.h — High-level VORTEX runtime API for embedding.
 *
 * This header provides a simplified C API for creating and using a VORTEX
 * runtime from external code (e.g. Rust bindings, language frontends).
 *
 * The runtime bundles all VORTEX subsystems (type system, GC, interpreter,
 * code cache, compilation threadpool) into a single struct with a simple
 * create/run/destroy lifecycle.
 */

#ifndef VORTEX_RUNTIME_H
#define VORTEX_RUNTIME_H

#include "runtime/arena.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "interp/dispatch.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "compile/threadpool.h"
#include "compile/orchestrator.h"
#include "compile/request.h"

#ifdef __cplusplus
extern "C" {
#endif

/* High-level runtime context — bundles all subsystems. */
typedef struct vtx_runtime_t {
    vtx_type_system_t    type_system;
    vtx_gc_t             gc;
    vtx_interp_t        *interp;
    vtx_code_cache_t     code_cache;
    vtx_method_registry_t method_registry;
    vtx_arena_t          arena;
    vtx_orchestrator_t  *orchestrator;
    vtx_threadpool_t    *threadpool;
    int                  initialized;
} vtx_runtime_t;

/* Compilation context is defined in compile/request.h */

/* ---- Lifecycle ---- */

/* Create a runtime with default settings. Returns 0 on success. */
int vtx_runtime_create(vtx_runtime_t *rt);

/* Destroy a runtime, freeing all resources. */
void vtx_runtime_destroy(vtx_runtime_t *rt);

/* ---- Execution ---- */

/* Run a bytecode method through the interpreter (T0). Returns the result. */
vtx_value_t vtx_runtime_run(vtx_runtime_t *rt, const vtx_bytecode_t *bc);

/* Run with arguments. */
vtx_value_t vtx_runtime_run_with_args(vtx_runtime_t *rt,
                                       const vtx_bytecode_t *bc,
                                       const vtx_value_t *args,
                                       uint32_t arg_count);

/* ---- Accessors (return pointers into the runtime struct) ---- */
vtx_interp_t *vtx_runtime_interp(vtx_runtime_t *rt);
vtx_type_system_t *vtx_runtime_type_system(vtx_runtime_t *rt);
vtx_gc_t *vtx_runtime_gc(vtx_runtime_t *rt);
vtx_code_cache_t *vtx_runtime_code_cache(vtx_runtime_t *rt);
vtx_compile_context_t *vtx_runtime_compile_ctx(vtx_runtime_t *rt);

/* ---- Threadpool ---- */
int vtx_runtime_start_threadpool(vtx_runtime_t *rt, uint32_t nthreads);
void vtx_runtime_stop_threadpool(vtx_runtime_t *rt);

/* ---- Bytecode loading ---- */
/* Load a .vtbc file from disk into a freshly allocated bytecode struct.
 * Returns NULL on failure. The caller owns the result. */
vtx_bytecode_t *vtx_bytecode_load(const char *path, vtx_arena_t *arena);

#ifdef __cplusplus
}
#endif

#endif /* VORTEX_RUNTIME_H */
