#include "interp/frame.h"

#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* ========================================================================== */
/* Frame block helpers                                                         */
/* ========================================================================== */

/**
 * Allocate a new frame block with the given size.
 * Returns the new block, or NULL on failure.
 */
static vtx_frame_block_t *frame_block_alloc(size_t size)
{
    vtx_frame_block_t *blk = (vtx_frame_block_t *)malloc(sizeof(vtx_frame_block_t));
    if (blk == NULL) {
        return NULL;
    }
    blk->memory = (uint8_t *)malloc(size);
    if (blk->memory == NULL) {
        free(blk);
        return NULL;
    }
    blk->size = size;
    blk->used = 0;
    blk->prev = NULL;
    return blk;
}

/**
 * Free a frame block.
 */
static void frame_block_free(vtx_frame_block_t *blk)
{
    if (blk != NULL) {
        free(blk->memory);
        free(blk);
    }
}

/* ========================================================================== */
/* Frame stack allocator                                                       */
/* ========================================================================== */

int vtx_frame_stack_init(vtx_frame_stack_t *fs)
{
    VTX_ASSERT(fs != NULL, "frame stack must not be NULL");

    fs->block_size = VTX_FRAME_STACK_BLOCK_SIZE;
    fs->block_count = 0;

    /* Allocate the first block */
    fs->current = frame_block_alloc(fs->block_size);
    if (fs->current == NULL) {
        return -1;
    }
    fs->block_count = 1;

    return 0;
}

void vtx_frame_stack_destroy(vtx_frame_stack_t *fs)
{
    VTX_ASSERT(fs != NULL, "frame stack must not be NULL");

    /* Free all blocks by walking the prev chain */
    vtx_frame_block_t *blk = fs->current;
    while (blk != NULL) {
        vtx_frame_block_t *prev = blk->prev;
        frame_block_free(blk);
        blk = prev;
    }
    fs->current = NULL;
    fs->block_count = 0;
}

size_t vtx_frame_stack_used(const vtx_frame_stack_t *fs)
{
    VTX_ASSERT(fs != NULL, "frame stack must not be NULL");
    size_t total = 0;
    const vtx_frame_block_t *blk = fs->current;
    while (blk != NULL) {
        total += blk->used;
        blk = blk->prev;
    }
    return total;
}

/* ========================================================================== */
/* Frame alignment helper                                                      */
/* ========================================================================== */

/**
 * Align a size up to VTX_FRAME_ALIGNMENT (16 bytes).
 */
static inline size_t frame_align(size_t value)
{
    return (value + (VTX_FRAME_ALIGNMENT - 1)) & ~(size_t)(VTX_FRAME_ALIGNMENT - 1);
}

/* ========================================================================== */
/* Frame create / destroy                                                      */
/* ========================================================================== */

/**
 * Compute the total memory needed for a frame with the given
 * max_locals and max_stack, including the frame struct itself,
 * the locals array, the operand stack, and the monitored types array.
 *
 * Layout in the frame stack block:
 *   [vtx_frame_t struct]
 *   [locals array: max_locals * sizeof(vtx_value_t)]
 *   [operand_stack array: max_stack * sizeof(vtx_value_t)]
 *   [monitored_types array: max_locals * sizeof(uint32_t)]
 *   [padding to VTX_FRAME_ALIGNMENT]
 */
static size_t frame_total_size(uint32_t max_locals, uint32_t max_stack)
{
    size_t total = 0;

    /* Frame struct */
    total += sizeof(vtx_frame_t);

    /* Locals array */
    total = frame_align(total);
    total += max_locals * sizeof(vtx_value_t);

    /* Operand stack */
    total = frame_align(total);
    total += max_stack * sizeof(vtx_value_t);

    /* Monitored types array */
    total = frame_align(total);
    total += max_locals * sizeof(uint32_t);

    /* Final alignment */
    total = frame_align(total);

    return total;
}

/**
 * Ensure the current block has enough space, or allocate a new block.
 * Returns 0 on success, -1 on failure.
 */
static int frame_stack_ensure_space(vtx_frame_stack_t *fs, size_t needed)
{
    if (fs->current != NULL &&
        fs->current->used + needed <= fs->current->size) {
        return 0; /* current block has space */
    }

    /* Allocate a new block. Use at least the needed size or the default. */
    size_t new_size = fs->block_size;
    if (needed > new_size) {
        new_size = needed;
    }
    /* Align to page boundary for mmap friendliness */
    new_size = (new_size + 4095) & ~(size_t)4095;

    vtx_frame_block_t *new_blk = frame_block_alloc(new_size);
    if (new_blk == NULL) {
        return -1;
    }

    /* Chain the new block to the current one */
    new_blk->prev = fs->current;
    fs->current = new_blk;
    fs->block_count++;

    return 0;
}

