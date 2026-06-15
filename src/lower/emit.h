#ifndef VORTEX_LOWER_EMIT_H
#define VORTEX_LOWER_EMIT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "lower/isel.h"
#include "lower/regalloc.h"
#include "lower/reloc.h"
#include "runtime/arena.h"

/**
 * VORTEX x86-64 Machine Code Emitter
 *
 * Emits x86-64 machine code bytes into a code buffer. All encodings
 * follow the Intel 64 and IA-32 Architectures Software Developer's
 * Manual (SDM). Each instruction is encoded as:
 *   [Legacy Prefixes] [REX] [Opcode] [ModR/M] [SIB] [Displacement] [Immediate]
 *
 * Register encoding (hardware order):
 *   RAX=0, RCX=1, RDX=2, RBX=3, RSP=4, RBP=5, RSI=6, RDI=7
 *   R8=8, R9=9, R10=10, R11=11, R12=12, R13=13, R14=14, R15=15
 *
 * For R8-R15, the low 3 bits go in ModR/M/SIB fields and the high bit
 * goes in the REX prefix (REX.R, REX.X, REX.B).
 */

/* ========================================================================== */
/* x86-64 emitter context                                                      */
/* ========================================================================== */

#define VTX_EMIT_INITIAL_CAPACITY 4096

typedef struct {
    uint8_t  *buffer;      /* code buffer (dynamically allocated) */
    uint32_t  position;    /* current write position */
    uint32_t  capacity;    /* allocated capacity in bytes */

    /* Relocation table for branch patching (F1 fix).
     * Initialized by vtx_x86_emit_function(), used to record branch
     * relocations that are resolved and applied after all emission. */
    vtx_reloc_table_t *relocs;
    vtx_arena_t       *reloc_arena;

    /* Label offset tracking: maps block index → native code offset.
     * Used to resolve branch targets after all blocks are emitted. */
    uint32_t *label_offsets;
    uint32_t  label_count;

    /* Constant pool for float immediate materialization.
     * Constants are emitted after all code blocks, referenced via
     * RIP-relative MOVSD loads. This is faster than the
     * MOV r64,imm64 + MOVQ xmm,r64 sequence (10+5=15 bytes vs 8 bytes)
     * and avoids a GPR↔XMM round-trip through the rename pipeline. */
    struct {
        uint64_t *values;       /* 64-bit constant values */
        uint32_t *ref_offsets;  /* native code offset of each disp32 reference */
        uint32_t  count;        /* number of constants in the pool */
        uint32_t  capacity;     /* allocated capacity */
    } const_pool;
} vtx_x86_emit_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the emitter with an initial buffer capacity.
 * Returns 0 on success, -1 on failure.
 */
int vtx_x86_emit_init(vtx_x86_emit_t *emit, uint32_t initial_capacity);

/**
 * Destroy the emitter and free the code buffer.
 */
void vtx_x86_emit_destroy(vtx_x86_emit_t *emit);

/**
 * Ensure the buffer has at least `needed` bytes of free space.
 * Returns 0 on success, -1 on failure.
 */
int vtx_x86_emit_ensure(vtx_x86_emit_t *emit, uint32_t needed);

/**
 * Get the current position (number of bytes emitted).
 */
static inline uint32_t vtx_x86_emit_position(const vtx_x86_emit_t *emit)
{
    return emit->position;
}

/**
 * Get a pointer to the emitted code.
 */
static inline const uint8_t *vtx_x86_emit_code(const vtx_x86_emit_t *emit)
{
    return emit->buffer;
}

/* ========================================================================== */
/* Low-level emit helpers                                                      */
/* ========================================================================== */

static inline void emit_byte(vtx_x86_emit_t *e, uint8_t b)
{
    VTX_ASSERT(e->position < e->capacity, "code buffer overflow");
    e->buffer[e->position++] = b;
}

static inline void emit_word(vtx_x86_emit_t *e, uint16_t w)
{
    emit_byte(e, (uint8_t)(w & 0xFF));
    emit_byte(e, (uint8_t)((w >> 8) & 0xFF));
}

static inline void emit_dword(vtx_x86_emit_t *e, uint32_t d)
{
    emit_byte(e, (uint8_t)(d & 0xFF));
    emit_byte(e, (uint8_t)((d >> 8) & 0xFF));
    emit_byte(e, (uint8_t)((d >> 16) & 0xFF));
    emit_byte(e, (uint8_t)((d >> 24) & 0xFF));
}

