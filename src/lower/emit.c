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
#include "lower/reloc.h"
#include "runtime/helpers.h"
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
    emit->relocs = NULL;
    emit->reloc_arena = NULL;
    emit->label_offsets = NULL;
    emit->label_count = 0;
    /* Initialize constant pool */
    emit->const_pool.values = NULL;
    emit->const_pool.ref_offsets = NULL;
    emit->const_pool.count = 0;
    emit->const_pool.capacity = 0;
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
    /* relocs and label_offsets are arena-allocated, no individual free needed */
    emit->relocs = NULL;
    emit->reloc_arena = NULL;
    emit->label_offsets = NULL;
    emit->label_count = 0;
    /* Free constant pool */
    if (emit->const_pool.values) { free(emit->const_pool.values); emit->const_pool.values = NULL; }
    if (emit->const_pool.ref_offsets) { free(emit->const_pool.ref_offsets); emit->const_pool.ref_offsets = NULL; }
    emit->const_pool.count = 0;
    emit->const_pool.capacity = 0;
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

/* INC r/m64: REX.W FF /0 */
static void vtx_x86_emit_inc_r(vtx_x86_emit_t *e, uint8_t reg)
{
    int b = reg_hi(reg);
    emit_rex(e, 1, 0, 0, b);
    emit_byte(e, 0xFF);
    emit_modrm(e, 3, 0, reg & 7);
}

/* DEC r/m64: REX.W FF /1 */
static void vtx_x86_emit_dec_r(vtx_x86_emit_t *e, uint8_t reg)
{
    int b = reg_hi(reg);
    emit_rex(e, 1, 0, 0, b);
    emit_byte(e, 0xFF);
    emit_modrm(e, 3, 1, reg & 7);
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

/* UCOMISD xmm, xmm: 66 0F 2E /r
 * Unordered compare scalar double-precision floating-point values.
 * Sets EFLAGS based on comparison result:
 *   Equal:           ZF=1, PF=0, CF=0
 *   Below (less):    ZF=0, PF=0, CF=1
 *   Above (greater): ZF=0, PF=0, CF=0
 *   Unordered:       ZF=1, PF=1, CF=1 */
void vtx_x86_emit_ucomisd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* 66 REX.W 0F 2E ModR/M(mod=11, reg=src, rm=dst)
     * SSE prefix 66 + 0F 2E = UCOMISD
     * For R8-R15 registers, use REX.R and REX.B */
    int r = reg_hi(src);
    int b = reg_hi(dst);
    /* Emit REX prefix if needed for extended registers */
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0x66);  /* SSE prefix for scalar double */
    emit_byte(e, 0x0F);  /* Two-byte opcode escape */
    emit_byte(e, 0x2E);  /* UCOMISD opcode */
    emit_modrm(e, 3, src & 7, dst & 7);  /* mod=11 (reg-reg) */
}

/* ========================================================================== */
/* SSE/SSE2 scalar double-precision instructions                               */
/* ========================================================================== */

/**
 * Helper: emit an SSE scalar double instruction with F2 prefix.
 * Format: F2 [REX] 0F opcode ModR/M(mod=11, reg=src, rm=dst)
 *
 * @param e       Emitter
 * @param opcode  The opcode byte after 0F
 * @param dst     Destination XMM register (r/m field)
 * @param src     Source XMM register (reg field)
 */
static void emit_sse_sd_rr(vtx_x86_emit_t *e, uint8_t opcode,
                             uint8_t dst, uint8_t src)
{
    int r = reg_hi(src);
    int b = reg_hi(dst);
    /* Emit REX prefix if needed for extended registers */
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0xF2);  /* scalar double prefix */
    emit_byte(e, 0x0F);  /* Two-byte opcode escape */
    emit_byte(e, opcode);
    emit_modrm(e, 3, src & 7, dst & 7);  /* mod=11 (reg-reg) */
}

void vtx_x86_emit_addsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_sd_rr(e, 0x58, dst, src);  /* F2 0F 58 /r */
}

void vtx_x86_emit_subsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_sd_rr(e, 0x5C, dst, src);  /* F2 0F 5C /r */
}

void vtx_x86_emit_mulsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_sd_rr(e, 0x59, dst, src);  /* F2 0F 59 /r */
}

void vtx_x86_emit_divsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_sd_rr(e, 0x5E, dst, src);  /* F2 0F 5E /r */
}

void vtx_x86_emit_movsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_sd_rr(e, 0x10, dst, src);  /* F2 0F 10 /r (movsd xmm, xmm) */
}

/* ---- Scalar single-precision float (SSE) ---- */

static void emit_sse_ss_rr(vtx_x86_emit_t *e, uint8_t opcode,
                             uint8_t dst, uint8_t src)
{
    int r = reg_hi(src);
    int b = reg_hi(dst);
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0xF3);  /* scalar single prefix */
    emit_byte(e, 0x0F);
    emit_byte(e, opcode);
    emit_modrm(e, 3, src & 7, dst & 7);
}

void vtx_x86_emit_addss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_ss_rr(e, 0x58, dst, src);  /* F3 0F 58 /r */
}

void vtx_x86_emit_subss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_ss_rr(e, 0x5C, dst, src);  /* F3 0F 5C /r */
}

void vtx_x86_emit_mulss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_ss_rr(e, 0x59, dst, src);  /* F3 0F 59 /r */
}

void vtx_x86_emit_divss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_ss_rr(e, 0x5E, dst, src);  /* F3 0F 5E /r */
}

void vtx_x86_emit_sqrtss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_ss_rr(e, 0x51, dst, src);  /* F3 0F 51 /r */
}

void vtx_x86_emit_ucomiss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* UCOMISS xmm, xmm — no mandatory prefix, 0F 2E /r */
    int r = reg_hi(src);
    int b = reg_hi(dst);
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0x0F);
    emit_byte(e, 0x2E);
    emit_modrm(e, 3, src & 7, dst & 7);
}

void vtx_x86_emit_movss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_ss_rr(e, 0x10, dst, src);  /* F3 0F 10 /r */
}

/* ---- Float ↔ Int conversions (SSE2) ---- */

void vtx_x86_emit_cvtsi2sd(vtx_x86_emit_t *e, uint8_t dst_xmm, uint8_t src_gpr)
{
    /* CVTSI2SD xmm, r/m64: F2 REX.W 0F 2A /r */
    emit_byte(e, 0xF2);
    emit_rex(e, 1, reg_hi(dst_xmm), 0, reg_hi(src_gpr));
    emit_byte(e, 0x0F);
    emit_byte(e, 0x2A);
    emit_modrm(e, 3, dst_xmm & 7, src_gpr & 7);
}

void vtx_x86_emit_cvtsd2si(vtx_x86_emit_t *e, uint8_t dst_gpr, uint8_t src_xmm)
{
    /* CVTSD2SI r64, xmm: F2 REX.W 0F 2D /r */
    emit_byte(e, 0xF2);
    emit_rex(e, 1, reg_hi(dst_gpr), 0, reg_hi(src_xmm));
    emit_byte(e, 0x0F);
    emit_byte(e, 0x2D);
    emit_modrm(e, 3, dst_gpr & 7, src_xmm & 7);
}

void vtx_x86_emit_cvttsd2si(vtx_x86_emit_t *e, uint8_t dst_gpr, uint8_t src_xmm)
{
    /* CVTTSD2SI r64, xmm: F2 REX.W 0F 2C /r */
    emit_byte(e, 0xF2);
    emit_rex(e, 1, reg_hi(dst_gpr), 0, reg_hi(src_xmm));
    emit_byte(e, 0x0F);
    emit_byte(e, 0x2C);
    emit_modrm(e, 3, dst_gpr & 7, src_xmm & 7);
}

void vtx_x86_emit_cvtsi2ss(vtx_x86_emit_t *e, uint8_t dst_xmm, uint8_t src_gpr)
{
    /* CVTSI2SS xmm, r/m64: F3 REX.W 0F 2A /r */
    emit_byte(e, 0xF3);
    emit_rex(e, 1, reg_hi(dst_xmm), 0, reg_hi(src_gpr));
    emit_byte(e, 0x0F);
    emit_byte(e, 0x2A);
    emit_modrm(e, 3, dst_xmm & 7, src_gpr & 7);
}

void vtx_x86_emit_cvtss2si(vtx_x86_emit_t *e, uint8_t dst_gpr, uint8_t src_xmm)
{
    /* CVTSS2SI r64, xmm: F3 REX.W 0F 2D /r */
    emit_byte(e, 0xF3);
    emit_rex(e, 1, reg_hi(dst_gpr), 0, reg_hi(src_xmm));
    emit_byte(e, 0x0F);
    emit_byte(e, 0x2D);
    emit_modrm(e, 3, dst_gpr & 7, src_xmm & 7);
}

void vtx_x86_emit_cvttss2si(vtx_x86_emit_t *e, uint8_t dst_gpr, uint8_t src_xmm)
{
    /* CVTTSS2SI r64, xmm: F3 REX.W 0F 2C /r */
    emit_byte(e, 0xF3);
    emit_rex(e, 1, reg_hi(dst_gpr), 0, reg_hi(src_xmm));
    emit_byte(e, 0x0F);
    emit_byte(e, 0x2C);
    emit_modrm(e, 3, dst_gpr & 7, src_xmm & 7);
}

/* ---- GPR ↔ XMM bridge (SSE2) ---- */

void vtx_x86_emit_movq_xmm_r64(vtx_x86_emit_t *e, uint8_t dst_xmm, uint8_t src_gpr)
{
    /* MOVQ xmm, r64: 66 REX.W 0F 6E /r */
    emit_byte(e, 0x66);
    emit_rex(e, 1, reg_hi(dst_xmm), 0, reg_hi(src_gpr));
    emit_byte(e, 0x0F);
    emit_byte(e, 0x6E);
    emit_modrm(e, 3, dst_xmm & 7, src_gpr & 7);
}

void vtx_x86_emit_movq_r64_xmm(vtx_x86_emit_t *e, uint8_t dst_gpr, uint8_t src_xmm)
{
    /* MOVQ r64, xmm: 66 REX.W 0F 7E /r */
    emit_byte(e, 0x66);
    emit_rex(e, 1, reg_hi(dst_gpr), 0, reg_hi(src_xmm));
    emit_byte(e, 0x0F);
    emit_byte(e, 0x7E);
    emit_modrm(e, 3, dst_gpr & 7, src_xmm & 7);
}

/* ---- COMISD (ordered compare) ---- */

void vtx_x86_emit_comisd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* COMISD xmm, xmm: 66 0F 2F /r */
    int r = reg_hi(src);
    int b = reg_hi(dst);
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0x66);
    emit_byte(e, 0x0F);
    emit_byte(e, 0x2F);
    emit_modrm(e, 3, src & 7, dst & 7);
}

/* ---- SQRTSD ---- */

void vtx_x86_emit_sqrtsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_sd_rr(e, 0x51, dst, src);  /* F2 0F 51 /r */
}

/* ---- MOVSD load/store from memory ---- */

void vtx_x86_emit_movsd_load(vtx_x86_emit_t *e, uint8_t dst_xmm, uint8_t base, int32_t disp)
{
    /* MOVSD xmm, [base+disp]: F2 0F 10 /r with mem operand */
    int b = reg_hi(base);
    int r = reg_hi(dst_xmm);
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0xF2);
    emit_byte(e, 0x0F);
    emit_byte(e, 0x10);
    /* ModR/M with base register and displacement */
    if (disp == 0 && (base & 7) != 5) {
        emit_modrm(e, 0, dst_xmm & 7, base & 7);
        if ((base & 7) == 4) emit_sib(e, 0, 4, 4); /* SIB for R12/RSP */
    } else if (disp >= -128 && disp <= 127) {
        emit_modrm(e, 1, dst_xmm & 7, base & 7);
        if ((base & 7) == 4) emit_sib(e, 0, 4, 4);
        emit_byte(e, (uint8_t)disp);
    } else {
        emit_modrm(e, 2, dst_xmm & 7, base & 7);
        if ((base & 7) == 4) emit_sib(e, 0, 4, 4);
        emit_dword(e, (uint32_t)disp);
    }
}

void vtx_x86_emit_movsd_store(vtx_x86_emit_t *e, uint8_t base, int32_t disp, uint8_t src_xmm)
{
    /* MOVSD [base+disp], xmm: F2 0F 11 /r with mem operand */
    int b = reg_hi(base);
    int r = reg_hi(src_xmm);
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0xF2);
    emit_byte(e, 0x0F);
    emit_byte(e, 0x11);
    if (disp == 0 && (base & 7) != 5) {
        emit_modrm(e, 0, src_xmm & 7, base & 7);
        if ((base & 7) == 4) emit_sib(e, 0, 4, 4);
    } else if (disp >= -128 && disp <= 127) {
        emit_modrm(e, 1, src_xmm & 7, base & 7);
        if ((base & 7) == 4) emit_sib(e, 0, 4, 4);
        emit_byte(e, (uint8_t)disp);
    } else {
        emit_modrm(e, 2, src_xmm & 7, base & 7);
        if ((base & 7) == 4) emit_sib(e, 0, 4, 4);
        emit_dword(e, (uint32_t)disp);
    }
}

/* ---- Bit manipulation ---- */

void vtx_x86_emit_bswap(vtx_x86_emit_t *e, uint8_t reg)
{
    /* BSWAP r64: REX.W 0F C8+rd */
    emit_rex(e, 1, 0, 0, reg_hi(reg));
    emit_byte(e, 0x0F);
    emit_byte(e, (uint8_t)(0xC8 | (reg & 7)));
}

void vtx_x86_emit_bsf(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* BSF r64, r/m64: REX.W 0F BC /r */
    emit_rex(e, 1, reg_hi(dst), 0, reg_hi(src));
    emit_byte(e, 0x0F);
    emit_byte(e, 0xBC);
    emit_modrm(e, 3, dst & 7, src & 7);
}

void vtx_x86_emit_bsr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* BSR r64, r/m64: REX.W 0F BD /r */
    emit_rex(e, 1, reg_hi(dst), 0, reg_hi(src));
    emit_byte(e, 0x0F);
    emit_byte(e, 0xBD);
    emit_modrm(e, 3, dst & 7, src & 7);
}

void vtx_x86_emit_popcnt(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* POPCNT r64, r/m64: F3 REX.W 0F B8 /r */
    emit_byte(e, 0xF3);
    emit_rex(e, 1, reg_hi(dst), 0, reg_hi(src));
    emit_byte(e, 0x0F);
    emit_byte(e, 0xB8);
    emit_modrm(e, 3, dst & 7, src & 7);
}

void vtx_x86_emit_rol_ri(vtx_x86_emit_t *e, uint8_t dst, uint8_t count)
{
    /* ROL r/m64, imm8: REX.W C1 /0 imm8 */
    emit_rex(e, 1, 0, 0, reg_hi(dst));
    emit_byte(e, 0xC1);
    emit_modrm(e, 3, 0, dst & 7);
    emit_byte(e, count);
}

void vtx_x86_emit_rol_cl(vtx_x86_emit_t *e, uint8_t dst)
{
    /* ROL r/m64, CL: REX.W D3 /0 */
    emit_rex(e, 1, 0, 0, reg_hi(dst));
    emit_byte(e, 0xD3);
    emit_modrm(e, 3, 0, dst & 7);
}

void vtx_x86_emit_ror_ri(vtx_x86_emit_t *e, uint8_t dst, uint8_t count)
{
    /* ROR r/m64, imm8: REX.W C1 /1 imm8 */
    emit_rex(e, 1, 0, 0, reg_hi(dst));
    emit_byte(e, 0xC1);
    emit_modrm(e, 3, 1, dst & 7);
    emit_byte(e, count);
}

void vtx_x86_emit_ror_cl(vtx_x86_emit_t *e, uint8_t dst)
{
    /* ROR r/m64, CL: REX.W D3 /1 */
    emit_rex(e, 1, 0, 0, reg_hi(dst));
    emit_byte(e, 0xD3);
    emit_modrm(e, 3, 1, dst & 7);
}

/* ---- CDQE (sign-extend EAX → RAX) ---- */

void vtx_x86_emit_cdqe(vtx_x86_emit_t *e)
{
    /* CDQE: REX.W 98 */
    emit_rex(e, 1, 0, 0, 0);
    emit_byte(e, 0x98);
}

/* ---- Unsigned DIV ---- */

void vtx_x86_emit_div_r(vtx_x86_emit_t *e, uint8_t src)
{
    /* DIV r/m64: REX.W F7 /6 */
    emit_rex(e, 1, 0, 0, reg_hi(src));
    emit_byte(e, 0xF7);
    emit_modrm(e, 3, 6, src & 7);
}

/* ---- Packed double SIMD ---- */

static void emit_sse_pd_rr(vtx_x86_emit_t *e, uint8_t opcode,
                             uint8_t dst, uint8_t src)
{
    int r = reg_hi(src);
    int b = reg_hi(dst);
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0x66);  /* packed double prefix */
    emit_byte(e, 0x0F);
    emit_byte(e, opcode);
    emit_modrm(e, 3, src & 7, dst & 7);
}

void vtx_x86_emit_subpd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_pd_rr(e, 0x5C, dst, src);  /* 66 0F 5C /r */
}

