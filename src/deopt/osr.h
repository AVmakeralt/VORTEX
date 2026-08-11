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

#ifndef VORTEX_DEOPT_OSR_H
#define VORTEX_DEOPT_OSR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/node.h"
#include "deopt/frame_state.h"
#include "deopt/side_table.h"
#include "deopt/types.h"
#include "codecache/types.h"

/* Forward declaration: vtx_method_registry_t is defined in codecache/install.h.
 * Forward-declared here so vtx_osr_up can take it as a parameter without
 * pulling install.h into every translation unit that includes osr.h. */
struct vtx_method_registry;
typedef struct vtx_method_registry vtx_method_registry_t;

/* Forward declaration: vtx_gc_t is defined in runtime/gc.h. Forward-declared
 * here so vtx_osr_up can take it as a parameter for the OSR-12 pre-asm
 * safepoint poll without pulling gc.h into every translation unit that
 * includes osr.h. The struct tag and typedef share the name vtx_gc_t,
 * which is legal in C (separate namespaces). */
struct vtx_gc_t;
typedef struct vtx_gc_t vtx_gc_t;

#ifndef VTX_CATCH_NONE
#define VTX_CATCH_NONE UINT32_MAX
#endif

/**
 * VORTEX On-Stack Replacement (OSR)
 *
 * Two directions of on-stack replacement:
 *
 * OSR Up (Interpreter → Compiled Code):
 *   When a hot loop is detected in the interpreter, the interpreter frame
 *   is converted to a JIT-compiled frame at the loop header. The compiled
 *   code continues execution from that point.
 *
 * OSR Down (Compiled Code → Interpreter):
 *   When a guard fails in compiled code, the compiled frame is converted
 *   back to an interpreter frame. The interpreter resumes from the guard's
 *   bytecode PC with the reconstructed frame state.
 *
 * Both directions require careful handling of the frame layout, local
 * variables, operand stack, and monitor state.
 */

/* ========================================================================== */
/* Interpreter frame (enhanced)                                                */
/* ========================================================================== */

/**
 * Frame kind: distinguishes the origin of a frame for stack walking.
 * The canonical definition is in deopt/stack_walk.h which has the full
 * set of frame kinds. We include it here to avoid a duplicate typedef.
 */
#include "deopt/stack_walk.h"

/**
 * Monitor state entry: tracks which objects are locked by this frame.
 * Each entry records the local variable index holding the locked object
 * and the object value itself (resolved at deopt time).
 */
typedef struct {
    uint32_t     local_index;     /* local variable holding the locked object */
    vtx_value_t  object;          /* the locked object value (heap pointer) */
} vtx_osr_monitor_entry_t;

/**
 * Enhanced representation of an interpreter frame for OSR transitions.
 * Matches the real interpreter frame from src/interp/frame.h more closely,
 * including monitor state, exception handlers, return address, and frame kind.
 */
typedef struct {
    uint32_t         method_id;      /* method being executed */
    uint32_t         bytecode_pc;    /* current bytecode PC */
    vtx_value_t     *locals;         /* local variable array */
    uint32_t         local_count;    /* number of locals */
    vtx_value_t     *stack;          /* operand stack */
    uint32_t         stack_top;      /* current stack depth */
    uint32_t         stack_capacity; /* max stack depth */
    /* DEOPT-005 fix: changed from vtx_frame_state_t * to void * to avoid
     * type-punning UB. The caller field is overloaded: it stores either
     * a vtx_frame_state_t * (when built from a FrameState chain) or a
     * vtx_interp_frame_t * (when built from an interpreter frame chain).
     * Using void * + explicit casts at the use sites is well-defined;
     * type-punning through incompatible pointer types is UB under
     * C17 strict aliasing (6.5p7) and is flagged by -fsanitize=undefined. */
    void             *caller;       /* caller's frame (vtx_frame_state_t * or vtx_interp_frame_t *) */
    /* OSR-32 fix: removed `osr_active` flag — was set by vtx_osr_up but
     * never read by any code (GC, stack walker, etc.). Dead-flag removal. */

    /* --- Enhanced fields (matching src/interp/frame.h) --- */

    /* Monitor state: which objects are locked in this frame.
     * During OSR down, monitors must be re-acquired in the interpreter
     * frame after deoptimization. */
    vtx_osr_monitor_entry_t *monitors;       /* array of locked monitors */
    uint32_t                 monitor_count;  /* number of active monitors */
    uint32_t                 monitor_capacity; /* allocated capacity */

    /* Exception handler: the active catch handler for this frame.
     * VTX_CATCH_NONE (UINT32_MAX) means no handler is active. */
    uint32_t         catch_handler_pc;  /* PC of current catch handler */

    /* Return address: bytecode PC in the caller to resume after return.
     * Used for stack walking to reconstruct the full call chain. */
    uint32_t         return_pc;     /* PC to resume in caller after return */

    /* Frame kind: distinguishes interpreter, JIT, and native frames
     * during stack walking. */
    vtx_frame_kind_t frame_kind;    /* interpreter, JIT, or native */
} vtx_interp_frame_t;

