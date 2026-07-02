#ifndef VORTEX_CALLEE_LOOKUP_H
#define VORTEX_CALLEE_LOOKUP_H

#include "compile/pipeline.h"
#include "codecache/install.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"

/**
 * Create a callee lookup context for the inliner.
 *
 * Returns a function pointer that the pipeline config's callee_lookup
 * field should be set to. The context should be passed as
 * callee_lookup_context.
 *
 * @param registry    The method registry
 * @param type_system The type system
 * @param gc          The GC
 * @param out_ctx     Receives the context (destroy with vtx_callee_lookup_destroy)
 * @return            The callback function, or NULL on failure
 */
vtx_callee_lookup_fn vtx_callee_lookup_create(vtx_method_registry_t *registry,
                                                vtx_type_system_t *type_system,
                                                vtx_gc_t *gc,
                                                void **out_ctx);

/**
 * Destroy a callee lookup context and free all cached graphs.
 */
void vtx_callee_lookup_destroy(void *ctx);

#endif /* VORTEX_CALLEE_LOOKUP_H */
