/* ========================================================================== */
/* AOT Background Compilation with Guard Recovery                               */
/* ========================================================================== */
/*
 * compile/aot.h — Background AOT compilation of traces with serialized
 * guards and bailout stubs.
 *
 * The AOT system runs as a background thread. It picks up "AOT artifacts"
 * (serialized compilation requests) from a queue and compiles them with
 * aggressive optimizations:
 *
 *   - Higher inlining threshold (4096 → 8192 callee node budget)
 *   - More loop unrolling (max 8 iterations instead of 4)
 *   - Speculative guard insertion (T3 mode)
 *   - Loop specialization + SIMD vectorization
 *
 * Each compiled artifact includes:
 *   - Native code (x86-64 machine code)
 *   - Guard metadata (bytecode_pc, jcc_offset, frame_state_index)
 *   - Side table (deopt PC → FrameState mapping)
 *   - Relocation table (external call addresses)
 *
 * On guard failure at runtime:
 *   1. Bailout stub (pre-generated) is entered
 *   2. Deopt handler reconstructs interpreter state from side table
 *   3. vtx_aot_on_guard_failure() feeds the failure into the retrace system
 *   4. The trace edge is marked "unstable" → next trace entry re-traces
 *
 * Memory management:
 *   - Artifacts are heap-allocated (malloc/free), NOT arena-allocated
 *   - This lets them outlive the arena scope (arena is per-compile)
 *   - The AOT manager owns artifacts until they're installed or discarded
 *   - Installed artifacts transfer code ownership to the code cache
 *
 * Thread safety:
 *   - The AOT queue is mutex-protected
 *   - The worker thread runs independently of the orchestrator
 *   - Statistics are read with atomic loads
 */

#ifndef VORTEX_COMPILE_AOT_H
#define VORTEX_COMPILE_AOT_H

#include "vortex_config.h"
#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "codecache/install.h"
#include "compile/threadpool.h"
#include "compile/pipeline.h"

/* ========================================================================== */
/* AOT Artifact — serialized compilation request                               */
/* ========================================================================== */

/* A guard entry in the serialized artifact. */
typedef struct {
    uint32_t bytecode_pc;       /* source PC for deopt recovery */
    uint32_t guard_node;        /* SoN node ID (for dependency tracking) */
    uint32_t cond;              /* vtx_cond_t (EQ, NE, LT, etc.) */
    uint32_t type_id;           /* expected type (for type guards) */
    uint32_t shape_id;          /* expected shape (for shape guards) */
    uint32_t jcc_offset;        /* offset of JCC in native code */
    uint32_t frame_state_index; /* index into side table */
} vtx_aot_guard_t;

/* An AOT artifact — a self-contained compilation request.
 *
 * Heap-allocated so it outlives the arena. The AOT manager owns the
 * artifact until code is installed (then code ownership transfers to
 * the code cache) or the artifact is discarded. */
typedef struct vtx_aot_artifact {
    /* ---- Input: what to compile ---- */
    uint32_t method_id;           /* method ID for registry lookup */
    uint32_t trace_id;            /* trace ID (for retrace system) */
    uint32_t tier;                /* target tier (2=T2, 3=T3) */

    /* The bytecode to compile (heap-copied — the caller may free the
     * original after submitting the artifact). */
    vtx_bytecode_t *bytecode;     /* heap-allocated copy */
    vtx_method_desc_t *method;    /* heap-allocated copy of method desc */

    /* Optimization level for AOT compilation.
     * AOT uses more aggressive settings than JIT:
     *   - inline_size_limit: 8192 (vs 4096 for JIT)
     *   - max_unroll_factor: 8 (vs 4 for JIT)
     *   - run_speculative: true (insert speculative guards)
     *   - run_loop_spec: true
     *   - run_vectorize: true */
    uint32_t inline_size_limit;
    uint32_t max_unroll_factor;
    bool     run_speculative;
    bool     run_loop_spec;
    bool     run_vectorize;

    /* ---- Output: compiled result ---- */
    uint8_t *code;                /* native code (heap-allocated) */
    uint32_t code_size;           /* size of code in bytes */
    vtx_aot_guard_t *guards;      /* guard metadata array (heap-allocated) */
    uint32_t guard_count;         /* number of guards */
    uint32_t guard_capacity;      /* allocated capacity for guards */
    void *side_table;             /* vtx_side_table_t* (heap-allocated) */
    void *reloc_table;            /* vtx_reloc_table_t* (heap-allocated) */
    void *frame_layout;           /* vtx_jit_frame_layout_t* (heap-allocated) */

    /* ---- Status ---- */
    bool is_compiled;             /* worker has compiled this */
    bool is_installed;             /* code is installed in cache */
    bool is_stale;                /* should be re-compiled? */

    /* ---- Linked list (for the queue) ---- */
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
    pthread_cond_t  cond;          /* signal when new artifact added */
    bool shutdown;
} vtx_aot_queue_t;

/* ========================================================================== */
/* AOT Manager — owns the queue and worker thread                              */
/* ========================================================================== */

