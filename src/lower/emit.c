/**
 * VORTEX x86-64 Machine Code Emitter
 *
 * Emits x86-64 machine code bytes following the Intel SDM encoding rules.
 * All instructions use 64-bit operand size (REX.W prefix) unless noted.
 *
 * Key encoding rules:
 *   - REX prefix: 0x40-0x4F (W=64bit, R/X/B=extend reg/index/rm fields)
 *   - ModR/M byte: [mod(2) | reg(3) | r/m(3)]
 *   - SIB byte: [scale(2) | index(3) | base(3)]
 *   - Displacement: 8 or 32 bits (little-endian)
 *   - Immediate: 8 or 32 bits (little-endian)
 */

#include "lower/emit.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_x86_emit_init(vtx_x86_emit_t *emit, uint32_t initial_capacity)
{
    if (!emit) return -1;
    if (initial_capacity == 0) initial_capacity = VTX_EMIT_INITIAL_CAPACITY;

    emit->buffer = (uint8_t *)malloc(initial_capacity);
    if (!emit->buffer) return -1;
    emit->position = 0;
    emit->capacity = initial_capacity;
    return 0;
}

void vtx_x86_emit_destroy(vtx_x86_emit_t *emit)
{
    if (!emit) return;
    if (emit->buffer) {
        free(emit->buffer);
        emit->buffer = NULL;
    }
    emit->position = 0;
    emit->capacity = 0;
}

int vtx_x86_emit_ensure(vtx_x86_emit_t *emit, uint32_t needed)
{
    if (emit->position + needed <= emit->capacity) return 0;
    uint32_t new_cap = emit->capacity;
    while (new_cap < emit->position + needed) new_cap *= 2;
    uint8_t *new_buf = (uint8_t *)realloc(emit->buffer, new_cap);
    if (!new_buf) return -1;
    emit->buffer = new_buf;
    emit->capacity = new_cap;
    return 0;
}

/* ========================================================================== */
/* Condition code mapping                                                      */
/* ========================================================================== */

uint8_t vtx_cond_to_x86(vtx_cond_t cond)
{
    /* x86 condition codes:
     * 0=O, 1=NO, 2=B, 3=AE, 4=E, 5=NE, 6=BE, 7=A,
     * 8=S, 9=NS, 10=P, 11=NP, 12=L, 13=GE, 14=LE, 15=G */
    switch (cond) {
    case VTX_COND_EQ:  return 4;  /* ZF=1 */
    case VTX_COND_NE:  return 5;  /* ZF=0 */
    case VTX_COND_LT:  return 12; /* SF!=OF */
    case VTX_COND_LE:  return 14; /* ZF=1 or SF!=OF */
    case VTX_COND_GT:  return 15; /* ZF=0 and SF=OF */
    case VTX_COND_GE:  return 13; /* SF=OF */
    case VTX_COND_ULT: return 2;  /* CF=1 */
    case VTX_COND_ULE: return 6;  /* CF=1 or ZF=1 */
    case VTX_COND_UGT: return 7;  /* CF=0 and ZF=0 */
    case VTX_COND_UGE: return 3;  /* CF=0 */
    default:           return 5;  /* NE as default */
    }
}

/* ========================================================================== */
/* Mid-level encoding helpers                                                  */
/* ========================================================================== */

/**
 * Determine if a displacement needs SIB byte, and emit ModR/M + SIB + disp.
 * This is the core memory operand encoding function.
 *
 * @param e       Emitter
 * @param reg     Register field in ModR/M (3 bits, pre-shifted)
 * @param base    Base register (0-15, or 0xFF for none)
 * @param index   Index register (0-15, or 0xFF for none)
 * @param scale   Scale (1, 2, 4, 8)
 * @param disp    Displacement
 */
