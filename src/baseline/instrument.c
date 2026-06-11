#include "baseline/instrument.h"
#include "baseline/codegen.h"
#include <string.h>

/* ========================================================================== */
/* Helper macros for x86-64 encoding                                          */
/* ========================================================================== */

#define REX_W      0x48
#define REX_WR     0x4C
#define REX_WB     0x49

static inline uint8_t modrm(uint8_t mod, uint8_t reg, uint8_t rm)
{
    return (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

/**
 * Emit REX prefix for 64-bit operation with register operands.
 */
static void emit_rex64(vtx_code_buffer_t *buf, vtx_reg_t reg, vtx_reg_t rm)
{
    uint8_t rex = REX_W;
    if (reg >= VTX_REG_R8) rex |= 0x04; /* REX.R */
    if (rm >= VTX_REG_R8)  rex |= 0x01; /* REX.B */
    vtx_code_buffer_emit_byte(buf, rex);
}

/**
 * Emit: add qword ptr [base + offset], 1
 * Increment a 64-bit counter in memory by 1.
 */
static void emit_inc_qword_mem(vtx_code_buffer_t *buf, vtx_reg_t base,
                                int32_t offset)
{
    /* REX.W + 83 /0 (ADD r/m64, imm8) with imm8=1 */
    /* Or: REX.W + FF /0 (INC r/m64) */
    emit_rex64(buf, (vtx_reg_t)0, base);

    vtx_code_buffer_emit_byte(buf, 0xFF); /* INC r/m64 */
    if (offset >= -128 && offset <= 127) {
        vtx_code_buffer_emit_byte(buf, modrm(1, 0, base)); /* /0 = INC */
        vtx_code_buffer_emit_byte(buf, (uint8_t)(offset & 0xFF));
    } else {
        vtx_code_buffer_emit_byte(buf, modrm(2, 0, base)); /* /0 = INC */
        vtx_code_buffer_emit_dword(buf, (uint32_t)offset);
    }
}

/**
 * Emit: add dword ptr [base + offset], 1
 * Increment a 32-bit counter in memory by 1.
 */
static void emit_inc_dword_mem(vtx_code_buffer_t *buf, vtx_reg_t base,
                                int32_t offset)
{
    /* 83 /0 (ADD r/m32, imm8) with imm8=1 */
    /* Or: FF /0 (INC r/m32) */
    /* Need REX only for extended registers */
    uint8_t rex = 0x40;
    bool need_rex = false;
    if (base >= VTX_REG_R8) { rex |= 0x01; need_rex = true; }
    if (need_rex) vtx_code_buffer_emit_byte(buf, rex);

    vtx_code_buffer_emit_byte(buf, 0xFF); /* INC r/m32 */
    if (offset >= -128 && offset <= 127) {
        vtx_code_buffer_emit_byte(buf, modrm(1, 0, base));
        vtx_code_buffer_emit_byte(buf, (uint8_t)(offset & 0xFF));
    } else {
        vtx_code_buffer_emit_byte(buf, modrm(2, 0, base));
        vtx_code_buffer_emit_dword(buf, (uint32_t)offset);
    }
}

/**
 * Emit: mov reg, [rbp + offset]  (64-bit)
 * Load a value from the JIT frame.
 */
static void emit_load_from_frame64(vtx_code_buffer_t *buf, vtx_reg_t reg,
                                    int32_t offset)
{
    emit_rex64(buf, reg, VTX_REG_RBP);
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
 * Emit: mov reg, imm64
 * Load a 64-bit immediate into a register.
 */
static void emit_mov_reg_imm64(vtx_code_buffer_t *buf, vtx_reg_t reg,
                                uint64_t imm64)
{
    uint8_t rex = REX_W;
    if (reg >= VTX_REG_R8) rex |= 0x01; /* REX.B */
    vtx_code_buffer_emit_byte(buf, rex);
    vtx_code_buffer_emit_byte(buf, (uint8_t)(0xB8 | (reg & 7)));
    vtx_code_buffer_emit_qword(buf, imm64);
}

/**
 * Emit: mov reg, imm32 (zero-extended to 64 bits)
 */
static void emit_mov_reg_imm32(vtx_code_buffer_t *buf, vtx_reg_t reg,
                                uint32_t imm32)
{
    if (reg >= VTX_REG_R8) {
        vtx_code_buffer_emit_byte(buf, 0x41); /* REX.B */
    }
    vtx_code_buffer_emit_byte(buf, (uint8_t)(0xB8 | (reg & 7)));
    vtx_code_buffer_emit_dword(buf, imm32);
}

/* ========================================================================== */
/* Invocation counter increment                                                */
/* ========================================================================== */

void vtx_instrument_emit_invocation_increment(vtx_code_buffer_t *buf,
                                               vtx_profile_data_t *profile_data)
{
    /*
     * Increment the invocation_count in the profile data.
     * The profile_data pointer is in the JIT frame at [RBP + VTX_FRAME_PROFILE_DATA_OFFSET].
     *
     * If profile_data is provided directly (non-NULL), we can use it as
     * a direct address. Otherwise, we load it from the frame.
     *
     * Generated code:
     *   mov rax, [rbp + VTX_FRAME_PROFILE_DATA_OFFSET]  ; load profile_data ptr
     *   inc qword ptr [rax + VTX_PD_INVOCATION_COUNT_OFFSET]
     *
     * Saturating increment: we use INC which will not overflow to 0
     * from UINT64_MAX because INC sets OF (overflow flag) but wraps.
     * For true saturating behavior, we would need:
     *   add [rax + offset], 1
     *   jno skip
     *   mov qword ptr [rax + offset], UINT64_MAX
     *   skip:
     *
     * However, at 1 billion invocations per second, UINT64_MAX would
     * take ~584 years to reach. We skip the saturating check for
     * performance and just use a simple INC.
     */

    if (profile_data != NULL) {
        /* Direct address — use mov rax, imm64 */
        emit_mov_reg_imm64(buf, VTX_REG_RAX, (uint64_t)(uintptr_t)profile_data);
    } else {
        /* Load from frame */
        emit_load_from_frame64(buf, VTX_REG_RAX, VTX_FRAME_PROFILE_DATA_OFFSET);
    }

    /* inc qword ptr [rax + invocation_count_offset] */
    emit_inc_qword_mem(buf, VTX_REG_RAX, VTX_PD_INVOCATION_COUNT_OFFSET);
}

/* ========================================================================== */
/* Call site type recording                                                    */
/* ========================================================================== */

void vtx_instrument_emit_call_type_record(vtx_code_buffer_t *buf,
                                           vtx_profile_data_t *profile_data,
                                           uint32_t call_site_pc,
                                           vtx_reg_t receiver_reg,
                                           vtx_reg_t typeid_reg)
{
    /*
     * Record the receiver TypeID at a call site in the profile data.
     *
     * The profile_data.call_site_types is an array of vtx_call_site_profile_t,
     * indexed by call site index (not PC). We use call_site_pc as a proxy
     * for the index for simplicity in the baseline JIT.
     *
     * Generated code:
     *   1. Load profile_data pointer
     *   2. Compute address of call_site_types[call_site_pc]
     *   3. Load current count from the call site profile
     *   4. If count < VTX_POLY_LIMIT:
     *      a. Store typeid at entries[count].typeid_
     *      b. Increment count
     *
     * For the baseline JIT, we simplify this: we use a helper function
     * call for the recording, to keep the inline code small.
     *
     * Actually, to avoid function call overhead, we emit inline code
     * that does a simple increment and store. We use a scratch register
     * (R8 or R10) for the computation.
     *
     * Simplified approach: just record the TypeID in the first available
     * slot. We use R10 as scratch (it's caller-saved and not used for
     * the expression stack in our design).
     */

    /* Save expression stack registers that we'll clobber.
     * RAX is used for loads; R10 is used as scratch.
     * We need to be careful not to lose expression stack values. */

    /* Step 1: Load profile_data pointer into R10 */
    if (profile_data != NULL) {
        emit_mov_reg_imm64(buf, VTX_REG_R10, (uint64_t)(uintptr_t)profile_data);
    } else {
        emit_load_from_frame64(buf, VTX_REG_R10, VTX_FRAME_PROFILE_DATA_OFFSET);
    }

    /* Step 2: Compute address of call_site_types[call_site_pc]
     * call_site_types is at profile_data + VTX_PD_CALL_SITE_TYPES_OFFSET
     * Each entry is sizeof(vtx_call_site_profile_t) bytes
     * The call_site_pc is used as an index (simplified).
     *
     * For the baseline JIT, we store the typeid at a fixed slot
     * rather than doing the full polymorphic tracking inline.
     * This is sufficient for the T2 compiler to get type feedback.
     *
     * Simplification: We store at entries[0].typeid_ = typeid
     * and set count = 1 if count was 0.
     *
     * address = profile_data + call_site_types_offset + call_site_pc * sizeof(vtx_call_site_profile_t)
     *
     * For small call_site_pc values, we can use immediate multiplication.
     * sizeof(vtx_call_site_profile_t) = VTX_POLY_LIMIT * sizeof(vtx_ic_entry_t) + sizeof(uint32_t)
     * = 4 * 16 + 4 = 68 bytes (approximately)
     *
     * Actually, the layout is:
     *   vtx_ic_entry_t entries[VTX_POLY_LIMIT]; // each 16 bytes (typeid_t + pointer)
     *   uint32_t count;
     * So sizeof = VTX_POLY_LIMIT * 16 + 4 = 68, padded to 72 or so.
     *
     * To keep things simple, we emit a call to the profiler function instead.
     * The call overhead is acceptable since it only happens on calls (which
     * are already expensive).
     */

    /* Push caller-saved registers that might hold expression stack values.
     * Our expression stack uses RAX, RCX, RDX, RBX. We need to save these
     * across the function call. */
    /* push rax */ vtx_code_buffer_emit_byte(buf, 0x50);
    /* push rcx */ vtx_code_buffer_emit_byte(buf, 0x51);
    /* push rdx */ vtx_code_buffer_emit_byte(buf, 0x52);
    /* push rbx */ vtx_code_buffer_emit_byte(buf, 0x53);

    /* Set up function call arguments (System V ABI):
     *   RDI = profile_data pointer
     *   RSI = method pointer (from frame)
     *   RDX = call_site_pc
     *   RCX = typeid
     */
    /* mov rdi, r10 (profile_data) */
    vtx_code_buffer_emit_byte(buf, REX_W);
    vtx_code_buffer_emit_byte(buf, 0x89);
    vtx_code_buffer_emit_byte(buf, modrm(3, VTX_REG_R10, VTX_REG_RDI));

    /* mov rsi, [rbp + VTX_FRAME_METHOD_PTR_OFFSET] */
    emit_load_from_frame64(buf, VTX_REG_RSI, VTX_FRAME_METHOD_PTR_OFFSET);

    /* mov rdx, call_site_pc */
    emit_mov_reg_imm32(buf, VTX_REG_RDX, call_site_pc);

    /* mov rcx, typeid_reg (if available) or extract from receiver */
    if (typeid_reg != VTX_REG_NONE) {
        vtx_code_buffer_emit_byte(buf, REX_W);
        vtx_code_buffer_emit_byte(buf, 0x89);
        vtx_code_buffer_emit_byte(buf, modrm(3, typeid_reg, VTX_REG_RCX));
    } else {
        /* Need to extract typeid from the receiver object.
         * This requires: untag the heap pointer, load type_id from offset 0.
         * For simplicity, we use 0 as a placeholder typeid — the profiler
         * function will extract it from the tagged value. */
        vtx_code_buffer_emit_byte(buf, REX_W);
        vtx_code_buffer_emit_byte(buf, 0x89);
        vtx_code_buffer_emit_byte(buf, modrm(3, receiver_reg, VTX_REG_RCX));
    }

    /* Call the profiler function */
    extern void vtx_profiler_record_call_type(vtx_profile_data_t *,
                                               const vtx_method_desc_t *,
                                               uint32_t, vtx_typeid_t);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_profiler_record_call_type);
    vtx_code_buffer_emit_byte(buf, 0xFF); /* CALL r/m64 */
    vtx_code_buffer_emit_byte(buf, modrm(3, 2, VTX_REG_RAX));

    /* Restore expression stack registers */
    /* pop rbx */ vtx_code_buffer_emit_byte(buf, 0x5B);
    /* pop rdx */ vtx_code_buffer_emit_byte(buf, 0x5A);
    /* pop rcx */ vtx_code_buffer_emit_byte(buf, 0x59);
    /* pop rax */ vtx_code_buffer_emit_byte(buf, 0x58);
}

/* ========================================================================== */
/* Branch outcome recording                                                    */
/* ========================================================================== */

void vtx_instrument_emit_branch_record(vtx_code_buffer_t *buf,
                                        vtx_profile_data_t *profile_data,
                                        uint32_t branch_pc,
                                        bool taken)
{
    /*
     * Record a branch outcome. Increment both the total count and
     * (if taken) the taken count.
     *
     * The branch counts are stored as uint32_t arrays indexed by
     * branch_pc. Since the PC can be large, we compute:
     *   branch_total_counts[branch_pc] += 1
     *   if taken: branch_taken_counts[branch_pc] += 1
     *
     * For the baseline JIT, we emit inline code for the total count
     * increment and conditionally for the taken count.
     */

    /* Load profile_data pointer into R10 */
    if (profile_data != NULL) {
        emit_mov_reg_imm64(buf, VTX_REG_R10, (uint64_t)(uintptr_t)profile_data);
    } else {
        emit_load_from_frame64(buf, VTX_REG_R10, VTX_FRAME_PROFILE_DATA_OFFSET);
    }

    /* Compute address: branch_total_counts + branch_pc * 4
     * R10 = profile_data
     * R11 = profile_data->branch_total_counts
     * Then: [R11 + branch_pc * 4] += 1
     */
    /* mov r11, [r10 + VTX_PD_BRANCH_TOTAL_COUNTS_OFFSET] */
    emit_rex64(buf, VTX_REG_R11, VTX_REG_R10);
    vtx_code_buffer_emit_byte(buf, 0x8B); /* MOV r64, r/m64 */
    vtx_code_buffer_emit_byte(buf, modrm(2, VTX_REG_R11, VTX_REG_R10));
    vtx_code_buffer_emit_dword(buf, VTX_PD_BRANCH_TOTAL_COUNTS_OFFSET);

    /* inc dword ptr [r11 + branch_pc * 4] */
    /* For small branch_pc values, compute offset directly */
    int32_t total_offset = (int32_t)(branch_pc * sizeof(uint32_t));
    emit_inc_dword_mem(buf, VTX_REG_R11, total_offset);

    if (taken) {
        /* Also increment the taken count */
        /* mov r11, [r10 + VTX_PD_BRANCH_TAKEN_COUNTS_OFFSET] */
        emit_rex64(buf, VTX_REG_R11, VTX_REG_R10);
        vtx_code_buffer_emit_byte(buf, 0x8B);
        vtx_code_buffer_emit_byte(buf, modrm(2, VTX_REG_R11, VTX_REG_R10));
        vtx_code_buffer_emit_dword(buf, VTX_PD_BRANCH_TAKEN_COUNTS_OFFSET);

        emit_inc_dword_mem(buf, VTX_REG_R11, total_offset);
    }
}

/* ========================================================================== */
/* Backward branch counter increment                                           */
/* ========================================================================== */

void vtx_instrument_emit_backward_branch_increment(vtx_code_buffer_t *buf,
                                                     vtx_profile_data_t *profile_data)
{
    /*
     * Increment the backward_branch_count.
     * This is the same as the invocation increment but using a different offset.
     */

    if (profile_data != NULL) {
        emit_mov_reg_imm64(buf, VTX_REG_RAX, (uint64_t)(uintptr_t)profile_data);
    } else {
        emit_load_from_frame64(buf, VTX_REG_RAX, VTX_FRAME_PROFILE_DATA_OFFSET);
    }

    /* inc qword ptr [rax + backward_branch_count_offset] */
    emit_inc_qword_mem(buf, VTX_REG_RAX, VTX_PD_BACKWARD_BRANCH_COUNT_OFFSET);
}
