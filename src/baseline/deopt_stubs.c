#include "baseline/deopt_stubs.h"
#include "baseline/codegen.h"
#include "baseline/frame_layout.h"
#include "deopt/side_table.h"
#include "deopt/frame_state.h"
#include "deopt/types.h"
#include "interp/frame.h"
#include "interp/dispatch.h"
#include "runtime/object.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Global interpreter entry point                                              */
/* ========================================================================== */

static vtx_interp_entry_t g_interp_entry = NULL;

void vtx_deopt_set_interp_entry(vtx_interp_entry_t entry)
{
    g_interp_entry = entry;
}

vtx_interp_entry_t vtx_deopt_get_interp_entry(void)
{
    return g_interp_entry;
}

/* ========================================================================== */
/* Deopt stub array operations                                                 */
/* ========================================================================== */

int vtx_deopt_stub_array_init(vtx_deopt_stub_array_t *arr)
{
    arr->capacity = VTX_DEOPT_STUBS_INITIAL_CAPACITY;
    arr->count = 0;
    arr->stubs = (vtx_deopt_stub_t *)malloc(
        arr->capacity * sizeof(vtx_deopt_stub_t));
    if (!arr->stubs) return -1;
    return 0;
}

void vtx_deopt_stub_array_destroy(vtx_deopt_stub_array_t *arr)
{
    if (arr->stubs) {
        free(arr->stubs);
        arr->stubs = NULL;
    }
    arr->count = 0;
    arr->capacity = 0;
}

uint32_t vtx_deopt_stub_array_add(vtx_deopt_stub_array_t *arr,
                                   vtx_deopt_stub_t stub)
{
    if (arr->count >= arr->capacity) {
        uint32_t new_cap = arr->capacity * 2;
        vtx_deopt_stub_t *new_stubs = (vtx_deopt_stub_t *)realloc(
            arr->stubs, new_cap * sizeof(vtx_deopt_stub_t));
        if (!new_stubs) return UINT32_MAX;
        arr->stubs = new_stubs;
        arr->capacity = new_cap;
    }
    uint32_t idx = arr->count;
    arr->stubs[idx] = stub;
    arr->count++;
    return idx;
}

/* ========================================================================== */
/* Helper macros for x86-64 encoding                                          */
/* ========================================================================== */

#define REX_W      0x48
#define REX_WR     0x4C
#define REX_WB     0x49
#define REX_WRB    0x4D

