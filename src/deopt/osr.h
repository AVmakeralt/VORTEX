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
/* Interpreter frame (simplified)                                             */
/* ========================================================================== */

/**
 * Simplified representation of an interpreter frame for OSR transitions.
 * The actual interpreter frame layout is defined in src/interp/frame.h,
 * but we need a portable representation here for the OSR transition code.
 */
typedef struct {
    uint32_t         method_id;      /* method being executed */
    uint32_t         bytecode_pc;    /* current bytecode PC */
    vtx_value_t     *locals;         /* local variable array */
    uint32_t         local_count;    /* number of locals */
    vtx_value_t     *stack;          /* operand stack */
    uint32_t         stack_top;      /* current stack depth */
    uint32_t         stack_capacity; /* max stack depth */
    vtx_frame_state_t *caller;       /* caller's interpreter frame (or NULL) */
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
 */
typedef struct {
    uint32_t             method_id;       /* method where guard failed */
    uint32_t             native_pc;       /* native PC of the guard */
    vtx_frame_state_t   *frame_state;     /* FrameState at the deopt point */
    vtx_side_table_t    *side_table;      /* side table for the compiled code */
    void                *frame_pointer;   /* frame pointer of the compiled frame */
    vtx_value_t         *register_map;    /* values of live registers at deopt */
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
 * @param interp         Current interpreter frame
 * @param method_id      Method being executed
 * @param compiled_code  Compiled code descriptor for the method
 * @param loop_header_pc Bytecode PC of the loop header (OSR entry point)
 * @return true if OSR up was successful, false if not
 */
bool vtx_osr_up(vtx_interp_frame_t *interp,
                 uint32_t method_id,
                 const vtx_compiled_code_t *compiled_code,
                 uint32_t loop_header_pc);

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
 * Uses the register map and frame state to look up the value.
 *
 * @param node_id      The NodeID to resolve
 * @param register_map Array of values indexed by NodeID (from side table)
 * @param map_size     Size of the register map array
 * @return The resolved value, or VTX_VALUE_UNDEFINED if not found
 */
vtx_value_t vtx_osr_resolve_node(vtx_nodeid_t node_id,
                                   const vtx_value_t *register_map,
                                   uint32_t map_size);

#endif /* VORTEX_DEOPT_OSR_H */
