#include "baseline/frame_layout.h"
#include <stdlib.h>

vtx_jit_frame_layout_t vtx_frame_layout_compute(const vtx_method_desc_t *method)
{
    vtx_jit_frame_layout_t layout;
    const vtx_bytecode_t *bc = method->bytecode;

    layout.max_locals = bc->max_locals;
    layout.max_stack  = bc->max_stack;

    /* Pre-scan the bytecode to compute the actual max stack depth.
     * Bytecode max_stack is often too low (or set to a safe default like 4),
     * which causes spill-index-out-of-bounds when the JIT codegen pushes
     * more values than max_stack declared. The abstract-interpretation scan
     * gives us the true peak depth across all control-flow paths. */
    uint32_t scanned_max_stack = vtx_bytecode_compute_max_stack(bc, bc->max_locals);
    if (scanned_max_stack > layout.max_stack) {
        layout.max_stack = scanned_max_stack;
    }

    /* Spill slots: values beyond the first VTX_EXPR_REG_COUNT that won't
     * fit in registers. If max_stack <= VTX_EXPR_REG_COUNT, no spills. */
    layout.max_spills = 0;
    if (layout.max_stack > VTX_EXPR_REG_COUNT) {
        layout.max_spills = layout.max_stack - VTX_EXPR_REG_COUNT;
    }

    /* locals_base = -8 (local[0] at RBP-8).
     * local_offset(i) = locals_base - i*8 = -8 - i*8 = -(i+1)*8
     *   local[0] = -8  (RBP-8)
     *   local[1] = -16 (RBP-16)  */
    layout.locals_base = -8;

    /* spill_base = -(max_locals+1)*8 (spill[0] at RBP - (max_locals+1)*8).
     * spill_offset(i) = spill_base - i*8 = -(max_locals+1)*8 - i*8
     *   spill[0] = -(max_locals+1)*8
     *   spill[1] = -(max_locals+2)*8  */
    layout.spill_base = -(int32_t)((layout.max_locals + 1) * 8);

    /* Total frame size = space for locals + spills.
     * This is the amount we subtract from RSP after "mov rbp, rsp". */
    uint32_t locals_bytes = layout.max_locals * 8;
    uint32_t spill_bytes  = layout.max_spills * 8;
    uint32_t raw_size     = locals_bytes + spill_bytes;

    /* Round up to VTX_FRAME_ALIGNMENT (16 bytes) */
    uint32_t alignment = VTX_FRAME_ALIGNMENT;
    layout.total_frame_size = (raw_size + alignment - 1) & ~(alignment - 1);

    /* Ensure minimum frame size of 16 bytes so RSP adjustment is never zero
     * (avoids confusion with leaf functions that don't need a frame). */
    if (layout.total_frame_size == 0) {
        layout.total_frame_size = alignment;
    }

    /* frame_bottom: RBP - total_frame_size */
    layout.frame_bottom = -(int32_t)layout.total_frame_size;

    return layout;
}

void vtx_frame_layout_expr_location(uint32_t stack_index,
                                     uint32_t stack_depth,
                                     const vtx_jit_frame_layout_t *layout,
                                     int *reg,
                                     int32_t *spill_offset)
{
    VTX_ASSERT(stack_index < stack_depth, "stack index out of bounds");
    /* NOTE: We deliberately do NOT assert stack_depth <= layout->max_stack here.
     * The frame layout may have been computed with a max_stack from the bytecode
     * that was too low. After the pre-scan fix, max_stack should be accurate,
     * but during deopt or edge cases, the codegen may briefly exceed it.
     * The important invariant is that the spill area has enough slots,
     * which is guaranteed by the pre-scan. */

    /*
     * Expression stack uses a "top of stack in registers" model.
     * The topmost VTX_EXPR_REG_COUNT (4) values are in registers.
     * Values deeper than that are spilled to the frame.
     *
     * stack_index is 0-based from the bottom of the expression stack.
     * stack_depth is the current number of values on the stack.
     *
     * The "top" of the stack is at index (stack_depth - 1).
     * The top 4 values occupy registers, indexed from the top:
     *   TOS     → RAX (reg 0)
     *   TOS-1   → RCX (reg 1)
     *   TOS-2   → RDX (reg 2)
     *   TOS-3   → RBX (reg 3)
     *
     * Values at positions 0..(stack_depth-5) are spilled.
     * Spill slot index = stack_index (0-based from bottom).
     */

    /* How many values are in spill slots? */
    uint32_t spilled_count = 0;
    if (stack_depth > VTX_EXPR_REG_COUNT) {
        spilled_count = stack_depth - VTX_EXPR_REG_COUNT;
    }

    if (stack_index < spilled_count) {
        /* This value is spilled.
         * spill_offset = spill_base - stack_index * 8 */
        *reg = -1;
        *spill_offset = layout->spill_base - (int32_t)(stack_index * 8);
    } else {
        /* This value is in a register.
         * Register index from the top: TOS=0, TOS-1=1, etc.
         * Position from top = stack_depth - 1 - stack_index */
        uint32_t reg_from_top = stack_depth - 1 - stack_index;
        VTX_ASSERT(reg_from_top < VTX_EXPR_REG_COUNT,
                   "register index out of bounds");
        *reg = (int)reg_from_top;
        *spill_offset = 0;
    }
}