static inline void emit_qword(vtx_x86_emit_t *e, uint64_t q)
{
    emit_dword(e, (uint32_t)(q & 0xFFFFFFFF));
    emit_dword(e, (uint32_t)((q >> 32) & 0xFFFFFFFF));
}

/**
 * Emit REX prefix if needed.
 * @param w  REX.W (1 for 64-bit operand)
 * @param r  REX.R (extends ModR/M reg field)
 * @param x  REX.X (extends SIB index field)
 * @param b  REX.B (extends ModR/M r/m or SIB base)
 */
static inline void emit_rex(vtx_x86_emit_t *e, int w, int r, int x, int b)
{
    uint8_t rex = 0x40;
    if (w) rex |= 0x08;
    if (r) rex |= 0x04;
    if (x) rex |= 0x02;
    if (b) rex |= 0x01;
    if (rex != 0x40 || w) { /* Always emit if W=1, or if R/X/B are set */
        emit_byte(e, rex);
    }
}

/**
 * Emit ModR/M byte.
 * @param mod  Mode (0=mem, 1=mem+disp8, 2=mem+disp32, 3=reg)
 * @param reg  Register or opcode extension (0-7)
 * @param rm   Register/memory operand (0-7)
 */
static inline void emit_modrm(vtx_x86_emit_t *e, uint8_t mod, uint8_t reg, uint8_t rm)
{
    emit_byte(e, (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
}

/**
 * Emit SIB byte.
 * @param scale  Scale (0=1, 1=2, 2=4, 3=8)
 * @param index  Index register (0-7, 4=no index)
 * @param base   Base register (0-7, 5=disp32 only with mod=0)
 */
static inline void emit_sib(vtx_x86_emit_t *e, uint8_t scale, uint8_t index, uint8_t base)
{
    emit_byte(e, (uint8_t)((scale << 6) | ((index & 7) << 3) | (base & 7)));
}

/* ========================================================================== */
/* Register helpers                                                            */
/* ========================================================================== */

static inline int reg_hi(uint8_t reg) { return (reg >> 3) & 1; }
static inline int reg_lo(uint8_t reg) { return reg & 7; }

/* ========================================================================== */
/* High-level instruction emission                                             */
/* ========================================================================== */

/**
 * Emit a REX.W + opcode + ModR/M for register-register instructions.
 * For example: add r64, r64 → REX.W 01 ModR/M
 * @param e        Emitter
 * @param opcode   Primary opcode byte
 * @param opcode2  Secondary opcode byte (0 = none, e.g. 0F prefix)
 * @param reg      Destination or source register (goes in reg field)
 * @param rm       Source or destination register (goes in r/m field)
 * @param direction 0=reg is r/m, rm is reg (opcode /r); 1=reg is reg, rm is r/m
 */
void vtx_x86_emit_rr(vtx_x86_emit_t *e, uint8_t opcode, uint8_t opcode2,
                      uint8_t reg, uint8_t rm);

/**
 * Emit a REX.W + opcode + ModR/M for register-immediate instructions.
 * The immediate is sign-extended from 32 bits to 64 bits.
 * @param e        Emitter
 * @param opcode   Primary opcode byte (e.g. 81 for imm32, 83 for imm8)
 * @param reg_ext  Opcode extension in ModR/M reg field (e.g. 0=ADD, 5=SUB)
 * @param rm       Target register
 * @param imm      Immediate value
 * @param imm_size 4 for imm32, 1 for imm8
 */
void vtx_x86_emit_ri(vtx_x86_emit_t *e, uint8_t opcode, uint8_t reg_ext,
                      uint8_t rm, int64_t imm, int imm_size);

/**
 * Emit a REX.W + opcode + ModR/M for register-memory instructions.
 * Memory is [base + disp].
 * @param e         Emitter
 * @param opcode    Primary opcode byte
 * @param opcode2   Secondary opcode byte (0 = none)
 * @param reg       Register operand
 * @param base      Base register
 * @param disp      Displacement
 * @param is_load   true=load (reg ← mem), false=store (mem ← reg)
 */
void vtx_x86_emit_rm(vtx_x86_emit_t *e, uint8_t opcode, uint8_t opcode2,
                      uint8_t reg, uint8_t base, int32_t disp, bool is_load);

/**
 * Emit a REX.W + opcode + ModR/M + SIB for [base + index*scale + disp].
 * @param e         Emitter
 * @param opcode    Primary opcode byte
 * @param opcode2   Secondary opcode byte (0 = none)
 * @param reg       Register operand
 * @param base      Base register
 * @param index     Index register
 * @param scale     Scale (1, 2, 4, 8)
 * @param disp      Displacement
 * @param is_load   true=load (reg ← mem), false=store (mem ← reg)
 */
void vtx_x86_emit_sib_mem(vtx_x86_emit_t *e, uint8_t opcode, uint8_t opcode2,
                           uint8_t reg, uint8_t base, uint8_t index,
                           uint8_t scale, int32_t disp, bool is_load);

/**
 * Emit mov r64, imm64 (movabs). Uses the B8+rd encoding with REX.W.
 */
void vtx_x86_emit_mov_imm64(vtx_x86_emit_t *e, uint8_t reg, uint64_t imm);

/**
 * Emit mov r/m64, imm32 (sign-extended to 64 bits).
 * Uses C7 /0 encoding with REX.W.
 */
void vtx_x86_emit_mov_imm32(vtx_x86_emit_t *e, uint8_t reg, int32_t imm);

/* ========================================================================== */
/* Specific instruction emission functions                                     */
/* ========================================================================== */

void vtx_x86_emit_add_rr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_add_ri(vtx_x86_emit_t *e, uint8_t dst, int32_t imm);
void vtx_x86_emit_sub_rr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_sub_ri(vtx_x86_emit_t *e, uint8_t dst, int32_t imm);
void vtx_x86_emit_imul_rr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_idiv_r(vtx_x86_emit_t *e, uint8_t src);
void vtx_x86_emit_shl_ri(vtx_x86_emit_t *e, uint8_t dst, uint8_t count);
void vtx_x86_emit_shl_cl(vtx_x86_emit_t *e, uint8_t dst);
void vtx_x86_emit_shr_ri(vtx_x86_emit_t *e, uint8_t dst, uint8_t count);
void vtx_x86_emit_shr_cl(vtx_x86_emit_t *e, uint8_t dst);
void vtx_x86_emit_sar_ri(vtx_x86_emit_t *e, uint8_t dst, uint8_t count);
void vtx_x86_emit_sar_cl(vtx_x86_emit_t *e, uint8_t dst);
void vtx_x86_emit_and_rr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_and_ri(vtx_x86_emit_t *e, uint8_t dst, int32_t imm);
void vtx_x86_emit_or_rr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_or_ri(vtx_x86_emit_t *e, uint8_t dst, int32_t imm);
void vtx_x86_emit_xor_rr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_xor_ri(vtx_x86_emit_t *e, uint8_t dst, int32_t imm);
void vtx_x86_emit_cmp_rr(vtx_x86_emit_t *e, uint8_t a, uint8_t b);
void vtx_x86_emit_cmp_ri(vtx_x86_emit_t *e, uint8_t reg, int32_t imm);
void vtx_x86_emit_test_rr(vtx_x86_emit_t *e, uint8_t a, uint8_t b);
void vtx_x86_emit_test_ri(vtx_x86_emit_t *e, uint8_t reg, int32_t imm);
void vtx_x86_emit_mov_rr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_mov_rmem(vtx_x86_emit_t *e, uint8_t dst, uint8_t base, int32_t disp);
void vtx_x86_emit_mov_memr(vtx_x86_emit_t *e, uint8_t base, int32_t disp, uint8_t src);
void vtx_x86_emit_lea_rmem(vtx_x86_emit_t *e, uint8_t dst, uint8_t base, int32_t disp);
void vtx_x86_emit_neg_r(vtx_x86_emit_t *e, uint8_t reg);
void vtx_x86_emit_not_r(vtx_x86_emit_t *e, uint8_t reg);
void vtx_x86_emit_cqo(vtx_x86_emit_t *e);
void vtx_x86_emit_setcc(vtx_x86_emit_t *e, uint8_t cond, uint8_t reg);
void vtx_x86_emit_cmovcc(vtx_x86_emit_t *e, uint8_t cond, uint8_t dst, uint8_t src);
void vtx_x86_emit_push_r(vtx_x86_emit_t *e, uint8_t reg);
void vtx_x86_emit_pop_r(vtx_x86_emit_t *e, uint8_t reg);
void vtx_x86_emit_jmp_rel32(vtx_x86_emit_t *e, int32_t offset);
void vtx_x86_emit_jcc_rel32(vtx_x86_emit_t *e, uint8_t cond, int32_t offset);
void vtx_x86_emit_call_rel32(vtx_x86_emit_t *e, int32_t offset);
void vtx_x86_emit_ret(vtx_x86_emit_t *e);
void vtx_x86_emit_nop(vtx_x86_emit_t *e);

/**
 * Emit UCOMISD xmm, xmm — unordered compare scalar double.
 * Sets EFLAGS for floating-point comparison.
 */
void vtx_x86_emit_ucomisd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);

/**
 * Emit ADDSD xmm, xmm — scalar double-precision add.
 * Encoding: F2 0F 58 /r
 */
void vtx_x86_emit_addsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);