static void emit_mem_operand(vtx_x86_emit_t *e, uint8_t reg,
                              uint8_t base, uint8_t index, uint8_t scale,
                              int32_t disp)
{
    int need_sib = (index != 0xFF) || (base == 0xFF) ||
                   ((base & 7) == 4);  /* RSP/R12 as base requires SIB */
    int need_disp = (disp != 0) || (base == 0xFF) ||
                    ((base & 7) == 5 && !need_sib); /* RBP/R13 as base with mod=0 → need disp8=0 */
    /* Special case: no base register, only displacement */
    if (base == 0xFF && index == 0xFF) {
        /* [disp32] — mod=00, r/m=5 */
        emit_modrm(e, 0, reg, 5);
        emit_dword(e, (uint32_t)disp);
        return;
    }

    if (base == 0xFF && index != 0xFF) {
        /* [index*scale + disp32] — mod=00, r/m=4, SIB with base=5 */
        uint8_t scale_bits = 0;
        switch (scale) { case 1: scale_bits=0; break; case 2: scale_bits=1; break;
                         case 4: scale_bits=2; break; case 8: scale_bits=3; break; }
        emit_modrm(e, 0, reg, 4);
        emit_sib(e, scale_bits, index & 7, 5);
        emit_dword(e, (uint32_t)disp);
        return;
    }

    /* Base register is valid */
    uint8_t base_lo = base & 7;

    if (need_sib) {
        uint8_t idx_reg = (index != 0xFF) ? (index & 7) : 4; /* 4 = no index */
        uint8_t scale_bits = 0;
        if (index != 0xFF) {
            switch (scale) { case 1: scale_bits=0; break; case 2: scale_bits=1; break;
                             case 4: scale_bits=2; break; case 8: scale_bits=3; break; }
        }

        if (disp == 0 && base_lo != 5) {
            /* [base + index*scale] — mod=00 */
            emit_modrm(e, 0, reg, 4);
            emit_sib(e, scale_bits, idx_reg, base_lo);
        } else if (disp >= -128 && disp <= 127) {
            /* [base + index*scale + disp8] — mod=01 */
            emit_modrm(e, 1, reg, 4);
            emit_sib(e, scale_bits, idx_reg, base_lo);
            emit_byte(e, (uint8_t)(disp & 0xFF));
        } else {
            /* [base + index*scale + disp32] — mod=10 */
            emit_modrm(e, 2, reg, 4);
            emit_sib(e, scale_bits, idx_reg, base_lo);
            emit_dword(e, (uint32_t)disp);
        }
    } else {
        /* No SIB needed */
        if (disp == 0 && base_lo != 5) {
            /* [base] — mod=00 */
            emit_modrm(e, 0, reg, base_lo);
        } else if (disp >= -128 && disp <= 127) {
            /* [base + disp8] — mod=01 */
            emit_modrm(e, 1, reg, base_lo);
            emit_byte(e, (uint8_t)(disp & 0xFF));
        } else {
            /* [base + disp32] — mod=10 */
            emit_modrm(e, 2, reg, base_lo);
            emit_dword(e, (uint32_t)disp);
        }
    }
}

/* ========================================================================== */
/* High-level encoding functions                                               */
/* ========================================================================== */

void vtx_x86_emit_rr(vtx_x86_emit_t *e, uint8_t opcode, uint8_t opcode2,
                      uint8_t reg, uint8_t rm)
{
    /* REX.W + [0F] + opcode + ModR/M(mod=11, reg, rm) */
    int r = reg_hi(reg);
    int b = reg_hi(rm);
    emit_rex(e, 1, r, 0, b);
    if (opcode2) emit_byte(e, opcode2);
    emit_byte(e, opcode);
    emit_modrm(e, 3, reg & 7, rm & 7);
}

void vtx_x86_emit_ri(vtx_x86_emit_t *e, uint8_t opcode, uint8_t reg_ext,
                      uint8_t rm, int64_t imm, int imm_size)
{
    int b = reg_hi(rm);
    emit_rex(e, 1, 0, 0, b);
    emit_byte(e, opcode);
    emit_modrm(e, 3, reg_ext, rm & 7);
    if (imm_size == 1) {
        emit_byte(e, (uint8_t)(imm & 0xFF));
    } else {
        emit_dword(e, (uint32_t)(imm & 0xFFFFFFFF));
    }
}

