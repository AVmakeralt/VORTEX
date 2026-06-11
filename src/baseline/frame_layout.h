#ifndef VORTEX_BASELINE_FRAME_LAYOUT_H
#define VORTEX_BASELINE_FRAME_LAYOUT_H

#include <stdint.h>
#include <stddef.h>
#include "vortex_config.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "deopt/types.h"

/**
 * VORTEX Baseline JIT Frame Layout
 *
 * JIT stack frames are RBP-based, mirroring the interpreter frame layout
 * but optimized for machine-code access. The frame is divided into:
 *
 * Prologue:
 *   push method_ptr       ; arg passed in register
 *   push deopt_info       ; arg passed in register
 *   push profile_data     ; arg passed in register
 *   push rbp              ; save caller RBP
 *   mov rbp, rsp
 *   sub rsp, total_frame_size  ; allocate locals + spills
 *
 * Stack layout after prologue:
 *   High addresses:
 *     +--------------------+
 *     | return address     |  [RBP+32] pushed by CALL
 *     | method pointer     |  [RBP+24] const vtx_method_desc_t*
 *     | deopt_info pointer |  [RBP+16] vtx_deopt_info_t*
 *     | profile_data ptr   |  [RBP+8]  vtx_profile_data_t*
 *     +--------------------+
 *     | caller RBP         |  [RBP+0]  saved by push rbp
 *     +--------------------+  <- RBP
 *
 *   Low addresses (below RBP):
 *     +--------------------+
 *     | local[0]           |  [RBP - 8]
 *     | local[1]           |  [RBP - 16]
 *     | ...                |
 *     | local[N-1]         |  [RBP - N*8]
 *     +--------------------+
 *     | spill[0]           |  [RBP - (N+1)*8]
 *     | spill[1]           |  [RBP - (N+2)*8]
 *     | ...                |
 *     | spill[M-1]         |
 *     +--------------------+  <- RSP (after frame setup)
 *
 * Expression stack top values are held in registers (RAX, RCX, RDX, RBX).
 * Values beyond the first 4 are spilled to the spill area.
 *
 * Total frame size is rounded up to VTX_FRAME_ALIGNMENT (16 bytes).
 */

/* ========================================================================== */
/* Frame header offsets (relative to RBP)                                      */
/* ========================================================================== */

/* Fixed offsets for every JIT frame */
#define VTX_FRAME_CALLER_RBP_OFFSET    0   /* [RBP+0]  */
#define VTX_FRAME_PROFILE_DATA_OFFSET  8   /* [RBP+8]  */
#define VTX_FRAME_DEOPT_INFO_OFFSET    16  /* [RBP+16] */
#define VTX_FRAME_METHOD_PTR_OFFSET    24  /* [RBP+24] */
#define VTX_FRAME_RETURN_ADDR_OFFSET   32  /* [RBP+32] */

/* Number of 8-byte slots in the frame header above RBP (excluding caller RBP at [RBP]) */
#define VTX_FRAME_HEADER_SLOTS_ABOVE   4   /* profile_data, deopt_info, method_ptr, return_addr */

/* Number of general-purpose registers used for expression stack top */
#define VTX_EXPR_REG_COUNT             4

/* ========================================================================== */
/* JIT frame layout descriptor                                                 */
/* ========================================================================== */

/**
 * Describes the layout of a compiled method's stack frame.
 * All offsets are relative to RBP. Negative offsets point below RBP
 * (locals and spills), positive offsets point above RBP (header).
 *
 * The total_frame_size is the number of bytes to subtract from RSP
 * during prologue (locals + spills, not including the 4 pushed header values
 * and the saved RBP which are handled by push instructions).
 */
