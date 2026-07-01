#ifndef VORTEX_DISPATCH_H
#define VORTEX_DISPATCH_H

#include "vortex_config.h"
#include <stdint.h>
#include <stdbool.h>
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/helpers.h"
#include "interp/frame.h"
#include "interp/profiler.h"
#include "interp/lookup.h"
#include "interp/type_feedback.h"
#include "compile/request.h"

/**
 * VORTEX Interpreter Dispatch Loop (Tier 0)
 *
 * Uses computed goto (GCC labels-as-values) for maximum dispatch
 * performance. The dispatch table is built at initialization from
 * opcode labels.
 *
 * Key design decisions:
 *   - No heap allocation in the dispatch loop (frame stack only)
 *   - Safe point checks at every backward branch
 *   - Profiling counters updated at branches and call sites
 *   - Inline caches stored per call site (one IC per bytecode PC slot)
 *   - Exception handling via frame chain catch handlers
 */

/* ========================================================================== */
/* Interpreter structure                                                       */
/* ========================================================================== */

/**
 * Per-method IC storage: one inline cache per bytecode position.
 * Only call site PCs will actually use their ICs, but this makes
 * indexing trivial (just use the call site's PC as the index).
 */
typedef struct {
    const vtx_method_desc_t *method; /* method this IC storage belongs to */
    vtx_inline_cache_t *ics;         /* array of ICs, indexed by bytecode PC */
    uint32_t            count;       /* size of the array (= bytecode length) */
} vtx_method_ic_storage_t;

/**
 * The interpreter state.
 */
typedef struct {
    /* Frame stack (pre-allocated memory for frames) */
    vtx_frame_stack_t   frame_stack;

    /* Current execution frame */
    vtx_frame_t        *current_frame;

    /* Profiling */
    vtx_profiler_t      profiler;

    /* Type feedback */
    vtx_type_feedback_t type_feedback;

    /* Type system and GC references */
    vtx_type_system_t  *type_system;
    vtx_gc_t           *gc;

    /* Compilation context: when non-NULL, the interpreter can
     * request JIT compilation for hot methods via
     * vtx_request_compilation(). This is the wiring that
     * connects the interpreter's hot-code detection to the
     * JIT compilation thread pool. */
    vtx_compile_context_t *compile_ctx;

    /* Dispatch table for computed goto (indexed by opcode) */
    void              **dispatch_table;

    /* Running state */
    bool                running;

    /* Deopt trigger: set by safepoint check when an invalidation
     * affects the current method. The dispatch loop checks this
     * flag at every backward branch and triggers deoptimization
     * if set. */
    bool                deopt_pending;
    uint32_t            deopt_method_id;   /* method_id that was invalidated */

    /* Deopt resume PC: when deopt occurs, the interpreter resumes
     * execution at this bytecode PC in the deoptimized frame.
     * Set by vtx_deopt_runtime_transition before the interpreter
     * re-enters the dispatch loop. */
    uint32_t            deopt_resume_pc;
    bool                deopt_resume_pending;

    /* Pending exception (VTX_VALUE_UNDEFINED if none) */
    vtx_value_t         exception;

    /* Method IC storage: growable array of IC arrays per method */
    vtx_method_ic_storage_t *method_ics;
    uint32_t                 method_ic_count;
    uint32_t                 method_ic_capacity;
} vtx_interp_t;

/* ========================================================================== */
/* Interpreter lifecycle                                                       */
/* ========================================================================== */

/**
 * Initialize the interpreter.
 * Builds the dispatch table, initializes the frame stack,
 * profiler, and type feedback collector.
 * Returns 0 on success, -1 on failure.
 */
int vtx_interp_init(vtx_interp_t *interp, vtx_type_system_t *ts, vtx_gc_t *gc);

/**
 * Destroy the interpreter and release all resources.
 */
void vtx_interp_destroy(vtx_interp_t *interp);

/**
 * Set the deopt resume PC for a frame. Called by the deopt runtime
 * transition after reconstructing the interpreter frame. The dispatch
 * loop checks deopt_resume_pending and resumes at deopt_resume_pc.
 */
void vtx_interp_set_deopt_pc(vtx_frame_t *frame, uint32_t pc);

/**
 * Execute a method with the given arguments.
 *
 * Creates a new frame for the method, copies arguments into locals,
 * and runs the dispatch loop until the method returns.
 *
 * Returns the method's return value (VTX_VALUE_UNDEFINED for void methods).
 */
vtx_value_t vtx_interp_run(vtx_interp_t *interp,
                            const vtx_method_desc_t *method,
                            vtx_value_t *args,
                            uint32_t arg_count);

/**
 * Get the inline cache for a specific call site in a method.
 * If the IC doesn't exist yet, it is created.
 * Returns the IC, or NULL on failure.
 */
vtx_inline_cache_t *vtx_interp_get_ic(vtx_interp_t *interp,
                                       const vtx_method_desc_t *method,
                                       uint32_t call_pc);

/**
 * Handle an uncaught exception: unwind all frames and return
 * the exception value.
 */
vtx_value_t vtx_interp_handle_uncaught(vtx_interp_t *interp,
                                        vtx_value_t exception);

/**
 * Set the compilation context on the interpreter.
 * When set, the interpreter will request JIT compilation for hot
 * methods via vtx_request_compilation(). This is the wiring that
 * connects the interpreter's hot-code detection to the JIT pipeline.
 *
 * Must be called after vtx_interp_init() and before vtx_interp_run().
 * The ctx pointer is stored but NOT owned by the interpreter.
 */
void vtx_interp_set_compile_ctx(vtx_interp_t *interp,
                                 vtx_compile_context_t *ctx);

#endif /* VORTEX_DISPATCH_H */