/* ========================================================================== */
/* Compiled code descriptor (defined in codecache/types.h)                    */
/* ========================================================================== */

/* ========================================================================== */
/* OSR deopt context                                                          */
/* ========================================================================== */

/**
 * Information provided when a guard fails and OSR down is needed.
 * This is populated by the deopt stub before calling vtx_osr_down.
 * This is distinct from vtx_deopt_info_t (which is the static per-method
 * deopt metadata) — this struct contains the dynamic runtime state
 * at the point of deoptimization.
 *
 * OSR-13 fix: `register_map` is now `const vtx_reg_map_entry_t *` (the
 * side table's native format — an array of {register_number, node_id}
 * entries). Previously this was typed `vtx_value_t *` and assumed a
 * flat [count, (node_id, value) pairs...] layout that did not exist
 * anywhere in the codebase. The format mismatch meant the resolver
 * always read garbage and returned VTX_VALUE_UNDEFINED, defeating
 * the register-map path of OSR-down reconstruction.
 */
typedef struct {
    uint32_t             method_id;       /* method where guard failed */
    uint32_t             native_pc;       /* native PC of the guard */
    vtx_frame_state_t   *frame_state;     /* FrameState at the deopt point */
    vtx_side_table_t    *side_table;      /* side table for the compiled code */
    void                *frame_pointer;   /* frame pointer of the compiled frame */
    const vtx_reg_map_entry_t *register_map; /* OSR-13: side-table native format */
    uint32_t             register_count;  /* number of entries in register_map */
} vtx_osr_deopt_context_t;

/* ========================================================================== */
/* OSR Up: Interpreter → Compiled Code                                        */
/* ========================================================================== */

/**
 * Perform OSR up: replace the interpreter frame with a compiled frame
 * and transfer control to the compiled code.
 *
 * Steps:
 *   1. Verify the compiled code exists for the method at the loop header.
 *   2. Build a FrameState from the interpreter's current local/stack state.
 *   3. Set up the JIT frame: copy locals and stack into the JIT frame layout.
 *   4. Patch the return address to point into the compiled code.
 *   5. Transfer execution to the compiled code's entry point.
 *
 * OSR-11 fix: `registry` is required so that vtx_osr_up can re-check
 * the registry's current `cm` for the method immediately before the asm
 * jump. If the version has changed (e.g., due to concurrent invalidation
 * or retrace), the function returns instead of jumping to freed memory.
 *
 * OSR-3 fix: this function returns `void`, NOT `bool`. On a successful
 * OSR transition the inline-asm trampoline jumps directly to the JIT
 * entry point and never returns to C — the JIT method's NaN-boxed
 * return value propagates back to the caller of vtx_interp_run through
 * RAX exactly as if the JIT had been called normally. Returning `bool`
 * was fundamentally broken because, after a successful transition,
 * the value left in RAX is the JIT method's return value (which can
 * be SMI 0 / undefined / null and thus be misread as `false`), causing
 * the dispatch loop to set jit_reenter_pending and re-execute the
 * entire method (side effects run twice).
 *
 * On any failure (NULL inputs, method/PC mismatch, side-table miss,
 * inlined-code entry, stack-depth mismatch, version mismatch), the
 * function returns normally so the caller can fall back to whole-method
 * re-enter. The caller detects failure simply by the function
 * returning — if vtx_osr_up returns at all, OSR failed.
 *
 * OSR-12 fix: `gc` is required so that vtx_osr_up can poll for a
 * pending safepoint immediately before the asm jump. The dispatch
 * loop's last safepoint check happened at the backward branch — a
 * GC may have been requested between then and the asm jump, and the
 * JIT code's own safepoint poll may be far away if the OSR entry is
 * at a loop header with a long body.
 *
 * @param interp         Current interpreter frame
 * @param method_id       Method being executed
 * @param compiled_code  Compiled code descriptor for the method
 * @param loop_header_pc Bytecode PC of the loop header (OSR entry point)
 * @param registry        Method registry (for OSR-11 version re-check)
 * @param gc              GC handle (for OSR-12 pre-asm safepoint poll)
 */