/**
 * Emit SUBSD xmm, xmm — scalar double-precision subtract.
 * Encoding: F2 0F 5C /r
 */
void vtx_x86_emit_subsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);

/**
 * Emit MULSD xmm, xmm — scalar double-precision multiply.
 * Encoding: F2 0F 59 /r
 */
void vtx_x86_emit_mulsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);

/**
 * Emit DIVSD xmm, xmm — scalar double-precision divide.
 * Encoding: F2 0F 5E /r
 */
void vtx_x86_emit_divsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);

/**
 * Emit XORPS xmm, xmm — bitwise XOR of 128-bit operands.
 * Used for float negation by XORing with a sign-bit mask.
 * Encoding: 0F 57 /r
 */
void vtx_x86_emit_xorps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);

/**
 * Emit MOVSD xmm, xmm — move scalar double between XMM registers.
 * Encoding: F2 0F 10 /r (load) or F2 0F 11 /r (store).
 * For reg-reg move: F2 0F 10 ModR/M(mod=11)
 */
void vtx_x86_emit_movsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);

/* ---- Scalar single-precision float (SSE) ---- */
void vtx_x86_emit_addss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_subss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_mulss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_divss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_sqrtss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_ucomiss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_movss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);

/* ---- Float ↔ Int conversions (SSE2) ---- */
void vtx_x86_emit_cvtsi2sd(vtx_x86_emit_t *e, uint8_t dst_xmm, uint8_t src_gpr);
void vtx_x86_emit_cvtsd2si(vtx_x86_emit_t *e, uint8_t dst_gpr, uint8_t src_xmm);
void vtx_x86_emit_cvttsd2si(vtx_x86_emit_t *e, uint8_t dst_gpr, uint8_t src_xmm);
void vtx_x86_emit_cvtsi2ss(vtx_x86_emit_t *e, uint8_t dst_xmm, uint8_t src_gpr);
void vtx_x86_emit_cvtss2si(vtx_x86_emit_t *e, uint8_t dst_gpr, uint8_t src_xmm);
void vtx_x86_emit_cvttss2si(vtx_x86_emit_t *e, uint8_t dst_gpr, uint8_t src_xmm);