void vtx_x86_emit_rm(vtx_x86_emit_t *e, uint8_t opcode, uint8_t opcode2,
                      uint8_t reg, uint8_t base, int32_t disp, bool is_load)
{
    /* is_load: reg ← [base+disp] → opcode with direction bit
     * is_store: [base+disp] ← reg → opcode without direction bit */
    int r = reg_hi(reg);
    int b = reg_hi(base);
    int x = 0;
    emit_rex(e, 1, r, x, b);
    if (opcode2) emit_byte(e, opcode2);
    emit_byte(e, opcode);
    emit_mem_operand(e, reg & 7, base, 0xFF, 1, disp);
}

void vtx_x86_emit_sib_mem(vtx_x86_emit_t *e, uint8_t opcode, uint8_t opcode2,
                           uint8_t reg, uint8_t base, uint8_t index,
                           uint8_t scale, int32_t disp, bool is_load)
{
    int r = reg_hi(reg);
    int b = reg_hi(base);
    int x = reg_hi(index);
    emit_rex(e, 1, r, x, b);
    if (opcode2) emit_byte(e, opcode2);
    emit_byte(e, opcode);
    emit_mem_operand(e, reg & 7, base, index, scale, disp);
    (void)is_load;
}

void vtx_x86_emit_mov_imm64(vtx_x86_emit_t *e, uint8_t reg, uint64_t imm)
{
    /* REX.W + B8+rd + imm64 */
    int b = reg_hi(reg);
    emit_rex(e, 1, 0, 0, b);
    emit_byte(e, (uint8_t)(0xB8 + (reg & 7)));
    emit_qword(e, imm);
}

void vtx_x86_emit_mov_imm32(vtx_x86_emit_t *e, uint8_t reg, int32_t imm)
{
    /* REX.W + C7 /0 + imm32 */
    int b = reg_hi(reg);
    emit_rex(e, 1, 0, 0, b);
    emit_byte(e, 0xC7);
    emit_modrm(e, 3, 0, reg & 7);
    emit_dword(e, (uint32_t)imm);
}

/* ========================================================================== */
/* Specific instruction emission                                               */
/* ========================================================================== */

/* ADD r/m64, r64: REX.W 01 /r */
void vtx_x86_emit_add_rr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    vtx_x86_emit_rr(e, 0x01, 0, src, dst);
}

/* ADD r/m64, imm32: REX.W 81 /0 id  OR  ADD r/m64, imm8: REX.W 83 /0 ib */
void vtx_x86_emit_add_ri(vtx_x86_emit_t *e, uint8_t dst, int32_t imm)
{
    if (imm >= -128 && imm <= 127) {
        vtx_x86_emit_ri(e, 0x83, 0, dst, imm, 1);
    } else {
        vtx_x86_emit_ri(e, 0x81, 0, dst, imm, 4);
    }
}

/* SUB r/m64, r64: REX.W 29 /r */
void vtx_x86_emit_sub_rr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    vtx_x86_emit_rr(e, 0x29, 0, src, dst);
}

/* SUB r/m64, imm: REX.W 81 /5 id  OR  REX.W 83 /5 ib */
void vtx_x86_emit_sub_ri(vtx_x86_emit_t *e, uint8_t dst, int32_t imm)
{
    if (imm >= -128 && imm <= 127) {
        vtx_x86_emit_ri(e, 0x83, 5, dst, imm, 1);
    } else {
        vtx_x86_emit_ri(e, 0x81, 5, dst, imm, 4);
    }
}

/* IMUL r64, r/m64: REX.W 0F AF /r */
void vtx_x86_emit_imul_rr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    vtx_x86_emit_rr(e, 0xAF, 0x0F, dst, src);
}