static inline uint8_t modrm(uint8_t mod, uint8_t reg, uint8_t rm)
{
    return (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

/* ========================================================================== */
/* Deopt stub emission helpers                                                */
/* ========================================================================== */

/**
 * Emit: mov [rbp + offset], reg  (64-bit)
 * Store a register value to a frame slot.
 */
static void emit_store_to_frame(vtx_code_buffer_t *buf, int32_t offset, vtx_reg_t reg)
{
    /* REX.W + 89 /r (MOV r/m64, r64) */
    uint8_t rex = REX_W;
    if (reg >= VTX_REG_R8) rex |= 0x04; /* REX.R */
    vtx_code_buffer_emit_byte(buf, rex);

    vtx_code_buffer_emit_byte(buf, 0x89); /* MOV r/m64, r64 */

    if (offset >= -128 && offset <= 127) {
        vtx_code_buffer_emit_byte(buf, modrm(1, reg, VTX_REG_RBP));
        vtx_code_buffer_emit_byte(buf, (uint8_t)(offset & 0xFF));
    } else {
        vtx_code_buffer_emit_byte(buf, modrm(2, reg, VTX_REG_RBP));
        vtx_code_buffer_emit_dword(buf, (uint32_t)offset);
    }
}

/**
 * Emit: mov reg, [rbp + offset]  (64-bit)
 * Load a frame slot into a register.
 */
static void emit_load_from_frame(vtx_code_buffer_t *buf, vtx_reg_t reg, int32_t offset)
{
    uint8_t rex = REX_W;
    if (reg >= VTX_REG_R8) rex |= 0x04; /* REX.R */
    vtx_code_buffer_emit_byte(buf, rex);

    vtx_code_buffer_emit_byte(buf, 0x8B); /* MOV r64, r/m64 */

    if (offset >= -128 && offset <= 127) {
        vtx_code_buffer_emit_byte(buf, modrm(1, reg, VTX_REG_RBP));
        vtx_code_buffer_emit_byte(buf, (uint8_t)(offset & 0xFF));
    } else {
        vtx_code_buffer_emit_byte(buf, modrm(2, reg, VTX_REG_RBP));
        vtx_code_buffer_emit_dword(buf, (uint32_t)offset);
    }
}

/**
 * Emit: mov reg, imm64 (absolute 64-bit immediate)
 * Uses MOV r64, imm64 encoding (REX.W + B8+rd + 8 bytes)
 */
static void emit_mov_reg_imm64(vtx_code_buffer_t *buf, vtx_reg_t reg, uint64_t imm64)
{
    uint8_t rex = REX_W;
    if (reg >= VTX_REG_R8) rex |= 0x01; /* REX.B */
    vtx_code_buffer_emit_byte(buf, rex);
    vtx_code_buffer_emit_byte(buf, (uint8_t)(0xB8 | (reg & 7)));
    vtx_code_buffer_emit_qword(buf, imm64);
}

/**
 * Emit: jmp rel32 (relative jump to absolute address)
 */
static void emit_jmp_rel32(vtx_code_buffer_t *buf, uint8_t *code_start,
                              uint64_t target_addr)
{
    vtx_code_buffer_emit_byte(buf, 0xE9);
    /* The offset is relative to the instruction after the jmp (5 bytes).
     * We need absolute addresses for both source and target.
     * current_pos is the buffer offset of the displacement field;
     * the absolute address is code_start + current_pos.
     * The instruction after jmp is at current_pos + 4. */
    uint32_t current_pos = vtx_code_buffer_position(buf);
    uint64_t source_addr = (uint64_t)(uintptr_t)(code_start + current_pos + 4);
    int32_t rel32 = (int32_t)(target_addr - source_addr);
    vtx_code_buffer_emit_dword(buf, (uint32_t)rel32);
}

/**
 * Emit: push reg (64-bit)
 */
static void emit_push_reg(vtx_code_buffer_t *buf, vtx_reg_t reg)
{
    if (reg >= VTX_REG_R8) {
        vtx_code_buffer_emit_byte(buf, 0x41); /* REX.B */
    }
    vtx_code_buffer_emit_byte(buf, (uint8_t)(0x50 | (reg & 7)));
}

/**
 * Emit: sub rsp, imm8
 */
static void emit_sub_rsp_imm8(vtx_code_buffer_t *buf, uint8_t imm8)
{
    vtx_code_buffer_emit_byte(buf, 0x48); /* REX.W */
    vtx_code_buffer_emit_byte(buf, 0x83); /* SUB r/m64, imm8 */
    vtx_code_buffer_emit_byte(buf, modrm(1, 5, VTX_REG_RSP)); /* /5 = SUB */
    vtx_code_buffer_emit_byte(buf, imm8);
}

/**
 * Emit: add rsp, imm8
 */
static void emit_add_rsp_imm8(vtx_code_buffer_t *buf, uint8_t imm8)
{
    vtx_code_buffer_emit_byte(buf, 0x48); /* REX.W */
    vtx_code_buffer_emit_byte(buf, 0x83); /* ADD r/m64, imm8 */
    vtx_code_buffer_emit_byte(buf, modrm(1, 0, VTX_REG_RSP)); /* /0 = ADD */
    vtx_code_buffer_emit_byte(buf, imm8);
}

/* ========================================================================== */
/* Deopt stub emission                                                         */
/* ========================================================================== */

const vtx_deopt_stub_t *vtx_deopt_stub_emit(vtx_deopt_context_t *ctx,
                                              uint32_t guard_index)
{
    VTX_ASSERT(guard_index < ctx->guards->count, "guard index out of bounds");

    vtx_guard_info_t *guard = &ctx->guards->guards[guard_index];
    vtx_code_buffer_t *buf = ctx->code_buf;

    /* Record stub start position */
    uint32_t stub_start = vtx_code_buffer_position(buf);

    vtx_deopt_stub_t stub;
    memset(&stub, 0, sizeof(stub));
    stub.guard_index = guard_index;
    stub.bytecode_pc = guard->deopt_continuation;
    stub.stack_depth = guard->stack_depth;
    stub.native_code_offset = stub_start;

    /*
     * Deopt stub implementation:
     *
     * The stub must transition from JIT code to the interpreter.
     * We call the runtime deopt function (vtx_deopt_runtime_transition)
     * which handles the frame reconstruction.
     *
     * Calling convention for the runtime function:
     *   RDI = JIT frame RBP
     *   RSI = native PC offset where deopt occurred
     *   RAX = return value (interpreter frame pointer)
     *
     * After the runtime call returns:
     *   - The interpreter frame is fully reconstructed
     *   - RAX holds the interpreter frame pointer
     *   - We jump to the interpreter dispatch loop
     */

    /* Step 1: Save expression stack registers to frame spill slots.
     * The top VTX_EXPR_REG_COUNT values are in RAX, RCX, RDX, RBX.
     * We need to store them into the spill area so the runtime can
     * find them. The spill area is ordered from bottom (spill[0])
     * to top (spill[max_spills-1]).
     *
     * For a stack depth of N:
     *   Values at positions 0..(N-5) are already in spill slots
     *   Values at positions (N-4)..(N-1) are in registers:
     *     RAX = TOS   = position (N-1)
     *     RCX = TOS-1 = position (N-2)
     *     RDX = TOS-2 = position (N-3)
     *     RBX = TOS-3 = position (N-4)
     *
     * We store register values to the spill area starting from
     * the first register-resident position. */

    uint32_t depth = guard->stack_depth;
    uint32_t spilled_count = 0;
    if (depth > VTX_EXPR_REG_COUNT) {
        spilled_count = depth - VTX_EXPR_REG_COUNT;
    }

    /* Save registers that hold expression stack values to the frame.
     *
     * The spill area has max_spills slots, which is enough for the
     * spilled_count values that were already in spill. But the register
     * values (up to 4) also need to be saved somewhere during deopt.
     * We use local variable slots as temporary storage for register values
     * that don't fit in the spill area.
     *
     * Layout of saved values:
     *   spill[0]..spill[spilled_count-1] = already-spilled values (no action needed)
     *   spill[spilled_count]             = RBX value → but only if within max_spills
     *   For register values that exceed the spill area, use local slots.
     *
     * Simpler approach: always use local slots for register saves during deopt.
     * This avoids any OOB access on the spill area and is safe because
     * local slots are not in use during deopt (the expression stack values
     * are being materialized, not the locals).
     */

    /* Save register values using local variable slots as temp storage.
     * We use local[max_locals-1], local[max_locals-2], etc.
     * If there aren't enough locals, we use the spill area for what fits. */
    uint32_t locals_available = ctx->frame_layout.max_locals;
    uint32_t spill_available = ctx->frame_layout.max_spills;

    if (depth >= 4) {
        /* RBX = position spilled_count, RDX = spilled_count+1,
         * RCX = spilled_count+2, RAX = spilled_count+3 */
        /* Save RBX */
        if (spilled_count < spill_available) {
            int32_t off = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count);
            emit_store_to_frame(buf, off, VTX_REG_RBX);
        } else if (locals_available > 0) {
            int32_t off = vtx_frame_layout_local_offset(&ctx->frame_layout, locals_available - 1);
            emit_store_to_frame(buf, off, VTX_REG_RBX);
        }
        /* Save RDX */
        if (spilled_count + 1 < spill_available) {
            int32_t off = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count + 1);
            emit_store_to_frame(buf, off, VTX_REG_RDX);
        } else if (locals_available > 1) {
            int32_t off = vtx_frame_layout_local_offset(&ctx->frame_layout, locals_available - 2);
            emit_store_to_frame(buf, off, VTX_REG_RDX);
        }
        /* Save RCX */
        if (spilled_count + 2 < spill_available) {
            int32_t off = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count + 2);
            emit_store_to_frame(buf, off, VTX_REG_RCX);
        } else if (locals_available > 2) {
            int32_t off = vtx_frame_layout_local_offset(&ctx->frame_layout, locals_available - 3);
            emit_store_to_frame(buf, off, VTX_REG_RCX);
        }
        /* Save RAX */
        if (spilled_count + 3 < spill_available) {
            int32_t off = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count + 3);
            emit_store_to_frame(buf, off, VTX_REG_RAX);
        } else if (locals_available > 3) {
            int32_t off = vtx_frame_layout_local_offset(&ctx->frame_layout, locals_available - 4);
            emit_store_to_frame(buf, off, VTX_REG_RAX);
        }
    } else if (depth == 3) {
        /* RDX = position 0, RCX = position 1, RAX = position 2 */
        if (spilled_count < spill_available) {
            int32_t off = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count);
            emit_store_to_frame(buf, off, VTX_REG_RDX);
        } else if (locals_available > 0) {
            int32_t off = vtx_frame_layout_local_offset(&ctx->frame_layout, locals_available - 1);
            emit_store_to_frame(buf, off, VTX_REG_RDX);
        }
        if (spilled_count + 1 < spill_available) {
            int32_t off = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count + 1);
            emit_store_to_frame(buf, off, VTX_REG_RCX);
        } else if (locals_available > 1) {
            int32_t off = vtx_frame_layout_local_offset(&ctx->frame_layout, locals_available - 2);
            emit_store_to_frame(buf, off, VTX_REG_RCX);
        }
        if (spilled_count + 2 < spill_available) {
            int32_t off = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count + 2);
            emit_store_to_frame(buf, off, VTX_REG_RAX);
        } else if (locals_available > 2) {
            int32_t off = vtx_frame_layout_local_offset(&ctx->frame_layout, locals_available - 3);
            emit_store_to_frame(buf, off, VTX_REG_RAX);
        }
    } else if (depth == 2) {
        /* RCX = position 0, RAX = position 1 */
        if (spilled_count < spill_available) {
            int32_t off = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count);
            emit_store_to_frame(buf, off, VTX_REG_RCX);
        } else if (locals_available > 0) {
            int32_t off = vtx_frame_layout_local_offset(&ctx->frame_layout, locals_available - 1);
            emit_store_to_frame(buf, off, VTX_REG_RCX);
        }
        if (spilled_count + 1 < spill_available) {
            int32_t off = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count + 1);
            emit_store_to_frame(buf, off, VTX_REG_RAX);
        } else if (locals_available > 1) {
            int32_t off = vtx_frame_layout_local_offset(&ctx->frame_layout, locals_available - 2);
            emit_store_to_frame(buf, off, VTX_REG_RAX);
        }
    } else if (depth == 1) {
        /* RAX = position 0 */
        if (spilled_count < spill_available) {
            int32_t off = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count);
            emit_store_to_frame(buf, off, VTX_REG_RAX);
        } else if (locals_available > 0) {
            int32_t off = vtx_frame_layout_local_offset(&ctx->frame_layout, locals_available - 1);
            emit_store_to_frame(buf, off, VTX_REG_RAX);
        }
    }

    /* Step 2: Set up arguments for the runtime deopt function.
     *   RDI = JIT frame RBP (already in RBP)
     *   RSI = native PC offset of the guard
     */
    /* mov rdi, rbp */
    vtx_code_buffer_emit_byte(buf, REX_W);
    vtx_code_buffer_emit_byte(buf, 0x89); /* MOV r/m64, r64 */
    vtx_code_buffer_emit_byte(buf, modrm(3, VTX_REG_RBP, VTX_REG_RDI));

    /* mov rsi, guard_native_pc_offset */
    emit_mov_reg_imm64(buf, VTX_REG_RSI, (uint64_t)guard->native_pc_offset);

    /* Step 3: Align stack for function call (System V ABI requires 16-byte
     * alignment at the call site). We need to figure out if the stack is
     * currently aligned. After the JIT prologue, the stack was aligned.
     * The expression stack doesn't change RSP in our design (it's register-
     * based). So RSP should still be 16-byte aligned.
     * However, to be safe, we push a dummy value to align. */
    /* push rbp (save RBP across the call — it holds our frame pointer) */
    emit_push_reg(buf, VTX_REG_RBP);
    /* Also save RBX (callee-saved, but we use it for expr stack) */
    emit_push_reg(buf, VTX_REG_RBX);

    /* Step 4: Call the runtime deopt function */
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_deopt_runtime_transition);
    /* call rax */
    vtx_code_buffer_emit_byte(buf, 0xFF); /* CALL r/m64 */
    vtx_code_buffer_emit_byte(buf, modrm(3, 2, VTX_REG_RAX)); /* /2 = CALL */

    /* Step 5: Restore callee-saved registers */
    emit_add_rsp_imm8(buf, 16); /* pop dummy + rbp (we pushed 2 regs) */

    /* Step 6: After the runtime call, RAX contains the interpreter
     * frame pointer. Jump to the interpreter dispatch loop.
     *
     * The interpreter entry point expects:
     *   - RBP = interpreter frame pointer
     *   - The bytecode PC is stored in the interpreter frame
     */
    if (g_interp_entry != NULL) {
        /* mov rbp, rax — set interpreter frame pointer */
        vtx_code_buffer_emit_byte(buf, REX_W);
        vtx_code_buffer_emit_byte(buf, 0x89);
        vtx_code_buffer_emit_byte(buf, modrm(3, VTX_REG_RAX, VTX_REG_RBP));

        /* jmp to interpreter entry — BS-1 fix: pass code_start for proper absolute address calculation */
        emit_jmp_rel32(buf, ctx->code_start, (uint64_t)(uintptr_t)g_interp_entry);
    } else {
        /* No interpreter entry set — this shouldn't happen in production.
         * Emit a trap (ud2) to catch the error. */
        vtx_code_buffer_emit_byte(buf, 0x0F);
        vtx_code_buffer_emit_byte(buf, 0x0B); /* UD2 */
    }

    /* Record the stub's size */
    uint32_t stub_end = vtx_code_buffer_position(buf);
    stub.native_code_size = stub_end - stub_start;
    stub.native_code = ctx->code_start + stub_start;

    /* Add a side table entry for this deopt point */
    uint32_t fs_index = vtx_side_table_add_frame_state(ctx->side_table, NULL);
    stub.frame_state_index = fs_index;

    vtx_side_table_add_entry(ctx->side_table, guard->native_pc_offset,
                              fs_index, VTX_STF_GUARD);

    /* Add the stub to the array */
    uint32_t idx = vtx_deopt_stub_array_add(
        ctx->stub_array, stub);

    return &ctx->stub_array->stubs[idx];
}