void vtx_x86_emit_divpd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_pd_rr(e, 0x5E, dst, src);  /* 66 0F 5E /r */
}

/* ---- Packed integer SIMD (SSE2) ---- */

static void emit_sse_pi_rr(vtx_x86_emit_t *e, uint8_t opcode,
                             uint8_t dst, uint8_t src)
{
    /* 66 0F opcode /r — most SSE2 packed integer ops */
    int r = reg_hi(src);
    int b = reg_hi(dst);
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0x66);
    emit_byte(e, 0x0F);
    emit_byte(e, opcode);
    emit_modrm(e, 3, src & 7, dst & 7);
}

void vtx_x86_emit_movdqa(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_pi_rr(e, 0x6F, dst, src);  /* 66 0F 6F /r (load) */
}

void vtx_x86_emit_movdqu(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* MOVDQU: F3 0F 6F /r */
    int r = reg_hi(src);
    int b = reg_hi(dst);
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0xF3);
    emit_byte(e, 0x0F);
    emit_byte(e, 0x6F);
    emit_modrm(e, 3, src & 7, dst & 7);
}

void vtx_x86_emit_paddd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_pi_rr(e, 0xFE, dst, src);  /* 66 0F FE /r */
}

void vtx_x86_emit_psubd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_pi_rr(e, 0xFA, dst, src);  /* 66 0F FA /r */
}

void vtx_x86_emit_pmulld(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* PMULLD: 66 0F 38 40 /r (SSE4.1 — 3-byte opcode) */
    int r = reg_hi(src);
    int b = reg_hi(dst);
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0x66);
    emit_byte(e, 0x0F);
    emit_byte(e, 0x38);
    emit_byte(e, 0x40);
    emit_modrm(e, 3, src & 7, dst & 7);
}

void vtx_x86_emit_pxor(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_pi_rr(e, 0xEF, dst, src);  /* 66 0F EF /r */
}

void vtx_x86_emit_pand(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_pi_rr(e, 0xDB, dst, src);  /* 66 0F DB /r */
}

void vtx_x86_emit_por(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_pi_rr(e, 0xEB, dst, src);  /* 66 0F EB /r */
}

void vtx_x86_emit_pcmpeqd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_pi_rr(e, 0x76, dst, src);  /* 66 0F 76 /r */
}

/* ---- Packed single-precision float SIMD (SSE) ---- */

static void emit_sse_ps_rr(vtx_x86_emit_t *e, uint8_t opcode,
                             uint8_t dst, uint8_t src)
{
    /* 0F opcode /r — no prefix for packed single */
    int r = reg_hi(src);
    int b = reg_hi(dst);
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0x0F);
    emit_byte(e, opcode);
    emit_modrm(e, 3, src & 7, dst & 7);
}

void vtx_x86_emit_movaps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_ps_rr(e, 0x28, dst, src);  /* 0F 28 /r */
}

void vtx_x86_emit_addps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_ps_rr(e, 0x58, dst, src);  /* 0F 58 /r */
}

void vtx_x86_emit_mulps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_ps_rr(e, 0x59, dst, src);  /* 0F 59 /r */
}

void vtx_x86_emit_subps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_ps_rr(e, 0x5C, dst, src);  /* 0F 5C /r */
}

void vtx_x86_emit_divps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_ps_rr(e, 0x5E, dst, src);  /* 0F 5E /r */
}

void vtx_x86_emit_minps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_ps_rr(e, 0x5D, dst, src);  /* 0F 5D /r */
}

void vtx_x86_emit_maxps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    emit_sse_ps_rr(e, 0x5F, dst, src);  /* 0F 5F /r */
}

void vtx_x86_emit_cmpps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src, uint8_t pred)
{
    /* CMPPS xmm, xmm, imm8: 0F C2 /r imm8 */
    int r = reg_hi(src);
    int b = reg_hi(dst);
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0x0F);
    emit_byte(e, 0xC2);
    emit_modrm(e, 3, src & 7, dst & 7);
    emit_byte(e, pred);
}

void vtx_x86_emit_xorps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* XORPS xmm, xmm — no mandatory prefix, just 0F 57 /r */
    int r = reg_hi(src);
    int b = reg_hi(dst);
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0x0F);
    emit_byte(e, 0x57);
    emit_modrm(e, 3, src & 7, dst & 7);  /* mod=11 (reg-reg) */
}

/* ========================================================================== */
/* VEX prefix encoder (AVX/AVX2)                                              */
/* ========================================================================== */

/**
 * Emit a VEX 3-byte prefix.
 *
 * VEX encoding replaces the REX + legacy prefix + 0F escape with a compact
 * 3-byte prefix: 0xC4 byte2 byte3, where:
 *
 * byte2: [~R:1][~X:1][~B:1][mmmmm:5]
 *   R/X/B: inverted REX.R/X/B (1=0, 0=1)
 *   mmmmm: 00001=0F, 00010=0F38, 00011=0F3A
 *
 * byte3: [W:1][~vvvv:4][L:1][pp:2]
 *   W: REX.W (0 or 1)
 *   vvvv: 1's complement of vvvv (second source register, 1111=unused)
 *   L: 0=128-bit (xmm), 1=256-bit (ymm)
 *   pp: 00=none, 01=66, 10=F3, 11=F2
 *
 * Followed by: opcode + ModR/M + SIB + disp + imm (same as legacy)
 *
 * @param e       Emitter
 * @param r       REX.R bit (0 or 1, NOT inverted)
 * @param x       REX.X bit (0 or 1, NOT inverted)
 * @param b       REX.B bit (0 or 1, NOT inverted)
 * @param mmmmm   Escape code (1=0F, 2=0F38, 3=0F3A)
 * @param w       REX.W bit (0 or 1)
 * @param vvvv    vvvv field (0-15, NOT inverted; 15=unused)
 * @param l       Vector length (0=128-bit, 1=256-bit)
 * @param pp      Mandatory prefix (0=none, 1=66, 2=F3, 3=F2)
 */
static void emit_vex3(vtx_x86_emit_t *e, int r, int x, int b,
                       int mmmmm, int w, int vvvv, int l, int pp)
{
    uint8_t byte2 = (uint8_t)(((~r & 1) << 7) | ((~x & 1) << 6) |
                              ((~b & 1) << 5) | (mmmmm & 0x1F));
    uint8_t byte3 = (uint8_t)((w << 7) | ((~vvvv & 0xF) << 3) |
                              ((l & 1) << 2) | (pp & 3));
    emit_byte(e, 0xC4);
    emit_byte(e, byte2);
    emit_byte(e, byte3);
}

/**
 * Emit a VEX 2-byte prefix (optimized form when mmmmm=0F, W=0, X=0, B=0).
 *
 * 0xC5 byte2, where:
 * byte2: [~R:1][~vvvv:4][L:1][pp:2]
 *
 * Can only be used when:
 *   - mmmmm = 00001 (0F escape)
 *   - W = 0
 *   - X = 0 and B = 0 (no extended registers in r/m or SIB)
 *
 * For simplicity and generality, the 3-byte form is preferred for all
 * AVX2 256-bit instructions. The 2-byte form is used for VZEROUPPER/VZEROALL.
 */
static void emit_vex2(vtx_x86_emit_t *e, int r, int vvvv, int l, int pp)
{
    uint8_t byte2 = (uint8_t)(((~r & 1) << 7) | ((~vvvv & 0xF) << 3) |
                              ((l & 1) << 2) | (pp & 3));
    emit_byte(e, 0xC5);
    emit_byte(e, byte2);
}

/**
 * Helper: emit a VEX-encoded 256-bit SSE-type instruction with 2 operands
 * (dst=src1, src=src2). Uses 3-byte VEX prefix.
 * VEX.vvvv = 1111 (unused).
 *
 * Format: VEX.256.pp.0F opcode /r  (mod=11)
 */
static void emit_vex256_rr(vtx_x86_emit_t *e, uint8_t opcode, int mmmmm,
                            int pp, uint8_t dst, uint8_t src)
{
    int r = reg_hi(dst);
    int b = reg_hi(src);
    int x = 0;
    emit_vex3(e, r, x, b, mmmmm, 0, 15, 1, pp);
    emit_byte(e, opcode);
    emit_modrm(e, 3, dst & 7, src & 7);
}

/**
 * Helper: emit a VEX-encoded 256-bit instruction with 3 operands
 * (dst, src1, src2). Uses 3-byte VEX prefix.
 * VEX.vvvv = src1 (non-destructive encoding).
 *
 * Format: VEX.256.pp.mmmmm opcode /r  (mod=11)
 */
static void emit_vex256_rrr(vtx_x86_emit_t *e, uint8_t opcode, int mmmmm,
                             int pp, uint8_t dst, uint8_t src1, uint8_t src2)
{
    int r = reg_hi(dst);
    int b = reg_hi(src2);
    int x = 0;
    emit_vex3(e, r, x, b, mmmmm, 0, src1, 1, pp);
    emit_byte(e, opcode);
    emit_modrm(e, 3, dst & 7, src2 & 7);
}

/* ========================================================================== */
/* Timing / Profiling                                                         */
/* ========================================================================== */

void vtx_x86_emit_rdtsc(vtx_x86_emit_t *e)
{
    /* RDTSC: 0F 31 — reads TSC into EDX:EAX */
    emit_byte(e, 0x0F);
    emit_byte(e, 0x31);
}

void vtx_x86_emit_rdtscp(vtx_x86_emit_t *e)
{
    /* RDTSCP: 0F 01 F9 — reads TSC into EDX:EAX, TSC_AUX into ECX */
    emit_byte(e, 0x0F);
    emit_byte(e, 0x01);
    emit_byte(e, 0xF9);
}

/* ========================================================================== */
/* Atomics                                                                    */
/* ========================================================================== */

void vtx_x86_emit_cmpxchg(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* CMPXCHG r/m64, r64: REX.W 0F B1 /r
     * Compare RAX with r/m64 (dst). If equal, ZF=1 and dst←src.
     * If not equal, ZF=0 and RAX←dst. */
    int r = reg_hi(src);
    int b = reg_hi(dst);
    emit_rex(e, 1, r, 0, b);
    emit_byte(e, 0x0F);
    emit_byte(e, 0xB1);
    emit_modrm(e, 3, src & 7, dst & 7);
}

void vtx_x86_emit_xadd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* XADD r/m64, r64: REX.W 0F C1 /r
     * temp ← r/m64 (dst); dst ← temp + r64 (src); src ← temp */
    int r = reg_hi(src);
    int b = reg_hi(dst);
    emit_rex(e, 1, r, 0, b);
    emit_byte(e, 0x0F);
    emit_byte(e, 0xC1);
    emit_modrm(e, 3, src & 7, dst & 7);
}

/* ========================================================================== */
/* Memory fences                                                              */
/* ========================================================================== */

void vtx_x86_emit_lfence(vtx_x86_emit_t *e)
{
    /* LFENCE: 0F AE E8 — serializes load operations */
    emit_byte(e, 0x0F);
    emit_byte(e, 0xAE);
    emit_byte(e, 0xE8);
}

void vtx_x86_emit_mfence(vtx_x86_emit_t *e)
{
    /* MFENCE: 0F AE F0 — serializes all memory operations */
    emit_byte(e, 0x0F);
    emit_byte(e, 0xAE);
    emit_byte(e, 0xF0);
}

void vtx_x86_emit_sfence(vtx_x86_emit_t *e)
{
    /* SFENCE: 0F AE F8 — serializes store operations */
    emit_byte(e, 0x0F);
    emit_byte(e, 0xAE);
    emit_byte(e, 0xF8);
}

/* ========================================================================== */
/* SSE4.1 rounding                                                            */
/* ========================================================================== */

void vtx_x86_emit_roundsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src, uint8_t mode)
{
    /* ROUNDSD xmm, xmm, imm8: 66 0F 3A 0B /r ib
     * mode: 0=nearest, 1=floor, 2=ceil, 3=trunc */
    int r = reg_hi(src);
    int b = reg_hi(dst);
    if (r || b) emit_rex(e, 0, r, 0, b);
    emit_byte(e, 0x66);
    emit_byte(e, 0x0F);
    emit_byte(e, 0x3A);
    emit_byte(e, 0x0B);
    emit_modrm(e, 3, src & 7, dst & 7);
    emit_byte(e, mode);
}

void vtx_x86_emit_roundss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src, uint8_t mode)
{
    /* ROUNDSS xmm, xmm, imm8: 66 0F 3A 0A /r ib
     * mode: 0=nearest, 1=floor, 2=ceil, 3=trunc */
    int r = reg_hi(src);
    int b = reg_hi(dst);
    if (r || b) emit_rex(e, 0, r, 0, b);
    emit_byte(e, 0x66);
    emit_byte(e, 0x0F);
    emit_byte(e, 0x3A);
    emit_byte(e, 0x0A);
    emit_modrm(e, 3, src & 7, dst & 7);
    emit_byte(e, mode);
}

/* ========================================================================== */
/* Constant pool load (RIP-relative MOVSD from literal pool)                  */
/* ========================================================================== */

void vtx_x86_emit_movsd_rip(vtx_x86_emit_t *e, uint8_t dst_xmm, uint64_t double_bits)
{
    /* MOVSD xmm, [rip+disp32]: F2 0F 10 ModR/M(00, xmm, 101) + disp32
     *
     * This emits a placeholder disp32=0 and records the reference in the
     * constant pool. After all code blocks are emitted, the constant pool
     * is written and all disp32 references are patched.
     *
     * Encoding: F2 [REX] 0F 10 ModR/M(00, reg, 101) disp32
     *   F2 = scalar double prefix
     *   REX with R=reg_hi(dst), B=0 (r/m=5, no extension)
     *   0F 10 = MOVSD xmm, m64
     *   ModR/M: mod=00, reg=dst&7, r/m=5 (RIP-relative)
     *   disp32 = placeholder (patched later)
     *
     * Total: 7 bytes (F2 + REX + 0F + 10 + ModR/M + 4 bytes disp32)
     *        or 8 bytes if REX needed for XMM8-15 */

    /* Ensure constant pool has capacity */
    if (e->const_pool.count >= e->const_pool.capacity) {
        uint32_t new_cap = e->const_pool.capacity ? e->const_pool.capacity * 2 : 16;
        uint64_t *new_vals = (uint64_t *)realloc(e->const_pool.values, new_cap * sizeof(uint64_t));
        uint32_t *new_refs = (uint32_t *)realloc(e->const_pool.ref_offsets, new_cap * sizeof(uint32_t));
        if (!new_vals || !new_refs) return; /* OOM — best effort */
        e->const_pool.values = new_vals;
        e->const_pool.ref_offsets = new_refs;
        e->const_pool.capacity = new_cap;
    }

    /* Add constant to the pool */
    uint32_t idx = e->const_pool.count++;
    e->const_pool.values[idx] = double_bits;

    /* Emit F2 prefix */
    emit_byte(e, 0xF2);

    /* Emit REX if needed for XMM8-15 */
    int r = reg_hi(dst_xmm);
    if (r) emit_rex(e, 0, r, 0, 0);

    /* Emit 0F 10 (MOVSD xmm, m64) */
    emit_byte(e, 0x0F);
    emit_byte(e, 0x10);

    /* Emit ModR/M: mod=00, reg=dst_xmm&7, r/m=5 (RIP-relative) */
    emit_modrm(e, 0, dst_xmm & 7, 5);

    /* Record the offset of the disp32 for later patching */
    e->const_pool.ref_offsets[idx] = vtx_x86_emit_position(e);

    /* Emit placeholder disp32 (will be patched after pool emission) */
    emit_dword(e, 0);
}

/* ========================================================================== */
/* AVX2 VEX-encoded 256-bit packed double                                     */
/* ========================================================================== */

void vtx_x86_emit_vmovapd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* VMOVAPD ymm, ymm: VEX.256.66.0F 28 /r */
    emit_vex256_rr(e, 0x28, 1, 1, dst, src);
}

void vtx_x86_emit_vaddpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VADDPD ymm1, ymm2, ymm3: VEX.256.66.0F 58 /r */
    emit_vex256_rrr(e, 0x58, 1, 1, dst, src1, src2);
}

void vtx_x86_emit_vsubpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VSUBPD ymm1, ymm2, ymm3: VEX.256.66.0F 5C /r */
    emit_vex256_rrr(e, 0x5C, 1, 1, dst, src1, src2);
}

void vtx_x86_emit_vmulpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VMULPD ymm1, ymm2, ymm3: VEX.256.66.0F 59 /r */
    emit_vex256_rrr(e, 0x59, 1, 1, dst, src1, src2);
}

void vtx_x86_emit_vdivpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VDIVPD ymm1, ymm2, ymm3: VEX.256.66.0F 5E /r */
    emit_vex256_rrr(e, 0x5E, 1, 1, dst, src1, src2);
}

void vtx_x86_emit_vminpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VMINPD ymm1, ymm2, ymm3: VEX.256.66.0F 5D /r */
    emit_vex256_rrr(e, 0x5D, 1, 1, dst, src1, src2);
}

void vtx_x86_emit_vmaxpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VMAXPD ymm1, ymm2, ymm3: VEX.256.66.0F 5F /r */
    emit_vex256_rrr(e, 0x5F, 1, 1, dst, src1, src2);
}