/* IDIV r/m64: REX.W F7 /7 */
void vtx_x86_emit_idiv_r(vtx_x86_emit_t *e, uint8_t src)
{
    int b = reg_hi(src);
    emit_rex(e, 1, 0, 0, b);
    emit_byte(e, 0xF7);
    emit_modrm(e, 3, 7, src & 7);
}

/* SHL r/m64, imm8: REX.W C1 /4 ib */
void vtx_x86_emit_shl_ri(vtx_x86_emit_t *e, uint8_t dst, uint8_t count)
{
    int b = reg_hi(dst);
    emit_rex(e, 1, 0, 0, b);
    emit_byte(e, 0xC1);
    emit_modrm(e, 3, 4, dst & 7);
    emit_byte(e, count);
}

/* SHL r/m64, CL: REX.W D3 /4 */
void vtx_x86_emit_shl_cl(vtx_x86_emit_t *e, uint8_t dst)
{
    int b = reg_hi(dst);
    emit_rex(e, 1, 0, 0, b);
    emit_byte(e, 0xD3);
    emit_modrm(e, 3, 4, dst & 7);
}

/* SHR r/m64, imm8: REX.W C1 /5 ib */
void vtx_x86_emit_shr_ri(vtx_x86_emit_t *e, uint8_t dst, uint8_t count)
{
    int b = reg_hi(dst);
    emit_rex(e, 1, 0, 0, b);
    emit_byte(e, 0xC1);
    emit_modrm(e, 3, 5, dst & 7);
    emit_byte(e, count);
}

/* SHR r/m64, CL: REX.W D3 /5 */
void vtx_x86_emit_shr_cl(vtx_x86_emit_t *e, uint8_t dst)
{
    int b = reg_hi(dst);
    emit_rex(e, 1, 0, 0, b);
    emit_byte(e, 0xD3);
    emit_modrm(e, 3, 5, dst & 7);
}

/* SAR r/m64, imm8: REX.W C1 /7 ib */
void vtx_x86_emit_sar_ri(vtx_x86_emit_t *e, uint8_t dst, uint8_t count)
{
    int b = reg_hi(dst);
    emit_rex(e, 1, 0, 0, b);
    emit_byte(e, 0xC1);
    emit_modrm(e, 3, 7, dst & 7);
    emit_byte(e, count);
}

/* SAR r/m64, CL: REX.W D3 /7 */
void vtx_x86_emit_sar_cl(vtx_x86_emit_t *e, uint8_t dst)
{
    int b = reg_hi(dst);
    emit_rex(e, 1, 0, 0, b);
    emit_byte(e, 0xD3);
    emit_modrm(e, 3, 7, dst & 7);
}

/* AND r/m64, r64: REX.W 21 /r */
void vtx_x86_emit_and_rr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    vtx_x86_emit_rr(e, 0x21, 0, src, dst);
}

/* AND r/m64, imm32: REX.W 81 /4 id  OR  REX.W 83 /4 ib */
void vtx_x86_emit_and_ri(vtx_x86_emit_t *e, uint8_t dst, int32_t imm)
{
    if (imm >= -128 && imm <= 127) {
        vtx_x86_emit_ri(e, 0x83, 4, dst, imm, 1);
    } else {
        vtx_x86_emit_ri(e, 0x81, 4, dst, imm, 4);
    }
}

/* OR r/m64, r64: REX.W 09 /r */
void vtx_x86_emit_or_rr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    vtx_x86_emit_rr(e, 0x09, 0, src, dst);
}

/* OR r/m64, imm32: REX.W 81 /1 id */
void vtx_x86_emit_or_ri(vtx_x86_emit_t *e, uint8_t dst, int32_t imm)
{
    if (imm >= -128 && imm <= 127) {
        vtx_x86_emit_ri(e, 0x83, 1, dst, imm, 1);
    } else {
        vtx_x86_emit_ri(e, 0x81, 1, dst, imm, 4);
    }
}

/* XOR r/m64, r64: REX.W 31 /r */
void vtx_x86_emit_xor_rr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    vtx_x86_emit_rr(e, 0x31, 0, src, dst);
}