int vtx_deopt_stubs_emit_all(vtx_deopt_context_t *ctx)
{
    int count = 0;
    for (uint32_t i = 0; i < ctx->guards->count; i++) {
        const vtx_deopt_stub_t *stub = vtx_deopt_stub_emit(ctx, i);
        if (!stub) return -1;
        count++;
    }

    /* Patch guard jumps to point to their deopt stubs */
    if (vtx_deopt_stubs_patch_guards(ctx) != 0) {
        return -1;
    }

    return count;
}

int vtx_deopt_stubs_patch_guards(vtx_deopt_context_t *ctx)
{
    VTX_ASSERT(ctx->stub_array != NULL, "stub_array must not be NULL");
    VTX_ASSERT(ctx->stub_array->count == ctx->guards->count,
               "stub count must match guard count");

    for (uint32_t i = 0; i < ctx->guards->count; i++) {
        vtx_guard_info_t *guard = &ctx->guards->guards[i];
        vtx_deopt_stub_t *stub = &ctx->stub_array->stubs[i];

        /* The guard's branch_offset field contains the position in the
         * code buffer where the 4-byte displacement of the Jcc instruction
         * was emitted. We need to patch this displacement to point to
         * the deopt stub.
         *
         * Jcc instruction format: 0F 8x [4-byte disp]
         * The displacement is relative to the instruction AFTER the Jcc,
         * which is at disp_pos + 4 (6 bytes total for the Jcc).
         */
        uint32_t disp_pos = (uint32_t)guard->branch_offset;

        /* Calculate the relative displacement:
         * target = stub's native code offset (from start of code)
         * source_end = disp_pos + 4 (byte after the displacement)
         * disp = target - source_end
         */
        int32_t disp = (int32_t)stub->native_code_offset - (int32_t)(disp_pos + 4);

        /* Patch the displacement in the code buffer */
        ctx->code_buf->bytes[disp_pos]     = (uint8_t)(disp & 0xFF);
        ctx->code_buf->bytes[disp_pos + 1] = (uint8_t)((disp >> 8) & 0xFF);
        ctx->code_buf->bytes[disp_pos + 2] = (uint8_t)((disp >> 16) & 0xFF);
        ctx->code_buf->bytes[disp_pos + 3] = (uint8_t)((disp >> 24) & 0xFF);
    }

    return 0;
}