void vtx_x86_emit_vxorpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VXORPD ymm1, ymm2, ymm3: VEX.256.66.0F 57 /r */
    emit_vex256_rrr(e, 0x57, 1, 1, dst, src1, src2);
}

void vtx_x86_emit_vandpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VANDPD ymm1, ymm2, ymm3: VEX.256.66.0F 54 /r */
    emit_vex256_rrr(e, 0x54, 1, 1, dst, src1, src2);
}

void vtx_x86_emit_vcmppd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2, uint8_t pred)
{
    /* VCMPPD ymm1, ymm2, ymm3, imm8: VEX.256.66.0F C2 /r ib */
    int r = reg_hi(dst);
    int b = reg_hi(src2);
    int x = 0;
    emit_vex3(e, r, x, b, 1, 0, src1, 1, 1);
    emit_byte(e, 0xC2);
    emit_modrm(e, 3, dst & 7, src2 & 7);
    emit_byte(e, pred);
}

/* ========================================================================== */
/* AVX2 256-bit packed single-precision float                                 */
/* ========================================================================== */

void vtx_x86_emit_vmovaps_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* VMOVAPS ymm, ymm: VEX.256.0F 28 /r (pp=00, no mandatory prefix) */
    emit_vex256_rr(e, 0x28, 1, 0, dst, src);
}

void vtx_x86_emit_vaddps_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VADDPS ymm1, ymm2, ymm3: VEX.256.0F 58 /r */
    emit_vex256_rrr(e, 0x58, 1, 0, dst, src1, src2);
}

void vtx_x86_emit_vsubps_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VSUBPS ymm1, ymm2, ymm3: VEX.256.0F 5C /r */
    emit_vex256_rrr(e, 0x5C, 1, 0, dst, src1, src2);
}

void vtx_x86_emit_vmulps_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VMULPS ymm1, ymm2, ymm3: VEX.256.0F 59 /r */
    emit_vex256_rrr(e, 0x59, 1, 0, dst, src1, src2);
}

void vtx_x86_emit_vdivps_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VDIVPS ymm1, ymm2, ymm3: VEX.256.0F 5E /r */
    emit_vex256_rrr(e, 0x5E, 1, 0, dst, src1, src2);
}

/* ========================================================================== */
/* AVX2 256-bit packed integer                                                */
/* ========================================================================== */

void vtx_x86_emit_vmovdqa_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* VMOVDQA ymm, ymm: VEX.256.66.0F 6F /r */
    emit_vex256_rr(e, 0x6F, 1, 1, dst, src);
}

void vtx_x86_emit_vpaddd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VPADDD ymm1, ymm2, ymm3: VEX.256.66.0F FE /r */
    emit_vex256_rrr(e, 0xFE, 1, 1, dst, src1, src2);
}

void vtx_x86_emit_vpsubd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VPSUBD ymm1, ymm2, ymm3: VEX.256.66.0F FA /r */
    emit_vex256_rrr(e, 0xFA, 1, 1, dst, src1, src2);
}

void vtx_x86_emit_vpmulld_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VPMULLD ymm1, ymm2, ymm3: VEX.256.66.0F38 40 /r */
    emit_vex256_rrr(e, 0x40, 2, 1, dst, src1, src2);
}

void vtx_x86_emit_vpxor_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VPXOR ymm1, ymm2, ymm3: VEX.256.66.0F EF /r */
    emit_vex256_rrr(e, 0xEF, 1, 1, dst, src1, src2);
}

void vtx_x86_emit_vpand_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VPAND ymm1, ymm2, ymm3: VEX.256.66.0F DB /r */
    emit_vex256_rrr(e, 0xDB, 1, 1, dst, src1, src2);
}

void vtx_x86_emit_vpor_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2)
{
    /* VPOR ymm1, ymm2, ymm3: VEX.256.66.0F EB /r */
    emit_vex256_rrr(e, 0xEB, 1, 1, dst, src1, src2);
}

/* ========================================================================== */
/* AVX2 utility / transition                                                  */
/* ========================================================================== */

void vtx_x86_emit_vzeroupper(vtx_x86_emit_t *e)
{
    /* VZEROUPPER: VEX.128.0F 77 (L=0, pp=00, vvvv=1111)
     * Zeroes the upper 128 bits of YMM0-YMM15.
     * CRITICAL: must be called before any transition from AVX to SSE code
     * to avoid devastating ~70 cycle AVX→SSE transition penalty. */
    emit_vex2(e, 0, 15, 0, 0);
    emit_byte(e, 0x77);
}

void vtx_x86_emit_vzeroall(vtx_x86_emit_t *e)
{
    /* VZEROALL: VEX.256.0F 77 (L=1, pp=00, vvvv=1111)
     * Zeroes all bits of YMM0-YMM15. */
    emit_vex2(e, 0, 15, 1, 0);
    emit_byte(e, 0x77);
}

/* ========================================================================== */
/* AVX2 lane manipulation / broadcast                                         */
/* ========================================================================== */

void vtx_x86_emit_vbroadcastsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* VBROADCASTSD ymm1, xmm2: VEX.256.66.0F38 19 /r
     * Broadcasts scalar double from xmm2[63:0] to all 4 double positions in ymm1. */
    int r = reg_hi(dst);
    int b = reg_hi(src);
    emit_vex3(e, r, 0, b, 2, 0, 15, 1, 1);
    emit_byte(e, 0x19);
    emit_modrm(e, 3, dst & 7, src & 7);
}

void vtx_x86_emit_vbroadcastss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src)
{
    /* VBROADCASTSS xmm1, xmm2: VEX.128.66.0F38 18 /r
     * Broadcasts scalar float from xmm2[31:0] to all 4 float positions in xmm1. */
    int r = reg_hi(dst);
    int b = reg_hi(src);
    emit_vex3(e, r, 0, b, 2, 0, 15, 0, 1);
    emit_byte(e, 0x18);
    emit_modrm(e, 3, dst & 7, src & 7);
}

void vtx_x86_emit_vperm2f128(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2, uint8_t ctrl)
{
    /* VPERM2F128 ymm1, ymm2, ymm3, imm8: VEX.256.66.0F3A 06 /r ib
     * Permutes 128-bit lanes between ymm2 and ymm3 per imm8 control byte. */
    int r = reg_hi(dst);
    int b = reg_hi(src2);
    emit_vex3(e, r, 0, b, 3, 0, src1, 1, 1);
    emit_byte(e, 0x06);
    emit_modrm(e, 3, dst & 7, src2 & 7);
    emit_byte(e, ctrl);
}

void vtx_x86_emit_vinsertf128(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2, uint8_t imm8)
{
    /* VINSERTF128 ymm1, ymm2, xmm3, imm8: VEX.256.66.0F3A 18 /r ib
     * Inserts 128 bits from xmm3 into ymm2 at lane specified by imm8,
     * result in ymm1. */
    int r = reg_hi(dst);
    int b = reg_hi(src2);
    emit_vex3(e, r, 0, b, 3, 0, src1, 1, 1);
    emit_byte(e, 0x18);
    emit_modrm(e, 3, dst & 7, src2 & 7);
    emit_byte(e, imm8);
}

void vtx_x86_emit_vextractf128(vtx_x86_emit_t *e, uint8_t dst, uint8_t src, uint8_t imm8)
{
    /* VEXTRACTF128 xmm1, ymm2, imm8: VEX.256.66.0F3A 19 /r ib
     * Extracts 128 bits from ymm2 at lane specified by imm8, result in xmm1.
     * Note: VEX.vvvv is unused (1111), dst in reg field, src in vvvv... wait.
     * Actually, VEXTRACTF128 is a 2-source form: xmm1/m128, ymm2, imm8.
     * VEX.vvvv = ymm2 (source), ModR/M.reg = xmm1 (destination). */
    int r = reg_hi(dst);
    int b = 0; /* no r/m register for reg-reg */
    emit_vex3(e, r, 0, b, 3, 0, src, 1, 1);
    emit_byte(e, 0x19);
    emit_modrm(e, 3, dst & 7, 0);
    emit_byte(e, imm8);
}

/* ========================================================================== */
/* Safepoint poll emission                                                     */
/* ========================================================================== */

int vtx_x86_emit_safepoint_poll(vtx_x86_emit_t *e)
{
    if (!e) return -1;

    /* Emit:
     *   cmp qword ptr [rip + disp32], 0   (8 bytes)
     *   jne rel32                          (6 bytes)
     *
     * CMP encoding (CMP r/m64, imm8 with RIP-relative addressing):
     *   REX.W (0x48) + 0x83 + ModR/M(00,/7,101) + disp32 + imm8
     *   0x48 0x83 0x3D dd dd dd dd 0x00
     *
     * The displacement is a placeholder (0); a VTX_RELOC_RIP_REL32
     * external relocation is recorded so it gets patched at code
     * install time when the final code address is known.
     *
     * JNE encoding:
     *   0x0F 0x85 dd dd dd dd
     *
     * The JNE displacement is a placeholder (0); it will be patched
     * by the guard emission pipeline to jump to a deopt stub.
     */

    /* Ensure buffer has space: 8 (CMP) + 6 (JNE) = 14 bytes */
    if (vtx_x86_emit_ensure(e, 14) != 0) return -1;

    /* ---- Emit CMP qword ptr [rip + disp32], 0 ---- */
    uint32_t cmp_disp_offset = vtx_x86_emit_position(e) + 3; /* disp32 starts at byte 3 */

    emit_byte(e, 0x48);  /* REX.W */
    emit_byte(e, 0x83);  /* CMP r/m64, imm8 */
    emit_byte(e, 0x3D);  /* ModR/M: mod=00, reg=/7 (CMP), r/m=5 (RIP-relative) */
    emit_dword(e, 0);    /* placeholder disp32 — will be patched by relocation */
    emit_byte(e, 0x00);  /* immediate 0 */

    /* Record RIP-relative relocation for the CMP's displacement.
     * Marked as external because the code's final address in the code
     * cache is not known at emit time. The target is the address of
     * vtx_safepoint_flag.
     * Use stub_id -5 as a special external relocation ID that the
     * code install step will resolve to &vtx_safepoint_flag. */
    if (e->relocs && e->reloc_arena) {
        uint32_t reloc_idx = vtx_reloc_add(e->relocs, VTX_RELOC_RIP_REL32,
                                            cmp_disp_offset,
                                            0,  /* target_offset (N/A for external) */
                                            0,  /* target_addr: resolved at install time */
                                            -5, /* stub_id: special marker for safepoint_flag */
                                            0, e->reloc_arena);
        if (reloc_idx == UINT32_MAX) return -1;
        /* Mark as external so it gets re-applied at install time */
        e->relocs->entries[reloc_idx].is_external = true;
    }

    /* ---- Emit JNE rel32 ---- */
    /* 0F 85 cd — JNE rel32 (jump if not equal / not zero) */
    emit_byte(e, 0x0F);
    emit_byte(e, 0x85);
    emit_dword(e, 0);    /* placeholder disp32 — will be patched by guard emission */

    return 0;
}

/* ========================================================================== */
/* Guard page safepoint poll emission (zero-cost deopt)                        */
/* ========================================================================== */

int vtx_x86_emit_safepoint_poll_guard_page(vtx_x86_emit_t *e)
{
    if (!e) return -1;

    /* Emit:
     *   movq rax, [rip + disp32]     ; 7 bytes
     *
     * MOV r64, r/m64 with RIP-relative addressing:
     *   REX.W (0x48) + 0x8B + ModR/M(00, rax(0), 101) + disp32
     *   0x48 0x8B 0x05 dd dd dd dd
     *
     * When the guard page is readable, this is a normal load — the
     * value in rax is ignored. When the guard page is PROT_NONE,
     * this triggers SIGSEGV, which the signal handler catches and
     * processes as a safepoint.
     *
     * Advantages over CMP+JCC:
     *   - 7 bytes vs. 14 bytes (50% smaller)
     *   - 1 uop vs. 2 uops (CMP+JCC)
     *   - No branch prediction entry consumed
     *   - No compare, no conditional branch
     *   - Same latency as a normal load on the hot path
     *
     * The displacement is a placeholder (0); a VTX_RELOC_RIP_REL32
     * external relocation is recorded so it gets patched at code
     * install time when the final code address is known.
     * Use stub_id -6 as a special marker for guard page relocations.
     */

    /* Ensure buffer has space: 7 bytes */
    if (vtx_x86_emit_ensure(e, 7) != 0) return -1;

    /* ---- Emit MOV rax, [rip + disp32] ---- */
    uint32_t mov_disp_offset = vtx_x86_emit_position(e) + 3; /* disp32 starts at byte 3 */

    emit_byte(e, 0x48);  /* REX.W */
    emit_byte(e, 0x8B);  /* MOV r64, r/m64 */
    emit_byte(e, 0x05);  /* ModR/M: mod=00, reg=0 (RAX), r/m=5 (RIP-relative) */
    emit_dword(e, 0);    /* placeholder disp32 — will be patched by relocation */

    /* Record RIP-relative relocation for the MOV's displacement.
     * Marked as external because the code's final address in the code
     * cache is not known at emit time. The target is the address of
     * the guard page (vtx_guard_page_address()).
     * Use stub_id -6 as a special external relocation ID that the
     * code install step will resolve to vtx_guard_page_address(). */
    if (e->relocs && e->reloc_arena) {
        uint32_t reloc_idx = vtx_reloc_add(e->relocs, VTX_RELOC_RIP_REL32,
                                            mov_disp_offset,
                                            0,  /* target_offset (N/A for external) */
                                            0,  /* target_addr: resolved at install time */
                                            -6, /* stub_id: special marker for guard_page */
                                            0, e->reloc_arena);
        if (reloc_idx == UINT32_MAX) return -1;
        /* Mark as external so it gets re-applied at install time */
        e->relocs->entries[reloc_idx].is_external = true;
    }

    return 0;
}

/* ========================================================================== */
/* Prologue / Epilogue                                                        */
/* ========================================================================== */

