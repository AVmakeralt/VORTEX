#ifndef VORTEX_FRAME_H
#define VORTEX_FRAME_H

#include "vortex_config.h"
#include <stdint.h>
#include <stdbool.h>
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"

/**
 * VORTEX Interpreter Stack Frame
 *
 * Each frame contains:
 *   - Local variables array (fixed size from method max_locals)
 *   - Operand stack (fixed size from method max_stack)
 *   - Return address (bytecode PC in the caller)
 *   - Caller frame pointer
 *   - Method descriptor pointer
 *   - Monitored type state for deopt (array of typeids per local)
 *
 * Frame allocation uses a pre-allocated 256KB block (frame stack).
 * When a block overflows, a new block is allocated and chained.
 * Frames are carved out sequentially from the current block.
 * No malloc is used in the dispatch loop.
 */

/* Frame stack block size: 256 KB */
#define VTX_FRAME_STACK_BLOCK_SIZE (256 * 1024)

/* Maximum operand stack depth per frame (safety limit) */
#define VTX_FRAME_MAX_STACK_DEPTH 1024

/* Maximum local variable count per frame (safety limit) */
#define VTX_FRAME_MAX_LOCALS 1024

/* ========================================================================== */
/* Interpreter stack frame                                                     */
/* ========================================================================== */

typedef struct vtx_frame vtx_frame_t;

struct vtx_frame {
    vtx_value_t        *locals;           /* local variable array */
    vtx_value_t        *operand_stack;    /* operand stack array */
    int                 stack_top;        /* index of next free slot on operand stack */
    int                 stack_capacity;   /* max operand stack depth */
    uint32_t            return_pc;        /* PC to resume in caller after return */
    vtx_frame_t        *caller;           /* caller's frame (for frame chain) */
    const vtx_method_desc_t *method;      /* method being executed */
    vtx_bytecode_t     *bytecode;         /* bytecode for this method (convenience) */
    uint32_t           *monitored_types;  /* typeid per local for deopt type state */
    uint32_t            locals_count;     /* number of local variable slots */
    uint32_t            catch_handler_pc; /* PC of current catch handler (VTX_CATCH_NONE if none) */
    vtx_value_t         exception;        /* pending exception (VTX_VALUE_UNDEFINED if none) */
};

/* Sentinel value for "no catch handler active" */
#define VTX_CATCH_NONE UINT32_MAX

/* ========================================================================== */
/* Frame stack block                                                           */
/* ========================================================================== */

/**
 * A single 256KB block in the frame stack. Blocks are chained together
 * to allow the frame stack to grow beyond a single block.
 */
typedef struct vtx_frame_block vtx_frame_block_t;

struct vtx_frame_block {
    uint8_t            *memory;       /* allocated block memory */
    size_t              size;         /* total size of this block */
    size_t              used;         /* bytes currently in use */
    vtx_frame_block_t  *prev;        /* previous block (for unwinding) */
};

/* ========================================================================== */
/* Frame stack allocator                                                       */
/* ========================================================================== */

/**
 * The frame stack is a chain of pre-allocated memory blocks from which
 * frames are carved out sequentially. This avoids malloc in the dispatch loop.
 * When a block is full, a new block is allocated and becomes the current block.
 * When the top frame is destroyed, if the current block is empty, it is
 * freed and the previous block becomes current.
 */
typedef struct {
    vtx_frame_block_t  *current;      /* current allocation block */
    size_t              block_size;   /* size of each block (VTX_FRAME_STACK_BLOCK_SIZE) */
    uint32_t            block_count;  /* number of blocks allocated */
} vtx_frame_stack_t;

/**
 * Initialize the frame stack allocator.
 * Allocates the first 256KB block.
 * Returns 0 on success, -1 on failure.
 */
int vtx_frame_stack_init(vtx_frame_stack_t *fs);

/**
 * Destroy the frame stack allocator and release all blocks.
 */
void vtx_frame_stack_destroy(vtx_frame_stack_t *fs);

/**
 * Get the total bytes currently in use across all blocks.
 */
size_t vtx_frame_stack_used(const vtx_frame_stack_t *fs);

/* ========================================================================== */
/* Frame operations                                                            */
/* ========================================================================== */

/**
 * Create a new frame for executing the given method.
 * Allocates locals and operand stack from the frame stack.
 * If the current block doesn't have enough space, a new block is allocated.
 * Returns the new frame, or NULL if out of memory.
 */
vtx_frame_t *vtx_frame_create(const vtx_method_desc_t *method,
                               vtx_frame_t *caller_frame,
                               uint32_t return_pc,
                               vtx_frame_stack_t *fs);

/**
 * Destroy a frame, returning its memory to the frame stack.
 * Since frames are LIFO, this moves the used pointer back.
 * If the current block becomes empty and has a previous block,
 * the empty block is freed.
 */
void vtx_frame_destroy(vtx_frame_t *frame, vtx_frame_stack_t *fs);

/**
 * Push a value onto the operand stack.
 */
static inline void vtx_frame_push(vtx_frame_t *frame, vtx_value_t value)
{
    VTX_ASSERT(frame != NULL, "frame must not be NULL");
    VTX_ASSERT(frame->stack_top < frame->stack_capacity,
               "operand stack overflow");
    frame->operand_stack[frame->stack_top++] = value;
}

/**
 * Pop a value from the operand stack.
 */
static inline vtx_value_t vtx_frame_pop(vtx_frame_t *frame)
{
    VTX_ASSERT(frame != NULL, "frame must not be NULL");
    VTX_ASSERT(frame->stack_top > 0, "operand stack underflow");
    return frame->operand_stack[--frame->stack_top];
}

/**
 * Peek at the value at `depth` from the top of the operand stack.
 * depth=0 is the top of stack, depth=1 is one below, etc.
 */
static inline vtx_value_t vtx_frame_peek(vtx_frame_t *frame, int depth)
{
    VTX_ASSERT(frame != NULL, "frame must not be NULL");
    VTX_ASSERT(depth >= 0 && depth < frame->stack_top,
               "peek depth out of bounds");
    return frame->operand_stack[frame->stack_top - 1 - depth];
}

/**
 * Get a local variable by index.
 */
static inline vtx_value_t vtx_frame_get_local(vtx_frame_t *frame, uint32_t index)
{
    VTX_ASSERT(frame != NULL, "frame must not be NULL");
    VTX_ASSERT(index < frame->locals_count, "local index out of bounds");
    return frame->locals[index];
}

/**
 * Set a local variable by index.
 */
static inline void vtx_frame_set_local(vtx_frame_t *frame, uint32_t index,
                                        vtx_value_t value)
{
    VTX_ASSERT(frame != NULL, "frame must not be NULL");
    VTX_ASSERT(index < frame->locals_count, "local index out of bounds");
    frame->locals[index] = value;
}

/**
 * Get the current operand stack depth.
 */
static inline int vtx_frame_stack_depth(vtx_frame_t *frame)
{
    VTX_ASSERT(frame != NULL, "frame must not be NULL");
    return frame->stack_top;
}

#endif /* VORTEX_FRAME_H */
