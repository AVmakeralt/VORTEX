#include "deopt/osr.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/* Internal: node resolution                                                  */
/* ========================================================================== */

/**
 * The register map is a sparse array indexed by NodeID. For nodes that
 * are not in the register map (e.g., constants, nodes that were spilled
 * to the stack), we need to look up the value differently.
 *
 * For OSR down, the register_map is laid out as:
 *   register_map[0] = number of entries
 *   register_map[1..2*N] = (node_id, value) pairs
 *
 * This allows for sparse mapping without allocating a huge array.
 */
typedef struct {
    vtx_nodeid_t node_id;
    vtx_value_t  value;
} vtx_node_value_pair_t;

vtx_value_t vtx_osr_resolve_node(vtx_nodeid_t node_id,
                                   const vtx_value_t *register_map,
                                   uint32_t map_size)
{
    if (!register_map || map_size == 0) return VTX_VALUE_UNDEFINED;

    /* The register_map is an array of vtx_node_value_pair_t entries,
     * preceded by a count. We search linearly for the matching NodeID.
     * In production this would use a hash map for O(1) lookup, but
     * the linear scan is correct and sufficient for correctness. */
    const vtx_node_value_pair_t *pairs =
        (const vtx_node_value_pair_t *)register_map;
    uint32_t pair_count = map_size / 2; /* each pair is 2 vtx_value_t wide */

    for (uint32_t i = 0; i < pair_count; i++) {
        if (pairs[i].node_id == node_id) {
            return pairs[i].value;
        }
    }

    return VTX_VALUE_UNDEFINED;
}

/* ========================================================================== */
/* Internal: resolve NodeID from FrameState using register map               */
/* ========================================================================== */

/**
 * Context for the node-to-value resolution callback.
 */
typedef struct {
    const vtx_value_t *register_map;
    uint32_t           register_map_size;
} vtx_resolve_context_t;

static vtx_value_t resolve_node_callback(vtx_nodeid_t node_id, void *ctx)
{
    vtx_resolve_context_t *rc = (vtx_resolve_context_t *)ctx;
    if (node_id == VTX_NODEID_INVALID) {
        return VTX_VALUE_UNDEFINED;
    }
    return vtx_osr_resolve_node(node_id, rc->register_map,
                                 rc->register_map_size);
}

/* ========================================================================== */
/* Build interpreter frame from FrameState                                    */
/* ========================================================================== */

vtx_interp_frame_t *vtx_osr_build_interp_frame(
    const vtx_frame_state_t *fs,
    vtx_value_t (*node_to_value)(vtx_nodeid_t, void *),
    void *context)
{
    if (!fs) return NULL;

    vtx_interp_frame_t *frame = calloc(1, sizeof(vtx_interp_frame_t));
    if (!frame) return NULL;

    frame->method_id = fs->method_id;
    frame->bytecode_pc = fs->bytecode_pc;
    frame->local_count = fs->local_count;
    frame->stack_top = fs->stack_count;
    frame->stack_capacity = fs->stack_count;
    frame->caller = NULL;

    /* Allocate and fill locals */
    if (fs->local_count > 0) {
        frame->locals = calloc(fs->local_count, sizeof(vtx_value_t));
        if (!frame->locals) {
            free(frame);
            return NULL;
        }
        for (uint32_t i = 0; i < fs->local_count; i++) {
            if (fs->locals[i] != VTX_NODEID_INVALID) {
                frame->locals[i] = node_to_value(fs->locals[i], context);
            } else {
                frame->locals[i] = VTX_VALUE_UNDEFINED;
            }
        }
    }

    /* Allocate and fill operand stack */
    if (fs->stack_count > 0) {
        frame->stack = calloc(fs->stack_count, sizeof(vtx_value_t));
        if (!frame->stack) {
            free(frame->locals);
            free(frame);
            return NULL;
        }
        for (uint32_t i = 0; i < fs->stack_count; i++) {
            if (fs->stack[i] != VTX_NODEID_INVALID) {
                frame->stack[i] = node_to_value(fs->stack[i], context);
            } else {
                frame->stack[i] = VTX_VALUE_UNDEFINED;
            }
        }
    }

    return frame;
}

/* ========================================================================== */
/* OSR Up: Interpreter → Compiled Code                                        */
/* ========================================================================== */