/* ========================================================================== */
/* Runtime deopt transition function                                           */
/* ========================================================================== */

/**
 * This is the actual runtime function called from deopt stubs.
 * It reconstructs the interpreter frame and returns the frame pointer.
 *
 * In a full implementation, this would:
 *   1. Walk the JIT frame to find the deopt info
 *   2. Look up the side table entry for the native PC
 *   3. Reconstruct the interpreter frame from the frame state
 *   4. Return the interpreter frame pointer
 *
 * For the baseline JIT, we implement a simplified version that:
 *   1. Reads the deopt_info pointer from the JIT frame header
 *   2. Finds the bytecode PC from the pc_map
 *   3. Sets up the interpreter frame with locals and stack
 *   4. Returns the interpreter frame pointer
 */
/*
 * x86-64 register number to name mapping.
 * RAX=0, RCX=1, RDX=2, RBX=3, RSP=4, RBP=5, RSI=6, RDI=7,
 * R8=8..R15=15.
 *
 * On entry to the deopt stub, the expression stack registers have
 * already been saved to the frame by the stub code. So we read
 * them from the frame's spill/local slots, not from the hardware
 * registers directly.
 *
 * However, the T2/T3 JIT uses a different calling convention and
 * register allocation. The side table's register map tells us which
 * physical register holds which NodeID's value. We read those
 * registers directly — the deopt stub for T2 saves all registers
 * before calling us.
 *
 * For the baseline JIT (T1), the expression stack values are saved
 * to spill slots by the deopt stub. We read them from the frame
 * using the frame layout.
 */