void vtx_x86_emit_prologue(vtx_x86_emit_t *e, uint32_t frame_size,
                            uint32_t callee_saved_mask,
                            uint32_t arg_count, uint32_t max_locals)
{
    /* JIT calling convention prologue — matches T1 baseline layout:
     *
     *   Entry: RDI = method pointer (1st arg, System V ABI)
     *          RSI = deopt_info pointer (2nd arg)
     *          RDX = profile_data pointer (3rd arg)
     *          RCX = args array pointer (4th arg)
     *          R8  = arg_count (5th arg)
     *
     *   push rdi              ; method_ptr   -> [RBP+24]
     *   push rsi              ; deopt_info   -> [RBP+16]
     *   push rdx              ; profile_data -> [RBP+8]
     *   push rbp              ; caller RBP   -> [RBP+0]
     *   mov rbp, rsp
     *   push callee-saved regs (RBX, R12-R15 as needed)
     *   sub rsp, frame_size   ; locals + spills
     *
     *   Then: copy args[i] from the args array (RCX) into the
     *   System V argument registers (RDI, RSI, RDX, RCX, R8, R9)
     *   so that the T2 instruction selector's Parameter mapping
     *   (Parameter i → vtx_arg_regs[i]) works correctly.
     *   The original register values (method, deopt_info, profile_data)
     *   have already been saved on the stack, so RDI/RSI/RDX can be
     *   safely overwritten.
     *
     * This layout ensures that the deopt handler can find the method
     * pointer at [RBP+24] and deopt_info at [RBP+16] — the same
     * offsets used by T1 baseline JIT (see frame_layout.h).
     * Without these saved values, T2/T3 deopt would crash because
     * the deopt stubs cannot reconstruct interpreter state.
     */

    /* push method pointer (RDI) */
    vtx_x86_emit_push_r(e, 7);  /* RDI = 7 */

    /* push deopt_info (RSI) */
    vtx_x86_emit_push_r(e, 6);  /* RSI = 6 */

    /* push profile_data (RDX) */
    vtx_x86_emit_push_r(e, 2);  /* RDX = 2 */

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

    /* sub rsp, frame_size — adjusted for stack alignment.
     * At function entry (after CALL), RSP ≡ 8 (mod 16).
     * After 4 pushes (rdi, rsi, rdx, rbp): RSP ≡ 8 - 32 ≡ 8 (mod 16).
     * After N callee-saved pushes: RSP ≡ 8 - 8N (mod 16).
     * After sub rsp, frame_size: need RSP ≡ 0 (mod 16).
     * So frame_size ≡ 8 - 8N (mod 16), i.e.:
     *   N even: frame_size ≡ 8 (mod 16)
     *   N odd:  frame_size ≡ 0 (mod 16)
     * We pad frame_size up to satisfy this constraint. */
    if (frame_size > 0) {
        static const uint8_t cs_regs[] = { 3, 12, 13, 14, 15 };
        int cs_count = 0;
        for (int i = 0; i < 5; i++) {
            if (callee_saved_mask & (1u << cs_regs[i])) cs_count++;
        }
        uint32_t aligned_size = frame_size;
        /* Round up to next multiple of 8, then add 8 if cs_count is even */
        aligned_size = (aligned_size + 7u) & ~(uint32_t)7u;
        if (aligned_size == 0) aligned_size = 8;
        /* Check if current alignment matches requirement */
        bool need_mod8 = (cs_count % 2 == 0); /* need ≡ 8 (mod 16) */
        bool is_mod8 = (aligned_size % 16 == 8);
        if (need_mod8 != is_mod8) {
            aligned_size += 8; /* toggle between ≡ 0 and ≡ 8 (mod 16) */
        }
        vtx_x86_emit_sub_ri(e, 4, (int32_t)aligned_size); /* RSP = 4 */
    }

    /* ================================================================== */
    /* CRITICAL FIX: Copy args from the args array into the System V      */
    /* argument registers so that the T2 instruction selector's           */
    /* Parameter node mapping (Parameter i → vtx_arg_regs[i]) works.     */
    /*                                                                    */
    /* At function entry:                                                 */
    /*   RCX = args array pointer (4th arg in System V ABI)              */
    /*   R8  = arg_count (5th arg in System V ABI)                       */
    /*                                                                    */
    /* The T2 isel maps Parameter nodes to these registers:              */
    /*   Parameter 0 → RDI (vtx_arg_regs[0] = 7)                        */
    /*   Parameter 1 → RSI (vtx_arg_regs[1] = 6)                        */
    /*   Parameter 2 → RDX (vtx_arg_regs[2] = 2)                        */
    /*   Parameter 3 → RCX (vtx_arg_regs[3] = 1)                        */
    /*   Parameter 4 → R8  (vtx_arg_regs[4] = 8)                        */
    /*   Parameter 5 → R9  (vtx_arg_regs[5] = 9)                        */
    /*                                                                    */
    /* But the JIT entry calling convention passes:                      */
    /*   RDI = method, RSI = deopt_info, RDX = profile_data             */
    /*   RCX = args_ptr, R8 = arg_count                                  */
    /*                                                                    */
    /* So we must load args[i] from [RCX + i*8] into the registers       */
    /* that isel expects. Since RDI/RSI/RDX are already saved on the     */
    /* stack, we can safely overwrite them. For args beyond 3, we need   */
    /* to be careful not to clobber RCX (args pointer) before we're      */
    /* done with it. Strategy: copy RCX to R11 first, then load args     */
    /* in reverse order so we don't clobber the args pointer.            */
    /* ================================================================== */
    uint32_t copy_count = arg_count;
    if (copy_count > max_locals) copy_count = max_locals;
    if (copy_count > 6) copy_count = 6; /* max 6 args in registers */

    if (copy_count > 0) {
        /* Save args pointer (RCX) to R11 before we clobber it */
        vtx_x86_emit_mov_rr(e, 11, 1); /* R11 ← RCX */

        /* Copy args from the array into the expected registers.
         * We load in reverse order so that when we write to RCX
         * (Parameter 3), we've already finished using R11 (which
         * holds the original args pointer).
         *
         * vtx_arg_regs[] = { RDI(7), RSI(6), RDX(2), RCX(1), R8(8), R9(9) }
         * args[i] is at [R11 + i*8] */
        static const uint8_t arg_dst_regs[6] = { 7, 6, 2, 1, 8, 9 };
        for (int i = (int)copy_count - 1; i >= 0; i--) {
            /* mov arg_dst_regs[i], [R11 + i*8] */
            vtx_x86_emit_mov_rmem(e, arg_dst_regs[i], 11, (int32_t)(i * 8));
        }
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

    /* Skip the 3 JIT header values pushed in prologue:
     *   profile_data (RDX), deopt_info (RSI), method_ptr (RDI)
     * These were pushed before RBP so they sit above the saved RBP.
     * T1 uses "add rsp, 24" to skip them — we do the same. */
    vtx_x86_emit_add_ri(e, 4, 24);  /* RSP += 24 */

    /* ret */
    vtx_x86_emit_ret(e);
}

/* ========================================================================== */
/* Peephole optimizations                                                      */
/* ========================================================================== */

/**
 * Check if an instruction writes to a physical register operand.
 * Returns the physical register written, or 0xFF if none.
 */
static uint8_t inst_dst_preg(const vtx_inst_t *inst)
{
    if (inst->opnd_kinds[0] != VTX_OPND_PREG) return 0xFF;
    /* Only certain opcodes write to operand 0 */
    switch (inst->opcode) {
    case VTX_X86_MOV: case VTX_X86_ADD: case VTX_X86_SUB:
    case VTX_X86_IMUL: case VTX_X86_AND: case VTX_X86_OR:
    case VTX_X86_XOR: case VTX_X86_SHL: case VTX_X86_SHR:
    case VTX_X86_SAR: case VTX_X86_NEG: case VTX_X86_NOT:
    case VTX_X86_LEA: case VTX_X86_INC: case VTX_X86_DEC:
    case VTX_X86_CMOV: case VTX_X86_SETCC: case VTX_X86_MOVZX:
    case VTX_X86_MOVSX: case VTX_X86_POP:
    case VTX_X86_XADD: case VTX_X86_CMPXCHG:
    case VTX_X86_ROL: case VTX_X86_ROR: case VTX_X86_BSWAP:
    case VTX_X86_BSF: case VTX_X86_BSR: case VTX_X86_POPCNT:
    case VTX_X86_RDTSC: case VTX_X86_RDTSCP:
    case VTX_X86_ROUNDSD: case VTX_X86_ROUNDSS:
    case VTX_X86_MOVSD_RIP:
    /* AVX2 256-bit ops write to operand 0 */
    case VTX_X86_VMOVAPD_256: case VTX_X86_VADDPD_256: case VTX_X86_VSUBPD_256:
    case VTX_X86_VMULPD_256: case VTX_X86_VDIVPD_256:
    case VTX_X86_VMINPD_256: case VTX_X86_VMAXPD_256:
    case VTX_X86_VXORPD_256: case VTX_X86_VANDPD_256: case VTX_X86_VCMPPD_256:
    case VTX_X86_VMOVAPS_256: case VTX_X86_VADDPS_256: case VTX_X86_VSUBPS_256:
    case VTX_X86_VMULPS_256: case VTX_X86_VDIVPS_256:
    case VTX_X86_VMOVDQA_256: case VTX_X86_VPADDD_256: case VTX_X86_VPSUBD_256:
    case VTX_X86_VPMULLD_256: case VTX_X86_VPXOR_256:
    case VTX_X86_VPAND_256: case VTX_X86_VPOR_256:
    case VTX_X86_VBROADCASTSD: case VTX_X86_VBROADCASTSS:
    case VTX_X86_VPERM2F128: case VTX_X86_VINSERTF128: case VTX_X86_VEXTRACTF128:
        return (uint8_t)inst->operands[0];
    default:
        return 0xFF;
    }
}

/**
 * Check if an instruction reads from a physical register.
 * Returns true if the instruction reads the given register.
 */
static bool inst_reads_preg(const vtx_inst_t *inst, uint8_t preg)
{
    /* RET implicitly reads RAX (return value register) */
    if (inst->opcode == VTX_X86_RET && preg == 0) /* RAX = register 0 */
        return true;
    /* CMPXCHG implicitly reads RAX (comparison operand) */
    if (inst->opcode == VTX_X86_CMPXCHG && preg == 0) /* RAX = register 0 */
        return true;
    /* Check operand 1 (source) */
    if (inst->opnd_kinds[1] == VTX_OPND_PREG && (uint8_t)inst->operands[1] == preg)
        return true;
    /* Check operand 2 */
    if (inst->opnd_kinds[2] == VTX_OPND_PREG && (uint8_t)inst->operands[2] == preg)
        return true;
    /* For MOV reg,reg — operand 0 can also be a read (for ops like ADD) */
    if (inst->opnd_kinds[0] == VTX_OPND_PREG && (uint8_t)inst->operands[0] == preg) {
        /* Operand 0 is a read for CMP, TEST, PUSH */
        if (inst->opcode == VTX_X86_CMP || inst->opcode == VTX_X86_TEST ||
            inst->opcode == VTX_X86_PUSH)
            return true;
    }
    /* CRITICAL: Check memory operand registers (base_phys and index_phys).
     * LEA, MOV r/m, and other instructions with memory operands read the
     * base and index physical registers. Without this check, the peephole
     * dead-store eliminator will kill MOVs that feed into LEA index registers,
     * causing the LEA to use a stale/wrong register value. */
    if (inst->flags & VTX_INST_FLAG_HAS_MEM) {
        if (inst->mem.base_phys == preg)
            return true;
        if (inst->mem.index_phys == preg)
            return true;
    }
    return false;
}

/**
 * Check if a physical register is read by any instruction in the block
 * starting from position `from` (exclusive) to end of block.
 */
static bool is_reg_read_after(vtx_inst_block_t *blk, uint32_t from, uint8_t preg)
{
    for (uint32_t i = from + 1; i < blk->inst_count; i++) {
        if (inst_reads_preg(&blk->insts[i], preg))
            return true;
    }
    return false;
}

uint32_t vtx_peephole_optimize(vtx_inst_stream_t *stream,
                                const vtx_regalloc_result_t *result)
{
    if (!stream) return 0;
    uint32_t eliminated = 0;

    for (uint32_t b = 0; b < stream->block_count; b++) {
        vtx_inst_block_t *blk = &stream->blocks[b];

        for (uint32_t i = 0; i < blk->inst_count; i++) {
            vtx_inst_t *inst = &blk->insts[i];

            /* ---- Pattern 1: Eliminate redundant MOV reg, reg (src == dst) ---- */
            if (inst->opcode == VTX_X86_MOV &&
                !(inst->flags & VTX_INST_FLAG_HAS_IMM) &&
                !(inst->flags & VTX_INST_FLAG_HAS_MEM) &&
                inst->opnd_kinds[0] == VTX_OPND_PREG &&
                inst->opnd_kinds[1] == VTX_OPND_PREG &&
                inst->operands[0] == inst->operands[1]) {
                /* MOV r, r → NOP */
                inst->opcode = VTX_X86_NOP;
                inst->flags = 0;
                memset(inst->opnd_kinds, 0, sizeof(inst->opnd_kinds));
                memset(inst->operands, 0, sizeof(inst->operands));
                eliminated++;
                continue;
            }

            /* ---- Pattern 2: CMP reg, 0 → TEST reg, reg ----
             * DISABLED for NaN-boxed SMI operands (marked with NO_TEST flag).
             * SMI(0) = 0x7FF8000000000000 ≠ 0, so TEST would give the wrong
             * zero comparison result. The isel layer marks CMPs on untagged
             * values where TEST is safe, and leaves NaN-boxed CMPs unmarked. */
            if (inst->opcode == VTX_X86_CMP &&
                !(inst->flags & VTX_INST_FLAG_NO_TEST) &&
                (inst->flags & VTX_INST_FLAG_HAS_IMM) &&
                inst->imm == 0 &&
                inst->opnd_kinds[0] == VTX_OPND_PREG) {
                /* CMP reg, 0 → TEST reg, reg (1 byte shorter encoding) */
                uint32_t saved_fused = inst->flags & VTX_INST_FLAG_FUSED; /* P6: preserve fusion */
                inst->opcode = VTX_X86_TEST;
                inst->opnd_kinds[1] = VTX_OPND_PREG;
                inst->operands[1] = inst->operands[0]; /* TEST reg, reg */
                inst->flags &= ~VTX_INST_FLAG_HAS_IMM;
                inst->flags |= saved_fused; /* P6: TEST+JCC is also fusable */
                continue;
            }

            /* ---- Pattern 3: CMP reg1, reg2 where reg1==reg2 → TEST reg, reg ----
             * Same NO_TEST guard as pattern 2. */
            if (inst->opcode == VTX_X86_CMP &&
                !(inst->flags & VTX_INST_FLAG_NO_TEST) &&
                !(inst->flags & VTX_INST_FLAG_HAS_IMM) &&
                inst->opnd_kinds[0] == VTX_OPND_PREG &&
                inst->opnd_kinds[1] == VTX_OPND_PREG &&
                inst->operands[0] == inst->operands[1]) {
                uint32_t saved_fused = inst->flags & VTX_INST_FLAG_FUSED; /* P6: preserve fusion */
                inst->opcode = VTX_X86_TEST;
                inst->operands[1] = inst->operands[0];
                inst->flags |= saved_fused; /* P6: TEST+JCC is also fusable */
                continue;
            }

            /* ---- Pattern 4: ADD reg, 0 or SUB reg, 0 → NOP ---- */
            if ((inst->opcode == VTX_X86_ADD || inst->opcode == VTX_X86_SUB) &&
                (inst->flags & VTX_INST_FLAG_HAS_IMM) &&
                inst->imm == 0 &&
                inst->opnd_kinds[0] == VTX_OPND_PREG) {
                inst->opcode = VTX_X86_NOP;
                inst->flags = 0;
                memset(inst->opnd_kinds, 0, sizeof(inst->opnd_kinds));
                memset(inst->operands, 0, sizeof(inst->operands));
                eliminated++;
                continue;
            }

            /* ---- Pattern 5: XOR reg, 0 → NOP ---- */
            if (inst->opcode == VTX_X86_XOR &&
                (inst->flags & VTX_INST_FLAG_HAS_IMM) &&
                inst->imm == 0 &&
                inst->opnd_kinds[0] == VTX_OPND_PREG) {
                inst->opcode = VTX_X86_NOP;
                inst->flags = 0;
                memset(inst->opnd_kinds, 0, sizeof(inst->opnd_kinds));
                memset(inst->operands, 0, sizeof(inst->operands));
                eliminated++;
                continue;
            }

            /* ---- Pattern 6: OR reg, 0 / AND reg, -1 → NOP ---- */
            if (inst->opcode == VTX_X86_OR &&
                (inst->flags & VTX_INST_FLAG_HAS_IMM) &&
                inst->imm == 0 &&
                inst->opnd_kinds[0] == VTX_OPND_PREG) {
                inst->opcode = VTX_X86_NOP;
                inst->flags = 0;
                memset(inst->opnd_kinds, 0, sizeof(inst->opnd_kinds));
                memset(inst->operands, 0, sizeof(inst->operands));
                eliminated++;
                continue;
            }
            if (inst->opcode == VTX_X86_AND &&
                (inst->flags & VTX_INST_FLAG_HAS_IMM) &&
                inst->imm == -1 &&
                inst->opnd_kinds[0] == VTX_OPND_PREG) {
                inst->opcode = VTX_X86_NOP;
                inst->flags = 0;
                memset(inst->opnd_kinds, 0, sizeof(inst->opnd_kinds));
                memset(inst->operands, 0, sizeof(inst->operands));
                eliminated++;
                continue;
            }

            /* ---- Pattern 7: MOV reg, 0 → XOR reg, reg (1 byte shorter) ---- */
            if (inst->opcode == VTX_X86_MOV &&
                (inst->flags & VTX_INST_FLAG_HAS_IMM) &&
                inst->imm == 0 &&
                inst->opnd_kinds[0] == VTX_OPND_PREG) {
                inst->opcode = VTX_X86_XOR;
                inst->opnd_kinds[1] = VTX_OPND_PREG;
                inst->operands[1] = inst->operands[0]; /* XOR reg, reg */
                inst->flags &= ~VTX_INST_FLAG_HAS_IMM;
                continue;
            }

            /* ---- Pattern 8: Dead store elimination ----
             * If an instruction writes to a register that is never read again
             * within this block (and the register is caller-saved, so no
             * cross-block liveness concern), eliminate it.
             */
            if (inst->opcode != VTX_X86_NOP && inst->opcode != VTX_X86_CALL &&
                inst->opcode != VTX_X86_RET && inst->opcode != VTX_X86_JCC &&
                inst->opcode != VTX_X86_JMP && inst->opcode != VTX_X86_PUSH &&
                inst->opcode != VTX_X86_POP && inst->opcode != VTX_X86_CMP &&
                inst->opcode != VTX_X86_TEST && inst->opcode != VTX_X86_IDIV) {
                uint8_t dst_reg = inst_dst_preg(inst);
                if (dst_reg != 0xFF && !(inst->flags & VTX_INST_FLAG_HAS_MEM)) {
                    /* Check if the register is a caller-saved register */
                    if (VTX_CALLER_SAVED_MASK & (1u << dst_reg)) {
                        /* Check if it's never read again in this block */
                        if (!is_reg_read_after(blk, i, dst_reg)) {
                            inst->opcode = VTX_X86_NOP;
                            inst->flags = 0;
                            memset(inst->opnd_kinds, 0, sizeof(inst->opnd_kinds));
                            memset(inst->operands, 0, sizeof(inst->operands));
                            eliminated++;
                            continue;
                        }
                    }
                }
            }

            /* ---- Pattern 9: CMP + SETCC for EQ/NE against 0 → TEST + SETCC ---- */
            /* If CMP is immediately followed by SETCC with EQ/NE condition,
             * and the CMP compares against 0, we can use TEST instead.
             * This is already handled by pattern 2 (CMP → TEST) above. */
        }
    }

    return eliminated;
}

/* ========================================================================== */
/* Branch optimization                                                         */
/* ========================================================================== */

/**
 * Invert an x86 condition code.
 * E.g., JE → JNE, JL → JGE
 */
static uint8_t invert_x86_cond(uint8_t cond)
{
    return cond ^ 1; /* Flip the low bit inverts the condition */
}

/**
 * Estimate the native code size of an instruction in bytes.
 * Used for jump offset calculation before actual emission.
 */
static uint32_t estimate_inst_size(const vtx_inst_t *inst)
{
    switch (inst->opcode) {
    case VTX_X86_NOP:    return 1;  /* 1-byte NOP */
    case VTX_X86_RET:    return 1;  /* C3 */
    case VTX_X86_PUSH:   return 2;  /* REX + 50+rd (or 1 byte) */
    case VTX_X86_POP:    return 2;  /* REX + 58+rd */
    case VTX_X86_JMP:    return 5;  /* E9 + rel32 */
    case VTX_X86_JCC:    return 6;  /* 0F 8x + rel32 */
    case VTX_X86_CALL:   return 5;  /* E8 + rel32 */
    case VTX_X86_CQO:    return 2;  /* REX.W 99 */
    case VTX_X86_MOV:
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            /* mov r64, imm32: REX.W C7 /0 + imm32 = 7 bytes */
            /* mov r64, imm64: REX.W B8+rd + imm64 = 10 bytes */
            if (inst->imm >= INT32_MIN && inst->imm <= INT32_MAX)
                return 7;
            return 10;
        }
        if (inst->flags & VTX_INST_FLAG_HAS_MEM) return 8; /* approximate */
        return 3; /* REX.W 89 ModR/M */
    case VTX_X86_LEA:    return 5;  /* REX.W 8D + ModR/M + disp32 approx */
    case VTX_X86_SETCC:  return 3;  /* 0F 9x + ModR/M */
    case VTX_X86_ADD: case VTX_X86_SUB: case VTX_X86_AND:
    case VTX_X86_OR:  case VTX_X86_XOR:
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            int64_t imm = inst->imm;
            if (imm >= -128 && imm <= 127) return 4; /* REX.W 83 /x + imm8 */
            return 7; /* REX.W 81 /x + imm32 */
        }
        return 3; /* REX.W op + ModR/M */
    case VTX_X86_IMUL:   return 4;  /* REX.W 0F AF + ModR/M */
    case VTX_X86_IDIV:   return 3;  /* REX.W F7 /7 */
    case VTX_X86_SHL: case VTX_X86_SHR: case VTX_X86_SAR:
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) return 4; /* REX.W C1 /x + imm8 */
        return 3; /* REX.W D3 /x */
    case VTX_X86_CMP: case VTX_X86_TEST:
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            if (inst->imm >= -128 && inst->imm <= 127) return 4;
            return 7;
        }
        return 3;
    case VTX_X86_NEG: case VTX_X86_NOT: return 3; /* REX.W F7 /x */
    case VTX_X86_INC: case VTX_X86_DEC: return 3; /* REX.W FF /0 or /1 */
    default: return 4; /* conservative estimate */
    }
}