/* ---- GPR ↔ XMM bridge (SSE2) ---- */
void vtx_x86_emit_movq_xmm_r64(vtx_x86_emit_t *e, uint8_t dst_xmm, uint8_t src_gpr);
void vtx_x86_emit_movq_r64_xmm(vtx_x86_emit_t *e, uint8_t dst_gpr, uint8_t src_xmm);

/* ---- COMISD / SQRTSD / MOVSD memory ---- */
void vtx_x86_emit_comisd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_sqrtsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_movsd_load(vtx_x86_emit_t *e, uint8_t dst_xmm, uint8_t base, int32_t disp);
void vtx_x86_emit_movsd_store(vtx_x86_emit_t *e, uint8_t base, int32_t disp, uint8_t src_xmm);

/* ---- Bit manipulation ---- */
void vtx_x86_emit_bswap(vtx_x86_emit_t *e, uint8_t reg);
void vtx_x86_emit_bsf(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_bsr(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_popcnt(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_rol_ri(vtx_x86_emit_t *e, uint8_t dst, uint8_t count);
void vtx_x86_emit_rol_cl(vtx_x86_emit_t *e, uint8_t dst);
void vtx_x86_emit_ror_ri(vtx_x86_emit_t *e, uint8_t dst, uint8_t count);
void vtx_x86_emit_ror_cl(vtx_x86_emit_t *e, uint8_t dst);

/* ---- CDQE / Unsigned DIV ---- */
void vtx_x86_emit_cdqe(vtx_x86_emit_t *e);
void vtx_x86_emit_div_r(vtx_x86_emit_t *e, uint8_t src);

/* ---- Timing / Profiling ---- */
void vtx_x86_emit_rdtsc(vtx_x86_emit_t *e);
void vtx_x86_emit_rdtscp(vtx_x86_emit_t *e);

/* ---- Atomics ---- */
void vtx_x86_emit_cmpxchg(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_xadd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);

/* ---- Memory fences ---- */
void vtx_x86_emit_lfence(vtx_x86_emit_t *e);
void vtx_x86_emit_mfence(vtx_x86_emit_t *e);
void vtx_x86_emit_sfence(vtx_x86_emit_t *e);

/* ---- SSE4.1 rounding ---- */
void vtx_x86_emit_roundsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src, uint8_t mode);
void vtx_x86_emit_roundss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src, uint8_t mode);

/* ---- Constant pool load (RIP-relative MOVSD from literal pool) ---- */
void vtx_x86_emit_movsd_rip(vtx_x86_emit_t *e, uint8_t dst_xmm, uint64_t double_bits);

/* ---- AVX2 VEX-encoded 256-bit packed double ---- */
void vtx_x86_emit_vmovapd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_vaddpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vsubpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vmulpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vdivpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vminpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vmaxpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vxorpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vandpd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vcmppd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2, uint8_t pred);