vtx_frame_t *vtx_frame_create(const vtx_method_desc_t *method,
                               vtx_frame_t *caller_frame,
                               uint32_t return_pc,
                               vtx_frame_stack_t *fs)
{
    VTX_ASSERT(method != NULL, "method must not be NULL");
    VTX_ASSERT(fs != NULL, "frame stack must not be NULL");

    /* Determine max_locals and max_stack from the method's bytecode */
    uint32_t max_locals = 0;
    uint32_t max_stack = 0;

    if (method->bytecode != NULL) {
        max_locals = method->bytecode->max_locals;
        max_stack = method->bytecode->max_stack;
    } else {
        /* Fallback for methods without bytecode */
        max_locals = 8;
        max_stack = 16;
    }

    /* Safety limits */
    if (max_locals > VTX_FRAME_MAX_LOCALS) {
        max_locals = VTX_FRAME_MAX_LOCALS;
    }
    if (max_stack > VTX_FRAME_MAX_STACK_DEPTH) {
        max_stack = VTX_FRAME_MAX_STACK_DEPTH;
    }
    /* Ensure at least 1 slot */
    if (max_locals == 0) max_locals = 1;
    if (max_stack == 0) max_stack = 1;

    /* Compute total allocation size */
    size_t total = frame_total_size(max_locals, max_stack);

    /* Ensure space in the frame stack (may allocate a new block) */
    if (frame_stack_ensure_space(fs, total) != 0) {
        return NULL;
    }

    /* Carve out the frame from the current block */
    vtx_frame_block_t *blk = fs->current;
    uint8_t *base = blk->memory + blk->used;
    blk->used += total;

    /* Layout the frame */
    vtx_frame_t *frame = (vtx_frame_t *)base;
    size_t offset = sizeof(vtx_frame_t);

    /* Locals array */
    offset = frame_align(offset);
    vtx_value_t *locals = (vtx_value_t *)(base + offset);
    offset += max_locals * sizeof(vtx_value_t);

    /* Operand stack array */
    offset = frame_align(offset);
    vtx_value_t *operand_stack = (vtx_value_t *)(base + offset);
    offset += max_stack * sizeof(vtx_value_t);

    /* Monitored types array */
    offset = frame_align(offset);
    uint32_t *monitored_types = (uint32_t *)(base + offset);

    /* Initialize the frame */
    frame->locals           = locals;
    frame->operand_stack    = operand_stack;
    frame->stack_top        = 0;
    frame->stack_capacity   = (int)max_stack;
    frame->return_pc        = return_pc;
    frame->caller           = caller_frame;
    frame->method           = method;
    frame->bytecode         = method->bytecode;
    frame->monitored_types  = monitored_types;
    frame->locals_count     = max_locals;
    frame->catch_handler_pc = VTX_CATCH_NONE;
    frame->exception        = VTX_VALUE_UNDEFINED;

    /* Initialize locals to undefined */
    for (uint32_t i = 0; i < max_locals; i++) {
        locals[i] = VTX_VALUE_UNDEFINED;
    }

    /* Initialize monitored types to VTX_TYPE_INVALID */
    for (uint32_t i = 0; i < max_locals; i++) {
        monitored_types[i] = VTX_TYPE_INVALID;
    }

    return frame;
}

void vtx_frame_destroy(vtx_frame_t *frame, vtx_frame_stack_t *fs)
{
    VTX_ASSERT(frame != NULL, "frame must not be NULL");
    VTX_ASSERT(fs != NULL, "frame stack must not be NULL");
    VTX_ASSERT(fs->current != NULL, "frame stack has no current block");

    /* Since frames are allocated in LIFO order from the frame stack,
     * destroying the top frame just moves the used pointer back. */
    uint32_t max_locals = frame->locals_count;
    uint32_t max_stack = (uint32_t)frame->stack_capacity;

    size_t total = frame_total_size(max_locals, max_stack);

    /* Verify that this frame is the top frame in the current block */
    uint8_t *frame_start = (uint8_t *)frame;
    vtx_frame_block_t *blk = fs->current;
    VTX_ASSERT(frame_start + total == blk->memory + blk->used,
               "can only destroy the top frame (LIFO order)");

    /* Reclaim the space */
    blk->used -= total;

    /* If the current block is now empty and has a previous block,
     * free the empty block and switch to the previous one. */
    if (blk->used == 0 && blk->prev != NULL) {
        vtx_frame_block_t *prev = blk->prev;
        frame_block_free(blk);
        fs->current = prev;
        fs->block_count--;
    }
}
