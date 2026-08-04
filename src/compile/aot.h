/* ========================================================================== */
/* AOT Background Compilation with Guard Recovery                               */
/* ========================================================================== */
/*
 * compile/aot.h — Background AOT compilation of traces with serialized
 * guards and bailout stubs.
 *
 * The AOT system works as follows:
 *
 *   1. TRACE SERIALIZATION: When a trace is compiled (via the pipeline),
 *      the result includes native code + guard metadata. The AOT system
 *      serializes this into an "AOT artifact" — a self-contained binary
 *      blob that can be compiled/recompiled in the background.
 *
 *   2. BACKGROUND COMPILATION: A worker thread picks up AOT artifacts
 *      and compiles them with aggressive optimizations (higher inlining
 *      threshold, more loop unrolling). The compiled code includes
 *      bailout stubs for each guard.
 *
 *   3. GUARD RECOVERY: When an AOT guard fails at runtime:
 *      a. The bailout stub is entered (pre-generated code)
 *      b. It reconstructs the interpreter frame state from the deopt
 *         metadata (using the existing deoptless continuation maps)
 *      c. It marks the trace edge as "unstable" in the retrace system
 *      d. It transfers control to the interpreter at the deopt PC
 *      e. The next time the interpreter hits the trace entry, the
 *         retrace system forces a re-trace with the new type info
 *
 * The AOT system integrates with the existing:
 *   - Guard emission (lower/guard_emit.h) — emits CMP+JCC+bailout stub
 *   - Deopt handler (runtime_stubs.c) — reconstructs interpreter state
 *   - Trace retrace (trace/retrace.h) — marks edges as unstable
 *   - Code cache (codecache/install.h) — installs AOT code
 *   - Versioned cache (codecache/versioned.h) — old version kept alive
 *
 * Thread safety: The AOT queue is protected by a mutex. The worker
 * thread runs independently of the orchestrator thread.
 */

#ifndef VORTEX_COMPILE_AOT_H
#define VORTEX_COMPILE_AOT_H

#include "vortex_config.h"
#include "runtime/arena.h"
#include "codecache/install.h"
#include "compile/threadpool.h"

/* ========================================================================== */
/* AOT Artifact — serialized trace for background compilation                  */
/* ========================================================================== */

/* A guard entry in the serialized artifact. Each guard has:
 *   - bytecode_pc: for deopt recovery (where to resume in interpreter)
 *   - guard_node: the SoN node ID (for dependency tracking)
 *   - cond: the comparison condition (EQ, NE, LT, etc.)
 *   - type_id: expected type (for type guards)
 *   - shape_id: expected shape (for shape guards)
 *   - jcc_offset: offset of the JCC instruction in the native code
 *   - frame_state_index: index into the side table for frame reconstruction
 */
typedef struct {
    uint32_t bytecode_pc;
    uint32_t guard_node;
    uint32_t cond;           /* vtx_cond_t */
    uint32_t type_id;
    uint32_t shape_id;
    uint32_t jcc_offset;     /* offset in native code where JCC is */
    uint32_t frame_state_index;
} vtx_aot_guard_t;

/* An AOT artifact — the serialized form of a compiled trace.
 *
 * Contains the native code blob + guard metadata, enough to:
 *   - Re-emit bailout stubs in the background
 *   - Patch JCC instructions to point to bailout stubs
 *   - Reconstruct interpreter state on guard failure
 *   - Mark trace edges as unstable for re-tracing
 */
typedef struct vtx_aot_artifact {
    uint32_t method_id;           /* the method this trace belongs to */
    uint32_t trace_id;            /* the trace ID (for retrace system) */
    uint32_t tier;                /* compilation tier (1=baseline, 2=optimizing) */

    /* Native code blob (copied from the pipeline result) */
    uint8_t *code;                /* native code bytes */
    uint32_t code_size;           /* size of code in bytes */

    /* Guard metadata */
    vtx_aot_guard_t *guards;      /* array of guard entries */
    uint32_t guard_count;         /* number of guards */

    /* Side table (deopt PC → FrameState mapping) */
    void *side_table;             /* vtx_side_table_t* (opaque for serialization) */
    uint32_t side_table_size;

    /* Relocation table (for external calls) */
    void *reloc_table;            /* vtx_reloc_table_t* (opaque) */
    uint32_t reloc_count;

    /* Frame layout (for OSR/deopt) */
    void *frame_layout;           /* vtx_jit_frame_layout_t* */

    /* Status */
    bool is_compiled;             /* has the AOT worker compiled this? */
    bool is_installed;            /* is the code installed in the cache? */
    bool is_stale;                /* should this be re-compiled? */

    /* Linked list (for the pending queue) */
    struct vtx_aot_artifact *next;
} vtx_aot_artifact_t;

/* ========================================================================== */
/* AOT Queue — pending artifacts awaiting background compilation                */
/* ========================================================================== */

typedef struct {
    vtx_aot_artifact_t *head;     /* linked list of pending artifacts */
    vtx_aot_artifact_t *tail;
    uint32_t count;               /* number of pending artifacts */
    pthread_mutex_t mutex;
    pthread_cond_t  cond;         /* signal when new artifact added */
    bool shutdown;
} vtx_aot_queue_t;

/* ========================================================================== */
/* AOT Manager — owns the queue and worker thread                              */
/* ========================================================================== */