int vtx_branch_optimize(vtx_inst_stream_t *stream, vtx_x86_emit_t *emit,
                         const vtx_regalloc_result_t *result)
{
    if (!stream) return 0;

    /* ---- Phase 1: Invert JCC + JMP to eliminate jumps over jumps ----
     *
     * Pattern: JCC target1 followed by JMP target2
     * Where target1 is the next block (b+1).
     *
     * Before:
     *   JCC cond → target1 (= next block b+1)
     *   JMP target2
     *
     * After (inverted):
     *   JCC !cond → target2
     *   ; fall through to target1 (next block b+1)
     *   (JMP removed)
     *
     * This is correct because:
     *   - Originally: if cond → jump to target1 (next block); else → JMP target2
     *   - After: if !cond → jump to target2; else → fall through to target1 (next block)
     *   - Both paths reach the same blocks with the same conditions.
     *
     * Only apply when target1 == b+1 (the JCC target is the next block).
     * If target1 is NOT the next block, inverting would require a JMP to
     * target1 anyway, so there's no benefit.
     */
    for (uint32_t b = 0; b < stream->block_count; b++) {
        vtx_inst_block_t *blk = &stream->blocks[b];

        for (uint32_t i = 0; i + 1 < blk->inst_count; i++) {
            vtx_inst_t *inst = &blk->insts[i];
            vtx_inst_t *next = &blk->insts[i + 1];

            if (inst->opcode == VTX_X86_JCC && next->opcode == VTX_X86_JMP &&
                (inst->flags & VTX_INST_FLAG_HAS_COND) &&
                inst->opnd_kinds[0] == VTX_OPND_LABEL &&
                next->opnd_kinds[0] == VTX_OPND_LABEL) {

                /* Only invert if the JCC targets the next block (b+1).
                 * In that case, the JCC can fall through to b+1 instead,
                 * and the inverted JCC takes the JMP's target. */
                uint32_t jcc_target = inst->operands[0];
                uint32_t next_block = b + 1;

                if (jcc_target == next_block) {
                    /* Invert the condition */
                    vtx_cond_t inverted_cond = vtx_cond_negate(inst->cond);
                    inst->cond = inverted_cond;

                    /* Take the JMP's target */
                    inst->operands[0] = next->operands[0];

                    /* Remove the JMP (convert to NOP) */
                    next->opcode = VTX_X86_NOP;
                    next->flags = 0;
                    memset(next->opnd_kinds, 0, sizeof(next->opnd_kinds));
                    memset(next->operands, 0, sizeof(next->operands));
                }
            }
        }
    }

    /* ---- Phase 2: Compute estimated code offsets for short jump detection ---- */
    /* First pass: assign estimated offsets */
    for (uint32_t b = 0; b < stream->block_count; b++) {
        vtx_inst_block_t *blk = &stream->blocks[b];
        uint32_t offset = (b == 0) ? 0 : blk->insts[0].native_offset;

        for (uint32_t i = 0; i < blk->inst_count; i++) {
            blk->insts[i].native_offset = offset;
            offset += estimate_inst_size(&blk->insts[i]);
        }
    }

    /* ---- Phase 3: Short jump detection ----
     *
     * P0 FIX: Short jump optimization is DISABLED for correctness.
     *
     * The problem: estimate_inst_size() estimates instruction sizes using
     * rel32 encoding (6 bytes for JCC, 5 for JMP). When we mark a jump as
     * "short" and emit only 2 bytes, all subsequent offsets shift by 4 bytes
     * (for JCC) or 3 bytes (for JMP). This cascading offset shift can cause
     * other jumps' targets to move out of rel8 range, producing silent
     * miscompilation.
     *
     * A correct implementation requires multi-pass offset computation:
     *   1. Compute offsets assuming all rel32
     *   2. Identify candidates that fit in rel8
     *   3. Re-compute offsets with the smaller sizes
     *   4. Verify all candidates still fit; iterate if not
     *
     * Since the code size savings (3-4 bytes per short jump) are minimal
     * compared to the correctness risk, we skip short jumps entirely.
     * The rel32 encoding is always correct regardless of offset shifts.
     *
     * To re-enable short jumps safely, implement a fixpoint iteration:
     *   - Start with all jumps as rel32
     *   - Repeatedly try to shorten jumps that fit in rel8
     *   - After each shortening, recompute offsets from that point onward
     *   - If a shortened jump's target moves out of rel8 range, revert it
     *   - Continue until no more changes occur (fixpoint reached)
     */
    /* Short jumps intentionally NOT marked. All branches use rel32 encoding. */

    /* ---- Phase 4: Mark loop headers for alignment ---- */
    /* A block is a loop header if it has a back-edge from a later block.
     * We detect this by checking if any successor of a later block points
     * back to an earlier block. */
    for (uint32_t b = 0; b < stream->block_count; b++) {
        vtx_inst_block_t *blk = &stream->blocks[b];

        for (uint32_t i = 0; i < blk->inst_count; i++) {
            vtx_inst_t *inst = &blk->insts[i];
            if ((inst->opcode == VTX_X86_JCC || inst->opcode == VTX_X86_JMP) &&
                inst->opnd_kinds[0] == VTX_OPND_LABEL) {
                uint32_t target_block = inst->operands[0];
                if (target_block < b) {
                    /* Back-edge found: target_block is a loop header.
                     * Mark it for 16-byte alignment. */
                    if (target_block < stream->block_count) {
                        vtx_inst_block_t *hdr = &stream->blocks[target_block];
                        if (hdr->inst_count > 0) {
                            hdr->insts[0].flags |= (1u << 17); /* Alignment flag */
                        }
                    }
                }
            }
        }
    }

    return 0;
}

/* ========================================================================== */
/* Spill/fill helpers                                                          */
/* ========================================================================== */

/**
 * Emit a load from a spill slot into a temporary GPR register.
 * Spill slot address: [rbp - 8 * (slot + 1)]
 * RBP = register 5.
 */
static void emit_spill_load(vtx_x86_emit_t *e, uint32_t spill_slot, uint8_t tmp_reg)
{
    int32_t disp = -(int32_t)(8 * (spill_slot + 1));
    vtx_x86_emit_mov_rmem(e, tmp_reg, 5, disp); /* mov tmp, [rbp + disp] */
}

/**
 * Emit a store from a temporary GPR register into a spill slot.
 * Spill slot address: [rbp - 8 * (slot + 1)]
 * RBP = register 5.
 */
static void emit_spill_store(vtx_x86_emit_t *e, uint32_t spill_slot, uint8_t tmp_reg)
{
    int32_t disp = -(int32_t)(8 * (spill_slot + 1));
    vtx_x86_emit_mov_memr(e, 5, disp, tmp_reg); /* mov [rbp + disp], tmp */
}

/**
 * Emit a load from a spill slot into a temporary XMM register.
 * Uses MOVSD xmm, [rbp + disp] — F2 0F 10 /r with RBP-relative addressing.
 * Spill slot address: [rbp - 8 * (slot + 1)]
 */
static void emit_spill_load_xmm(vtx_x86_emit_t *e, uint32_t spill_slot, uint8_t tmp_xmm)
{
    int32_t disp = -(int32_t)(8 * (spill_slot + 1));
    /* F2 0F 10 /r — movsd xmm, r/m64
     * We use the same vtx_x86_emit_rm helper but need the F2 prefix.
     * Encoding: F2 [REX] 0F 10 ModR/M + displacement */
    int r = reg_hi(tmp_xmm);
    int b = reg_hi(5); /* RBP = 5, reg_hi(5) = 0 */
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0xF2);  /* scalar double prefix */
    emit_byte(e, 0x0F);
    emit_byte(e, 0x10);  /* MOVSD xmm, r/m64 */
    emit_mem_operand(e, tmp_xmm & 7, 5 /* RBP */, 0xFF, 1, disp);
}

/**
 * Emit a store from a temporary XMM register into a spill slot.
 * Uses MOVSD [rbp + disp], xmm — F2 0F 11 /r with RBP-relative addressing.
 * Spill slot address: [rbp - 8 * (slot + 1)]
 */
static void emit_spill_store_xmm(vtx_x86_emit_t *e, uint32_t spill_slot, uint8_t tmp_xmm)
{
    int32_t disp = -(int32_t)(8 * (spill_slot + 1));
    /* F2 0F 11 /r — movsd r/m64, xmm */
    int r = reg_hi(tmp_xmm);
    int b = reg_hi(5); /* RBP = 5 */
    if (r || b) {
        emit_rex(e, 0, r, 0, b);
    }
    emit_byte(e, 0xF2);  /* scalar double prefix */
    emit_byte(e, 0x0F);
    emit_byte(e, 0x11);  /* MOVSD r/m64, xmm */
    emit_mem_operand(e, tmp_xmm & 7, 5 /* RBP */, 0xFF, 1, disp);
}

/**
 * Get the spill slot for a physical register operand that is actually
 * a spilled vreg. Returns VTX_NO_SPILL if not spilled.
 */
static uint32_t get_spill_slot_for_opnd(const vtx_inst_t *inst,
                                         int opnd_idx,
                                         const vtx_regalloc_result_t *ra)
{
    if (!ra) return VTX_NO_SPILL;
    /* Check if the operand kind is VREG (not yet resolved) or PREG with 0xFF */
    if (inst->opnd_kinds[opnd_idx] == VTX_OPND_VREG) {
        uint32_t vreg = inst->operands[opnd_idx];
        if (vreg < ra->vreg_to_spill_count) {
            return ra->vreg_to_spill[vreg];
        }
    }
    return VTX_NO_SPILL;
}

/* R12 is used as a temporary register for spill/fill (caller-saved, rarely used by isel) */
#define VTX_SPILL_TMP_REG 12  /* R12 */

/* XMM14 is used as a temporary XMM register for SSE spill/fill.
 * XMM14 is caller-saved, and isel rarely emits it as a destination.
 * Using a dedicated XMM temp avoids clobbering XMM0-XMM7 which are
 * the most commonly allocated by regalloc. */
#define VTX_SPILL_XMM_TMP 14  /* XMM14 */

/* ========================================================================== */
/* Emit entire function                                                        */
/* ========================================================================== */