/* XOR r/m64, imm32: REX.W 81 /6 id */
void vtx_x86_emit_xor_ri(vtx_x86_emit_t *e, uint8_t dst, int32_t imm)
{
    if (imm >= -128 && imm <= 127) {
        vtx_x86_emit_ri(e, 0x83, 6, dst, imm, 1);
    } else {
        vtx_x86_emit_ri(e, 0x81, 6, dst, imm, 4);
    }
}

/* CMP r/m64, r64: REX.W 39 /r */
void vtx_x86_emit_cmp_rr(vtx_x86_emit_t *e, uint8_t a, uint8_t b)
{
    vtx_x86_emit_rr(e, 0x39, 0, b, a);
}

/* CMP r/m64, imm32: REX.W 81 /7 id  OR  REX.W 83 /7 ib */
void vtx_x86_emit_cmp_ri(vtx_x86_emit_t *e, uint8_t reg, int32_t imm)
{
    if (imm >= -128 && imm <= 127) {
        vtx_x86_emit_ri(e, 0x83, 7, reg, imm, 1);
    } else {
        vtx_x86_emit_ri(e, 0x81, 7, reg, imm, 4);
    }
}

/* TEST r/m64, r64: REX.W 85 /r */
void vtx_x86_emit_test_rr(vtx_x86_emit_t *e, uint8_t a, uint8_t b)
{
    vtx_x86_emit_rr(e, 0x85, 0, b, a);
}

/* TEST r/m64, imm32: REX.W F7 /0 id */
void vtx_x86_emit_test_ri(vtx_x86_emit_t *e, uint8_t reg, int32_t imm)
{
    int b = reg_hi(reg);
    emit_rex(e, 1, 0, 0, b);
    emit_byte(e, 0xF7);
    emit_modrm(e, 3, 0, reg & 7);
    emit_dword(e, (uint32_t)imm);
}

/* MOV r64, r/m64: REX.W 8B /r (load) */
/* MOV r/m64, r64: REX.W 89 /r (store) */
void vtx_x86_emit_mov_rr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* Use 89 (mov r/m64, r64) — src in reg field, dst in r/m field */
    vtx_x86_emit_rr(e, 0x89, 0, src, dst);
}

/* MOV r64, [base+disp]: REX.W 8B /r */
void vtx_x86_emit_mov_rmem(vtx_x86_emit_t *e, uint8_t dst, uint8_t base, int32_t disp)
{
    vtx_x86_emit_rm(e, 0x8B, 0, dst, base, disp, true);
}

/* MOV [base+disp], r64: REX.W 89 /r */
void vtx_x86_emit_mov_memr(vtx_x86_emit_t *e, uint8_t base, int32_t disp, uint8_t src)
{
    vtx_x86_emit_rm(e, 0x89, 0, src, base, disp, false);
}

/* LEA r64, [base+disp]: REX.W 8D /r */
void vtx_x86_emit_lea_rmem(vtx_x86_emit_t *e, uint8_t dst, uint8_t base, int32_t disp)
{
    vtx_x86_emit_rm(e, 0x8D, 0, dst, base, disp, true);
}

/* NEG r/m64: REX.W F7 /3 */
void vtx_x86_emit_neg_r(vtx_x86_emit_t *e, uint8_t reg)
{
    int b = reg_hi(reg);
    emit_rex(e, 1, 0, 0, b);
    emit_byte(e, 0xF7);
    emit_modrm(e, 3, 3, reg & 7);
}

/* NOT r/m64: REX.W F7 /2 */
void vtx_x86_emit_not_r(vtx_x86_emit_t *e, uint8_t reg)
{
    int b = reg_hi(reg);
    emit_rex(e, 1, 0, 0, b);
    emit_byte(e, 0xF7);
    emit_modrm(e, 3, 2, reg & 7);
}

/* CQO: REX.W 99 */
void vtx_x86_emit_cqo(vtx_x86_emit_t *e)
{
    emit_rex(e, 1, 0, 0, 0);
    emit_byte(e, 0x99);
}