/* ---- AVX2 256-bit packed single-precision float ---- */
void vtx_x86_emit_vmovaps_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_vaddps_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vsubps_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vmulps_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vdivps_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);

/* ---- AVX2 256-bit packed integer ---- */
void vtx_x86_emit_vmovdqa_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_vpaddd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vpsubd_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vpmulld_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vpxor_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vpand_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);
void vtx_x86_emit_vpor_256(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2);

/* ---- AVX2 utility / transition ---- */
void vtx_x86_emit_vzeroupper(vtx_x86_emit_t *e);
void vtx_x86_emit_vzeroall(vtx_x86_emit_t *e);

/* ---- AVX2 lane manipulation / broadcast ---- */
void vtx_x86_emit_vbroadcastsd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_vbroadcastss(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_vperm2f128(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2, uint8_t ctrl);
void vtx_x86_emit_vinsertf128(vtx_x86_emit_t *e, uint8_t dst, uint8_t src1, uint8_t src2, uint8_t imm8);
void vtx_x86_emit_vextractf128(vtx_x86_emit_t *e, uint8_t dst, uint8_t src, uint8_t imm8);

/* ---- Packed double SIMD ---- */
void vtx_x86_emit_subpd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_divpd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);

/* ---- Packed integer SIMD (SSE2) ---- */
void vtx_x86_emit_movdqa(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_movdqu(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_paddd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_psubd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_pmulld(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_pxor(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_pand(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_por(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_pcmpeqd(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);

/* ---- Packed single-precision float SIMD (SSE) ---- */
void vtx_x86_emit_movaps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_addps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_mulps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_subps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_divps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_minps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_maxps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src);
void vtx_x86_emit_cmpps(vtx_x86_emit_t *e, uint8_t dst, uint8_t src, uint8_t pred);

/**
 * Emit safepoint poll at a loop back-edge.
 * Emits:
 *   cmpq [vtx_safepoint_flag], 0   (RIP-relative, 8 bytes)
 *   jne  deopt_stub                (rel32, 6 bytes)
 *
 * The CMP uses RIP-relative addressing to access the global
 * vtx_safepoint_flag. A VTX_RELOC_RIP_REL32 external relocation is
 * recorded so the displacement is patched at code install time.
 *
 * The JNE has its displacement set to 0 (placeholder). It is marked
 * with VTX_INST_FLAG_IS_GUARD in the instruction stream and will be
 * patched by the guard emission pipeline to jump to a deopt stub.
 *
 * @param e  Emitter context (must have relocs initialized)
 * @return   0 on success, -1 on failure
 */
int vtx_x86_emit_safepoint_poll(vtx_x86_emit_t *e);

/**
 * Emit a zero-cost guard page safepoint poll at a loop back-edge.
 * Replaces the CMP+JCC approach with a single MOV load.
 *
 * When the guard page is readable (normal operation), the MOV completes
 * silently — zero overhead beyond a single load instruction. When a
 * safepoint is requested, the page is mprotected to PROT_NONE, causing
 * the MOV to SIGSEGV. The signal handler catches the fault, processes
 * the safepoint, re-arms the page, and returns.
 *
 * Emitted code:
 *   movq rax, [rip + disp32]    ; 7 bytes (REX.W + 8B + ModR/M + disp32)
 *   ; loaded value is discarded — rax is a scratch register
 *
 * Total: 7 bytes (vs. 14 bytes for CMP+JCC). No compare, no branch,
 * no branch-prediction entry consumed.
 *
 * A VTX_RELOC_RIP_REL32 external relocation is recorded so the
 * displacement is patched at code install time to point to the
 * guard page. The special stub_id -6 is used to mark this as a
 * guard page relocation (distinct from -5 which is the flag-based
 * safepoint poll).
 *
 * @param e  Emitter context (must have relocs initialized)
 * @return   0 on success, -1 on failure
 */
int vtx_x86_emit_safepoint_poll_guard_page(vtx_x86_emit_t *e);

/* ========================================================================== */
/* Function prologue/epilogue                                                  */
/* ========================================================================== */

/**
 * Emit function prologue:
 *   push rbp
 *   mov rbp, rsp
 *   sub rsp, frame_size
 *   push callee-saved registers (for each bit set in callee_saved_mask)
 *
 * @param e                  Emitter
 * @param frame_size         Frame size in bytes (locals + spills, aligned)
 * @param callee_saved_mask  Bitmask of callee-saved registers to save
 */
void vtx_x86_emit_prologue(vtx_x86_emit_t *e, uint32_t frame_size,
                            uint32_t callee_saved_mask,
                            uint32_t arg_count, uint32_t max_locals);

/**
 * Emit function epilogue:
 *   pop callee-saved registers
 *   mov rsp, rbp
 *   pop rbp
 *   ret
 *
 * @param e                  Emitter
 * @param callee_saved_mask  Bitmask of callee-saved registers to restore
 */
void vtx_x86_emit_epilogue(vtx_x86_emit_t *e, uint32_t callee_saved_mask);

/**
 * Emit the entire instruction stream into the code buffer.
 * Uses the register allocation result to resolve physical registers.
 *
 * @param emit     Emitter context
 * @param stream   Instruction stream (after register allocation)
 * @param result   Register allocation result
 * @param arena    Arena for temporary allocations
 * @return         0 on success, -1 on failure
 */
int vtx_x86_emit_function(vtx_x86_emit_t *emit, vtx_inst_stream_t *stream,
                           const vtx_regalloc_result_t *result, vtx_arena_t *arena);

/**
 * Apply peephole optimizations to the instruction stream.
 * Must be called after register allocation (physical registers resolved).
 * Optimizations applied:
 *   - Eliminate redundant MOV reg, reg (src == dst)
 *   - CMP reg, 0 → TEST reg, reg (1 byte shorter)
 *   - Fold ADD/SUB with 0 into NOP
 *   - Eliminate dead code (write to unused register)
 *
 * @param stream   Instruction stream (modified in place)
 * @param result   Register allocation result
 * @return         Number of instructions eliminated
 */
uint32_t vtx_peephole_optimize(vtx_inst_stream_t *stream,
                                const vtx_regalloc_result_t *result);

/**
 * Optimize branch layout in the instruction stream.
 * Must be called after peephole optimization and before emission.
 * Optimizations applied:
 *   - Invert JCC + JMP to fall-through where possible
 *   - Use short jumps (2 bytes) when target is within ±127 bytes
 *   - Align loop headers to 16-byte boundaries for I-cache performance
 *
 * @param stream   Instruction stream (modified in place)
 * @param emit     Emitter context (for position tracking)
 * @param result   Register allocation result
 * @return         0 on success, -1 on failure
 */
int vtx_branch_optimize(vtx_inst_stream_t *stream, vtx_x86_emit_t *emit,
                         const vtx_regalloc_result_t *result);

/**
 * Map VTX condition code to x86-64 condition code byte.
 * Returns the x86 condition code (0-15) for JCC/SETCC/CMOVcc.
 */
uint8_t vtx_cond_to_x86(vtx_cond_t cond);

#endif /* VORTEX_LOWER_EMIT_H */