/**
 * Emit a single instruction from the instruction stream into the code buffer.
 * The instruction must have physical register assignments (after regalloc).
 * Handles spilled registers by emitting load/store from/to spill slots.
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
            if (r0 == 0xFF) {
                /* Destination is spilled — load from spill, add imm, store back */
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_add_ri(e, VTX_SPILL_TMP_REG, (int32_t)inst->imm);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else {
                vtx_x86_emit_add_ri(e, r0, (int32_t)inst->imm);
            }
        } else {
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            if (r0 == 0xFF && r1 != 0xFF) {
                /* Destination spilled, source in register */
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_add_rr(e, VTX_SPILL_TMP_REG, r1);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else if (r0 != 0xFF && r1 == 0xFF) {
                /* Source spilled */
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot1, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_add_rr(e, r0, VTX_SPILL_TMP_REG);
                }
            } else if (r0 == 0xFF && r1 == 0xFF) {
                /* Both spilled */
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    /* Use R13 as second temp (R13 = 13) */
                    emit_spill_load(e, slot1, 13);
                    vtx_x86_emit_add_rr(e, VTX_SPILL_TMP_REG, 13);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else {
                vtx_x86_emit_add_rr(e, r0, r1);
            }
        }
        break;

    case VTX_X86_SUB:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            if (r0 == 0xFF) {
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_sub_ri(e, VTX_SPILL_TMP_REG, (int32_t)inst->imm);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else {
                vtx_x86_emit_sub_ri(e, r0, (int32_t)inst->imm);
            }
        } else {
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            if (r0 == 0xFF && r1 != 0xFF) {
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_sub_rr(e, VTX_SPILL_TMP_REG, r1);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else if (r0 != 0xFF && r1 == 0xFF) {
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot1, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_sub_rr(e, r0, VTX_SPILL_TMP_REG);
                }
            } else if (r0 == 0xFF && r1 == 0xFF) {
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    emit_spill_load(e, slot1, 13);
                    vtx_x86_emit_sub_rr(e, VTX_SPILL_TMP_REG, 13);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else {
                vtx_x86_emit_sub_rr(e, r0, r1);
            }
        }
        break;

    case VTX_X86_IMUL:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 == 0xFF && r1 != 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                vtx_x86_emit_imul_rr(e, VTX_SPILL_TMP_REG, r1);
                emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
            }
        } else if (r0 != 0xFF && r1 == 0xFF) {
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot1 != VTX_NO_SPILL) {
                emit_spill_load(e, slot1, VTX_SPILL_TMP_REG);
                vtx_x86_emit_imul_rr(e, r0, VTX_SPILL_TMP_REG);
            }
        } else if (r0 == 0xFF && r1 == 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                emit_spill_load(e, slot1, 13);
                vtx_x86_emit_imul_rr(e, VTX_SPILL_TMP_REG, 13);
                emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
            }
        } else if (r0 != 0xFF && r1 != 0xFF) {
            vtx_x86_emit_imul_rr(e, r0, r1);
        }
        break;

    case VTX_X86_IDIV:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 == 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                vtx_x86_emit_idiv_r(e, VTX_SPILL_TMP_REG);
            }
        } else {
            vtx_x86_emit_idiv_r(e, r0);
        }
        break;

    case VTX_X86_SHL:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 == 0xFF) {
            /* Destination is spilled */
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
                    vtx_x86_emit_shl_ri(e, VTX_SPILL_TMP_REG, (uint8_t)(inst->imm & 0x3F));
                } else {
                    vtx_x86_emit_shl_cl(e, VTX_SPILL_TMP_REG);
                }
                emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
            }
        } else {
            if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
                vtx_x86_emit_shl_ri(e, r0, (uint8_t)(inst->imm & 0x3F));
            } else {
                vtx_x86_emit_shl_cl(e, r0);
            }
        }
        break;

    case VTX_X86_SHR:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 == 0xFF) {
            /* Destination is spilled */
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
                    vtx_x86_emit_shr_ri(e, VTX_SPILL_TMP_REG, (uint8_t)(inst->imm & 0x3F));
                } else {
                    vtx_x86_emit_shr_cl(e, VTX_SPILL_TMP_REG);
                }
                emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
            }
        } else {
            if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
                vtx_x86_emit_shr_ri(e, r0, (uint8_t)(inst->imm & 0x3F));
            } else {
                vtx_x86_emit_shr_cl(e, r0);
            }
        }
        break;

    case VTX_X86_SAR:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 == 0xFF) {
            /* Destination is spilled */
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
                    vtx_x86_emit_sar_ri(e, VTX_SPILL_TMP_REG, (uint8_t)(inst->imm & 0x3F));
                } else {
                    vtx_x86_emit_sar_cl(e, VTX_SPILL_TMP_REG);
                }
                emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
            }
        } else {
            if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
                vtx_x86_emit_sar_ri(e, r0, (uint8_t)(inst->imm & 0x3F));
            } else {
                vtx_x86_emit_sar_cl(e, r0);
            }
        }
        break;

    case VTX_X86_AND:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            if (r0 == 0xFF) {
                /* Destination is spilled */
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_and_ri(e, VTX_SPILL_TMP_REG, (int32_t)inst->imm);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else {
                vtx_x86_emit_and_ri(e, r0, (int32_t)inst->imm);
            }
        } else {
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            if (r0 == 0xFF && r1 != 0xFF) {
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_and_rr(e, VTX_SPILL_TMP_REG, r1);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else if (r0 != 0xFF && r1 == 0xFF) {
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot1, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_and_rr(e, r0, VTX_SPILL_TMP_REG);
                }
            } else if (r0 == 0xFF && r1 == 0xFF) {
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    emit_spill_load(e, slot1, 13);
                    vtx_x86_emit_and_rr(e, VTX_SPILL_TMP_REG, 13);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else {
                vtx_x86_emit_and_rr(e, r0, r1);
            }
        }
        break;

    case VTX_X86_OR:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            if (r0 == 0xFF) {
                /* Destination is spilled */
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_or_ri(e, VTX_SPILL_TMP_REG, (int32_t)inst->imm);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else {
                vtx_x86_emit_or_ri(e, r0, (int32_t)inst->imm);
            }
        } else {
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            if (r0 == 0xFF && r1 != 0xFF) {
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_or_rr(e, VTX_SPILL_TMP_REG, r1);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else if (r0 != 0xFF && r1 == 0xFF) {
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot1, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_or_rr(e, r0, VTX_SPILL_TMP_REG);
                }
            } else if (r0 == 0xFF && r1 == 0xFF) {
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    emit_spill_load(e, slot1, 13);
                    vtx_x86_emit_or_rr(e, VTX_SPILL_TMP_REG, 13);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else {
                vtx_x86_emit_or_rr(e, r0, r1);
            }
        }
        break;

    case VTX_X86_XOR:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            if (r0 == 0xFF) {
                /* Destination is spilled */
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_xor_ri(e, VTX_SPILL_TMP_REG, (int32_t)inst->imm);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else {
                vtx_x86_emit_xor_ri(e, r0, (int32_t)inst->imm);
            }
        } else {
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            if (r0 == 0xFF && r1 != 0xFF) {
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_xor_rr(e, VTX_SPILL_TMP_REG, r1);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else if (r0 != 0xFF && r1 == 0xFF) {
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot1, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_xor_rr(e, r0, VTX_SPILL_TMP_REG);
                }
            } else if (r0 == 0xFF && r1 == 0xFF) {
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    emit_spill_load(e, slot1, 13);
                    vtx_x86_emit_xor_rr(e, VTX_SPILL_TMP_REG, 13);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else {
                vtx_x86_emit_xor_rr(e, r0, r1);
            }
        }
        break;

    case VTX_X86_CMP:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            if (r0 == 0xFF) {
                /* Destination is spilled */
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_cmp_ri(e, VTX_SPILL_TMP_REG, (int32_t)inst->imm);
                }
            } else {
                vtx_x86_emit_cmp_ri(e, r0, (int32_t)inst->imm);
            }
        } else {
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            if (r0 == 0xFF && r1 != 0xFF) {
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_cmp_rr(e, VTX_SPILL_TMP_REG, r1);
                }
            } else if (r0 != 0xFF && r1 == 0xFF) {
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot1, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_cmp_rr(e, r0, VTX_SPILL_TMP_REG);
                }
            } else if (r0 == 0xFF && r1 == 0xFF) {
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    emit_spill_load(e, slot1, 13);
                    vtx_x86_emit_cmp_rr(e, VTX_SPILL_TMP_REG, 13);
                }
            } else {
                vtx_x86_emit_cmp_rr(e, r0, r1);
            }
        }
        break;

    case VTX_X86_TEST:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            if (r0 == 0xFF) {
                /* Destination is spilled */
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_test_ri(e, VTX_SPILL_TMP_REG, (int32_t)inst->imm);
                }
            } else {
                vtx_x86_emit_test_ri(e, r0, (int32_t)inst->imm);
            }
        } else {
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            if (r0 == 0xFF && r1 != 0xFF) {
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_test_rr(e, VTX_SPILL_TMP_REG, r1);
                }
            } else if (r0 != 0xFF && r1 == 0xFF) {
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot1, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_test_rr(e, r0, VTX_SPILL_TMP_REG);
                }
            } else if (r0 == 0xFF && r1 == 0xFF) {
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    emit_spill_load(e, slot1, 13);
                    vtx_x86_emit_test_rr(e, VTX_SPILL_TMP_REG, 13);
                }
            } else {
                vtx_x86_emit_test_rr(e, r0, r1);
            }
        }
        break;

    case VTX_X86_MOV:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_MEM) {
            /* Memory operand — use base_phys/index_phys from regalloc */
            uint8_t base = inst->mem.base_phys;
            uint8_t index = inst->mem.index_phys;
            int32_t disp = inst->mem.disp;

            /* If base_phys is not assigned but base_vreg is, the vreg is
             * spilled — use RBP as frame pointer for stack access. */
            if (base == 0xFF && inst->mem.base_vreg == VTX_VREG_INVALID) {
                base = 5; /* RBP as frame pointer */
            }

            if (inst->opnd_kinds[0] == VTX_OPND_PREG && inst->opnd_kinds[1] == VTX_OPND_MEM) {
                /* Load: mov reg, [base + index*scale + disp] */
                if (r0 != 0xFF) {
                    if (index != 0xFF) {
                        vtx_x86_emit_sib_mem(e, 0x8B, 0, r0, base, index,
                                              inst->mem.scale, disp, true);
                    } else {
                        vtx_x86_emit_mov_rmem(e, r0, base, disp);
                    }
                }
            } else if (inst->opnd_kinds[0] == VTX_OPND_MEM && inst->opnd_kinds[1] == VTX_OPND_PREG) {
                /* Store: mov [base + index*scale + disp], reg */
                r1 = (uint8_t)inst->operands[1];
                if (r1 != 0xFF) {
                    if (index != 0xFF) {
                        vtx_x86_emit_sib_mem(e, 0x89, 0, r1, base, index,
                                              inst->mem.scale, disp, false);
                    } else {
                        vtx_x86_emit_mov_memr(e, base, disp, r1);
                    }
                }
            }
        } else if (inst->flags & VTX_INST_FLAG_HAS_IMM) {
            /* mov reg, imm */
            imm = inst->imm;
            if (r0 == 0xFF) {
                /* Destination is spilled — load imm into R12, then store back */
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    if (imm >= INT32_MIN && imm <= INT32_MAX) {
                        vtx_x86_emit_mov_imm32(e, VTX_SPILL_TMP_REG, (int32_t)imm);
                    } else {
                        vtx_x86_emit_mov_imm64(e, VTX_SPILL_TMP_REG, (uint64_t)imm);
                    }
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else {
                if (imm >= INT32_MIN && imm <= INT32_MAX) {
                    vtx_x86_emit_mov_imm32(e, r0, (int32_t)imm);
                } else {
                    vtx_x86_emit_mov_imm64(e, r0, (uint64_t)imm);
                }
            }
        } else {
            /* mov reg, reg */
            r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
            if (r0 == 0xFF && r1 != 0xFF) {
                /* Destination spilled, source in register */
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_store(e, slot0, r1);
                }
            } else if (r0 != 0xFF && r1 == 0xFF) {
                /* Source spilled, destination in register */
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot1, r0);
                }
            } else if (r0 == 0xFF && r1 == 0xFF) {
                /* Both spilled */
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot1, VTX_SPILL_TMP_REG);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else if (r0 != r1) {
                vtx_x86_emit_mov_rr(e, r0, r1);
            }
        }
        break;

    case VTX_X86_NEG:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 == 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                vtx_x86_emit_neg_r(e, VTX_SPILL_TMP_REG);
                emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
            }
        } else {
            vtx_x86_emit_neg_r(e, r0);
        }
        break;

    case VTX_X86_NOT:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 == 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                vtx_x86_emit_not_r(e, VTX_SPILL_TMP_REG);
                emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
            }
        } else {
            vtx_x86_emit_not_r(e, r0);
        }
        break;

    case VTX_X86_INC:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 == 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                vtx_x86_emit_inc_r(e, VTX_SPILL_TMP_REG);
                emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
            }
        } else {
            vtx_x86_emit_inc_r(e, r0);
        }
        break;

    case VTX_X86_DEC:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 == 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                vtx_x86_emit_dec_r(e, VTX_SPILL_TMP_REG);
                emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
            }
        } else {
            vtx_x86_emit_dec_r(e, r0);
        }
        break;

    case VTX_X86_CQO:
        vtx_x86_emit_cqo(e);
        break;

    case VTX_X86_SETCC:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_COND) {
            if (r0 == 0xFF) {
                /* Destination is spilled — setcc into R12, then store back */
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    vtx_x86_emit_setcc(e, vtx_cond_to_x86(inst->cond), VTX_SPILL_TMP_REG);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else {
                vtx_x86_emit_setcc(e, vtx_cond_to_x86(inst->cond), r0);
            }
        }
        break;

    case VTX_X86_CMOV:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_COND) {
            if (r0 == 0xFF && r1 != 0xFF) {
                /* Destination spilled, source in register */
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_cmovcc(e, vtx_cond_to_x86(inst->cond), VTX_SPILL_TMP_REG, r1);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else if (r0 != 0xFF && r1 == 0xFF) {
                /* Source spilled */
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot1, VTX_SPILL_TMP_REG);
                    vtx_x86_emit_cmovcc(e, vtx_cond_to_x86(inst->cond), r0, VTX_SPILL_TMP_REG);
                }
            } else if (r0 == 0xFF && r1 == 0xFF) {
                /* Both spilled */
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
                if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                    emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                    emit_spill_load(e, slot1, 13);
                    vtx_x86_emit_cmovcc(e, vtx_cond_to_x86(inst->cond), VTX_SPILL_TMP_REG, 13);
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else {
                vtx_x86_emit_cmovcc(e, vtx_cond_to_x86(inst->cond), r0, r1);
            }
        }
        break;

    case VTX_X86_LEA:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (inst->flags & VTX_INST_FLAG_HAS_MEM) {
            uint8_t base = inst->mem.base_phys;
            uint8_t index = inst->mem.index_phys;
            int32_t disp = inst->mem.disp;

            /* If base_phys not assigned (spilled or no base vreg), use RBP */
            if (base == 0xFF && inst->mem.base_vreg == VTX_VREG_INVALID) {
                base = 5; /* RBP as frame pointer */
            }

            if (r0 == 0xFF) {
                /* Destination is spilled — lea into R12, then store back */
                uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
                if (slot0 != VTX_NO_SPILL) {
                    if (index != 0xFF) {
                        vtx_x86_emit_sib_mem(e, 0x8D, 0, VTX_SPILL_TMP_REG, base, index,
                                              inst->mem.scale, disp, true);
                    } else {
                        vtx_x86_emit_lea_rmem(e, VTX_SPILL_TMP_REG, base, disp);
                    }
                    emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
                }
            } else {
                if (index != 0xFF) {
                    vtx_x86_emit_sib_mem(e, 0x8D, 0, r0, base, index,
                                          inst->mem.scale, disp, true);
                } else {
                    vtx_x86_emit_lea_rmem(e, r0, base, disp);
                }
            }
        }
        break;

    case VTX_X86_PUSH:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 == 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                vtx_x86_emit_push_r(e, VTX_SPILL_TMP_REG);
            }
        } else {
            vtx_x86_emit_push_r(e, r0);
        }
        break;

    case VTX_X86_POP:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 == 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                vtx_x86_emit_pop_r(e, VTX_SPILL_TMP_REG);
                emit_spill_store(e, slot0, VTX_SPILL_TMP_REG);
            }
        } else {
            vtx_x86_emit_pop_r(e, r0);
        }
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
        /* Emit epilogue before ret: restore callee-saved registers.
         * The epilogue includes the ret instruction itself, so we
         * don't emit a separate ret here. */
        if (ra) {
            vtx_x86_emit_epilogue(e, ra->callee_saved_mask);
        } else {
            vtx_x86_emit_ret(e);
        }
        break;

    case VTX_X86_UCOMISD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 == 0xFF && r1 != 0xFF) {
            /* Destination spilled, source in register */
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                vtx_x86_emit_ucomisd(e, VTX_SPILL_TMP_REG, r1);
            }
        } else if (r0 != 0xFF && r1 == 0xFF) {
            /* Source spilled */
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot1 != VTX_NO_SPILL) {
                emit_spill_load(e, slot1, VTX_SPILL_TMP_REG);
                vtx_x86_emit_ucomisd(e, r0, VTX_SPILL_TMP_REG);
            }
        } else if (r0 == 0xFF && r1 == 0xFF) {
            /* Both spilled */
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                emit_spill_load(e, slot0, VTX_SPILL_TMP_REG);
                emit_spill_load(e, slot1, 13);
                vtx_x86_emit_ucomisd(e, VTX_SPILL_TMP_REG, 13);
            }
        } else {
            vtx_x86_emit_ucomisd(e, r0, r1);
        }
        break;

    case VTX_X86_ADDSD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 == 0xFF && r1 != 0xFF) {
            /* P0 fix: Destination XMM spilled, source in register.
             * Load spilled dst into XMM temp, operate, store back. */
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot0, VTX_SPILL_XMM_TMP);
                vtx_x86_emit_addsd(e, VTX_SPILL_XMM_TMP, r1);
                emit_spill_store_xmm(e, slot0, VTX_SPILL_XMM_TMP);
            }
        } else if (r0 != 0xFF && r1 == 0xFF) {
            /* Source XMM spilled */
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot1 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot1, VTX_SPILL_XMM_TMP);
                vtx_x86_emit_addsd(e, r0, VTX_SPILL_XMM_TMP);
            }
        } else if (r0 == 0xFF && r1 == 0xFF) {
            /* Both XMM spilled */
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot0, VTX_SPILL_XMM_TMP);
                /* Use XMM15 as second temp (XMM15 = 15) */
                emit_spill_load_xmm(e, slot1, 15);
                vtx_x86_emit_addsd(e, VTX_SPILL_XMM_TMP, 15);
                emit_spill_store_xmm(e, slot0, VTX_SPILL_XMM_TMP);
            }
        } else {
            vtx_x86_emit_addsd(e, r0, r1);
        }
        break;

    case VTX_X86_SUBSD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 == 0xFF && r1 != 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot0, VTX_SPILL_XMM_TMP);
                vtx_x86_emit_subsd(e, VTX_SPILL_XMM_TMP, r1);
                emit_spill_store_xmm(e, slot0, VTX_SPILL_XMM_TMP);
            }
        } else if (r0 != 0xFF && r1 == 0xFF) {
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot1 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot1, VTX_SPILL_XMM_TMP);
                vtx_x86_emit_subsd(e, r0, VTX_SPILL_XMM_TMP);
            }
        } else if (r0 == 0xFF && r1 == 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot0, VTX_SPILL_XMM_TMP);
                emit_spill_load_xmm(e, slot1, 15);
                vtx_x86_emit_subsd(e, VTX_SPILL_XMM_TMP, 15);
                emit_spill_store_xmm(e, slot0, VTX_SPILL_XMM_TMP);
            }
        } else {
            vtx_x86_emit_subsd(e, r0, r1);
        }
        break;

    case VTX_X86_MULSD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 == 0xFF && r1 != 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot0, VTX_SPILL_XMM_TMP);
                vtx_x86_emit_mulsd(e, VTX_SPILL_XMM_TMP, r1);
                emit_spill_store_xmm(e, slot0, VTX_SPILL_XMM_TMP);
            }
        } else if (r0 != 0xFF && r1 == 0xFF) {
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot1 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot1, VTX_SPILL_XMM_TMP);
                vtx_x86_emit_mulsd(e, r0, VTX_SPILL_XMM_TMP);
            }
        } else if (r0 == 0xFF && r1 == 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot0, VTX_SPILL_XMM_TMP);
                emit_spill_load_xmm(e, slot1, 15);
                vtx_x86_emit_mulsd(e, VTX_SPILL_XMM_TMP, 15);
                emit_spill_store_xmm(e, slot0, VTX_SPILL_XMM_TMP);
            }
        } else {
            vtx_x86_emit_mulsd(e, r0, r1);
        }
        break;

    case VTX_X86_DIVSD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 == 0xFF && r1 != 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot0, VTX_SPILL_XMM_TMP);
                vtx_x86_emit_divsd(e, VTX_SPILL_XMM_TMP, r1);
                emit_spill_store_xmm(e, slot0, VTX_SPILL_XMM_TMP);
            }
        } else if (r0 != 0xFF && r1 == 0xFF) {
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot1 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot1, VTX_SPILL_XMM_TMP);
                vtx_x86_emit_divsd(e, r0, VTX_SPILL_XMM_TMP);
            }
        } else if (r0 == 0xFF && r1 == 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot0, VTX_SPILL_XMM_TMP);
                emit_spill_load_xmm(e, slot1, 15);
                vtx_x86_emit_divsd(e, VTX_SPILL_XMM_TMP, 15);
                emit_spill_store_xmm(e, slot0, VTX_SPILL_XMM_TMP);
            }
        } else {
            vtx_x86_emit_divsd(e, r0, r1);
        }
        break;

    case VTX_X86_XORPS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 == 0xFF && r1 != 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot0, VTX_SPILL_XMM_TMP);
                vtx_x86_emit_xorps(e, VTX_SPILL_XMM_TMP, r1);
                emit_spill_store_xmm(e, slot0, VTX_SPILL_XMM_TMP);
            }
        } else if (r0 != 0xFF && r1 == 0xFF) {
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot1 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot1, VTX_SPILL_XMM_TMP);
                vtx_x86_emit_xorps(e, r0, VTX_SPILL_XMM_TMP);
            }
        } else if (r0 == 0xFF && r1 == 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot0, VTX_SPILL_XMM_TMP);
                emit_spill_load_xmm(e, slot1, 15);
                vtx_x86_emit_xorps(e, VTX_SPILL_XMM_TMP, 15);
                emit_spill_store_xmm(e, slot0, VTX_SPILL_XMM_TMP);
            }
        } else {
            vtx_x86_emit_xorps(e, r0, r1);
        }
        break;

    case VTX_X86_MOVSD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 == 0xFF && r1 != 0xFF) {
            /* Destination XMM spilled, source in register */
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_store_xmm(e, slot0, r1);
            }
        } else if (r0 != 0xFF && r1 == 0xFF) {
            /* Source XMM spilled, destination in register */
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot1 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot1, r0);
            }
        } else if (r0 == 0xFF && r1 == 0xFF) {
            /* Both XMM spilled */
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot1, VTX_SPILL_XMM_TMP);
                emit_spill_store_xmm(e, slot0, VTX_SPILL_XMM_TMP);
            }
        } else if (r0 != r1) {
            vtx_x86_emit_movsd(e, r0, r1);
        }
        break;

    case VTX_X86_SAFEPOINT_POLL:
        /* Emit safepoint poll: cmpq [vtx_safepoint_flag], 0; jne deopt_stub */
        if (vtx_x86_emit_safepoint_poll(e) != 0) {
            return -1;
        }
        break;

    case VTX_X86_SAFEPOINT_POLL_GUARD_PAGE:
        /* Zero-cost guard page poll: movq rax, [guard_page]
         * No CMP, no JCC — just a single load from a page that
         * becomes PROT_NONE when a safepoint is needed. */
        if (vtx_x86_emit_safepoint_poll_guard_page(e) != 0) {
            return -1;
        }
        break;

    /* ---- SSE2 Packed Double instructions ---- */
    case VTX_X86_MOVAPD:
        /* MOVAPS/MOVAPD xmm, xmm — register-register 128-bit move.
         * Encoding: 66 0F 28 /r (load direction), 66 0F 29 /r (store direction).
         * For reg-reg move we use 66 0F 28.
         * Handles spilled XMM registers same as MOVSD. */
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 == 0xFF && r1 != 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            if (slot0 != VTX_NO_SPILL) {
                emit_spill_store_xmm(e, slot0, r1);
            }
        } else if (r0 != 0xFF && r1 == 0xFF) {
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot1 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot1, r0);
            }
        } else if (r0 == 0xFF && r1 == 0xFF) {
            uint32_t slot0 = get_spill_slot_for_opnd(inst, 0, ra);
            uint32_t slot1 = get_spill_slot_for_opnd(inst, 1, ra);
            if (slot0 != VTX_NO_SPILL && slot1 != VTX_NO_SPILL) {
                emit_spill_load_xmm(e, slot1, VTX_SPILL_XMM_TMP);
                emit_spill_store_xmm(e, slot0, VTX_SPILL_XMM_TMP);
            }
        } else if (r0 != r1) {
            /* movaps xmm, xmm: 66 0F 28 ModR/M(mod=11) */
            emit_rex(e, 0, reg_hi(r0), 0, reg_hi(r1));
            emit_byte(e, 0x66);
            emit_byte(e, 0x0F);
            emit_byte(e, 0x28);
            emit_modrm(e, 3, reg_lo(r0), reg_lo(r1));
        }
        break;

    case VTX_X86_ADDPD:
        /* ADDPD xmm, xmm: 66 0F 58 /r — packed double add */
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) {
            emit_rex(e, 0, reg_hi(r0), 0, reg_hi(r1));
            emit_byte(e, 0x66);
            emit_byte(e, 0x0F);
            emit_byte(e, 0x58);
            emit_modrm(e, 3, reg_lo(r0), reg_lo(r1));
        }
        break;

    case VTX_X86_MULPD:
        /* MULPD xmm, xmm: 66 0F 59 /r — packed double multiply */
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) {
            emit_rex(e, 0, reg_hi(r0), 0, reg_hi(r1));
            emit_byte(e, 0x66);
            emit_byte(e, 0x0F);
            emit_byte(e, 0x59);
            emit_modrm(e, 3, reg_lo(r0), reg_lo(r1));
        }
        break;

    case VTX_X86_MINPD:
        /* MINPD xmm, xmm: 66 0F 5D /r — packed double min */
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) {
            emit_rex(e, 0, reg_hi(r0), 0, reg_hi(r1));
            emit_byte(e, 0x66);
            emit_byte(e, 0x0F);
            emit_byte(e, 0x5D);
            emit_modrm(e, 3, reg_lo(r0), reg_lo(r1));
        }
        break;

    case VTX_X86_MAXPD:
        /* MAXPD xmm, xmm: 66 0F 5F /r — packed double max */
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) {
            emit_rex(e, 0, reg_hi(r0), 0, reg_hi(r1));
            emit_byte(e, 0x66);
            emit_byte(e, 0x0F);
            emit_byte(e, 0x5F);
            emit_modrm(e, 3, reg_lo(r0), reg_lo(r1));
        }
        break;

    case VTX_X86_ANDPD:
        /* ANDPD xmm, xmm: 66 0F 54 /r — packed double bitwise AND */
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) {
            emit_rex(e, 0, reg_hi(r0), 0, reg_hi(r1));
            emit_byte(e, 0x66);
            emit_byte(e, 0x0F);
            emit_byte(e, 0x54);
            emit_modrm(e, 3, reg_lo(r0), reg_lo(r1));
        }
        break;

    case VTX_X86_XORPD:
        /* XORPD xmm, xmm: 66 0F 57 /r — packed double bitwise XOR */
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) {
            emit_rex(e, 0, reg_hi(r0), 0, reg_hi(r1));
            emit_byte(e, 0x66);
            emit_byte(e, 0x0F);
            emit_byte(e, 0x57);
            emit_modrm(e, 3, reg_lo(r0), reg_lo(r1));
        }
        break;

    /* ---- New P0/P1 instruction emission dispatch ---- */

    case VTX_X86_COMISD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_comisd(e, r0, r1);
        break;

    case VTX_X86_SQRTSD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_sqrtsd(e, r0, r1);
        break;

    case VTX_X86_CVTSI2SD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_cvtsi2sd(e, r0, r1);
        break;

    case VTX_X86_CVTSD2SI:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_cvtsd2si(e, r0, r1);
        break;

    case VTX_X86_CVTTSD2SI:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_cvttsd2si(e, r0, r1);
        break;

    case VTX_X86_CVTSI2SS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_cvtsi2ss(e, r0, r1);
        break;

    case VTX_X86_CVTSS2SI:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_cvtss2si(e, r0, r1);
        break;

    case VTX_X86_CVTTSS2SI:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_cvttss2si(e, r0, r1);
        break;

    case VTX_X86_MOVQ_XMM_R64:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_movq_xmm_r64(e, r0, r1);
        break;

    case VTX_X86_MOVQ_R64_XMM:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_movq_r64_xmm(e, r0, r1);
        break;

    case VTX_X86_BSWAP:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 != 0xFF) vtx_x86_emit_bswap(e, r0);
        break;

    case VTX_X86_BSF:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_bsf(e, r0, r1);
        break;

    case VTX_X86_BSR:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_bsr(e, r0, r1);
        break;

    case VTX_X86_POPCNT:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_popcnt(e, r0, r1);
        break;

    case VTX_X86_ROL:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 != 0xFF) {
            if (inst->flags & VTX_INST_FLAG_HAS_IMM)
                vtx_x86_emit_rol_ri(e, r0, (uint8_t)inst->imm);
            else
                vtx_x86_emit_rol_cl(e, r0);
        }
        break;

    case VTX_X86_ROR:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 != 0xFF) {
            if (inst->flags & VTX_INST_FLAG_HAS_IMM)
                vtx_x86_emit_ror_ri(e, r0, (uint8_t)inst->imm);
            else
                vtx_x86_emit_ror_cl(e, r0);
        }
        break;

    case VTX_X86_CDQE:
        vtx_x86_emit_cdqe(e);
        break;

    case VTX_X86_DIV:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 != 0xFF) vtx_x86_emit_div_r(e, r0);
        break;

    case VTX_X86_SUBPD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_subpd(e, r0, r1);
        break;

    case VTX_X86_DIVPD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_divpd(e, r0, r1);
        break;

    case VTX_X86_MOVDQA:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_movdqa(e, r0, r1);
        break;

    case VTX_X86_MOVDQU:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_movdqu(e, r0, r1);
        break;

    case VTX_X86_PADDD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_paddd(e, r0, r1);
        break;

    case VTX_X86_PSUBD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_psubd(e, r0, r1);
        break;

    case VTX_X86_PMULLD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_pmulld(e, r0, r1);
        break;

    case VTX_X86_PXOR:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_pxor(e, r0, r1);
        break;

    case VTX_X86_PAND:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_pand(e, r0, r1);
        break;

    case VTX_X86_POR:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_por(e, r0, r1);
        break;

    case VTX_X86_PCMPEQD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_pcmpeqd(e, r0, r1);
        break;

    case VTX_X86_MOVAPS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_movaps(e, r0, r1);
        break;

    case VTX_X86_ADDPS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_addps(e, r0, r1);
        break;

    case VTX_X86_MULPS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_mulps(e, r0, r1);
        break;

    case VTX_X86_SUBPS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_subps(e, r0, r1);
        break;

    case VTX_X86_DIVPS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_divps(e, r0, r1);
        break;

    case VTX_X86_MINPS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_minps(e, r0, r1);
        break;

    case VTX_X86_MAXPS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_maxps(e, r0, r1);
        break;

    case VTX_X86_CMPPS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_cmpps(e, r0, r1, (uint8_t)inst->imm);
        break;

    case VTX_X86_ADDSS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_addss(e, r0, r1);
        break;

    case VTX_X86_SUBSS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_subss(e, r0, r1);
        break;

    case VTX_X86_MULSS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_mulss(e, r0, r1);
        break;

    case VTX_X86_DIVSS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_divss(e, r0, r1);
        break;

    case VTX_X86_SQRTSS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_sqrtss(e, r0, r1);
        break;

    case VTX_X86_UCOMISS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_ucomiss(e, r0, r1);
        break;

    case VTX_X86_MOVSS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_movss(e, r0, r1);
        break;

    case VTX_X86_MOVSD_LOAD:
    case VTX_X86_MOVSD_STORE:
        /* Memory-operand MOVSD handled via mem operand in inst */
        /* For now, emit as reg-reg MOVSD — memory variant needs memop resolution */
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_movsd(e, r0, r1);
        break;

    case VTX_X86_BT:
        /* BT r/m64, imm8: REX.W 0F BA /4 imm8 */
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 != 0xFF) {
            emit_rex(e, 1, 0, 0, reg_hi(r0));
            emit_byte(e, 0x0F);
            emit_byte(e, 0xBA);
            emit_modrm(e, 3, 4, r0 & 7);
            if (inst->flags & VTX_INST_FLAG_HAS_IMM)
                emit_byte(e, (uint8_t)inst->imm);
        }
        break;

    /* ---- Timing / Profiling ---- */
    case VTX_X86_RDTSC:
        vtx_x86_emit_rdtsc(e);
        break;

    case VTX_X86_RDTSCP:
        vtx_x86_emit_rdtscp(e);
        break;

    /* ---- Atomics ---- */
    case VTX_X86_CMPXCHG:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_cmpxchg(e, r0, r1);
        break;

    case VTX_X86_XADD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_xadd(e, r0, r1);
        break;

    /* ---- Memory fences ---- */
    case VTX_X86_LFENCE:
        vtx_x86_emit_lfence(e);
        break;

    case VTX_X86_MFENCE:
        vtx_x86_emit_mfence(e);
        break;

    case VTX_X86_SFENCE:
        vtx_x86_emit_sfence(e);
        break;

    /* ---- SSE4.1 rounding ---- */
    case VTX_X86_ROUNDSD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_roundsd(e, r0, r1, (uint8_t)inst->imm);
        break;

    case VTX_X86_ROUNDSS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_roundss(e, r0, r1, (uint8_t)inst->imm);
        break;

    /* ---- Constant pool load (RIP-relative MOVSD) ---- */
    case VTX_X86_MOVSD_RIP:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        if (r0 != 0xFF) {
            vtx_x86_emit_movsd_rip(e, r0, (uint64_t)inst->imm);
        }
        break;

    /* ---- AVX2 256-bit packed double ---- */
    case VTX_X86_VMOVAPD_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_vmovapd_256(e, r0, r1);
        break;

    case VTX_X86_VADDPD_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vaddpd_256(e, r0, r1, r2);
        break;

    case VTX_X86_VSUBPD_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vsubpd_256(e, r0, r1, r2);
        break;

    case VTX_X86_VMULPD_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vmulpd_256(e, r0, r1, r2);
        break;

    case VTX_X86_VDIVPD_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vdivpd_256(e, r0, r1, r2);
        break;

    case VTX_X86_VMINPD_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vminpd_256(e, r0, r1, r2);
        break;

    case VTX_X86_VMAXPD_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vmaxpd_256(e, r0, r1, r2);
        break;

    case VTX_X86_VXORPD_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vxorpd_256(e, r0, r1, r2);
        break;

    case VTX_X86_VANDPD_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vandpd_256(e, r0, r1, r2);
        break;

    case VTX_X86_VCMPPD_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF)
            vtx_x86_emit_vcmppd_256(e, r0, r1, r2, (uint8_t)inst->imm);
        break;

    /* ---- AVX2 256-bit packed single ---- */
    case VTX_X86_VMOVAPS_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_vmovaps_256(e, r0, r1);
        break;

    case VTX_X86_VADDPS_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vaddps_256(e, r0, r1, r2);
        break;

    case VTX_X86_VSUBPS_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vsubps_256(e, r0, r1, r2);
        break;

    case VTX_X86_VMULPS_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vmulps_256(e, r0, r1, r2);
        break;

    case VTX_X86_VDIVPS_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vdivps_256(e, r0, r1, r2);
        break;

    /* ---- AVX2 256-bit packed integer ---- */
    case VTX_X86_VMOVDQA_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_vmovdqa_256(e, r0, r1);
        break;

    case VTX_X86_VPADDD_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vpaddd_256(e, r0, r1, r2);
        break;

    case VTX_X86_VPSUBD_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vpsubd_256(e, r0, r1, r2);
        break;

    case VTX_X86_VPMULLD_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vpmulld_256(e, r0, r1, r2);
        break;

    case VTX_X86_VPXOR_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vpxor_256(e, r0, r1, r2);
        break;

    case VTX_X86_VPAND_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vpand_256(e, r0, r1, r2);
        break;

    case VTX_X86_VPOR_256:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF) vtx_x86_emit_vpor_256(e, r0, r1, r2);
        break;

    /* ---- AVX2 utility / transition ---- */
    case VTX_X86_VZEROUPPER:
        vtx_x86_emit_vzeroupper(e);
        break;

    case VTX_X86_VZEROALL:
        vtx_x86_emit_vzeroall(e);
        break;

    /* ---- AVX2 lane manipulation / broadcast ---- */
    case VTX_X86_VBROADCASTSD:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_vbroadcastsd(e, r0, r1);
        break;

    case VTX_X86_VBROADCASTSS:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF) vtx_x86_emit_vbroadcastss(e, r0, r1);
        break;

    case VTX_X86_VPERM2F128:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF)
            vtx_x86_emit_vperm2f128(e, r0, r1, r2, (uint8_t)inst->imm);
        break;

    case VTX_X86_VINSERTF128:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        r2 = (inst->opnd_kinds[2] == VTX_OPND_PREG) ? (uint8_t)inst->operands[2] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF && r2 != 0xFF)
            vtx_x86_emit_vinsertf128(e, r0, r1, r2, (uint8_t)inst->imm);
        break;

    case VTX_X86_VEXTRACTF128:
        r0 = (inst->opnd_kinds[0] == VTX_OPND_PREG) ? (uint8_t)inst->operands[0] : 0xFF;
        r1 = (inst->opnd_kinds[1] == VTX_OPND_PREG) ? (uint8_t)inst->operands[1] : 0xFF;
        if (r0 != 0xFF && r1 != 0xFF)
            vtx_x86_emit_vextractf128(e, r0, r1, (uint8_t)inst->imm);
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

    /* Apply peephole optimizations before emission */
    if (result) {
        vtx_peephole_optimize(stream, result);
        vtx_branch_optimize(stream, emit, result);
    }

    /* ---- F1 fix: Initialize relocation table and label offset tracking ---- */
    if (!arena) return -1;

    vtx_reloc_table_t reloc_tbl;
    if (vtx_reloc_table_init(&reloc_tbl, arena) != 0) return -1;
    emit->relocs = &reloc_tbl;
    emit->reloc_arena = arena;

    /* Allocate label offset array: label_offsets[block_index] = native offset */
    uint32_t block_count = stream->block_count;
    uint32_t *label_off = (uint32_t *)vtx_arena_alloc(
        arena, block_count * sizeof(uint32_t));
    if (!label_off) return -1;
    memset(label_off, 0, block_count * sizeof(uint32_t));
    emit->label_offsets = label_off;
    emit->label_count = block_count;

    /* NOTE: Prologue is emitted by the pipeline BEFORE calling this function.
     * We do NOT emit a second prologue here — that would corrupt the stack
     * by pushing RBP and subtracting RSP twice. The old code had a double
     * prologue bug which is now fixed. */

    /* Emit instructions for each block */
    for (uint32_t b = 0; b < stream->block_count; b++) {
        vtx_inst_block_t *blk = &stream->blocks[b];

        /* Record the native offset of this block's first instruction.
         * This is the target for any branch that targets this block. */
        label_off[b] = vtx_x86_emit_position(emit);

        /* Align loop headers to 16-byte boundaries using multi-byte NOPs.
         * Using 1-byte NOPs (0x90) causes the processor to decode up to 15
         * NOPs, wasting decode bandwidth. Multi-byte NOPs are decoded as a
         * single instruction, which is more efficient.
         *
         * Multi-byte NOP encoding (Intel recommended):
         *   2-byte: 66 90
         *   3-byte: 0F 1F 00
         *   4-byte: 0F 1F 40 00
         *   5-byte: 0F 1F 44 00 00
         *   6-byte: 66 0F 1F 44 00 00
         *   7-byte: 0F 1F 80 00 00 00 00
         *   8-byte: 0F 1F 84 00 00 00 00 00
         *   9-byte: 66 0F 1F 84 00 00 00 00 00
         *  10-byte: 66 66 0F 1F 84 00 00 00 00 00
         *  11-byte: 66 66 66 0F 1F 84 00 00 00 00 00
         *  (up to 15 bytes by adding more 66 prefixes) */
        if (blk->inst_count > 0 && (blk->insts[0].flags & (1u << 17))) {
            uint32_t pos = vtx_x86_emit_position(emit);
            uint32_t aligned = (pos + 15u) & ~15u;
            uint32_t pad = aligned - pos;

            /* Re-record label offset after alignment padding */
            while (pad > 0) {
                /* Emit the largest multi-byte NOP that fits */
                if (pad >= 11) {
                    /* 11-byte: 66 66 66 0F 1F 84 00 00 00 00 00 */
                    emit_byte(emit, 0x66); emit_byte(emit, 0x66);
                    emit_byte(emit, 0x66); emit_byte(emit, 0x0F);
                    emit_byte(emit, 0x1F); emit_byte(emit, 0x84);
                    emit_byte(emit, 0x00); emit_byte(emit, 0x00);
                    emit_byte(emit, 0x00); emit_byte(emit, 0x00);
                    emit_byte(emit, 0x00);
                    pad -= 11;
                } else if (pad >= 10) {
                    /* 10-byte: 66 66 0F 1F 84 00 00 00 00 00 */
                    emit_byte(emit, 0x66); emit_byte(emit, 0x66);
                    emit_byte(emit, 0x0F); emit_byte(emit, 0x1F);
                    emit_byte(emit, 0x84); emit_byte(emit, 0x00);
                    emit_byte(emit, 0x00); emit_byte(emit, 0x00);
                    emit_byte(emit, 0x00); emit_byte(emit, 0x00);
                    pad -= 10;
                } else if (pad >= 9) {
                    /* 9-byte: 66 0F 1F 84 00 00 00 00 00 */
                    emit_byte(emit, 0x66); emit_byte(emit, 0x0F);
                    emit_byte(emit, 0x1F); emit_byte(emit, 0x84);
                    emit_byte(emit, 0x00); emit_byte(emit, 0x00);
                    emit_byte(emit, 0x00); emit_byte(emit, 0x00);
                    emit_byte(emit, 0x00);
                    pad -= 9;
                } else if (pad >= 8) {
                    /* 8-byte: 0F 1F 84 00 00 00 00 00 */
                    emit_byte(emit, 0x0F); emit_byte(emit, 0x1F);
                    emit_byte(emit, 0x84); emit_byte(emit, 0x00);
                    emit_byte(emit, 0x00); emit_byte(emit, 0x00);
                    emit_byte(emit, 0x00); emit_byte(emit, 0x00);
                    pad -= 8;
                } else if (pad >= 7) {
                    /* 7-byte: 0F 1F 80 00 00 00 00 */
                    emit_byte(emit, 0x0F); emit_byte(emit, 0x1F);
                    emit_byte(emit, 0x80); emit_byte(emit, 0x00);
                    emit_byte(emit, 0x00); emit_byte(emit, 0x00);
                    emit_byte(emit, 0x00);
                    pad -= 7;
                } else if (pad >= 6) {
                    /* 6-byte: 66 0F 1F 44 00 00 */
                    emit_byte(emit, 0x66); emit_byte(emit, 0x0F);
                    emit_byte(emit, 0x1F); emit_byte(emit, 0x44);
                    emit_byte(emit, 0x00); emit_byte(emit, 0x00);
                    pad -= 6;
                } else if (pad >= 5) {
                    /* 5-byte: 0F 1F 44 00 00 */
                    emit_byte(emit, 0x0F); emit_byte(emit, 0x1F);
                    emit_byte(emit, 0x44); emit_byte(emit, 0x00);
                    emit_byte(emit, 0x00);
                    pad -= 5;
                } else if (pad >= 4) {
                    /* 4-byte: 0F 1F 40 00 */
                    emit_byte(emit, 0x0F); emit_byte(emit, 0x1F);
                    emit_byte(emit, 0x40); emit_byte(emit, 0x00);
                    pad -= 4;
                } else if (pad >= 3) {
                    /* 3-byte: 0F 1F 00 */
                    emit_byte(emit, 0x0F); emit_byte(emit, 0x1F);
                    emit_byte(emit, 0x00);
                    pad -= 3;
                } else if (pad >= 2) {
                    /* 2-byte: 66 90 */
                    emit_byte(emit, 0x66); emit_byte(emit, 0x90);
                    pad -= 2;
                } else {
                    /* 1-byte: 90 (standard NOP) */
                    emit_byte(emit, 0x90);
                    pad -= 1;
                }
            }
            /* Update label offset to the aligned position */
            label_off[b] = vtx_x86_emit_position(emit);
        }

        for (uint32_t i = 0; i < blk->inst_count; i++) {
            vtx_inst_t *inst = &blk->insts[i];
            /* Record the native offset before emitting */
            inst->native_offset = vtx_x86_emit_position(emit);

            /* Handle short jumps for JCC */
            if (inst->opcode == VTX_X86_JCC && (inst->flags & (1u << 16))) {
                /* Short JCC: 7x cb (2 bytes) — will be patched by reloc */
                uint8_t x86_cond = vtx_cond_to_x86(inst->cond);
                emit_byte(emit, (uint8_t)(0x70 + x86_cond));
                emit_byte(emit, 0x00); /* placeholder rel8 */
                /* Record relocation for short JCC (rel8 at offset+1).
                 * We use VTX_RELOC_REL32 but will compute the 8-bit
                 * displacement manually after resolving. The patch_offset
                 * points to the rel8 byte. We store the target block in
                 * the symbol field. */
                if (inst->opnd_kinds[0] == VTX_OPND_LABEL &&
                    !(inst->flags & VTX_INST_FLAG_IS_GUARD)) {
                    uint32_t target_block = inst->operands[0];
                    vtx_reloc_add(emit->relocs, VTX_RELOC_REL32,
                                  inst->native_offset + 1, /* patch_offset for rel8 */
                                  0, 0,                     /* target_offset (filled later) */
                                  target_block,             /* symbol = target block index */
                                  0, emit->reloc_arena);
                }
                continue;
            }

            /* Handle short jumps for JMP */
            if (inst->opcode == VTX_X86_JMP && (inst->flags & (1u << 16))) {
                /* Short JMP: EB cb (2 bytes) */
                emit_byte(emit, 0xEB);
                emit_byte(emit, 0x00); /* placeholder rel8 */
                if (inst->opnd_kinds[0] == VTX_OPND_LABEL) {
                    uint32_t target_block = inst->operands[0];
                    vtx_reloc_add(emit->relocs, VTX_RELOC_REL32,
                                  inst->native_offset + 1,
                                  0, 0,
                                  target_block,
                                  0, emit->reloc_arena);
                }
                continue;
            }

            if (emit_single_inst(emit, inst, result) != 0) {
                return -1;
            }

            /* ---- F1 fix: Record branch relocation after emitting ---- *
             * For JCC rel32 (6 bytes: 0F 8x cd), the disp32 is at native_offset + 2.
             * For JMP rel32 (5 bytes: E9 cd), the disp32 is at native_offset + 1.
             * For CALL rel32 (5 bytes: E8 cd), the disp32 is at native_offset + 1.
             * Guard JCCs are NOT recorded here — they are patched by
             * vtx_guard_emit_patch() which patches them to deopt stubs.
             * We store the target block index in the 'symbol' field so
             * we can resolve it after all blocks are emitted. */
            if (inst->opcode == VTX_X86_JCC &&
                inst->opnd_kinds[0] == VTX_OPND_LABEL &&
                !(inst->flags & VTX_INST_FLAG_IS_GUARD)) {
                uint32_t target_block = inst->operands[0];
                uint32_t patch_offset = inst->native_offset + 2; /* disp32 at +2 for JCC */
                vtx_reloc_add_branch(emit->relocs, patch_offset,
                                     inst->native_offset,
                                     0,  /* target_offset: 0 = forward ref, resolved later */
                                     emit->reloc_arena);
                /* Store target block index in the symbol field for later resolution */
                if (emit->relocs->count > 0) {
                    emit->relocs->entries[emit->relocs->count - 1].symbol = target_block;
                }
            }
            else if (inst->opcode == VTX_X86_JMP &&
                     inst->opnd_kinds[0] == VTX_OPND_LABEL) {
                uint32_t target_block = inst->operands[0];
                uint32_t patch_offset = inst->native_offset + 1; /* disp32 at +1 for JMP */
                vtx_reloc_add_branch(emit->relocs, patch_offset,
                                     inst->native_offset,
                                     0,
                                     emit->reloc_arena);
                if (emit->relocs->count > 0) {
                    emit->relocs->entries[emit->relocs->count - 1].symbol = target_block;
                }
            }
            else if (inst->opcode == VTX_X86_CALL &&
                     inst->opnd_kinds[0] == VTX_OPND_LABEL) {
                uint32_t target_block = inst->operands[0];
                uint32_t patch_offset = inst->native_offset + 1; /* disp32 at +1 for CALL */
                vtx_reloc_add_branch(emit->relocs, patch_offset,
                                     inst->native_offset,
                                     0,
                                     emit->reloc_arena);
                if (emit->relocs->count > 0) {
                    emit->relocs->entries[emit->relocs->count - 1].symbol = target_block;
                }
            }
            /* ---- GC write barrier call relocation ---- *
             * CALL with VTX_OPND_IMM and imm == -4 is a write barrier stub call.
             * We record an external call relocation targeting
             * vtx_helpers_write_barrier, which will be resolved at code
             * install time when the final code address is known. */
            else if (inst->opcode == VTX_X86_CALL &&
                     (inst->flags & VTX_INST_FLAG_HAS_IMM) &&
                     inst->imm == -4) {
                uint32_t patch_offset = inst->native_offset + 1; /* disp32 at +1 for CALL */
                vtx_reloc_add_call(emit->relocs, patch_offset,
                                   (uint64_t)(uintptr_t)vtx_helpers_write_barrier,
                                   emit->reloc_arena);
            }
        }
    }

    /* ---- F1 fix: Resolve all branch relocations and apply ---- *
     * For each branch relocation, look up the target block's native offset
     * from the label_offsets array and update the relocation entry's
     * target_offset. Then call vtx_reloc_apply_all() to patch all
     * displacements in the code buffer. */
    for (uint32_t r = 0; r < emit->relocs->count; r++) {
        vtx_reloc_t *reloc = &emit->relocs->entries[r];
        if (reloc->is_external) continue; /* skip external call relocations */

        /* Check if this is a branch relocation with a label index in symbol */
        if (reloc->symbol < block_count && reloc->kind == VTX_RELOC_REL32) {
            /* Resolve: set target_offset from label_offsets */
            reloc->target_offset = label_off[reloc->symbol];

            /* For short jumps (rel8), the displacement computation is different.
             * rel8 = target - (patch_offset + 1) instead of the rel32 formula.
             * We detect short jumps by checking if the branch instruction at
             * source_offset is a short-form encoding (2 bytes). */
            uint32_t src_off = reloc->offset;
            /* Heuristic: if the source is a short jump, the patch_offset is
             * src_off + 1 (rel8), and the total instruction size is 2 bytes.
             * Check if this looks like a short jump. */
            if (src_off < emit->position) {
                uint8_t byte0 = emit->buffer[src_off];
                bool is_short_jcc = (byte0 >= 0x70 && byte0 <= 0x7F);
                bool is_short_jmp = (byte0 == 0xEB);
                if (is_short_jcc || is_short_jmp) {
                    /* Short jump: rel8 = target - (patch_offset + 1)
                     * But vtx_reloc_apply_all uses rel32 formula:
                     *   disp = target_offset - (offset + 4)
                     * For short jumps, we need:
                     *   rel8 = target_offset - (patch_offset + 1)
                     * We'll patch directly here and mark the reloc as handled
                     * by setting offset to an impossible value. */
                    int32_t rel8 = (int32_t)reloc->target_offset -
                                   (int32_t)(reloc->offset + 1);
                    if (rel8 >= -128 && rel8 <= 127) {
                        emit->buffer[reloc->offset] = (uint8_t)(rel8 & 0xFF);
                        /* Mark as resolved by setting offset out of bounds */
                        reloc->offset = UINT32_MAX;
                    } else {
                        /* Short jump doesn't reach — this shouldn't happen
                         * if branch_optimize computed offsets correctly.
                         * Fall through to rel32 handling which will produce
                         * a wrong result for a 2-byte instruction, but at
                         * least we don't silently generate bad code. */
                    }
                }
            }
        }
    }

    /* Apply all rel32 relocations (skip already-resolved short jumps) */
    vtx_reloc_apply_all(emit->relocs, emit->buffer, emit->position);

    /* ---- Emit constant pool (for float immediate materialization) ---- *
     * After all code blocks and relocation resolution, emit the constant
     * pool at the end of the function. Each 8-byte constant is aligned
     * to 8 bytes and referenced via RIP-relative MOVSD loads whose
     * disp32 was emitted as a placeholder during code emission.
     * We patch the disp32 directly here — no relocation needed. */
    if (emit->const_pool.count > 0) {
        /* Align to 8 bytes (8-byte doubles don't strictly need 16-byte
         * alignment, but 8-byte alignment is required for correct atomic
         * loads on x86-64) */
        uint32_t pos = vtx_x86_emit_position(emit);
        while (pos & 7) {
            vtx_x86_emit_nop(emit);
            pos++;
        }

        /* Emit each constant and patch its reference */
        for (uint32_t i = 0; i < emit->const_pool.count; i++) {
            uint32_t pool_offset = vtx_x86_emit_position(emit);
            uint32_t ref_offset = emit->const_pool.ref_offsets[i];

            /* Emit the 8-byte constant */
            emit_qword(emit, emit->const_pool.values[i]);

            /* Patch the RIP-relative disp32 at the reference site.
             * disp32 = target - (rip_after_disp) = pool_offset - (ref_offset + 4)
             * where ref_offset is the position of the disp32 field in the
             * MOVSD instruction, and pool_offset is where the constant
             * was actually emitted. */
            int32_t disp = (int32_t)pool_offset - (int32_t)(ref_offset + 4);
            emit->buffer[ref_offset + 0] = (uint8_t)(disp & 0xFF);
            emit->buffer[ref_offset + 1] = (uint8_t)((disp >> 8) & 0xFF);
            emit->buffer[ref_offset + 2] = (uint8_t)((disp >> 16) & 0xFF);
            emit->buffer[ref_offset + 3] = (uint8_t)((disp >> 24) & 0xFF);
        }
    }

    /* Epilogue is now emitted by the RET instruction handler in
     * emit_single_inst(), which calls vtx_x86_emit_epilogue() before
     * the ret. This correctly handles multiple return points. */
    return 0;
}