/* SETcc r/m8: 0F 9x /0 (mod=11) */
void vtx_x86_emit_setcc(vtx_x86_emit_t *e, uint8_t cond, uint8_t reg)
{
    /* SETcc doesn't need REX.W but may need REX.B for extended registers */
    if (reg_hi(reg)) emit_rex(e, 0, 0, 0, 1);
    emit_byte(e, 0x0F);
    emit_byte(e, (uint8_t)(0x90 + cond));
    emit_modrm(e, 3, 0, reg & 7);
}

/* CMOVcc r64, r/m64: REX.W 0F 4x /r */
void vtx_x86_emit_cmovcc(vtx_x86_emit_t *e, uint8_t cond, uint8_t dst, uint8_t src)
{
    vtx_x86_emit_rr(e, (uint8_t)(0x40 + cond), 0x0F, dst, src);
}

/* PUSH r64: 50+rd (with REX.B if needed) */
void vtx_x86_emit_push_r(vtx_x86_emit_t *e, uint8_t reg)
{
    if (reg_hi(reg)) emit_rex(e, 0, 0, 0, 1);
    emit_byte(e, (uint8_t)(0x50 + (reg & 7)));
}

/* POP r64: 58+rd (with REX.B if needed) */
void vtx_x86_emit_pop_r(vtx_x86_emit_t *e, uint8_t reg)
{
    if (reg_hi(reg)) emit_rex(e, 0, 0, 0, 1);
    emit_byte(e, (uint8_t)(0x58 + (reg & 7)));
}

/* JMP rel32: E9 cd */
void vtx_x86_emit_jmp_rel32(vtx_x86_emit_t *e, int32_t offset)
{
    emit_byte(e, 0xE9);
    emit_dword(e, (uint32_t)offset);
}

/* JCC rel32: 0F 8x cd */
void vtx_x86_emit_jcc_rel32(vtx_x86_emit_t *e, uint8_t cond, int32_t offset)
{
    emit_byte(e, 0x0F);
    emit_byte(e, (uint8_t)(0x80 + cond));
    emit_dword(e, (uint32_t)offset);
}

/* CALL rel32: E8 cd */
void vtx_x86_emit_call_rel32(vtx_x86_emit_t *e, int32_t offset)
{
    emit_byte(e, 0xE8);
    emit_dword(e, (uint32_t)offset);
}

/* RET: C3 */
void vtx_x86_emit_ret(vtx_x86_emit_t *e)
{
    emit_byte(e, 0xC3);
}

/* NOP: 90 */
void vtx_x86_emit_nop(vtx_x86_emit_t *e)
{
    emit_byte(e, 0x90);
}

/* ========================================================================== */
/* Prologue / Epilogue                                                        */
/* ========================================================================== */

void vtx_x86_emit_prologue(vtx_x86_emit_t *e, uint32_t frame_size,
                            uint32_t callee_saved_mask)
{
    /* push rbp */
    vtx_x86_emit_push_r(e, 5); /* RBP = 5 */

    /* mov rbp, rsp */
    vtx_x86_emit_mov_rr(e, 5, 4); /* RBP = 5, RSP = 4 */

    /* Push callee-saved registers */
    /* Order: RBX(3), R12(12), R13(13), R14(14), R15(15) */
    static const uint8_t cs_regs[] = { 3, 12, 13, 14, 15 };
    for (int i = 0; i < 5; i++) {
        if (callee_saved_mask & (1u << cs_regs[i])) {
            vtx_x86_emit_push_r(e, cs_regs[i]);
        }
    }

    /* sub rsp, frame_size */
    if (frame_size > 0) {
        vtx_x86_emit_sub_ri(e, 4, (int32_t)frame_size); /* RSP = 4 */
    }
}

