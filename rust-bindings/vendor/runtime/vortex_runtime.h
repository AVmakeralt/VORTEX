/* runtime/vortex_runtime.h — High-level VORTEX runtime API for embedding.
 *
 * Bundles all VORTEX subsystems into a single struct with create/run/destroy.
 * vtx_runtime_run() goes through the REAL JIT pipeline: the interpreter
 * dispatches to compiled code when available, and hot methods are compiled
 * by the background threadpool (T1 baseline → T2 optimizing).
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
#include "compile/pipeline.h"
#include "baseline/codegen.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vtx_runtime_t {
    vtx_type_system_t         type_system;
    vtx_gc_t                  gc;
    vtx_interp_t             *interp;
    vtx_code_cache_t          code_cache;
    vtx_method_registry_t     method_registry;
    vtx_arena_t               arena;
    vtx_orchestrator_t       *orchestrator;
    vtx_threadpool_t         *threadpool;
    vtx_compile_context_t    *compile_ctx;
    int                       initialized;
    int                       use_jit;       /* 1 = enable JIT compilation */
    uint32_t                  hot_threshold; /* invocations before compile */
} vtx_runtime_t;

/* ---- Lifecycle ---- */
int  vtx_runtime_create(vtx_runtime_t *rt);
void vtx_runtime_destroy(vtx_runtime_t *rt);

/* Enable JIT: start the background compilation threadpool and wire
 * the interpreter's compile callback so hot methods get compiled. */
int  vtx_runtime_enable_jit(vtx_runtime_t *rt, uint32_t nthreads);

/* ---- Execution ---- */
/* Run bytecode through the interpreter. If JIT is enabled and the method
 * has been compiled (either eagerly or via tier-up), the interpreter
 * dispatches to the compiled code automatically. */
vtx_value_t vtx_runtime_run(vtx_runtime_t *rt, const vtx_bytecode_t *bc);
vtx_value_t vtx_runtime_run_with_args(vtx_runtime_t *rt,
                                       const vtx_bytecode_t *bc,
                                       const vtx_value_t *args,
                                       uint32_t arg_count);

/* Eagerly compile a method at the given tier (1=baseline, 2=optimizing).
 * Returns 0 on success, -1 on failure. After this, vtx_runtime_run will
 * dispatch to the compiled code. */
int  vtx_runtime_compile(vtx_runtime_t *rt, vtx_method_desc_t *method,
                          int tier);

/* ---- Accessors ---- */
vtx_interp_t          *vtx_runtime_interp(vtx_runtime_t *rt);
vtx_type_system_t     *vtx_runtime_type_system(vtx_runtime_t *rt);
vtx_gc_t              *vtx_runtime_gc(vtx_runtime_t *rt);
vtx_code_cache_t      *vtx_runtime_code_cache(vtx_runtime_t *rt);
vtx_compile_context_t *vtx_runtime_compile_ctx(vtx_runtime_t *rt);

/* ---- Bytecode loading ---- */
vtx_bytecode_t *vtx_bytecode_load(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* VORTEX_RUNTIME_H */
