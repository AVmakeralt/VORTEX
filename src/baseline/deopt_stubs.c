#include "baseline/deopt_stubs.h"
#include "baseline/codegen.h"
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
static void emit_jmp_rel32(vtx_code_buffer_t *buf, uint64_t target_addr)
{
    vtx_code_buffer_emit_byte(buf, 0xE9);
    /* The offset is relative to the instruction after the jmp (5 bytes) */
    uint32_t current_pos = vtx_code_buffer_position(buf);
    int32_t rel32 = (int32_t)(target_addr - (uint64_t)(current_pos + 4));
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

    /* Save registers that hold expression stack values to spill slots.
     * We use the spill area as temporary storage for register values.
     * Register value positions (from bottom of stack):
     *   reg TOS-3 (RBX) → position spilled_count     → spill[spilled_count]
     *   reg TOS-2 (RDX) → position spilled_count+1   → spill[spilled_count+1]
     *   reg TOS-1 (RCX) → position spilled_count+2   → spill[spilled_count+2]
     *   reg TOS   (RAX) → position spilled_count+3   → spill[spilled_count+3]
     */
    if (depth >= 4) {
        int32_t off_bx = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count);
        int32_t off_dx = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count + 1);
        int32_t off_cx = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count + 2);
        int32_t off_ax = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count + 3);

        /* Save RBX first (since we need it), then RDX, RCX, RAX */
        emit_store_to_frame(buf, off_bx, VTX_REG_RBX);
        emit_store_to_frame(buf, off_dx, VTX_REG_RDX);
        emit_store_to_frame(buf, off_cx, VTX_REG_RCX);
        emit_store_to_frame(buf, off_ax, VTX_REG_RAX);
    } else if (depth == 3) {
        int32_t off_bx = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count);
        int32_t off_dx = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count + 1);
        int32_t off_cx = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count + 2);

        emit_store_to_frame(buf, off_bx, VTX_REG_RBX);
        emit_store_to_frame(buf, off_dx, VTX_REG_RDX);
        emit_store_to_frame(buf, off_cx, VTX_REG_RCX);
    } else if (depth == 2) {
        int32_t off_bx = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count);
        int32_t off_dx = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count + 1);

        emit_store_to_frame(buf, off_bx, VTX_REG_RBX);
        emit_store_to_frame(buf, off_dx, VTX_REG_RDX);
    } else if (depth == 1) {
        int32_t off_bx = vtx_frame_layout_spill_offset(&ctx->frame_layout, spilled_count);
        emit_store_to_frame(buf, off_bx, VTX_REG_RBX);
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

        /* jmp to interpreter entry */
        emit_jmp_rel32(buf, (uint64_t)(uintptr_t)g_interp_entry);
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
void *vtx_deopt_runtime_transition(void *jit_rbp, uint32_t native_pc)
{
    uint8_t *rbp = (uint8_t *)jit_rbp;

    /* Read deopt info from frame header */
    vtx_deopt_info_t *deopt_info = *(vtx_deopt_info_t **)(
        rbp + VTX_FRAME_DEOPT_INFO_OFFSET);

    if (!deopt_info) {
        /* No deopt info — fatal error */
        VTX_ASSERT(false, "deopt_info is NULL in JIT frame");
        return NULL;
    }

    /* Look up the bytecode PC for this native PC offset */
    uint32_t bytecode_pc = 0;
    uint32_t stack_depth = 0;
    bool found = false;

    /* Binary search in the pc_map (sorted by native_offset) */
    uint32_t lo = 0, hi = deopt_info->pc_map_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (deopt_info->pc_map[mid] <= native_pc) {
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

    /*
     * At this point we have the bytecode PC and stack depth.
     * The full interpreter frame reconstruction would involve:
     *
     * 1. Allocating an interpreter frame
     * 2. Copying locals from the JIT frame to the interpreter frame
     * 3. Reconstructing the operand stack from spill slots and registers
     * 4. Setting the interpreter's PC and frame pointer
     *
     * This requires access to the interpreter's frame structure,
     * which is defined in src/interp/frame.h. For the baseline JIT,
     * we delegate to the interpreter module's deopt entry point.
     *
     * The interpreter's deopt entry point reads the bytecode PC from
     * a thread-local storage location and starts dispatching from there.
     *
     * For now, we return the JIT RBP and let the interpreter's
     * deopt handler do the rest. The real implementation would
     * call a full deopt reconstruction function.
     */
    (void)stack_depth;

    /* Return the JIT RBP — the interpreter's deopt handler will
     * use it to reconstruct the interpreter frame. */
    return jit_rbp;
}