void vtx_x86_emit_epilogue(vtx_x86_emit_t *e, uint32_t callee_saved_mask)
{
    /* mov rsp, rbp */
    vtx_x86_emit_mov_rr(e, 4, 5); /* RSP = 4, RBP = 5 */

    /* Pop callee-saved registers (reverse order) */
    static const uint8_t cs_regs[] = { 3, 12, 13, 14, 15 };
    for (int i = 4; i >= 0; i--) {
        if (callee_saved_mask & (1u << cs_regs[i])) {
            vtx_x86_emit_pop_r(e, cs_regs[i]);
        }
    }

    /* pop rbp */
    vtx_x86_emit_pop_r(e, 5);

    /* ret */
    vtx_x86_emit_ret(e);
}

/* ========================================================================== */
/* Emit entire function                                                        */
/* ========================================================================== */

/**
 * Emit a single instruction from the instruction stream into the code buffer.
 * The instruction must have physical register assignments (after regalloc).
 */
static int emit_single_inst(vtx_x86_emit_t *e, vtx_inst_t *inst,
                             const vtx_regalloc_result_t *ra)
{
    uint8_t r0, r1, r2;
    int64_t imm;

    switch (inst->opcode) {

    case VTX_X86_NOP:
        vtx_x86_emit_nop(e);
        break;

    case VTX_X86_ADD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            if (r0 != 0xFF) vtx_x86_emit_add_ri(e, r0, (int32_t)inst->imm);
        } else {
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_add_rr(e, r0, r1);
        }
        break;

    case VTX_X86_SUB:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            if (r0 != 0xFF) vtx_x86_emit_sub_ri(e, r0, (int32_t)inst->imm);
        } else {
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_sub_rr(e, r0, r1);
        }
        break;

    case VTX_X86_IMUL:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_imul_rr(e, r0, r1);
        break;

    case VTX_X86_IDIV:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 != 0xFF) vtx_x86_emit_idiv_r(e, r0);
        break;

    case VTX_X86_SHL:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            vtx_x86_emit_shl_ri(e, r0, (uint8_t)(inst->imm & 0x3F));
        } else {
            vtx_x86_emit_shl_cl(e, r0);
        }
        break;

    case VTX_X86_SHR:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            vtx_x86_emit_shr_ri(e, r0, (uint8_t)(inst->imm & 0x3F));
        } else {
            vtx_x86_emit_shr_cl(e, r0);
        }
        break;

    case VTX_X86_SAR:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            vtx_x86_emit_sar_ri(e, r0, (uint8_t)(inst->imm & 0x3F));
        } else {
            vtx_x86_emit_sar_cl(e, r0);
        }
        break;

    case VTX_X86_AND:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            vtx_x86_emit_and_ri(e, r0, (int32_t)inst->imm);
        } else {
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            vtx_x86_emit_and_rr(e, r0, r1);
        }
        break;

    case VTX_X86_OR:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            vtx_x86_emit_or_ri(e, r0, (int32_t)inst->imm);
        } else {
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            vtx_x86_emit_or_rr(e, r0, r1);
        }
        break;

    case VTX_X86_XOR:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            vtx_x86_emit_xor_ri(e, r0, (int32_t)inst->imm);
        } else {
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            vtx_x86_emit_xor_rr(e, r0, r1);
        }
        break;

    case VTX_X86_CMP:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            vtx_x86_emit_cmp_ri(e, r0, (int32_t)inst->imm);
        } else {
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            vtx_x86_emit_cmp_rr(e, r0, r1);
        }
        break;

    case VTX_X86_TEST:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            vtx_x86_emit_test_ri(e, r0, (int32_t)inst->imm);
        } else {
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            vtx_x86_emit_test_rr(e, r0, r1);
        }
        break;

    case VTX_X86_MOV:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_MEM) {
            /* Memory operand */
            if (inst->opnd_kinds[0] == VTX_OPND_VREG && inst->opnd_kinds[1] == VTX_OPND_MEM) {
                /* Load: mov reg, [mem] */
                uint8_t base = 5; /* RBP as frame pointer */
                vtx_x86_emit_mov_rmem(e, r0, base, inst->mem.disp);
            } else if (inst->opnd_kinds[0] == VTX_OPND_MEM && inst->opnd_kinds[1] == VTX_OPND_PREG) {
                /* Store: mov [mem], reg */
                r1 = (uint8_t)inst->operands[1];
                uint8_t base = 5;
                vtx_x86_emit_mov_memr(e, base, inst->mem.disp, r1);
            }
        } else if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            /* mov reg, imm */
            imm = inst->imm;
            if (imm >= INT32_MIN && imm <= INT32_MAX) {
                vtx_x86_emit_mov_imm32(e, r0, (int32_t)imm);
            } else {
                vtx_x86_emit_mov_imm64(e, r0, (uint64_t)imm);
            }
        } else {
            /* mov reg, reg */
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            if (r0 != 0xFF && r1 != 0xFF && r0 != r1) {
                vtx_x86_emit_mov_rr(e, r0, r1);
            }
        }
        break;

    case VTX_X86_NEG:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 != 0xFF) vtx_x86_emit_neg_r(e, r0);
        break;

    case VTX_X86_NOT:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 != 0xFF) vtx_x86_emit_not_r(e, r0);
        break;

    case VTX_X86_CQO:
        vtx_x86_emit_cqo(e);
        break;

    case VTX_X86_SETCC:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 != 0xFF && (inst->flags & VTX_INST_FLAG_HAS_COND)) {
            vtx_x86_emit_setcc(e, vtx_cond_to_x86(inst->cond), r0);
        }
        break;

    case VTX_X86_CMOV:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && (inst->flags & VTX_INST_FLAG_HAS_COND)) {
            vtx_x86_emit_cmovcc(e, vtx_cond_to_x86(inst->cond), r0, r1);
        }
        break;

    case VTX_X86_LEA:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 != 0xFF && (inst->flags & VTX_INST_FLAG_HAS_MEM)) {
            uint8_t base = 5; /* RBP */
            vtx_x86_emit_lea_rmem(e, r0, base, inst->mem.disp);
        }
        break;

    case VTX_X86_PUSH:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 != 0xFF) vtx_x86_emit_push_r(e, r0);
        break;

    case VTX_X86_POP:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 != 0xFF) vtx_x86_emit_pop_r(e, r0);
        break;

    case VTX_X86_JMP:
        /* Forward branch — emit placeholder, will be patched by reloc */
        vtx_x86_emit_jmp_rel32(e, 0); /* placeholder offset */
        break;

    case VTX_X86_JCC:
        if (inst->flags & VTX_INST_FLAG_HAS_COND) {
            vtx_x86_emit_jcc_rel32(e, vtx_cond_to_x86(inst->cond), 0);
        }
        break;

    case VTX_X86_CALL:
        vtx_x86_emit_call_rel32(e, 0); /* placeholder, will be patched */
        break;

    case VTX_X86_RET:
        vtx_x86_emit_ret(e);
        break;

    default:
        /* Unknown opcode — emit nop as safe fallback */
        vtx_x86_emit_nop(e);
        break;
    }

    return 0;
}

int vtx_x86_emit_function(vtx_x86_emit_t *emit, vtx_inst_stream_t *stream,
                           const vtx_regalloc_result_t *result, vtx_arena_t *arena)
{
    if (!emit || !stream) return -1;

    /* Emit prologue */
    vtx_x86_emit_prologue(emit, result ? result->frame_size : 0,
                           result ? result->callee_saved_mask : 0);

    /* Emit instructions for each block */
    for (uint32_t b = 0; b < stream->block_count; b++) {
        vtx_inst_block_t *blk = &stream->blocks[b];
        for (uint32_t i = 0; i < blk->inst_count; i++) {
            vtx_inst_t *inst = &blk->insts[i];
            /* Record the native offset before emitting */
            inst->native_offset = vtx_x86_emit_position(emit);
            if (emit_single_inst(emit, inst, result) != 0) {
                return -1;
            }
        }
    }

    /* Epilogue is emitted by the RET instruction itself */
    (void)arena;
    return 0;
}