typedef struct vtx_aot_manager {
    vtx_aot_queue_t     queue;          /* pending artifacts */
    vtx_code_cache_t   *code_cache;     /* where to install compiled code */
    vtx_method_registry_t *registry;    /* method registry for install */
    pthread_t           worker_thread;  /* background AOT worker */
    bool                 worker_running;

    /* Statistics */
    uint64_t            total_artifacts;     /* total artifacts processed */
    uint64_t            total_compiled;      /* artifacts successfully compiled */
    uint64_t            total_installed;     /* artifacts installed in cache */
    uint64_t            total_bailouts;      /* guard failures handled */
    uint64_t            total_retraces_triggered;  /* re-traces triggered by AOT */
} vtx_aot_manager_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/* Initialize the AOT manager. Does NOT start the worker thread.
 * Call vtx_aot_start() to begin background compilation. */
int vtx_aot_init(vtx_aot_manager_t *aot,
                   vtx_code_cache_t *cache,
                   vtx_method_registry_t *registry);

/* Destroy the AOT manager. Stops the worker thread if running. */
void vtx_aot_destroy(vtx_aot_manager_t *aot);

/* Start the background AOT worker thread. */
int vtx_aot_start(vtx_aot_manager_t *aot);

/* Stop the background AOT worker thread. */
void vtx_aot_stop(vtx_aot_manager_t *aot);

/* ========================================================================== */
/* Artifact creation and submission                                            */
/* ========================================================================== */

/* Create an AOT artifact from a pipeline compile result.
 *
 * Copies the native code and guard metadata into a self-contained
 * artifact that can be compiled in the background.
 *
 * Parameters:
 *   method_id  - the method this trace belongs to
 *   trace_id   - the trace ID (for retrace system)
 *   tier       - compilation tier
 *   code       - native code bytes (will be copied)
 *   code_size  - size of code
 *   arena      - arena for allocations (the artifact copies are arena-allocated)
 *
 * Returns the artifact, or NULL on failure.
 */
vtx_aot_artifact_t *vtx_aot_create_artifact(uint32_t method_id,
                                              uint32_t trace_id,
                                              uint32_t tier,
                                              const uint8_t *code,
                                              uint32_t code_size,
                                              vtx_arena_t *arena);

/* Add a guard to an artifact. */
int vtx_aot_add_guard(vtx_aot_artifact_t *artifact,
                        uint32_t bytecode_pc,
                        uint32_t guard_node,
                        uint32_t cond,
                        uint32_t type_id,
                        uint32_t shape_id,
                        uint32_t jcc_offset,
                        uint32_t frame_state_index,
                        vtx_arena_t *arena);

/* Submit an artifact to the AOT queue for background compilation.
 * The AOT manager takes ownership of the artifact. */
int vtx_aot_submit(vtx_aot_manager_t *aot, vtx_aot_artifact_t *artifact);

/* ========================================================================== */
/* Bailout stubs                                                               */
/* ========================================================================== */

/* Generate bailout stubs for all guards in an artifact.
 *
 * A bailout stub is a short sequence of native code that:
 *   1. Stores the frame_state_index in a register (for the deopt handler)
 *   2. Loads the deopt handler address
 *   3. Jumps to the deopt handler
 *
 * The bailout stub is placed after the main code. The guard's JCC
 * instruction is then patched to jump to the bailout stub instead of
 * falling through.
 *
 * This function writes the bailout stubs into a newly allocated buffer
 * and patches the JCC displacements in the artifact's code.
 *
 * Parameters:
 *   artifact - the artifact to generate stubs for
 *   arena     - arena for allocations
 *
 * Returns 0 on success, -1 on failure.
 */
int vtx_aot_generate_bailout_stubs(vtx_aot_artifact_t *artifact,
                                     vtx_arena_t *arena);

/* ========================================================================== */
/* Guard failure handling                                                      */
/* ========================================================================== */

/* Called when an AOT guard fails at runtime.
 *
 * This function:
 *   1. Marks the trace edge as "unstable" in the retrace system
 *   2. Increments the bailout counter
 *   3. May trigger a re-trace if the failure count exceeds the threshold
 *
 * The actual interpreter state reconstruction is handled by the existing
 * deopt handler (vtx_deopt_handler_stub in runtime_stubs.c). This function
 * is called FROM the deopt handler to feed the failure into the AOT system.
 *
 * Parameters:
 *   aot       - the AOT manager
 *   method_id - the method that deoptimized
 *   guard_id  - the guard that failed (index into artifact->guards)
 */
void vtx_aot_on_guard_failure(vtx_aot_manager_t *aot,
                                uint32_t method_id,
                                uint32_t guard_id);

/* ========================================================================== */
/* Introspection                                                               */
/* ========================================================================== */

typedef struct {
    uint32_t pending_count;          /* artifacts in queue */
    uint32_t compiled_count;         /* artifacts compiled */
    uint32_t installed_count;         /* artifacts installed */
    uint64_t total_bailouts;          /* guard failures handled */
    uint64_t total_retraces;          /* re-traces triggered */
} vtx_aot_stats_t;

vtx_aot_stats_t vtx_aot_stats(const vtx_aot_manager_t *aot);

#endif /* VORTEX_COMPILE_AOT_H */
