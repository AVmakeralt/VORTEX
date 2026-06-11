#ifndef VORTEX_DEOPT_FRAME_STATE_H
#define VORTEX_DEOPT_FRAME_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/node.h"
#include "runtime/type_system.h"
#include "runtime/arena.h"

/**
 * VORTEX Deoptimization Frame State
 *
 * A FrameState captures the complete execution state at a deoptimization point:
 * the bytecode PC, the values of local variables and the operand stack (as
 * NodeIDs in the Sea-of-Nodes IR), the monitor lock state, and the exception
 * handler state.
 *
 * FrameStates are chained: each deopt point knows the FrameState of its caller,
 * forming a linked list that represents the entire interpreter call stack at
 * that deopt point. When deoptimization occurs, the chain is walked to
 * reconstruct all interpreter frames.
 *
 * FrameState nodes in the SoN graph (VTX_OP_FrameState) reference these
 * structures at deopt time. The mapping from native PC to FrameState is stored
 * in the side table (side_table.h).
 */

/* ========================================================================== */
/* Monitor state                                                              */
/* ========================================================================== */

/**
 * Represents the state of a single monitor (lock) held at a deopt point.
 * The monitor_object is a NodeID that evaluates to the locked object.
 */
typedef struct {
    vtx_nodeid_t monitor_object; /* NodeID of the locked object */
} vtx_monitor_state_t;

/* ========================================================================== */
/* Exception handler state                                                    */
/* ========================================================================== */

/**
 * Represents the active exception handler at a deopt point.
 * If handler_pc != VTX_DEOPT_NO_HANDLER, an exception handler is active
 * and covers this PC.
 */
#define VTX_DEOPT_NO_HANDLER 0xFFFFFFFFu

typedef struct {
    uint32_t handler_pc;      /* bytecode PC of the catch handler, or NO_HANDLER */
    vtx_typeid_t catch_type;  /* type of exception caught (0 = catch-all) */
} vtx_exception_state_t;

/* ========================================================================== */
/* Frame state                                                                */
/* ========================================================================== */

/**
 * Complete execution state at a single deopt point.
 *
 * locals and stack are arrays of NodeIDs. Each NodeID represents the
 * SoN node that produces the value in that slot at the deopt point.
 * VTX_NODEID_INVALID means the slot is undefined/unused.
 *
 * The structure is allocated from an arena (no individual free).
 */
typedef struct vtx_frame_state vtx_frame_state_t;

struct vtx_frame_state {
    uint32_t            bytecode_pc;     /* bytecode PC at deopt point */
    uint32_t            method_id;       /* method identifier */

    /* Local variable state */
    vtx_nodeid_t       *locals;          /* array of local variable NodeIDs */
    uint32_t            local_count;     /* number of local variable slots */

    /* Operand stack state */
    vtx_nodeid_t       *stack;           /* array of operand stack NodeIDs */
    uint32_t            stack_count;     /* number of values on operand stack */

    /* Monitor state */
    vtx_monitor_state_t *monitors;       /* array of active monitors */
    uint32_t             monitor_count;  /* number of active monitors */

    /* Exception handler state */
    vtx_exception_state_t exception;     /* active exception handler */

    /* Caller chain: link to the FrameState of the calling method.
     * NULL if this is the outermost frame. */
    vtx_frame_state_t  *caller;

    /* For inlined methods: the relock flag indicates that monitors
     * need to be reacquired during deopt materialization. */
    bool                relock_needed;
};

/* ========================================================================== */
/* Creation                                                                   */
/* ========================================================================== */

/**
 * Create a FrameState allocated from the given arena.
 * Allocates locals and stack arrays from the arena.
 * Returns NULL on allocation failure.
 *
 * @param arena      Arena to allocate from
 * @param pc         Bytecode PC at the deopt point
 * @param method_id  Method identifier
 * @param local_count Number of local variable slots
 * @param stack_count Number of operand stack slots
 */
vtx_frame_state_t *vtx_frame_state_create(vtx_arena_t *arena,
                                           uint32_t pc,
                                           uint32_t method_id,
                                           uint32_t local_count,
                                           uint32_t stack_count);

/**
 * Create a chain of FrameStates representing a full call stack.
 * The frames array is ordered from innermost (callee) to outermost (caller).
 * Each frame's `caller` pointer is set to the next frame in the array.
 *
 * @param arena       Arena to allocate from
 * @param frame_pcs   Array of bytecode PCs (innermost first)
 * @param method_ids  Array of method IDs (innermost first)
 * @param local_counts  Array of local counts (innermost first)
 * @param stack_counts   Array of stack counts (innermost first)
 * @param depth       Number of frames in the chain
 * @return The innermost FrameState (head of the chain), or NULL on failure
 */
vtx_frame_state_t *vtx_frame_state_chain_create(vtx_arena_t *arena,
                                                  const uint32_t *frame_pcs,
                                                  const uint32_t *method_ids,
                                                  const uint32_t *local_counts,
                                                  const uint32_t *stack_counts,
                                                  uint32_t depth);

/* ========================================================================== */
/* Accessors                                                                  */
/* ========================================================================== */

/**
 * Set the local variable NodeID at the given index.
 */
static inline void vtx_frame_state_set_local(vtx_frame_state_t *fs,
                                              uint32_t index,
                                              vtx_nodeid_t node_id)
{
    VTX_ASSERT(fs != NULL, "frame state must not be NULL");
    VTX_ASSERT(index < fs->local_count, "local index out of bounds");
    fs->locals[index] = node_id;
}

/**
 * Get the local variable NodeID at the given index.
 */
static inline vtx_nodeid_t vtx_frame_state_get_local(const vtx_frame_state_t *fs,
                                                       uint32_t index)
{
    VTX_ASSERT(fs != NULL, "frame state must not be NULL");
    VTX_ASSERT(index < fs->local_count, "local index out of bounds");
    return fs->locals[index];
}

/**
 * Set the operand stack NodeID at the given index.
 */
static inline void vtx_frame_state_set_stack(vtx_frame_state_t *fs,
                                              uint32_t index,
                                              vtx_nodeid_t node_id)
{
    VTX_ASSERT(fs != NULL, "frame state must not be NULL");
    VTX_ASSERT(index < fs->stack_count, "stack index out of bounds");
    fs->stack[index] = node_id;
}

/**
 * Get the operand stack NodeID at the given index.
 */
static inline vtx_nodeid_t vtx_frame_state_get_stack(const vtx_frame_state_t *fs,
                                                       uint32_t index)
{
    VTX_ASSERT(fs != NULL, "frame state must not be NULL");
    VTX_ASSERT(index < fs->stack_count, "stack index out of bounds");
    return fs->stack[index];
}

/**
 * Set the exception handler state.
 */
static inline void vtx_frame_state_set_exception(vtx_frame_state_t *fs,
                                                   uint32_t handler_pc,
                                                   vtx_typeid_t catch_type)
{
    VTX_ASSERT(fs != NULL, "frame state must not be NULL");
    fs->exception.handler_pc = handler_pc;
    fs->exception.catch_type = catch_type;
}

/**
 * Walk the caller chain and return the depth (number of frames).
 */
uint32_t vtx_frame_state_chain_depth(const vtx_frame_state_t *fs);

/**
 * Find the FrameState for the nth caller (0 = self, 1 = caller, etc.).
 * Returns NULL if n exceeds the chain depth.
 */
const vtx_frame_state_t *vtx_frame_state_nth_caller(const vtx_frame_state_t *fs,
                                                       uint32_t n);

#endif /* VORTEX_DEOPT_FRAME_STATE_H */