void vtx_osr_up(vtx_interp_frame_t *interp,
                 uint32_t method_id,
                 const vtx_compiled_code_t *compiled_code,
                 uint32_t loop_header_pc,
                 struct vtx_method_registry *registry,
                 vtx_gc_t *gc);

/* ========================================================================== */
/* OSR Down: Compiled Code → Interpreter                                      */
/* ========================================================================== */

/**
 * Perform OSR down: replace the compiled frame with an interpreter frame
 * and resume execution in the interpreter.
 *
 * Steps:
 *   1. Look up the FrameState from the side table using the native PC.
 *   2. Reconstruct the interpreter operand stack from the FrameState:
 *      - For each NodeID in the FrameState, evaluate the node to get a value.
 *      - Map NodeIDs to values using the register map and frame state.
 *   3. Reconstruct the interpreter local variables similarly.
 *   4. Handle monitors: relock any monitors that were held in compiled code.
 *   5. Handle exception handlers: set up the active handler from FrameState.
 *   6. Walk the caller chain and reconstruct caller interpreter frames.
 *   7. Transfer execution to the interpreter dispatch loop at the deopt PC.
 *
 * @param interp   Interpreter state to resume (output: populated with frame)
 * @param deopt_info Information about the deoptimization point
 * @return The interpreter resume frame, or NULL on failure
 */
vtx_interp_frame_t *vtx_osr_down(vtx_interp_frame_t *interp,
                                   const vtx_osr_deopt_context_t *deopt_ctx);

/* ========================================================================== */
/* Internal helpers (exposed for testing)                                     */
/* ========================================================================== */

/**
 * Build an interpreter frame from a FrameState and a value resolution function.
 *
 * @param fs           The FrameState to convert
 * @param node_to_value Function that maps a NodeID to its current vtx_value_t
 * @param context      Opaque context passed to node_to_value
 * @return A newly allocated interpreter frame, or NULL on failure
 */
vtx_interp_frame_t *vtx_osr_build_interp_frame(
    const vtx_frame_state_t *fs,
    vtx_value_t (*node_to_value)(vtx_nodeid_t, void *),
    void *context);

/**
 * Resolve a NodeID to its value at deopt time.
 *
 * OSR-13 fix: the register_map parameter is now `const vtx_reg_map_entry_t *`
 * (the side table's native format — an array of {register_number, node_id}
 * entries), and `register_count` is the number of entries (NOT a "total
 * vtx_value_t elements" count as the old docs claimed). The prior signature
 * assumed a fabricated [count, (node_id, value) pairs...] layout that did
 * not exist anywhere; the resolver always read garbage from real side-table
 * memory and returned VTX_VALUE_UNDEFINED.
 *
 * OSR-9 fix: when a NodeID is NOT in the register map (the common case
 * for VTX_OP_Constant, VTX_OP_Parameter, and spilled nodes), this now
 * falls back to alternative resolution paths:
 *   - VTX_OP_Constant: read the constant value from the IR node table
 *     (passed via `node_table`). Returns the boxed constant value.
 *   - VTX_OP_Parameter: read the parameter's value from the caller's
 *     interpreter locals (passed via `locals` / `local_count`). The
 *     parameter's index is the node's `local_index` field.
 *   - Spilled nodes (any other opcode not in the register map): return
 *     VTX_VALUE_UNDEFINED — the caller is responsible for reading the
 *     spill slot directly from the JIT frame. (Without a T2 register
 *     save area, we cannot resolve spilled values from here.)
 *
 * @param node_id        The NodeID to resolve
 * @param register_map   Side-table register map (OSR-13: native format)
 * @param register_count Number of entries in register_map
 * @param node_table     IR node table (OSR-9: for VTX_OP_Constant fallback)
 * @param locals         Interpreter locals (OSR-9: for VTX_OP_Parameter fallback)
 * @param local_count    Number of locals in the `locals` array
 * @return The resolved value, or VTX_VALUE_UNDEFINED if not resolvable
 */
vtx_value_t vtx_osr_resolve_node(vtx_nodeid_t node_id,
                                   const vtx_reg_map_entry_t *register_map,
                                   uint32_t register_count,
                                   const vtx_node_table_t *node_table,
                                   const vtx_value_t *locals,
                                   uint32_t local_count);

#endif /* VORTEX_DEOPT_OSR_H */