typedef struct vtx_aot_manager {
    vtx_aot_queue_t     queue;          /* pending artifacts */
    vtx_code_cache_t   *code_cache;     /* where to install compiled code */
    vtx_method_registry_t *registry;    /* method registry for install */
    pthread_t           worker_thread; /* background AOT worker */
    bool                 worker_running;

    /* Statistics (atomically updated) */
    uint64_t            total_artifacts;     /* total artifacts processed */
    uint64_t            total_compiled;      /* artifacts successfully compiled */
    uint64_t            total_installed;     /* artifacts installed in cache */
    uint64_t            total_failed;        /* artifacts that failed compilation */
    uint64_t            total_bailouts;      /* guard failures handled */
    uint64_t            total_retraces_triggered;  /* re-traces triggered by AOT */
} vtx_aot_manager_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/* Initialize the AOT manager. Does NOT start the worker thread. */
int vtx_aot_init(vtx_aot_manager_t *aot,
                   vtx_code_cache_t *cache,
                   vtx_method_registry_t *registry);

/* Destroy the AOT manager. Stops the worker thread if running.
 * Frees all pending artifacts (their code, guards, etc.). */
void vtx_aot_destroy(vtx_aot_manager_t *aot);

/* Start the background AOT worker thread. */
int vtx_aot_start(vtx_aot_manager_t *aot);

/* Stop the background AOT worker thread. */
void vtx_aot_stop(vtx_aot_manager_t *aot);

/* ========================================================================== */
/* Artifact creation and submission                                            */
/* ========================================================================== */

/* Create an AOT artifact for background compilation.
 *
 * Makes a HEAP-COPY of the bytecode and method descriptor so the artifact
 * is self-contained — the caller can free the originals after submitting.
 *
 * The artifact starts with aggressive AOT optimization settings:
 *   - inline_size_limit = 8192
 *   - max_unroll_factor = 8
 *   - run_speculative = true
 *   - run_loop_spec = true
 *   - run_vectorize = true
 *
 * Returns the artifact (heap-allocated), or NULL on failure.
 * Caller must submit it via vtx_aot_submit() or free it via
 * vtx_aot_artifact_free(). */
vtx_aot_artifact_t *vtx_aot_create_artifact(uint32_t method_id,
                                              uint32_t trace_id,
                                              uint32_t tier,
                                              const vtx_bytecode_t *bytecode,
                                              const vtx_method_desc_t *method);

/* Free an AOT artifact and all its heap-allocated contents.
 * Safe to call on NULL. Does NOT free code that was installed in the
 * cache (ownership transferred). */
void vtx_aot_artifact_free(vtx_aot_artifact_t *artifact);

/* Add a guard to an artifact. The guards array grows dynamically (realloc). */
int vtx_aot_add_guard(vtx_aot_artifact_t *artifact,
                        uint32_t bytecode_pc,
                        uint32_t guard_node,
                        uint32_t cond,
                        uint32_t type_id,
                        uint32_t shape_id,
                        uint32_t jcc_offset,
                        uint32_t frame_state_index);

/* Submit an artifact to the AOT queue for background compilation.
 * The AOT manager takes ownership — do NOT free the artifact after this. */
int vtx_aot_submit(vtx_aot_manager_t *aot, vtx_aot_artifact_t *artifact);

/* ========================================================================== */
/* Bailout stubs                                                               */
/* ========================================================================== */

/* Generate bailout stubs for all guards in an artifact.
 *
 * This is called by the worker thread after compilation. It records
 * the jcc_offset and frame_state_index for each guard so the deopt
 * handler can reconstruct interpreter state on failure.
 *
 * The actual bailout stub CODE is emitted by the pipeline's guard
 * emission (vtx_guard_emit_deopt_stubs). This function just records
 * the metadata. */
int vtx_aot_generate_bailout_stubs(vtx_aot_artifact_t *artifact);

/* ========================================================================== */
/* Guard failure handling                                                      */
/* ========================================================================== */

/* Called when an AOT guard fails at runtime.
 *
 * Feeds the failure into the retrace system and increments counters.
 * The actual interpreter state reconstruction is handled by the existing
 * deopt handler. */
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
    uint32_t failed_count;           /* artifacts that failed */
    uint64_t total_bailouts;          /* guard failures handled */
    uint64_t total_retraces;          /* re-traces triggered */
} vtx_aot_stats_t;

vtx_aot_stats_t vtx_aot_stats(const vtx_aot_manager_t *aot);

/* ========================================================================== */
/* AOT pipeline configuration                                                  */
/* ========================================================================== */

/* Get a pipeline config for AOT compilation.
 *
 * AOT uses more aggressive optimization than JIT:
 *   - T3 base config (speculative guards)
 *   - inline_size_limit = 8192 (vs 4096 for JIT)
 *   - GVN iterations = 5 (vs 3 for JIT)
 *   - SCCP iterations = 10 (vs 5 for JIT)
 *
 * The caller can override these per-artifact by modifying the artifact's
 * optimization fields before submitting. */
vtx_pipeline_config_t vtx_pipeline_config_aot(void);

#endif /* VORTEX_COMPILE_AOT_H */