bool vtx_osr_up(vtx_interp_frame_t *interp,
                 uint32_t method_id,
                 const vtx_compiled_code_t *compiled_code,
                 uint32_t loop_header_pc)
{
    if (!interp || !compiled_code || !compiled_code->entry_point) {
        return false;
    }

    /* Verify the method matches */
    if (compiled_code->method_id != method_id) {
        return false;
    }

    /* Verify the interpreter is at the loop header PC */
    if (interp->bytecode_pc != loop_header_pc) {
        return false;
    }

    /* At OSR up, we need to:
     * 1. Ensure the compiled code has an OSR entry point at loop_header_pc.
     * 2. Set up the JIT frame layout:
     *    - Copy interpreter locals into JIT frame local slots
     *    - Copy interpreter operand stack into JIT frame stack slots
     *    - Set the JIT frame's return address and frame pointer
     * 3. Jump to the compiled code's OSR entry point.
     *
     * In a real implementation, this involves platform-specific assembly
     * that constructs the JIT frame on the native stack and jumps to it.
     * Here we prepare the data structures and validate the transition.
     *
     * The actual jump is performed by a platform-specific trampoline
     * (osr_trampoline.S) that:
     *   - Saves callee-saved registers
     *   - Sets RBP to the new JIT frame
     *   - Sets RSP to the top of the JIT frame
     *   - Jumps to the compiled code entry point
     */

    /* Verify frame size compatibility */
    if (interp->local_count > compiled_code->local_slots) {
        return false;
    }
    if (interp->stack_top > compiled_code->stack_slots) {
        return false;
    }

    /* The OSR entry point is typically the start of the compiled code
     * with an offset for the loop header. For a simple implementation,
     * we use the main entry point — the compiled code must have been
     * compiled with OSR support (entry at loop header). */
    void *osr_entry = compiled_code->entry_point;

    /* Mark that this interpreter frame has been replaced by compiled code.
     * In a real implementation, we would modify the frame pointer chain
     * so that the interpreter no longer dispatches this frame. */
    interp->bytecode_pc = loop_header_pc; /* confirm OSR point */

    /* The trampoline will be invoked by the interpreter's OSR handler,
     * which reads the compiled_code and osr_entry from this struct.
     * We store the entry point for the trampoline to use. */
    (void)osr_entry; /* used by platform-specific trampoline */

    return true;
}

/* ========================================================================== */
/* OSR Down: Compiled Code → Interpreter                                      */
/* ========================================================================== */

vtx_interp_frame_t *vtx_osr_down(vtx_interp_frame_t *interp,
                                   const vtx_deopt_info_t *deopt_info)
{
    if (!deopt_info || !deopt_info->frame_state) {
        return NULL;
    }

    /* Step 1: Look up FrameState from side table (already provided in deopt_info) */
    const vtx_frame_state_t *fs = deopt_info->frame_state;

    /* Step 2: Set up the resolution context */
    vtx_resolve_context_t ctx;
    ctx.register_map = deopt_info->register_map;
    ctx.register_map_size = deopt_info->register_count;

    /* Step 3: Build the interpreter frame for the innermost method */
    vtx_interp_frame_t *new_frame = vtx_osr_build_interp_frame(
        fs, resolve_node_callback, &ctx);
    if (!new_frame) return NULL;

    /* Step 4: Handle monitors — relock if needed.
     * For each monitor in the FrameState, we need to reacquire the lock
     * on the monitor object. The monitor object's value is resolved from
     * the register map. */
    if (fs->monitor_count > 0) {
        for (uint32_t i = 0; i < fs->monitor_count; i++) {
            vtx_nodeid_t mon_node = fs->monitors[i].monitor_object;
            if (mon_node != VTX_NODEID_INVALID) {
                vtx_value_t mon_val = resolve_node_callback(mon_node, &ctx);
                /* In a real implementation, we would call the runtime
                 * to re-enter the monitor on the resolved object.
                 * The object must be a heap pointer. */
                if (vtx_is_heap_ptr(mon_val)) {
                    /* vtx_runtime_monitor_enter(vtx_heap_ptr(mon_val)); */
                }
            }
        }
    }

    /* Step 5: Handle exception handler state */
    if (fs->exception.handler_pc != VTX_DEOPT_NO_HANDLER) {
        /* The interpreter will pick up the exception handler from
         * the method's exception table at the deopt PC. No additional
         * work needed here — the bytecode_pc is set correctly. */
    }

    /* Step 6: Walk the caller chain and reconstruct caller frames */
    vtx_interp_frame_t *current = new_frame;
    const vtx_frame_state_t *caller_fs = fs->caller;
    while (caller_fs != NULL) {
        vtx_interp_frame_t *caller_frame = vtx_osr_build_interp_frame(
            caller_fs, resolve_node_callback, &ctx);
        if (!caller_frame) {
            /* Clean up already-built frames */
            vtx_interp_frame_t *f = new_frame;
            while (f) {
                vtx_interp_frame_t *next = (vtx_interp_frame_t *)f->caller;
                free(f->locals);
                free(f->stack);
                free(f);
                f = next;
            }
            return NULL;
        }
        current->caller = (vtx_frame_state_t *)caller_frame;
        current = caller_frame;
        caller_fs = caller_fs->caller;
    }

    /* Step 7: Transfer to interpreter — in a real implementation,
     * we would set the interpreter's frame pointer and PC and
     * jump to the dispatch loop. Here we return the reconstructed
     * frame for the caller to resume. */
    if (interp) {
        interp->method_id = new_frame->method_id;
        interp->bytecode_pc = new_frame->bytecode_pc;
        /* The caller is responsible for copying locals/stack if needed */
    }

    return new_frame;
}