typedef struct {
    uint32_t max_locals;       /* number of local variable slots */
    uint32_t max_stack;        /* max operand stack depth */
    uint32_t max_spills;       /* max spill slots (max_stack - VTX_EXPR_REG_COUNT, min 0) */

    /* Byte offset from RBP to local[i].
     * local[i] is at RBP + local_offset(i) = RBP - (i+1)*8.
     * locals_base = -8 (offset of local[0]). */
    int32_t  locals_base;      /* offset of local[0], always -8 */

    /* Byte offset from RBP to spill[i].
     * spill[i] is at RBP + spill_offset(i) = RBP - (max_locals+1+i)*8.
     * spill_base = -(max_locals+1)*8 (offset of spill[0]). */
    int32_t  spill_base;       /* offset of spill[0], negative */

    /* Total frame size in bytes (the amount to subtract from RSP after
     * "mov rbp, rsp"). Covers locals + spills. Rounded up to
     * VTX_FRAME_ALIGNMENT (16 bytes). */
    uint32_t total_frame_size;

    /* Byte offset from RBP to the bottom of the frame.
     * Equal to -(int32_t)total_frame_size. */
    int32_t  frame_bottom;
} vtx_jit_frame_layout_t;

/* ========================================================================== */
/* Functions                                                                   */
/* ========================================================================== */

/**
 * Compute the JIT frame layout for a method.
 *
 * Layout:
 *   1. Frame header above RBP (pushed by prologue + CALL).
 *   2. Locals below RBP, starting at RBP-8, growing downward.
 *   3. Expression stack spills below locals, growing downward.
 *      Spill slots = max(0, max_stack - VTX_EXPR_REG_COUNT).
 *   4. Total frame size = locals_bytes + spill_bytes, rounded to 16.
 *
 * @param method  The method descriptor (provides max_locals, max_stack)
 * @return        The computed frame layout
 */
vtx_jit_frame_layout_t vtx_frame_layout_compute(const vtx_method_desc_t *method);

/**
 * Get the byte offset from RBP to local variable slot `index`.
 * Returns a negative offset (locals are below RBP).
 */
static inline int32_t vtx_frame_layout_local_offset(
    const vtx_jit_frame_layout_t *layout, uint32_t index)
{
    VTX_ASSERT(index < layout->max_locals, "local index out of bounds");
    /* local[i] at RBP - (i+1)*8 = locals_base - i*8 */
    return layout->locals_base - (int32_t)(index * 8);
}

/**
 * Get the byte offset from RBP to spill slot `index`.
 * Returns a negative offset (spills are below locals).
 */
static inline int32_t vtx_frame_layout_spill_offset(
    const vtx_jit_frame_layout_t *layout, uint32_t index)
{
    VTX_ASSERT(index < layout->max_spills, "spill index out of bounds");
    /* spill[i] at RBP - (max_locals+1+i)*8 = spill_base - i*8 */
    return layout->spill_base - (int32_t)(index * 8);
}

/**
 * Determine which physical register or spill slot holds the Nth element
 * of the expression stack.
 *
 * The topmost VTX_EXPR_REG_COUNT (4) values are in registers:
 *   TOS   → RAX (reg 0), TOS-1 → RCX (reg 1),
 *   TOS-2 → RDX (reg 2), TOS-3 → RBX (reg 3).
 * Values deeper than that are spilled to the frame.
 *
 * @param stack_index  0-based index from the bottom of the expression stack
 * @param stack_depth  current depth of the expression stack
 * @param layout       frame layout (for spill offset computation)
 * @param[out] reg     set to the register number (0=RAX,1=RCX,2=RDX,3=RBX)
 *                     if the value is in a register, or -1 if spilled
 * @param[out] spill_offset  set to the RBP-relative offset if spilled,
 *                           or 0 if in a register
 */
void vtx_frame_layout_expr_location(uint32_t stack_index,
                                     uint32_t stack_depth,
                                     const vtx_jit_frame_layout_t *layout,
                                     int *reg,
                                     int32_t *spill_offset);

#endif /* VORTEX_BASELINE_FRAME_LAYOUT_H */