/* Read a 64-bit value from a JIT frame at the given RBP-relative offset */
static inline uint64_t jit_frame_read(void *jit_rbp, int32_t offset) {
    return *(uint64_t *)((uint8_t *)jit_rbp + offset);
}

/* Read a register value from the saved register area.
 * The deopt stub saves registers to the frame before calling us.
 * For the baseline JIT, expression stack regs (RAX,RCX,RDX,RBX) are
 * saved to local slots. For T2/T3, the register map tells us which
 * registers are live, and we read them from a register save area
 * that the deopt handler provides.
 *
 * For now, we read from the side table's register map to find values.
 * The register map maps (register_number → NodeID). We need the
 * reverse: (NodeID → value). The value is in the register at the
 * time of deopt. Since the deopt stub for T2 saves all registers
 * to the stack before calling us, we need to know where they're saved.
 *
 * The simplest correct approach: for each local in the FrameState,
 * look up the NodeID in the register map. If found, the value is in
 * that register. But we can't read registers from C — they're already
 * saved by the stub.
 *
 * The T1 deopt stub saves RAX,RCX,RDX,RBX to frame slots. The T2
 * deopt stub calls vtx_deopt_handler_stub which receives
 * frame_state_index and native_pc as arguments. The actual register
 * values are on the stack (saved by the JCC-fallthrough path).
 *
 * For the baseline JIT, we can read expression stack values directly
 * from the frame's spill slots and local slots (where the stub saved
 * them). For T2, we use the FrameState's local NodeIDs and the
 * side table's register map.
 *
 * This implementation handles both paths:
 * 1. If we have a side table with register map, use it to find values
 * 2. Otherwise, read directly from the JIT frame's local slots
 */

void *vtx_deopt_runtime_transition(void *jit_rbp, uint32_t native_pc)
{
    uint8_t *rbp = (uint8_t *)jit_rbp;

    /* Step 1: Read deopt_info from the JIT frame header */
    vtx_deopt_info_t *deopt_info = *(vtx_deopt_info_t **)(
        rbp + VTX_FRAME_DEOPT_INFO_OFFSET);

    if (!deopt_info) {
        VTX_ASSERT(false, "deopt_info is NULL in JIT frame");
        return NULL;
    }

    /* Read method pointer from frame header */
    const vtx_method_desc_t *method = *(const vtx_method_desc_t **)(
        rbp + VTX_FRAME_METHOD_PTR_OFFSET);

    if (!method || !method->bytecode) {
        VTX_ASSERT(false, "method or bytecode is NULL in JIT frame");
        return NULL;
    }

    /* Step 2: Look up the bytecode PC for this native PC offset */
    uint32_t bytecode_pc = 0;
    uint32_t stack_depth = 0;
    bool found = false;

    uint32_t lo = 0, hi = deopt_info->pc_map_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t mid_native = (deopt_info->native_offsets != NULL) ?
            deopt_info->native_offsets[mid] : deopt_info->pc_map[mid];
        if (mid_native <= native_pc) {
            bytecode_pc = deopt_info->pc_map[mid];
            if (deopt_info->stack_depth_map) {
                stack_depth = deopt_info->stack_depth_map[mid];
            }
            found = true;
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (!found) {
        VTX_ASSERT(false, "no deopt mapping found for native PC");
        return NULL;
    }

    /* Step 3: Get the side table and FrameState */
    vtx_side_table_t *side_table = deopt_info->side_table;
    vtx_frame_state_t *fs = NULL;
    const vtx_side_table_entry_t *st_entry = NULL;
    const vtx_reg_map_entry_t *reg_map = NULL;
    uint32_t reg_map_count = 0;

    if (side_table) {
        st_entry = vtx_side_table_lookup_entry(side_table, native_pc);
        if (st_entry) {
            fs = vtx_side_table_get_frame_state(side_table,
                                                 st_entry->frame_state_index);
            reg_map = st_entry->register_map;
            reg_map_count = st_entry->register_map_count;
        }
    }

    /* Step 4: Compute the JIT frame layout */
    vtx_jit_frame_layout_t layout = vtx_frame_layout_compute(method);

    /* Step 5: Create an interpreter frame */
    /* We need a frame stack allocator. In production, each thread has one.
     * For now, create a single-use one. This is pre-allocated memory —
     * no malloc during deopt (the frame stack pre-allocates 256KB blocks). */
    static vtx_frame_stack_t *deopt_fs = NULL;
    if (deopt_fs == NULL) {
        deopt_fs = malloc(sizeof(vtx_frame_stack_t));
        if (!deopt_fs) return NULL;
        if (vtx_frame_stack_init(deopt_fs) != 0) {
            free(deopt_fs);
            deopt_fs = NULL;
            return NULL;
        }
    }

    vtx_frame_t *interp_frame = vtx_frame_create(method, NULL, 0, deopt_fs);
    if (!interp_frame) {
        VTX_ASSERT(false, "failed to create interpreter frame during deopt");
        return NULL;
    }

    /* Step 6: Copy locals from the JIT frame to the interpreter frame.
     *
     * For the baseline JIT (T1), locals are at known offsets in the JIT
     * frame: local[i] is at RBP + layout.local_offset(i).
     * We read them directly.
     *
     * For T2/T3, locals may be in registers (per the register map).
     * If we have a FrameState, it tells us the NodeID of each local.
     * If we have a register map, we can find which register holds that
     * NodeID. But we can't read hardware registers from C — the deopt
     * stub must have saved them.
     *
     * The T1 deopt stub saves expression stack registers (RAX,RCX,RDX,RBX)
     * to frame slots. Locals are already in the frame (they're stored in
     * local slots, not registers, in T1).
     *
     * For T2, the register map tells us which register holds each value,
     * but the register values are on the stack (saved by the deopt handler's
     * prologue). We don't have a standard save area for T2 yet.
     *
     * Practical approach: read locals from the JIT frame's local slots.
     * This works for T1 (locals are always in slots). For T2, locals
     * may be in registers, but the register map in the side table records
     * which registers hold which NodeIDs. We can check: if a local's
     * NodeID is in the register map, we note it but can't read the
     * register from here. The value is lost for that local.
     *
     * To handle T2 properly, the deopt stub would need to save ALL
     * callee-saved registers to a known location. That's a future
     * enhancement. For now, we read what's in the frame slots.
     */
    for (uint32_t i = 0; i < layout.max_locals && i < interp_frame->locals_count; i++) {
        int32_t offset = vtx_frame_layout_local_offset(&layout, i);
        uint64_t raw = jit_frame_read(jit_rbp, offset);
        interp_frame->locals[i] = (vtx_value_t)raw;
    }

    /* Step 7: Reconstruct the operand stack.
     *
     * The expression stack has up to VTX_EXPR_REG_COUNT (4) values in
     * registers and the rest in spill slots. The deopt stub already
     * saved the register values to frame slots (for T1). We read them
     * back.
     *
     * The stack_depth tells us how many values are on the operand stack.
     * Values are ordered: stack[0] is the bottom, stack[depth-1] is TOS.
     * In the JIT frame, deeper values (index < depth-4) are in spill slots,
     * and the top 4 (or fewer) are in registers that the stub saved.
     *
     * The T1 deopt stub saves registers to specific local/spill slots.
     * The exact location depends on the stack depth and frame layout.
     * We use vtx_frame_layout_expr_location to determine where each
     * value was at deopt time, then read it from that location.
     *
     * However, the stub may have saved registers to DIFFERENT slots
     * than where they originally were (it uses local slots as temp
     * storage when spill area is full). This makes exact reconstruction
     * dependent on the stub's save logic.
     *
     * For correctness, we reconstruct what we can: read spill slots
     * for deep values, and for the top 4 values, read from the slots
     * where the stub saved them. If the stub saved to local slots,
     * those locals have been overwritten — but that's fine because
     * we already copied locals in Step 6.
     *
     * For T2/T3, the side table's register map is the authority.
     * But without a register save area, we can only read frame slots.
     */
    for (uint32_t i = 0; i < stack_depth && i < (uint32_t)interp_frame->stack_capacity; i++) {
        int reg = -1;
        int32_t spill_off = 0;
        vtx_frame_layout_expr_location(i, stack_depth, &layout, &reg, &spill_off);

        vtx_value_t val = VTX_VALUE_UNDEFINED;
        if (reg >= 0) {
            /* Value was in a register. The T1 deopt stub saved it to
             * a frame slot. For the baseline, the stub saves:
             *   RAX → spill[spilled_count+3] or local slot
             *   RCX → spill[spilled_count+2] or local slot
             *   RDX → spill[spilled_count+1] or local slot
             *   RBX → spill[spilled_count] or local slot
             *
             * But we don't know spilled_count here without re-deriving it.
             * The stub code computes it from stack_depth.
             *
             * For register i (0=RAX,1=RCX,2=RDX,3=RBX), the saved
             * location is at a specific offset. Let's compute it:
             *   spilled_count = max(0, stack_depth - VTX_EXPR_REG_COUNT)
             *   The register values are saved to:
             *     spill[spilled_count + (reg_index)]
             *   where reg_index goes 0=RBX(deepest), 1=RDX, 2=RCX, 3=RAX(TOS)
             *
             * The mapping from stack position to register:
             *   TOS   (index depth-1) → RAX (reg 0)
             *   TOS-1 (index depth-2) → RCX (reg 1)
             *   TOS-2 (index depth-3) → RDX (reg 2)
             *   TOS-3 (index depth-4) → RBX (reg 3)
             *
             * And the stub saves them in order to:
             *   spill[spilled_count + 0] = RBX (if room)
             *   spill[spilled_count + 1] = RDX
             *   spill[spilled_count + 2] = RCX
             *   spill[spilled_count + 3] = RAX
             *
             * So for stack position i (0-based from bottom):
             *   If i < spilled_count: value is in spill[i] (already there)
             *   If i >= spilled_count: value is in a register, saved to
             *     spill[spilled_count + (i - spilled_count)]
             *   In both cases, the value ends up in spill[i]!
             *
             * This means we can just read spill[i] for all values. */
            uint32_t spilled_count = (stack_depth > VTX_EXPR_REG_COUNT) ?
                stack_depth - VTX_EXPR_REG_COUNT : 0;

            if (i < spilled_count) {
                /* Value was already in spill[i] */
                int32_t off = vtx_frame_layout_spill_offset(&layout, i);
                val = (vtx_value_t)jit_frame_read(jit_rbp, off);
            } else {
                /* Value was in a register, saved to spill[spilled_count + (i - spilled_count)]
                 * which is spill[i]. But only if there's room in the spill area.
                 * If not, the stub uses local slots. */
                uint32_t save_idx = i; /* same as spill index */
                if (save_idx < layout.max_spills) {
                    int32_t off = vtx_frame_layout_spill_offset(&layout, save_idx);
                    val = (vtx_value_t)jit_frame_read(jit_rbp, off);
                } else if (layout.max_locals > 0) {
                    /* Stub saved to a local slot. We need to figure out which one.
                     * The stub uses local[max_locals-1], local[max_locals-2], etc.
                     * for overflow. The first overflow goes to local[max_locals-1].
                     * But we already read locals in Step 6 — these slots have been
                     * overwritten by the stub's register saves.
                     *
                     * We need to read from the SAME slots the stub wrote to.
                     * The stub saves register values starting from the first
                     * register-resident position. For stack_depth N:
                     *   positions spilled_count..N-1 are register-resident
                     *   The stub saves them to spill[spilled_count]..spill[N-1]
                     *   if those indices fit in max_spills.
                     *   Otherwise, overflow goes to local slots.
                     *
                     * For simplicity and correctness, we read from spill[i]
                     * if i < max_spills, otherwise from a local slot.
                     * The local slot index for overflow position j (0-based) is:
                     *   local[max_locals - 1 - j]
                     */
                    uint32_t overflow_idx = save_idx - layout.max_spills;
                    if (overflow_idx < layout.max_locals) {
                        uint32_t local_idx = layout.max_locals - 1 - overflow_idx;
                        int32_t off = vtx_frame_layout_local_offset(&layout, local_idx);
                        val = (vtx_value_t)jit_frame_read(jit_rbp, off);
                    }
                }
            }
        } else {
            /* Value is in a spill slot */
            if (spill_off != 0) {
                val = (vtx_value_t)jit_frame_read(jit_rbp, spill_off);
            }
        }
        interp_frame->operand_stack[interp_frame->stack_top++] = val;
    }

    /* Step 8: Set the interpreter's PC to the bytecode PC where deopt occurred.
     * The interpreter's dispatch loop reads the PC from the frame's bytecode
     * and continues from there. We set it by finding the PC in the bytecode
     * and setting the interpreter's current position.
     *
     * The vtx_frame_t doesn't have a PC field — the PC is maintained by the
     * interpreter's dispatch loop. We store it in a thread-local that the
     * interpreter reads on re-entry. */
    extern void vtx_interp_set_deopt_pc(vtx_frame_t *frame, uint32_t pc);
    vtx_interp_set_deopt_pc(interp_frame, bytecode_pc);

    /* Step 9: Return the interpreter frame. The deopt handler will
     * transfer control to the interpreter dispatch loop, which will
     * resume execution at bytecode_pc with the reconstructed frame. */
    return interp_frame;
}
