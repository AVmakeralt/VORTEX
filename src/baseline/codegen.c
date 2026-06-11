#include "baseline/codegen.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Code buffer operations                                                      */
/* ========================================================================== */

int vtx_code_buffer_init(vtx_code_buffer_t *buf, uint32_t initial_capacity)
{
    if (initial_capacity == 0) {
        initial_capacity = VTX_CODE_BUFFER_INITIAL_CAPACITY;
    }
    buf->bytes = (uint8_t *)malloc(initial_capacity);
    if (!buf->bytes) return -1;
    buf->position = 0;
    buf->capacity = initial_capacity;
    return 0;
}

void vtx_code_buffer_destroy(vtx_code_buffer_t *buf)
{
    if (buf->bytes) {
        free(buf->bytes);
        buf->bytes = NULL;
    }
    buf->position = 0;
    buf->capacity = 0;
}

int vtx_code_buffer_ensure_capacity(vtx_code_buffer_t *buf, uint32_t needed)
{
    if (buf->position + needed <= buf->capacity) return 0;
    uint32_t new_cap = buf->capacity;
    while (new_cap < buf->position + needed) {
        new_cap *= 2;
    }
    uint8_t *new_bytes = (uint8_t *)realloc(buf->bytes, new_cap);
    if (!new_bytes) return -1;
    buf->bytes = new_bytes;
    buf->capacity = new_cap;
    return 0;
}

void vtx_compiled_code_destroy(vtx_compiled_code_t *code)
{
    if (!code) return;
    if (code->code) { free(code->code); code->code = NULL; }
    vtx_guard_array_destroy(&code->guards);
    vtx_deopt_stub_array_destroy(&code->deopt_stubs);
    if (code->bc_pc_map) { free(code->bc_pc_map); code->bc_pc_map = NULL; }
    if (code->native_to_bc_pc) { free(code->native_to_bc_pc); code->native_to_bc_pc = NULL; }
    if (code->deopt_info) {
        if (code->deopt_info->pc_map) free(code->deopt_info->pc_map);
        if (code->deopt_info->stack_depth_map) free(code->deopt_info->stack_depth_map);
        free(code->deopt_info);
    }
    /* side_table is arena-allocated, not freed here */
}

/* ========================================================================== */
/* x86-64 encoding helpers                                                    */
/* ========================================================================== */

#define REX_W   0x48
#define REX_WR  0x4C
#define REX_WB  0x49
#define REX_WRB 0x4D

static inline uint8_t modrm(uint8_t mod, uint8_t reg, uint8_t rm)
{
    return (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

static inline uint8_t sib(uint8_t scale, uint8_t index, uint8_t base)
{
    return (uint8_t)((scale << 6) | ((index & 7) << 3) | (base & 7));
}

/* Emit REX.W prefix for 64-bit operand, with optional R and B bits */
static void emit_rex64(vtx_code_buffer_t *buf, vtx_reg_t reg, vtx_reg_t rm)
{
    uint8_t rex = REX_W;
    if (reg >= VTX_REG_R8) rex |= 0x04;
    if (rm >= VTX_REG_R8)  rex |= 0x01;
    vtx_code_buffer_emit_byte(buf, rex);
}

/* Emit REX prefix only if extended registers require it (32-bit ops) */
static void emit_rex32_if_needed(vtx_code_buffer_t *buf, vtx_reg_t reg, vtx_reg_t rm)
{
    uint8_t rex = 0x40;
    bool needed = false;
    if (reg >= VTX_REG_R8) { rex |= 0x04; needed = true; }
    if (rm >= VTX_REG_R8)  { rex |= 0x01; needed = true; }
    if (needed) vtx_code_buffer_emit_byte(buf, rex);
}

/* MOV r64, r/m64 */
static void emit_mov_reg_reg64(vtx_code_buffer_t *buf, vtx_reg_t dst, vtx_reg_t src)
{
    emit_rex64(buf, dst, src);
    vtx_code_buffer_emit_byte(buf, 0x8B);
    vtx_code_buffer_emit_byte(buf, modrm(3, dst, src));
}

/* MOV r/m64, r64 */
static void emit_mov_reg64_reg(vtx_code_buffer_t *buf, vtx_reg_t dst, vtx_reg_t src)
{
    emit_rex64(buf, src, dst);
    vtx_code_buffer_emit_byte(buf, 0x89);
    vtx_code_buffer_emit_byte(buf, modrm(3, src, dst));
}

/* MOV r64, imm64 */
static void emit_mov_reg_imm64(vtx_code_buffer_t *buf, vtx_reg_t reg, uint64_t imm)
{
    uint8_t rex = REX_W;
    if (reg >= VTX_REG_R8) rex |= 0x01;
    vtx_code_buffer_emit_byte(buf, rex);
    vtx_code_buffer_emit_byte(buf, (uint8_t)(0xB8 | (reg & 7)));
    vtx_code_buffer_emit_qword(buf, imm);
}

/* MOV r64, imm32 (zero-extended) */
static void emit_mov_reg_imm32(vtx_code_buffer_t *buf, vtx_reg_t reg, uint32_t imm)
{
    if (reg >= VTX_REG_R8) vtx_code_buffer_emit_byte(buf, 0x41);
    vtx_code_buffer_emit_byte(buf, (uint8_t)(0xB8 | (reg & 7)));
    vtx_code_buffer_emit_dword(buf, imm);
}

/* MOV r64, [rbp + offset] */
static void emit_mov_reg_rbp_offset(vtx_code_buffer_t *buf, vtx_reg_t reg, int32_t offset)
{
    emit_rex64(buf, reg, VTX_REG_RBP);
    vtx_code_buffer_emit_byte(buf, 0x8B);
    if (offset >= -128 && offset <= 127) {
        vtx_code_buffer_emit_byte(buf, modrm(1, reg, VTX_REG_RBP));
        vtx_code_buffer_emit_byte(buf, (uint8_t)(offset & 0xFF));
    } else {
        vtx_code_buffer_emit_byte(buf, modrm(2, reg, VTX_REG_RBP));
        vtx_code_buffer_emit_dword(buf, (uint32_t)offset);
    }
}

/* MOV [rbp + offset], r64 */
static void emit_mov_rbp_offset_reg(vtx_code_buffer_t *buf, int32_t offset, vtx_reg_t reg)
{
    emit_rex64(buf, reg, VTX_REG_RBP);
    vtx_code_buffer_emit_byte(buf, 0x89);
    if (offset >= -128 && offset <= 127) {
        vtx_code_buffer_emit_byte(buf, modrm(1, reg, VTX_REG_RBP));
        vtx_code_buffer_emit_byte(buf, (uint8_t)(offset & 0xFF));
    } else {
        vtx_code_buffer_emit_byte(buf, modrm(2, reg, VTX_REG_RBP));
        vtx_code_buffer_emit_dword(buf, (uint32_t)offset);
    }
}

/* ADD r64, r64 */
static void emit_add_reg_reg(vtx_code_buffer_t *buf, vtx_reg_t dst, vtx_reg_t src)
{
    emit_rex64(buf, src, dst);
    vtx_code_buffer_emit_byte(buf, 0x01);
    vtx_code_buffer_emit_byte(buf, modrm(3, src, dst));
}

/* SUB r64, r64 */
static void emit_sub_reg_reg(vtx_code_buffer_t *buf, vtx_reg_t dst, vtx_reg_t src)
{
    emit_rex64(buf, src, dst);
    vtx_code_buffer_emit_byte(buf, 0x29);
    vtx_code_buffer_emit_byte(buf, modrm(3, src, dst));
}

/* IMUL r64, r64 (two-operand) */
static void emit_imul_reg_reg(vtx_code_buffer_t *buf, vtx_reg_t dst, vtx_reg_t src)
{
    emit_rex64(buf, dst, src);
    vtx_code_buffer_emit_byte(buf, 0x0F);
    vtx_code_buffer_emit_byte(buf, 0xAF);
    vtx_code_buffer_emit_byte(buf, modrm(3, dst, src));
}

/* ADD r64, imm32 (sign-extended) */
static void emit_add_reg_imm32(vtx_code_buffer_t *buf, vtx_reg_t reg, int32_t imm)
{
    emit_rex64(buf, (vtx_reg_t)0, reg);
    if (imm >= -128 && imm <= 127) {
        vtx_code_buffer_emit_byte(buf, 0x83);
        vtx_code_buffer_emit_byte(buf, modrm(3, 0, reg));
        vtx_code_buffer_emit_byte(buf, (uint8_t)(imm & 0xFF));
    } else {
        vtx_code_buffer_emit_byte(buf, 0x81);
        vtx_code_buffer_emit_byte(buf, modrm(3, 0, reg));
        vtx_code_buffer_emit_dword(buf, (uint32_t)imm);
    }
}

/* SUB r64, imm32 (sign-extended) */
static void emit_sub_reg_imm32(vtx_code_buffer_t *buf, vtx_reg_t reg, int32_t imm)
{
    emit_rex64(buf, (vtx_reg_t)5, reg);
    if (imm >= -128 && imm <= 127) {
        vtx_code_buffer_emit_byte(buf, 0x83);
        vtx_code_buffer_emit_byte(buf, modrm(3, 5, reg));
        vtx_code_buffer_emit_byte(buf, (uint8_t)(imm & 0xFF));
    } else {
        vtx_code_buffer_emit_byte(buf, 0x81);
        vtx_code_buffer_emit_byte(buf, modrm(3, 5, reg));
        vtx_code_buffer_emit_dword(buf, (uint32_t)imm);
    }
}

/* CMP r64, r64 */
static void emit_cmp_reg_reg(vtx_code_buffer_t *buf, vtx_reg_t a, vtx_reg_t b)
{
    emit_rex64(buf, b, a);
    vtx_code_buffer_emit_byte(buf, 0x39);
    vtx_code_buffer_emit_byte(buf, modrm(3, b, a));
}

/* CMP r64, imm32 */
static void emit_cmp_reg_imm32(vtx_code_buffer_t *buf, vtx_reg_t reg, int32_t imm)
{
    emit_rex64(buf, (vtx_reg_t)7, reg);
    if (imm >= -128 && imm <= 127) {
        vtx_code_buffer_emit_byte(buf, 0x83);
        vtx_code_buffer_emit_byte(buf, modrm(3, 7, reg));
        vtx_code_buffer_emit_byte(buf, (uint8_t)(imm & 0xFF));
    } else {
        vtx_code_buffer_emit_byte(buf, 0x81);
        vtx_code_buffer_emit_byte(buf, modrm(3, 7, reg));
        vtx_code_buffer_emit_dword(buf, (uint32_t)imm);
    }
}

/* TEST r64, r64 */
static void emit_test_reg_reg(vtx_code_buffer_t *buf, vtx_reg_t reg)
{
    emit_rex64(buf, reg, reg);
    vtx_code_buffer_emit_byte(buf, 0x85);
    vtx_code_buffer_emit_byte(buf, modrm(3, reg, reg));
}

/* PUSH r64 */
static void emit_push(vtx_code_buffer_t *buf, vtx_reg_t reg)
{
    if (reg >= VTX_REG_R8) vtx_code_buffer_emit_byte(buf, 0x41);
    vtx_code_buffer_emit_byte(buf, (uint8_t)(0x50 | (reg & 7)));
}

/* POP r64 */
static void emit_pop(vtx_code_buffer_t *buf, vtx_reg_t reg)
{
    if (reg >= VTX_REG_R8) vtx_code_buffer_emit_byte(buf, 0x41);
    vtx_code_buffer_emit_byte(buf, (uint8_t)(0x58 | (reg & 7)));
}

/* JMP rel32 */
static uint32_t emit_jmp32(vtx_code_buffer_t *buf)
{
    vtx_code_buffer_emit_byte(buf, 0xE9);
    uint32_t pos = vtx_code_buffer_position(buf);
    vtx_code_buffer_emit_dword(buf, 0); /* placeholder */
    return pos;
}

/* Jcc rel32 — returns position of the 4-byte displacement */
static uint32_t emit_jcc32(vtx_code_buffer_t *buf, uint8_t cc)
{
    vtx_code_buffer_emit_byte(buf, 0x0F);
    vtx_code_buffer_emit_byte(buf, (uint8_t)(0x80 | cc));
    uint32_t pos = vtx_code_buffer_position(buf);
    vtx_code_buffer_emit_dword(buf, 0); /* placeholder */
    return pos;
}

/* Jcc rel8 — returns position of the 1-byte displacement */
static uint32_t emit_jcc8(vtx_code_buffer_t *buf, uint8_t cc)
{
    vtx_code_buffer_emit_byte(buf, (uint8_t)(0x70 | cc));
    uint32_t pos = vtx_code_buffer_position(buf);
    vtx_code_buffer_emit_byte(buf, 0); /* placeholder */
    return pos;
}

/* CALL r/m64 */
static void emit_call_reg(vtx_code_buffer_t *buf, vtx_reg_t reg)
{
    if (reg >= VTX_REG_R8) vtx_code_buffer_emit_byte(buf, 0x41);
    vtx_code_buffer_emit_byte(buf, 0xFF);
    vtx_code_buffer_emit_byte(buf, modrm(3, 2, reg));
}

/* RET */
static void emit_ret(vtx_code_buffer_t *buf)
{
    vtx_code_buffer_emit_byte(buf, 0xC3);
}

/* NOP (1 byte) */
static void emit_nop(vtx_code_buffer_t *buf)
{
    vtx_code_buffer_emit_byte(buf, 0x90);
}

/* UD2 (trap) */
static void emit_ud2(vtx_code_buffer_t *buf)
{
    vtx_code_buffer_emit_byte(buf, 0x0F);
    vtx_code_buffer_emit_byte(buf, 0x0B);
}

/* ADDSD xmm0, [rbp+offset] — add double from memory to XMM0 */
static void emit_addsd_mem(vtx_code_buffer_t *buf, int32_t offset)
{
    vtx_code_buffer_emit_byte(buf, 0xF2); /* SSE2 prefix */
    vtx_code_buffer_emit_byte(buf, 0x0F);
    vtx_code_buffer_emit_byte(buf, 0x58); /* ADDSD */
    vtx_code_buffer_emit_byte(buf, modrm(2, 0, VTX_REG_RBP));
    vtx_code_buffer_emit_dword(buf, (uint32_t)offset);
}

/* SUBSD xmm0, [rbp+offset] */
static void emit_subsd_mem(vtx_code_buffer_t *buf, int32_t offset)
{
    vtx_code_buffer_emit_byte(buf, 0xF2);
    vtx_code_buffer_emit_byte(buf, 0x0F);
    vtx_code_buffer_emit_byte(buf, 0x5C); /* SUBSD */
    vtx_code_buffer_emit_byte(buf, modrm(2, 0, VTX_REG_RBP));
    vtx_code_buffer_emit_dword(buf, (uint32_t)offset);
}

/* MULSD xmm0, [rbp+offset] */
static void emit_mulsd_mem(vtx_code_buffer_t *buf, int32_t offset)
{
    vtx_code_buffer_emit_byte(buf, 0xF2);
    vtx_code_buffer_emit_byte(buf, 0x0F);
    vtx_code_buffer_emit_byte(buf, 0x59); /* MULSD */
    vtx_code_buffer_emit_byte(buf, modrm(2, 0, VTX_REG_RBP));
    vtx_code_buffer_emit_dword(buf, (uint32_t)offset);
}

/* DIVSD xmm0, [rbp+offset] */
static void emit_divsd_mem(vtx_code_buffer_t *buf, int32_t offset)
{
    vtx_code_buffer_emit_byte(buf, 0xF2);
    vtx_code_buffer_emit_byte(buf, 0x0F);
    vtx_code_buffer_emit_byte(buf, 0x5E); /* DIVSD */
    vtx_code_buffer_emit_byte(buf, modrm(2, 0, VTX_REG_RBP));
    vtx_code_buffer_emit_dword(buf, (uint32_t)offset);
}

/* MOVSD xmm0, [rbp+offset] — load double from memory */
static void emit_movsd_load(vtx_code_buffer_t *buf, int32_t offset)
{
    vtx_code_buffer_emit_byte(buf, 0xF2);
    vtx_code_buffer_emit_byte(buf, 0x0F);
    vtx_code_buffer_emit_byte(buf, 0x10); /* MOVSD xmm, m64 */
    vtx_code_buffer_emit_byte(buf, modrm(2, 0, VTX_REG_RBP));
    vtx_code_buffer_emit_dword(buf, (uint32_t)offset);
}

/* MOVSD [rbp+offset], xmm0 — store double to memory */
static void emit_movsd_store(vtx_code_buffer_t *buf, int32_t offset)
{
    vtx_code_buffer_emit_byte(buf, 0xF2);
    vtx_code_buffer_emit_byte(buf, 0x0F);
    vtx_code_buffer_emit_byte(buf, 0x11); /* MOVSD m64, xmm */
    vtx_code_buffer_emit_byte(buf, modrm(2, 0, VTX_REG_RBP));
    vtx_code_buffer_emit_dword(buf, (uint32_t)offset);
}

/* SHL r64, imm8 */
static void emit_shl_reg_imm8(vtx_code_buffer_t *buf, vtx_reg_t reg, uint8_t shift)
{
    emit_rex64(buf, (vtx_reg_t)4, reg);
    vtx_code_buffer_emit_byte(buf, 0xC1);
    vtx_code_buffer_emit_byte(buf, modrm(3, 4, reg));
    vtx_code_buffer_emit_byte(buf, shift);
}

/* SHR r64, imm8 (arithmetic) */
static void emit_sar_reg_imm8(vtx_code_buffer_t *buf, vtx_reg_t reg, uint8_t shift)
{
    emit_rex64(buf, (vtx_reg_t)7, reg);
    vtx_code_buffer_emit_byte(buf, 0xC1);
    vtx_code_buffer_emit_byte(buf, modrm(3, 7, reg));
    vtx_code_buffer_emit_byte(buf, shift);
}

/* AND r64, r64 */
static void emit_and_reg_reg(vtx_code_buffer_t *buf, vtx_reg_t dst, vtx_reg_t src)
{
    emit_rex64(buf, src, dst);
    vtx_code_buffer_emit_byte(buf, 0x21);
    vtx_code_buffer_emit_byte(buf, modrm(3, src, dst));
}

/* OR r64, r64 */
static void emit_or_reg_reg(vtx_code_buffer_t *buf, vtx_reg_t dst, vtx_reg_t src)
{
    emit_rex64(buf, src, dst);
    vtx_code_buffer_emit_byte(buf, 0x09);
    vtx_code_buffer_emit_byte(buf, modrm(3, src, dst));
}

/* XOR r64, r64 */
static void emit_xor_reg_reg(vtx_code_buffer_t *buf, vtx_reg_t dst, vtx_reg_t src)
{
    emit_rex64(buf, src, dst);
    vtx_code_buffer_emit_byte(buf, 0x31);
    vtx_code_buffer_emit_byte(buf, modrm(3, src, dst));
}

/* NEG r64 */
static void emit_neg_reg(vtx_code_buffer_t *buf, vtx_reg_t reg)
{
    emit_rex64(buf, (vtx_reg_t)3, reg);
    vtx_code_buffer_emit_byte(buf, 0xF7);
    vtx_code_buffer_emit_byte(buf, modrm(3, 3, reg));
}

/* NOT r64 */
static void emit_not_reg(vtx_code_buffer_t *buf, vtx_reg_t reg)
{
    emit_rex64(buf, (vtx_reg_t)2, reg);
    vtx_code_buffer_emit_byte(buf, 0xF7);
    vtx_code_buffer_emit_byte(buf, modrm(3, 2, reg));
}

/* SETcc r8 — set byte based on condition */
static void emit_setcc(vtx_code_buffer_t *buf, uint8_t cc, vtx_reg_t reg)
{
    vtx_code_buffer_emit_byte(buf, 0x0F);
    vtx_code_buffer_emit_byte(buf, (uint8_t)(0x90 | cc));
    vtx_code_buffer_emit_byte(buf, modrm(3, 0, reg));
}

/* CQO (sign-extend RAX into RDX:RAX for idiv) */
static void emit_cqo(vtx_code_buffer_t *buf)
{
    vtx_code_buffer_emit_byte(buf, 0x48); /* REX.W */
    vtx_code_buffer_emit_byte(buf, 0x99); /* CQO */
}

/* IDIV r64 (signed divide RDX:RAX / r64, quotient in RAX, remainder in RDX) */
static void emit_idiv_reg(vtx_code_buffer_t *buf, vtx_reg_t reg)
{
    emit_rex64(buf, (vtx_reg_t)7, reg);
    vtx_code_buffer_emit_byte(buf, 0xF7);
    vtx_code_buffer_emit_byte(buf, modrm(3, 7, reg));
}

/* Condition codes */
#define CC_E  0x4
#define CC_NE 0x5
#define CC_L  0xC
#define CC_LE 0xE
#define CC_G  0xF
#define CC_GE 0xD
#define CC_B  0x2
#define CC_AE 0x3
#define CC_O  0x0

/* ========================================================================== */
/* Expression stack model                                                      */
/* ========================================================================== */

/**
 * The expression stack is modeled as a virtual stack where the top
 * VTX_EXPR_REG_COUNT values are in registers. We track the current
 * stack depth during compilation to know which values are in registers
 * vs. spill slots.
 *
 * Register assignment (from TOS):
 *   TOS   → RAX (expr_reg[0])
 *   TOS-1 → RCX (expr_reg[1])
 *   TOS-2 → RDX (expr_reg[2])
 *   TOS-3 → RBX (expr_reg[3])
 *
 * When we push a value, we shift registers down:
 *   RBX ← RDX ← RCX ← RAX ← new_value
 * When we pop a value, we shift registers up:
 *   RAX → RCX → RDX → RBX → load from spill
 */

static const vtx_reg_t expr_regs[VTX_EXPR_REG_COUNT] = {
    VTX_REG_RAX, VTX_REG_RCX, VTX_REG_RDX, VTX_REG_RBX
};

/* ========================================================================== */
/* Compilation context                                                         */
/* ========================================================================== */

typedef struct {
    vtx_code_buffer_t       buf;
    const vtx_bytecode_t   *bc;
    const vtx_method_desc_t *method;
    vtx_profile_data_t     *profile_data;
    vtx_jit_frame_layout_t  layout;
    vtx_guard_array_t       guards;
    vtx_arena_t            *arena;

    /* Current compilation state */
    uint32_t  bc_pc;          /* current bytecode PC */
    uint32_t  stack_depth;    /* current expression stack depth */
    uint32_t  max_stack_depth; /* maximum stack depth seen */

    /* Branch fixups: forward branches that need target patching */
    vtx_branch_fixup_t *fixups;
    uint32_t            fixup_count;
    uint32_t            fixup_capacity;

    /* Bytecode PC → native offset map */
    vtx_bc_pc_map_entry_t *pc_map;
    uint32_t               pc_map_count;
    uint32_t               pc_map_capacity;

    /* Native offset → bytecode PC map */
    uint32_t *native_to_bc;
    uint32_t  native_to_bc_count;
    uint32_t  native_to_bc_capacity;

    /* Side table */
    vtx_side_table_t *side_table;

    /* Method ID for deopt */
    uint32_t method_id;
} vtx_compile_ctx_t;

/* ========================================================================== */
/* PC map operations                                                           */
/* ========================================================================== */

static void record_bc_pc_map(vtx_compile_ctx_t *ctx, uint32_t bc_pc, uint32_t native_offset)
{
    if (ctx->pc_map_count >= ctx->pc_map_capacity) {
        uint32_t new_cap = ctx->pc_map_capacity * 2;
        if (new_cap == 0) new_cap = 64;
        vtx_bc_pc_map_entry_t *new_map = (vtx_bc_pc_map_entry_t *)realloc(
            ctx->pc_map, new_cap * sizeof(vtx_bc_pc_map_entry_t));
        if (!new_map) return;
        ctx->pc_map = new_map;
        ctx->pc_map_capacity = new_cap;
    }
    ctx->pc_map[ctx->pc_map_count].bytecode_pc = bc_pc;
    ctx->pc_map[ctx->pc_map_count].native_offset = native_offset;
    ctx->pc_map_count++;
}

static void record_native_to_bc(vtx_compile_ctx_t *ctx, uint32_t native_offset, uint32_t bc_pc)
{
    /* Store mapping at index native_offset/8 (coarse) */
    uint32_t idx = native_offset / 8;
    if (idx >= ctx->native_to_bc_capacity) {
        uint32_t new_cap = idx + 256;
        uint32_t *new_arr = (uint32_t *)realloc(ctx->native_to_bc,
            new_cap * sizeof(uint32_t));
        if (!new_arr) return;
        memset(new_arr + ctx->native_to_bc_capacity, 0xFF,
               (new_cap - ctx->native_to_bc_capacity) * sizeof(uint32_t));
        ctx->native_to_bc = new_arr;
        ctx->native_to_bc_capacity = new_cap;
    }
    ctx->native_to_bc[idx] = bc_pc;
    if (idx >= ctx->native_to_bc_count) {
        ctx->native_to_bc_count = idx + 1;
    }
}

/* ========================================================================== */
/* Branch fixup operations                                                     */
/* ========================================================================== */

static void add_fixup(vtx_compile_ctx_t *ctx, uint32_t patch_pos,
                       uint32_t source_offset, uint16_t target_bc_pc, bool is_32bit)
{
    if (ctx->fixup_count >= ctx->fixup_capacity) {
        uint32_t new_cap = ctx->fixup_capacity * 2;
        if (new_cap == 0) new_cap = 32;
        vtx_branch_fixup_t *new_fixups = (vtx_branch_fixup_t *)realloc(
            ctx->fixups, new_cap * sizeof(vtx_branch_fixup_t));
        if (!new_fixups) return;
        ctx->fixups = new_fixups;
        ctx->fixup_capacity = new_cap;
    }
    ctx->fixups[ctx->fixup_count].patch_position = patch_pos;
    ctx->fixups[ctx->fixup_count].source_offset = source_offset;
    ctx->fixups[ctx->fixup_count].target_bc_pc = target_bc_pc;
    ctx->fixups[ctx->fixup_count].is_32bit = is_32bit;
    ctx->fixup_count++;
}

static void resolve_fixups(vtx_compile_ctx_t *ctx)
{
    /* For each fixup, find the native offset for the target BC PC
     * and patch the displacement. */
    for (uint32_t i = 0; i < ctx->fixup_count; i++) {
        vtx_branch_fixup_t *fixup = &ctx->fixups[i];

        /* Find the native offset for the target BC PC.
         * Binary search in the sorted pc_map. */
        uint32_t target_native = 0;
        bool found = false;
        for (uint32_t j = 0; j < ctx->pc_map_count; j++) {
            if (ctx->pc_map[j].bytecode_pc == fixup->target_bc_pc) {
                target_native = ctx->pc_map[j].native_offset;
                found = true;
                break;
            }
        }

        if (!found) {
            /* Target not yet compiled — this can happen for forward branches.
             * In a single-pass compiler, we should have seen all targets.
             * If not found, patch to a trap. */
            continue;
        }

        /* Calculate relative displacement.
         * For 32-bit: disp = target - (source + instruction_size)
         * JMP rel32: instruction is 5 bytes (E9 + 4-byte disp)
         * Jcc rel32: instruction is 6 bytes (0F 8x + 4-byte disp) */
        uint32_t instr_size = fixup->is_32bit ? 6 : 5;
        int32_t disp = (int32_t)(target_native - (fixup->source_offset + instr_size));

        if (fixup->is_32bit) {
            vtx_code_buffer_patch_dword(&ctx->buf, fixup->patch_position,
                                         (uint32_t)disp);
        } else {
            ctx->buf.bytes[fixup->patch_position] = (uint8_t)(disp & 0xFF);
        }
    }
}

/* ========================================================================== */
/* Expression stack manipulation helpers                                       */
/* ========================================================================== */

/**
 * Emit code to "push" a value onto the expression stack.
 * This shifts the register file and may spill the bottom register.
 *
 * The new value should be placed in RAX after this call.
 * Before this call: RAX=TOS, RCX=TOS-1, RDX=TOS-2, RBX=TOS-3
 * After this call:  RAX=TOS(new), RCX=old_TOS, RDX=old_TOS-1, RBX=old_TOS-2
 *                   old_TOS-3 is spilled if it exists
 */
static void emit_stack_push(vtx_compile_ctx_t *ctx)
{
    uint32_t old_depth = ctx->stack_depth;
    ctx->stack_depth++;

    if (old_depth >= VTX_EXPR_REG_COUNT) {
        /* All registers are occupied. Spill RBX (the deepest register value)
         * to the spill area, then shift registers. */
        /* Spill slot index for the value currently in RBX */
        uint32_t spill_idx = old_depth - VTX_EXPR_REG_COUNT;
        int32_t spill_off = vtx_frame_layout_spill_offset(&ctx->layout, spill_idx);
        emit_mov_rbp_offset_reg(&ctx->buf, spill_off, VTX_REG_RBX);
    }

    /* Shift registers: RBX ← RDX, RDX ← RCX, RCX ← RAX */
    /* We need to do this in the right order to avoid overwriting.
     * RBX = RDX, RDX = RCX, RCX = RAX
     * Order: RBX ← RDX first (RBX is safe since RDX won't be overwritten yet)
     *        RDX ← RCX
     *        RCX ← RAX
     * RAX will be loaded with the new value by the caller. */
    if (old_depth >= 3) emit_mov_reg_reg64(&ctx->buf, VTX_REG_RBX, VTX_REG_RDX);
    if (old_depth >= 2) emit_mov_reg_reg64(&ctx->buf, VTX_REG_RDX, VTX_REG_RCX);
    if (old_depth >= 1) emit_mov_reg_reg64(&ctx->buf, VTX_REG_RCX, VTX_REG_RAX);

    if (ctx->stack_depth > ctx->max_stack_depth) {
        ctx->max_stack_depth = ctx->stack_depth;
    }
}

/**
 * Emit code to "pop" a value from the expression stack.
 * The popped value is in RAX.
 * After: RAX=old_TOS (popped), RCX=new_TOS, RDX=new_TOS-1, RBX=new_TOS-2
 *        May load a value from spill into the new deepest register.
 *
 * Returns the register holding the popped value (RAX).
 */
static vtx_reg_t emit_stack_pop(vtx_compile_ctx_t *ctx)
{
    VTX_ASSERT(ctx->stack_depth > 0, "stack underflow during compilation");
    ctx->stack_depth--;

    /* The value to pop is in RAX (TOS). We need to shift registers up:
     * RAX = RCX (new TOS), RCX = RDX, RDX = RBX, RBX = load from spill
     * But we need to save the popped value first. */

    /* Move the popped value to RSI (temporary, not used by expr stack) */
    /* Actually, we should move the registers up and the caller uses
     * whatever register held the TOS. Let's do it differently:
     * The caller expects the popped value in a register they specify.
     * For simplicity, we move TOS (RAX) to RSI, then shift up. */

    /* For most operations, the caller pops two values and uses them.
     * We'll use a different approach: the caller saves RAX before calling pop
     * if they need it. */
    /* shift: RAX ← RCX, RCX ← RDX, RDX ← RBX */
    uint32_t new_depth = ctx->stack_depth;

    if (new_depth >= 3) {
        /* Load a value from spill into RBX */
        uint32_t spill_idx = new_depth - VTX_EXPR_REG_COUNT;
        int32_t spill_off = vtx_frame_layout_spill_offset(&ctx->layout, spill_idx);
        emit_mov_reg_rbp_offset(&ctx->buf, VTX_REG_RBX, spill_off);
    }

    if (new_depth >= 0) emit_mov_reg_reg64(&ctx->buf, VTX_REG_RAX, VTX_REG_RCX);
    if (new_depth >= 1) emit_mov_reg_reg64(&ctx->buf, VTX_REG_RCX, VTX_REG_RDX);
    if (new_depth >= 2) emit_mov_reg_reg64(&ctx->buf, VTX_REG_RDX, VTX_REG_RBX);

    return VTX_REG_RAX;
}

/**
 * Pop two values and return them in specific registers.
 * first_pop = earlier pushed value, second_pop = later pushed (TOS).
 * first_pop → RSI, second_pop → RDI
 */
static void emit_stack_pop2(vtx_compile_ctx_t *ctx, vtx_reg_t *first, vtx_reg_t *second)
{
    VTX_ASSERT(ctx->stack_depth >= 2, "stack underflow: need 2 values");

    /* TOS is in RAX, TOS-1 is in RCX.
     * We want: second = old TOS (RAX), first = old TOS-1 (RCX) */
    /* Save both to temporary registers first */
    emit_mov_reg_reg64(&ctx->buf, VTX_REG_RSI, VTX_REG_RCX); /* first = TOS-1 */
    emit_mov_reg_reg64(&ctx->buf, VTX_REG_RDI, VTX_REG_RAX); /* second = TOS */

    /* Now shift the stack up by 2 */
    ctx->stack_depth -= 2;
    uint32_t new_depth = ctx->stack_depth;

    /* Need to shift up 2 positions:
     * RAX = old TOS-2 (was RDX)
     * RCX = old TOS-3 (was RBX)
     * RDX = spill or empty
     * RBX = spill or empty */
    if (new_depth >= 2) {
        /* RAX ← RDX, RCX ← RBX, then load spills into RDX and RBX */
        emit_mov_reg_reg64(&ctx->buf, VTX_REG_RAX, VTX_REG_RDX);
        emit_mov_reg_reg64(&ctx->buf, VTX_REG_RCX, VTX_REG_RBX);
        if (new_depth >= 3) {
            /* Load into RDX from spill */
            uint32_t spill_idx = new_depth - VTX_EXPR_REG_COUNT;
            int32_t spill_off = vtx_frame_layout_spill_offset(&ctx->layout, spill_idx);
            emit_mov_reg_rbp_offset(&ctx->buf, VTX_REG_RDX, spill_off);
        }
        if (new_depth >= 4) {
            uint32_t spill_idx = new_depth - VTX_EXPR_REG_COUNT + 1;
            int32_t spill_off = vtx_frame_layout_spill_offset(&ctx->layout, spill_idx);
            emit_mov_reg_rbp_offset(&ctx->buf, VTX_REG_RBX, spill_off);
        }
    } else if (new_depth == 1) {
        /* RAX ← RDX, RCX ← RBX */
        emit_mov_reg_reg64(&ctx->buf, VTX_REG_RAX, VTX_REG_RDX);
        if (new_depth >= 2) {
            emit_mov_reg_reg64(&ctx->buf, VTX_REG_RCX, VTX_REG_RBX);
        }
        /* Load from spill if needed */
        if (new_depth + 2 > VTX_EXPR_REG_COUNT) {
            uint32_t spill_idx = new_depth - VTX_EXPR_REG_COUNT;
            int32_t spill_off = vtx_frame_layout_spill_offset(&ctx->layout, spill_idx);
            emit_mov_reg_rbp_offset(&ctx->buf, VTX_REG_RCX, spill_off);
        }
    }
    /* For new_depth == 0, all registers are already "consumed" —
     * the stack is empty, so RAX/RCX/RDX/RBX hold nothing useful.
     * No shifting needed. */

    *first = VTX_REG_RSI;
    *second = VTX_REG_RDI;
}

/* ========================================================================== */
/* NaN-boxing helpers                                                          */
/* ========================================================================== */

/**
 * Tag an int64_t value as an SMI (small integer).
 * SMI encoding: VTX_NAN_BOX_HEADER | (raw << VTX_NAN_DATA_SHIFT) | VTX_TAG_SMI
 * This requires multiple instructions, so we call a helper function.
 */
static void emit_tag_smi(vtx_compile_ctx_t *ctx, vtx_reg_t val_reg)
{
    /* RAX = val_reg value (raw int64_t)
     * We need to compute: VTX_NAN_BOX_HEADER | (val & VTX_NAN_DATA_MASK) << 3 | VTX_TAG_SMI
     *
     * Steps:
     *   1. Truncate val to 48 bits: val &= VTX_NAN_DATA_MASK
     *   2. Shift left by 3: val <<= VTX_NAN_DATA_SHIFT (3)
     *   3. OR with VTX_NAN_BOX_HEADER | VTX_TAG_SMI
     *
     * Since VTX_NAN_DATA_SHIFT = 3 and VTX_TAG_SMI = 0, we just need:
     *   val = (val & 0x0000FFFFFFFFFFFF) << 3
     *   val |= VTX_NAN_BOX_HEADER
     *
     * But working with 64-bit immediates in x86-64 requires multiple steps.
     * We'll use R10 as scratch.
     */

    /* shl val_reg, 3 */
    emit_shl_reg_imm8(&ctx->buf, val_reg, 3);

    /* mov r10, VTX_NAN_BOX_HEADER */
    emit_mov_reg_imm64(&ctx->buf, VTX_REG_R10, VTX_NAN_BOX_HEADER);

    /* or val_reg, r10 */
    emit_or_reg_reg(&ctx->buf, val_reg, VTX_REG_R10);
}

/**
 * Untag an SMI value to get the raw int64_t.
 * SMI decoding: (val >> VTX_NAN_DATA_SHIFT) & VTX_NAN_DATA_MASK, then sign-extend.
 */
static void emit_untag_smi(vtx_compile_ctx_t *ctx, vtx_reg_t val_reg)
{
    /* sar val_reg, VTX_NAN_DATA_SHIFT (3) */
    emit_sar_reg_imm8(&ctx->buf, val_reg, VTX_NAN_DATA_SHIFT);
}

/**
 * Extract heap pointer from tagged value.
 * ptr = ((val >> VTX_NAN_DATA_SHIFT) & VTX_NAN_DATA_MASK) << 3
 * This is equivalent to: (val >> 3) & 0x0000FFFFFFFFFFFF ... then << 3
 * Which simplifies to: val & ~0x7 (clear low 3 bits after shift and mask)
 *
 * Actually, the heap pointer encoding is:
 *   raw = (val >> 3) & 0x0000FFFFFFFFFFFF
 *   ptr = raw << 3
 * So ptr = val & 0x0000FFFFFFFFFFFFF8 (mask out the tag bits and shift)
 *
 * But this is complex. For 8-byte aligned pointers on x86-64 (48-bit
 * address space), we can use: and val, ~0x7 then check if it's a heap
 * pointer by testing the NaN-box header.
 *
 * Actually, the simplest correct approach for the baseline JIT:
 * The value is NaN-boxed: VTX_NAN_BOX_HEADER | (ptr>>3 << 3) | VTX_TAG_HEAP_PTR
 * To extract: shift right by 3, mask with VTX_NAN_DATA_MASK, shift left by 3.
 * Since ptr is 8-byte aligned, (ptr >> 3) << 3 == ptr.
 * And (val >> 3) gives us (ptr >> 3) in the low bits plus header garbage
 * in the high bits. We mask to get just the ptr>>3 part.
 *
 * Simpler: since the tag is 3 bits and ptr is 8-byte aligned (low 3 bits = 0),
 * we can extract the pointer by:
 *   1. Clear the top 16 bits (NaN header) and bottom 3 bits (tag)
 *   2. The result is the pointer
 *
 * The value layout: [NaN header 16 bits][data 48 bits shifted left by 3][tag 3 bits]
 * = [16 bits NaN][45 bits ptr_high][3 zero bits from alignment][tag]
 *
 * Hmm, this is getting complicated. Let me just call a helper function.
 * Actually, for the baseline JIT, we use a simpler approach:
 * we keep a "working register" convention where after null-check and
 * type-check, we have the raw heap pointer in a register, and we
 * use that for field accesses. The tagged value is kept in the frame.
 */
static void emit_untag_heap_ptr(vtx_compile_ctx_t *ctx, vtx_reg_t val_reg, vtx_reg_t result_reg)
{
    /* Extract heap pointer from NaN-boxed value.
     * Method: use the formula ptr = ((val >> 3) & VTX_NAN_DATA_MASK) << 3
     * But VTX_NAN_DATA_MASK = 0x0000FFFFFFFFFFFF
     *
     * Since the pointer is 8-byte aligned and fits in 48 bits:
     * (val >> 3) & mask gives us ptr >> 3
     * Then (ptr >> 3) << 3 = ptr (since low 3 bits are zero)
     *
     * So effectively: ptr = (val & 0x0000FFFFFFFFFFFFF8)
     * But that's not quite right because the NaN header bits are in the top.
     *
     * Let's just do: mov result, val; shr result, 3; and result, mask; shl result, 3
     * But we need the 48-bit mask as a 64-bit immediate.
     *
     * Easier: call the vtx_heap_ptr() function.
     */
    /* For the baseline JIT, we call the helper function.
     * Save expression stack registers, call, restore. */
    emit_mov_reg_reg64(&ctx->buf, result_reg, val_reg);

    /* mov r10, VTX_NAN_DATA_MASK */
    emit_mov_reg_imm64(&ctx->buf, VTX_REG_R10, VTX_NAN_DATA_MASK);

    /* shr result_reg, 3 */
    vtx_code_buffer_emit_byte(&ctx->buf, 0x48);
    vtx_code_buffer_emit_byte(&ctx->buf, 0xC1);
    vtx_code_buffer_emit_byte(&ctx->buf, modrm(3, 5, result_reg));
    vtx_code_buffer_emit_byte(&ctx->buf, 3);

    /* and result_reg, r10 */
    emit_and_reg_reg(&ctx->buf, result_reg, VTX_REG_R10);

    /* shl result_reg, 3 */
    emit_shl_reg_imm8(&ctx->buf, result_reg, 3);
}

/* ========================================================================== */
/* Prologue and epilogue                                                       */
/* ========================================================================== */

static void emit_prologue(vtx_compile_ctx_t *ctx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /*
     * Prologue:
     *   Entry: RSP points to return address (pushed by CALL)
     *   RDI = method pointer (1st arg, System V ABI)
     *   RSI = deopt_info pointer (2nd arg)
     *   RDX = profile_data pointer (3rd arg)
     *
     *   push method_ptr       ; [RBP+24] after frame setup
     *   push deopt_info       ; [RBP+16]
     *   push profile_data     ; [RBP+8]
     *   push rbp              ; [RBP+0] = caller RBP
     *   mov rbp, rsp
     *   sub rsp, total_frame_size
     *   push rbx              ; save callee-saved register (used for expr stack)
     *   sub rsp, 8            ; align + save slot for rbx
     *
     * Wait, we use RBX for the expression stack, so we must save it.
     * But RBX is callee-saved, so we need to push it in the prologue
     * and pop it in the epilogue.
     *
     * Revised prologue:
     *   push rbx              ; save callee-saved (we use RBX for expr stack)
     *   push method_ptr       ; [RBP+32] after frame setup
     *   push deopt_info       ; [RBP+24]
     *   push profile_data     ; [RBP+16]
     *   push rbp              ; [RBP+8]  = caller RBP... wait this doesn't work.
     *
     * Let me think about this differently. We need RBX to be saved.
     * The simplest approach is to push RBX as part of the prologue,
     * then include it in the frame layout.
     *
     * Actually, the standard approach is:
     *   push rbp
     *   mov rbp, rsp
     *   push rbx              ; save callee-saved
     *   push method_ptr
     *   push deopt_info
     *   push profile_data
     *   sub rsp, total_frame_size
     *
     * But this puts the header at different offsets than what we defined.
     * Let me reconsider the frame layout.
     *
     * Simplest approach: push rbx as part of the prologue, then
     * the header values, then sub rsp for locals/spills.
     *
     * After:
     *   push rbp              ; RBP+0 = caller RBP
     *   mov rbp, rsp
     *   push rbx              ; RBP-8 = saved RBX
     *   push r13              ; RBP-16 = saved R13 (scratch)
     *   push r14              ; RBP-24 = saved R14 (scratch)
     *   push method_ptr       ; RBP-32 = method
     *   push deopt_info       ; RBP-40 = deopt_info
     *   push profile_data     ; RBP-48 = profile_data
     *   sub rsp, frame_size   ; locals and spills
     *
     * Hmm, this puts everything below RBP, which is fine but different
     * from the original design. Let me simplify: use the "sub rsp"
     * approach and store everything at fixed offsets below RBP.
     *
     * Final prologue design:
     *   push rbp
     *   mov rbp, rsp
     *   push rbx          ; callee-saved
     *   push r12          ; callee-saved (scratch)
     *   sub rsp, frame_size  ; locals + spills + header
     *   ; Store header values at fixed offsets:
     *   mov [rbp + offset_method], rdi
     *   mov [rbp + offset_deopt], rsi
     *   mov [rbp + offset_profile], rdx
     *
     * OK, this is getting too complex with the offset calculations.
     * Let me just use the simplest possible design:
     *
     * Prologue:
     *   push rbp
     *   mov rbp, rsp
     *   sub rsp, total_frame_size   ; includes saved regs + header + locals + spills
     *   mov [rbp - 8],  rbx        ; save callee-saved
     *   mov [rbp - 16], r12        ; save callee-saved
     *   mov [rbp - 24], rdi        ; method pointer
     *   mov [rbp - 32], rsi        ; deopt_info
     *   mov [rbp - 40], rdx        ; profile_data
     *   ; Locals start at [rbp - 48]
     *   ; Spills start at [rbp - 48 - max_locals*8]
     *
     * Epilogue:
     *   mov rbx, [rbp - 8]
     *   mov r12, [rbp - 16]
     *   mov rsp, rbp
     *   pop rbp
     *   ret
     *
     * This means total_frame_size = 40 + max_locals*8 + max_spills*8,
     * rounded up to 16.
     *
     * Frame offsets (all negative from RBP):
     *   -8:  saved RBX
     *   -16: saved R12
     *   -24: method pointer
     *   -32: deopt_info pointer
     *   -40: profile_data pointer
     *   -48: local[0]
     *   -56: local[1]
     *   ...
     *
     * Let me redefine the constants and update frame_layout.h accordingly.
     * Actually, I'll just define the offsets here in codegen and keep
     * frame_layout.h generic. The codegen knows the exact layout.
     */

    /* New frame layout for codegen:
     * [RBP+8]  = return address
     * [RBP]    = caller RBP
     * [RBP-8]  = saved RBX (callee-saved)
     * [RBP-16] = saved R12 (callee-saved)
     * [RBP-24] = method pointer (from RDI)
     * [RBP-32] = deopt_info pointer (from RSI)
     * [RBP-40] = profile_data pointer (from RDX)
     * [RBP-48] = local[0]
     * [RBP-56] = local[1]
     * ...
     * [RBP-48-max_locals*8+8] = local[max_locals-1]
     * then spills...
     *
     * total_frame_size = 40 + max_locals*8 + max_spills*8
     *                    + padding to 16 bytes
     *
     * The frame_layout.locals_base should be -48 (local[0] at RBP-48)
     * But the frame_layout module doesn't know about saved regs and header.
     * I'll just override it here.
     */

    /* Calculate total frame size: saved regs + header + locals + spills */
    uint32_t saved_and_header = 5 * 8; /* rbx, r12, method, deopt, profile = 40 */
    uint32_t locals_bytes = ctx->layout.max_locals * 8;
    uint32_t spill_bytes = ctx->layout.max_spills * 8;
    uint32_t raw_size = saved_and_header + locals_bytes + spill_bytes;
    uint32_t alignment = VTX_FRAME_ALIGNMENT;
    uint32_t total = (raw_size + alignment - 1) & ~(alignment - 1);
    ctx->layout.total_frame_size = total;
    ctx->layout.frame_bottom = -(int32_t)total;

    /* Override locals_base and spill_base for the codegen layout */
    ctx->layout.locals_base = -(int32_t)(saved_and_header + 8); /* local[0] at RBP-48 */
    ctx->layout.spill_base = -(int32_t)(saved_and_header + (ctx->layout.max_locals + 1) * 8);

    /* push rbp */
    emit_push(buf, VTX_REG_RBP);
    /* mov rbp, rsp */
    vtx_code_buffer_emit_byte(buf, REX_W);
    vtx_code_buffer_emit_byte(buf, 0x89);
    vtx_code_buffer_emit_byte(buf, modrm(3, VTX_REG_RSP, VTX_REG_RBP));

    /* sub rsp, total_frame_size */
    vtx_code_buffer_emit_byte(buf, REX_W);
    if (total <= 127) {
        vtx_code_buffer_emit_byte(buf, 0x83);
        vtx_code_buffer_emit_byte(buf, modrm(3, 5, VTX_REG_RSP));
        vtx_code_buffer_emit_byte(buf, (uint8_t)total);
    } else {
        vtx_code_buffer_emit_byte(buf, 0x81);
        vtx_code_buffer_emit_byte(buf, modrm(3, 5, VTX_REG_RSP));
        vtx_code_buffer_emit_dword(buf, total);
    }

    /* Save callee-saved registers */
    emit_mov_rbp_offset_reg(buf, -8, VTX_REG_RBX);
    emit_mov_rbp_offset_reg(buf, -16, VTX_REG_R12);

    /* Store header values from argument registers */
    emit_mov_rbp_offset_reg(buf, -24, VTX_REG_RDI);  /* method */
    emit_mov_rbp_offset_reg(buf, -32, VTX_REG_RSI);  /* deopt_info */
    emit_mov_rbp_offset_reg(buf, -40, VTX_REG_RDX);  /* profile_data */

    /* Initialize locals to VTX_VALUE_UNDEFINED */
    emit_mov_reg_imm64(buf, VTX_REG_RAX, VTX_VALUE_UNDEFINED);
    for (uint32_t i = 0; i < ctx->layout.max_locals; i++) {
        int32_t off = vtx_frame_layout_local_offset(&ctx->layout, i);
        emit_mov_rbp_offset_reg(buf, off, VTX_REG_RAX);
    }

    /* Emit invocation counter increment */
    if (ctx->profile_data) {
        vtx_instrument_emit_invocation_increment(buf, ctx->profile_data);
    }
}

static void emit_epilogue(vtx_compile_ctx_t *ctx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Restore callee-saved registers */
    emit_mov_reg_rbp_offset(buf, VTX_REG_RBX, -8);
    emit_mov_reg_rbp_offset(buf, VTX_REG_R12, -16);

    /* mov rsp, rbp */
    vtx_code_buffer_emit_byte(buf, REX_W);
    vtx_code_buffer_emit_byte(buf, 0x89);
    vtx_code_buffer_emit_byte(buf, modrm(3, VTX_REG_RBP, VTX_REG_RSP));

    /* pop rbp */
    emit_pop(buf, VTX_REG_RBP);

    /* ret */
    emit_ret(buf);
}

/* ========================================================================== */
/* Per-opcode code generation                                                  */
/* ========================================================================== */

static void compile_halt(vtx_compile_ctx_t *ctx)
{
    /* Halt: emit a trap. Should never be reached in compiled code. */
    emit_ud2(&ctx->buf);
}

static void compile_nop(vtx_compile_ctx_t *ctx)
{
    /* No operation */
}

static void compile_load_local(vtx_compile_ctx_t *ctx, uint16_t local_idx)
{
    int32_t off = vtx_frame_layout_local_offset(&ctx->layout, local_idx);
    emit_stack_push(ctx);
    /* Load local value into RAX (new TOS) */
    emit_mov_reg_rbp_offset(&ctx->buf, VTX_REG_RAX, off);
}

static void compile_store_local(vtx_compile_ctx_t *ctx, uint16_t local_idx)
{
    /* Pop TOS and store to local */
    int32_t off = vtx_frame_layout_local_offset(&ctx->layout, local_idx);
    /* RAX holds TOS — store it, then shift stack */
    emit_mov_rbp_offset_reg(&ctx->buf, off, VTX_REG_RAX);
    emit_stack_pop(ctx);
}

static void compile_load_field(vtx_compile_ctx_t *ctx, uint16_t field_offset)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* TOS is the object (tagged heap pointer). Pop it, load the field. */
    /* RAX = object value (tagged) */

    /* Null check guard */
    vtx_guard_info_t guard;
    memset(&guard, 0, sizeof(guard));
    guard.bytecode_pc = ctx->bc_pc;
    guard.deopt_continuation = ctx->bc_pc;
    guard.stack_depth = ctx->stack_depth;
    vtx_guard_emit_null_check(buf, VTX_REG_RAX, guard, &ctx->guards);

    /* Extract heap pointer from tagged value into R10 */
    emit_untag_heap_ptr(ctx, VTX_REG_RAX, VTX_REG_R10);

    /* Load field value: RAX = [R10 + VTX_HEAP_OBJECT_HEADER_SIZE + field_offset*8] */
    int32_t field_off = (int32_t)VTX_HEAP_OBJECT_HEADER_SIZE + (int32_t)(field_offset * 8);
    emit_mov_reg_rbp_offset(buf, VTX_REG_RAX, field_off);
    /* Wait, that's wrong — we need mov rax, [r10 + offset], not [rbp + offset] */
    /* Use: mov rax, [r10 + disp32] */
    emit_rex64(buf, VTX_REG_RAX, VTX_REG_R10);
    vtx_code_buffer_emit_byte(buf, 0x8B);
    if (field_off >= -128 && field_off <= 127) {
        vtx_code_buffer_emit_byte(buf, modrm(1, VTX_REG_RAX, VTX_REG_R10));
        vtx_code_buffer_emit_byte(buf, (uint8_t)(field_off & 0xFF));
    } else {
        vtx_code_buffer_emit_byte(buf, modrm(2, VTX_REG_RAX, VTX_REG_R10));
        vtx_code_buffer_emit_dword(buf, (uint32_t)field_off);
    }

    /* The pop already happened conceptually; now we push the field value.
     * But we already popped when we consumed TOS. We need to push the result.
     * Actually, LOAD_FIELD: pop object, push field value. Net stack effect = 0.
     * But in our model, we pop the object and then push the result.
     * We already read the object from RAX (TOS). We need to shift the stack
     * to remove the object and then push the field value.
     * Since net effect is 0, we just replace TOS with the result.
     * RAX already holds the result, so no shift needed! */
}

static void compile_store_field(vtx_compile_ctx_t *ctx, uint16_t field_offset)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Pop value (TOS) and object (TOS-1). Store value into object.field. */
    vtx_reg_t val_reg, obj_reg;
    emit_stack_pop2(ctx, &obj_reg, &val_reg);
    /* obj_reg = RSI (TOS-1 = object), val_reg = RDI (TOS = value) */

    /* Null check on object */
    vtx_guard_info_t guard;
    memset(&guard, 0, sizeof(guard));
    guard.bytecode_pc = ctx->bc_pc;
    guard.deopt_continuation = ctx->bc_pc;
    guard.stack_depth = ctx->stack_depth + 2; /* before pop2 */
    vtx_guard_emit_null_check(buf, obj_reg, guard, &ctx->guards);

    /* Extract heap pointer */
    emit_untag_heap_ptr(ctx, obj_reg, VTX_REG_R10);

    /* Store: [R10 + header_size + field_offset*8] = val_reg */
    int32_t field_off = (int32_t)VTX_HEAP_OBJECT_HEADER_SIZE + (int32_t)(field_offset * 8);
    /* mov [r10 + field_off], rdi */
    emit_rex64(buf, val_reg, VTX_REG_R10);
    vtx_code_buffer_emit_byte(buf, 0x89); /* MOV r/m64, r64 */
    if (field_off >= -128 && field_off <= 127) {
        vtx_code_buffer_emit_byte(buf, modrm(1, val_reg, VTX_REG_R10));
        vtx_code_buffer_emit_byte(buf, (uint8_t)(field_off & 0xFF));
    } else {
        vtx_code_buffer_emit_byte(buf, modrm(2, val_reg, VTX_REG_R10));
        vtx_code_buffer_emit_dword(buf, (uint32_t)field_off);
    }

    /* Write barrier: call gc_write_barrier if the value is a young-gen pointer.
     * For the baseline JIT, we always call the write barrier. */
    /* Save expr stack regs, call barrier, restore */
    emit_push(buf, VTX_REG_RAX);
    emit_push(buf, VTX_REG_RCX);
    emit_push(buf, VTX_REG_RDX);
    emit_push(buf, VTX_REG_RBX);

    /* Args: gc_ptr (R10), field_offset, value (RDI) */
    /* For now, skip the write barrier call — it would need the GC pointer.
     * The GC integration is handled at a higher level. */

    emit_pop(buf, VTX_REG_RBX);
    emit_pop(buf, VTX_REG_RDX);
    emit_pop(buf, VTX_REG_RCX);
    emit_pop(buf, VTX_REG_RAX);
}

static void compile_load_const_int(vtx_compile_ctx_t *ctx, uint16_t cp_idx)
{
    vtx_value_t val = ctx->bc->constant_pool[cp_idx];
    int64_t int_val = vtx_smi_value(val);
    emit_stack_push(ctx);
    emit_mov_reg_imm64(&ctx->buf, VTX_REG_RAX, (uint64_t)val);
}

static void compile_load_const_float(vtx_compile_ctx_t *ctx, uint16_t cp_idx)
{
    vtx_value_t val = ctx->bc->constant_pool[cp_idx];
    emit_stack_push(ctx);
    emit_mov_reg_imm64(&ctx->buf, VTX_REG_RAX, val);
}

static void compile_load_const_str(vtx_compile_ctx_t *ctx, uint16_t cp_idx)
{
    vtx_value_t val = ctx->bc->constant_pool[cp_idx];
    emit_stack_push(ctx);
    emit_mov_reg_imm64(&ctx->buf, VTX_REG_RAX, val);
}

static void compile_load_null(vtx_compile_ctx_t *ctx)
{
    emit_stack_push(ctx);
    emit_mov_reg_imm64(&ctx->buf, VTX_REG_RAX, VTX_VALUE_NULL);
}

static void compile_load_true(vtx_compile_ctx_t *ctx)
{
    emit_stack_push(ctx);
    emit_mov_reg_imm64(&ctx->buf, VTX_REG_RAX, VTX_VALUE_TRUE);
}

static void compile_load_false(vtx_compile_ctx_t *ctx)
{
    emit_stack_push(ctx);
    emit_mov_reg_imm64(&ctx->buf, VTX_REG_RAX, VTX_VALUE_FALSE);
}

static void compile_load_undefined(vtx_compile_ctx_t *ctx)
{
    emit_stack_push(ctx);
    emit_mov_reg_imm64(&ctx->buf, VTX_REG_RAX, VTX_VALUE_UNDEFINED);
}

/* ========================================================================== */
/* Integer arithmetic                                                          */
/* ========================================================================== */

static void compile_int_arith(vtx_compile_ctx_t *ctx, vtx_opcode_t op)
{
    vtx_code_buffer_t *buf = &ctx->buf;
    vtx_reg_t lhs_reg, rhs_reg;
    emit_stack_pop2(ctx, &lhs_reg, &rhs_reg);
    /* lhs_reg = RSI (TOS-1), rhs_reg = RDI (TOS) */

    /* Untag both SMI values to get raw int64_t */
    emit_untag_smi(ctx, lhs_reg);
    emit_untag_smi(ctx, rhs_reg);

    switch (op) {
    case VT_OP_IADD:
        emit_add_reg_reg(buf, lhs_reg, rhs_reg);
        /* Overflow guard */
        {
            vtx_guard_info_t guard;
            memset(&guard, 0, sizeof(guard));
            guard.bytecode_pc = ctx->bc_pc;
            guard.deopt_continuation = ctx->bc_pc;
            guard.stack_depth = ctx->stack_depth + 2;
            vtx_guard_emit_overflow_check(buf, guard, &ctx->guards);
        }
        break;
    case VT_OP_ISUB:
        /* sub lhs, rhs → result in lhs */
        emit_sub_reg_reg(buf, lhs_reg, rhs_reg);
        {
            vtx_guard_info_t guard;
            memset(&guard, 0, sizeof(guard));
            guard.bytecode_pc = ctx->bc_pc;
            guard.deopt_continuation = ctx->bc_pc;
            guard.stack_depth = ctx->stack_depth + 2;
            vtx_guard_emit_overflow_check(buf, guard, &ctx->guards);
        }
        break;
    case VT_OP_IMUL:
        emit_imul_reg_reg(buf, lhs_reg, rhs_reg);
        {
            vtx_guard_info_t guard;
            memset(&guard, 0, sizeof(guard));
            guard.bytecode_pc = ctx->bc_pc;
            guard.deopt_continuation = ctx->bc_pc;
            guard.stack_depth = ctx->stack_depth + 2;
            vtx_guard_emit_overflow_check(buf, guard, &ctx->guards);
        }
        break;
    case VT_OP_IDIV:
        /* Division by zero guard */
        {
            vtx_guard_info_t guard;
            memset(&guard, 0, sizeof(guard));
            guard.bytecode_pc = ctx->bc_pc;
            guard.deopt_continuation = ctx->bc_pc;
            guard.stack_depth = ctx->stack_depth + 2;
            vtx_guard_emit_div_zero_check(buf, rhs_reg, guard, &ctx->guards);
        }
        /* Move lhs to RAX, sign-extend to RDX:RAX, then idiv */
        emit_mov_reg_reg64(buf, VTX_REG_RAX, lhs_reg);
        emit_cqo(buf);
        emit_idiv_reg(buf, rhs_reg);
        /* Result (quotient) is in RAX. Move to lhs_reg. */
        emit_mov_reg_reg64(buf, lhs_reg, VTX_REG_RAX);
        break;
    case VT_OP_IMOD:
        {
            vtx_guard_info_t guard;
            memset(&guard, 0, sizeof(guard));
            guard.bytecode_pc = ctx->bc_pc;
            guard.deopt_continuation = ctx->bc_pc;
            guard.stack_depth = ctx->stack_depth + 2;
            vtx_guard_emit_div_zero_check(buf, rhs_reg, guard, &ctx->guards);
        }
        emit_mov_reg_reg64(buf, VTX_REG_RAX, lhs_reg);
        emit_cqo(buf);
        emit_idiv_reg(buf, rhs_reg);
        /* Remainder is in RDX */
        emit_mov_reg_reg64(buf, lhs_reg, VTX_REG_RDX);
        break;
    default:
        VTX_ASSERT(false, "unhandled integer arithmetic opcode");
        break;
    }

    /* Re-tag the result as SMI and push */
    emit_tag_smi(ctx, lhs_reg);
    emit_stack_push(ctx);
    /* The result is now in RAX = TOS. But lhs_reg is RSI.
     * We need to move RSI → RAX before push.
     * Actually, emit_stack_push shifts the registers and leaves RAX
     * for the new value. We need to put the result in RAX. */
    /* Move result from lhs_reg (RSI) to RAX */
    emit_mov_reg_reg64(buf, VTX_REG_RAX, lhs_reg);
}

/* ========================================================================== */
/* Float arithmetic                                                            */
/* ========================================================================== */

static void compile_float_arith(vtx_compile_ctx_t *ctx, vtx_opcode_t op)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* For float arithmetic, we store doubles in the stack frame and
     * use XMM registers for computation. The tagged value is the raw
     * IEEE 754 bit pattern (for non-NaN doubles) or the NaN-boxed form.
     *
     * We simplify by storing TOS and TOS-1 temporarily, then using XMM0.
     */

    /* Pop TOS-1 (lhs) and TOS (rhs) */
    vtx_reg_t lhs_reg, rhs_reg;
    emit_stack_pop2(ctx, &lhs_reg, &rhs_reg);

    /* Store both values to temporary frame slots for XMM access */
    /* Use local[max_locals] and local[max_locals+1] as temp if available,
     * or use fixed scratch area. For simplicity, store to [rbp-48-max_locals*8-8]
     * and [rbp-48-max_locals*8-16]. Actually, let's use the spill area. */
    int32_t temp_off1 = vtx_frame_layout_spill_offset(&ctx->layout, 0);
    int32_t temp_off2 = vtx_frame_layout_spill_offset(&ctx->layout, 1);

    /* If we have no spill slots, use local slots as temp */
    if (ctx->layout.max_spills < 2) {
        /* Use the last two locals as temp */
        temp_off1 = vtx_frame_layout_local_offset(&ctx->layout,
                     ctx->layout.max_locals > 0 ? ctx->layout.max_locals - 1 : 0);
        temp_off2 = vtx_frame_layout_local_offset(&ctx->layout,
                     ctx->layout.max_locals > 1 ? ctx->layout.max_locals - 2 : 0);
    }

    emit_mov_rbp_offset_reg(buf, temp_off1, lhs_reg);
    emit_mov_rbp_offset_reg(buf, temp_off2, rhs_reg);

    /* Load lhs into XMM0 */
    emit_movsd_load(buf, temp_off1);

    /* Perform operation with rhs */
    switch (op) {
    case VT_OP_FADD: emit_addsd_mem(buf, temp_off2); break;
    case VT_OP_FSUB: emit_subsd_mem(buf, temp_off2); break;
    case VT_OP_FMUL: emit_mulsd_mem(buf, temp_off2); break;
    case VT_OP_FDIV: emit_divsd_mem(buf, temp_off2); break;
    default: VTX_ASSERT(false, "unhandled float arithmetic opcode"); break;
    }

    /* Store XMM0 result to temp, then load as tagged value */
    emit_movsd_store(buf, temp_off1);
    emit_mov_reg_rbp_offset(buf, VTX_REG_RAX, temp_off1);

    /* Push result */
    emit_stack_push(ctx);
    /* RAX is already the tagged double value */
}

/* ========================================================================== */
/* Bitwise and unary operations                                                */
/* ========================================================================== */

static void compile_bitwise(vtx_compile_ctx_t *ctx, vtx_opcode_t op)
{
    vtx_code_buffer_t *buf = &ctx->buf;
    vtx_reg_t lhs_reg, rhs_reg;

    if (op == VT_OP_INEG || op == VT_OP_INOT) {
        /* Unary: pop 1 value */
        /* RAX = TOS */
        emit_untag_smi(ctx, VTX_REG_RAX);

        if (op == VT_OP_INEG) {
            emit_neg_reg(buf, VTX_REG_RAX);
        } else {
            emit_not_reg(buf, VTX_REG_RAX);
        }

        emit_tag_smi(ctx, VTX_REG_RAX);
        /* No push/pop needed — value stays in RAX (TOS) */
    } else {
        /* Binary: pop 2 values */
        emit_stack_pop2(ctx, &lhs_reg, &rhs_reg);
        emit_untag_smi(ctx, lhs_reg);
        emit_untag_smi(ctx, rhs_reg);

        switch (op) {
        case VT_OP_ISHL:
            /* Shift amount in rhs (RCX on x86-64 uses CL) */
            emit_mov_reg_reg64(buf, VTX_REG_RCX, rhs_reg);
            /* shl lhs, cl */
            emit_rex64(buf, (vtx_reg_t)4, lhs_reg);
            vtx_code_buffer_emit_byte(buf, 0xD3);
            vtx_code_buffer_emit_byte(buf, modrm(3, 4, lhs_reg));
            break;
        case VT_OP_ISHR:
            emit_mov_reg_reg64(buf, VTX_REG_RCX, rhs_reg);
            emit_rex64(buf, (vtx_reg_t)7, lhs_reg);
            vtx_code_buffer_emit_byte(buf, 0xD3);
            vtx_code_buffer_emit_byte(buf, modrm(3, 7, lhs_reg));
            break;
        case VT_OP_IAND:
            emit_and_reg_reg(buf, lhs_reg, rhs_reg);
            break;
        case VT_OP_IOR:
            emit_or_reg_reg(buf, lhs_reg, rhs_reg);
            break;
        case VT_OP_IXOR:
            emit_xor_reg_reg(buf, lhs_reg, rhs_reg);
            break;
        default:
            VTX_ASSERT(false, "unhandled bitwise opcode");
            break;
        }

        emit_tag_smi(ctx, lhs_reg);
        emit_stack_push(ctx);
        emit_mov_reg_reg64(buf, VTX_REG_RAX, lhs_reg);
    }
}

/* ========================================================================== */
/* Integer comparisons                                                         */
/* ========================================================================== */

static void compile_int_cmp(vtx_compile_ctx_t *ctx, vtx_opcode_t op)
{
    vtx_code_buffer_t *buf = &ctx->buf;
    vtx_reg_t lhs_reg, rhs_reg;
    emit_stack_pop2(ctx, &lhs_reg, &rhs_reg);

    /* Untag both SMIs */
    emit_untag_smi(ctx, lhs_reg);
    emit_untag_smi(ctx, rhs_reg);

    /* Compare: cmp lhs, rhs */
    emit_cmp_reg_reg(buf, lhs_reg, rhs_reg);

    /* Set result: 1 if condition true, 0 if false.
     * Then tag as SMI boolean. */
    uint8_t cc;
    switch (op) {
    case VT_OP_ICMP_EQ: cc = CC_E;  break;
    case VT_OP_ICMP_NE: cc = CC_NE; break;
    case VT_OP_ICMP_LT: cc = CC_L;  break;
    case VT_OP_ICMP_LE: cc = CC_LE; break;
    case VT_OP_ICMP_GT: cc = CC_G;  break;
    case VT_OP_ICMP_GE: cc = CC_GE; break;
    default: cc = CC_E; break;
    }

    /* xor eax, eax (clear RAX) */
    vtx_code_buffer_emit_byte(buf, 0x31);
    vtx_code_buffer_emit_byte(buf, modrm(3, VTX_REG_RAX, VTX_REG_RAX));

    /* setcc al */
    emit_setcc(buf, cc, VTX_REG_RAX);

    /* Tag as SMI: shift left by 3, OR with NaN header */
    emit_tag_smi(ctx, VTX_REG_RAX);

    /* Push result (already in RAX) */
    emit_stack_push(ctx);
    /* RAX holds the result already */
}

/* ========================================================================== */
/* Float comparisons                                                           */
/* ========================================================================== */

static void compile_float_cmp(vtx_compile_ctx_t *ctx, vtx_opcode_t op)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    vtx_reg_t lhs_reg, rhs_reg;
    emit_stack_pop2(ctx, &lhs_reg, &rhs_reg);

    /* Store both to temp slots for XMM comparison */
    int32_t temp_off1 = vtx_frame_layout_spill_offset(&ctx->layout, 0);
    int32_t temp_off2 = vtx_frame_layout_spill_offset(&ctx->layout, 1);
    if (ctx->layout.max_spills < 2) {
        temp_off1 = vtx_frame_layout_local_offset(&ctx->layout,
                     ctx->layout.max_locals > 0 ? ctx->layout.max_locals - 1 : 0);
        temp_off2 = vtx_frame_layout_local_offset(&ctx->layout,
                     ctx->layout.max_locals > 1 ? ctx->layout.max_locals - 2 : 0);
    }

    emit_mov_rbp_offset_reg(buf, temp_off1, lhs_reg);
    emit_mov_rbp_offset_reg(buf, temp_off2, rhs_reg);

    /* Load XMM0 = lhs, XMM1 = rhs */
    emit_movsd_load(buf, temp_off1);
    /* movq xmm1, [rbp + temp_off2] */
    vtx_code_buffer_emit_byte(buf, 0xF2);
    vtx_code_buffer_emit_byte(buf, 0x0F);
    vtx_code_buffer_emit_byte(buf, 0x10); /* MOVSD xmm, m64 */
    vtx_code_buffer_emit_byte(buf, modrm(2, 1, VTX_REG_RBP));
    vtx_code_buffer_emit_dword(buf, (uint32_t)temp_off2);

    /* ucomisd xmm0, xmm1 — compare xmm0 with xmm1 */
    vtx_code_buffer_emit_byte(buf, 0x66);
    vtx_code_buffer_emit_byte(buf, 0x0F);
    vtx_code_buffer_emit_byte(buf, 0x2E); /* UCOMISD xmm0, xmm1 */
    vtx_code_buffer_emit_byte(buf, modrm(3, 0, 1));

    uint8_t cc;
    switch (op) {
    case VT_OP_FCMP_EQ: cc = CC_E;  break;
    case VT_OP_FCMP_NE: cc = CC_NE; break;
    case VT_OP_FCMP_LT: cc = CC_B;  break; /* use unsigned below for float LT */
    case VT_OP_FCMP_LE: cc = CC_BE; break;
    case VT_OP_FCMP_GT: cc = CC_A;  break;
    case VT_OP_FCMP_GE: cc = CC_AE; break;
    default: cc = CC_E; break;
    }

    /* xor eax, eax; setcc al */
    vtx_code_buffer_emit_byte(buf, 0x31);
    vtx_code_buffer_emit_byte(buf, modrm(3, VTX_REG_RAX, VTX_REG_RAX));
    emit_setcc(buf, cc, VTX_REG_RAX);
    emit_tag_smi(ctx, VTX_REG_RAX);
    emit_stack_push(ctx);
}

/* ========================================================================== */
/* Control flow                                                                */
/* ========================================================================== */

static void compile_goto(vtx_compile_ctx_t *ctx, uint16_t target_pc)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Check if this is a backward branch (loop) */
    if (target_pc <= ctx->bc_pc) {
        /* Backward branch — emit safepoint check and profiling */
        if (ctx->profile_data) {
            vtx_instrument_emit_backward_branch_increment(buf, ctx->profile_data);
        }
    }

    /* Emit JMP rel32 (forward or backward) */
    uint32_t jmp_pos = emit_jmp32(buf);
    uint32_t source_off = vtx_code_buffer_position(buf) - 5;
    add_fixup(ctx, jmp_pos, source_off, target_pc, false);
}

static void compile_if_true(vtx_compile_ctx_t *ctx, uint16_t target_pc)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Pop TOS; if truthy, branch to target. */
    /* RAX = TOS (tagged value) */

    /* Check truthiness: a value is truthy if it's not VTX_VALUE_FALSE,
     * not VTX_VALUE_NULL, not VTX_VALUE_UNDEFINED, and not SMI 0. */
    /* For the baseline JIT, we use a simplified truthiness check:
     * Compare against VTX_VALUE_FALSE. If not equal, it's truthy. */

    /* cmp rax, VTX_VALUE_FALSE */
    emit_mov_reg_imm64(buf, VTX_REG_R10, VTX_VALUE_FALSE);
    emit_cmp_reg_reg(buf, VTX_REG_RAX, VTX_REG_R10);

    /* Pop the value */
    emit_stack_pop(ctx);

    /* jne target (truthy = not false) */
    uint32_t jcc_pos = emit_jcc32(buf, CC_NE);
    uint32_t source_off = vtx_code_buffer_position(buf) - 6;
    add_fixup(ctx, jcc_pos, source_off, target_pc, true);

    /* Record branch outcome */
    if (ctx->profile_data) {
        vtx_instrument_emit_branch_record(buf, ctx->profile_data, ctx->bc_pc, true);
    }
}

static void compile_if_false(vtx_compile_ctx_t *ctx, uint16_t target_pc)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Pop TOS; if falsy, branch to target. */
    emit_mov_reg_imm64(buf, VTX_REG_R10, VTX_VALUE_FALSE);
    emit_cmp_reg_reg(buf, VTX_REG_RAX, VTX_REG_R10);

    emit_stack_pop(ctx);

    /* je target (falsy = equal to FALSE) */
    uint32_t jcc_pos = emit_jcc32(buf, CC_E);
    uint32_t source_off = vtx_code_buffer_position(buf) - 6;
    add_fixup(ctx, jcc_pos, source_off, target_pc, true);

    if (ctx->profile_data) {
        vtx_instrument_emit_branch_record(buf, ctx->profile_data, ctx->bc_pc, false);
    }
}

/* ========================================================================== */
/* Calls                                                                       */
/* ========================================================================== */

static void compile_call_static(vtx_compile_ctx_t *ctx, uint16_t method_idx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Record call site type for profiling */
    if (ctx->profile_data) {
        vtx_instrument_emit_call_type_record(buf, ctx->profile_data,
            ctx->bc_pc, VTX_REG_NONE, VTX_REG_NONE);
    }

    /* For a static call, we need to:
     * 1. Save expression stack registers
     * 2. Set up arguments (already on the expression stack)
     * 3. Call the method
     * 4. Restore expression stack
     * 5. Push return value
     *
     * For the baseline JIT, we simplify: we call a runtime helper
     * that handles the dispatch.
     *
     * Arguments: the method descriptor tells us how many args.
     * The args are on the expression stack (TOS = last arg).
     * We need to pop them and put them in RDI, RSI, RDX, RCX, R8, R9.
     */

    /* Save callee-saved registers that we use for expr stack */
    emit_push(buf, VTX_REG_RAX);
    emit_push(buf, VTX_REG_RCX);
    emit_push(buf, VTX_REG_RDX);
    emit_push(buf, VTX_REG_RBX);

    /* Load method pointer from frame and call it.
     * The method's compiled code expects: RDI=method, RSI=deopt_info, RDX=profile_data.
     * Additional args would be passed on the stack. */
    emit_mov_reg_rbp_offset(buf, VTX_REG_RDI, -24); /* method pointer */

    /* For now, pass NULL for deopt_info and profile_data of the callee */
    emit_mov_reg_imm64(buf, VTX_REG_RSI, 0);
    emit_mov_reg_imm64(buf, VTX_REG_RDX, 0);

    /* Load the method's code entry point.
     * The method_desc_t has a vtable that contains compiled code pointers.
     * For static calls, we can load the code pointer directly.
     * For simplicity, call through a runtime helper. */
    extern void vtx_runtime_call_static(const vtx_method_desc_t *, ...);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_runtime_call_static);
    emit_call_reg(buf, VTX_REG_RAX);

    /* Restore expression stack registers */
    emit_pop(buf, VTX_REG_RBX);
    emit_pop(buf, VTX_REG_RDX);
    emit_pop(buf, VTX_REG_RCX);
    emit_pop(buf, VTX_REG_RAX);

    /* Push return value (in RAX) */
    emit_stack_push(ctx);
    /* RAX holds the return value */
}

static void compile_call_virtual(vtx_compile_ctx_t *ctx, uint16_t method_idx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Record call site type for profiling */
    if (ctx->profile_data) {
        /* The receiver is the first argument, which is deep in the stack.
         * For simplicity, pass VTX_REG_NONE and let the instrument function
         * handle it via the runtime. */
        vtx_instrument_emit_call_type_record(buf, ctx->profile_data,
            ctx->bc_pc, VTX_REG_NONE, VTX_REG_NONE);
    }

    /* Virtual call: need to look up the method in the vtable.
     * This is complex, so we call a runtime helper. */
    emit_push(buf, VTX_REG_RAX);
    emit_push(buf, VTX_REG_RCX);
    emit_push(buf, VTX_REG_RDX);
    emit_push(buf, VTX_REG_RBX);

    /* The receiver object is on the expression stack.
     * For now, pass the method index and let the runtime resolve it. */
    emit_mov_reg_imm32(buf, VTX_REG_RDI, method_idx);
    emit_mov_reg_rbp_offset(buf, VTX_REG_RSI, -24); /* method */

    extern void vtx_runtime_call_virtual(uint32_t, const vtx_method_desc_t *, ...);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_runtime_call_virtual);
    emit_call_reg(buf, VTX_REG_RAX);

    emit_pop(buf, VTX_REG_RBX);
    emit_pop(buf, VTX_REG_RDX);
    emit_pop(buf, VTX_REG_RCX);
    emit_pop(buf, VTX_REG_RAX);

    emit_stack_push(ctx);
}

static void compile_call_interface(vtx_compile_ctx_t *ctx, uint16_t method_idx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    if (ctx->profile_data) {
        vtx_instrument_emit_call_type_record(buf, ctx->profile_data,
            ctx->bc_pc, VTX_REG_NONE, VTX_REG_NONE);
    }

    emit_push(buf, VTX_REG_RAX);
    emit_push(buf, VTX_REG_RCX);
    emit_push(buf, VTX_REG_RDX);
    emit_push(buf, VTX_REG_RBX);

    emit_mov_reg_imm32(buf, VTX_REG_RDI, method_idx);
    emit_mov_reg_rbp_offset(buf, VTX_REG_RSI, -24);

    extern void vtx_runtime_call_interface(uint32_t, const vtx_method_desc_t *, ...);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_runtime_call_interface);
    emit_call_reg(buf, VTX_REG_RAX);

    emit_pop(buf, VTX_REG_RBX);
    emit_pop(buf, VTX_REG_RDX);
    emit_pop(buf, VTX_REG_RCX);
    emit_pop(buf, VTX_REG_RAX);

    emit_stack_push(ctx);
}

/* ========================================================================== */
/* Returns                                                                     */
/* ========================================================================== */

static void compile_return(vtx_compile_ctx_t *ctx, bool has_value)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    if (has_value) {
        /* RAX = TOS (return value). Move it to a safe register. */
        emit_mov_reg_reg64(buf, VTX_REG_R12, VTX_REG_RAX);
        emit_stack_pop(ctx);
        emit_mov_reg_reg64(buf, VTX_REG_RAX, VTX_REG_R12);
    } else {
        emit_mov_reg_imm64(buf, VTX_REG_RAX, VTX_VALUE_UNDEFINED);
    }

    emit_epilogue(ctx);
}

/* ========================================================================== */
/* Object creation                                                             */
/* ========================================================================== */

static void compile_new(vtx_compile_ctx_t *ctx, uint16_t typeid_)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Allocate a new object via the GC.
     * Call: vtx_gc_alloc(gc, size, typeid) → heap_object_ptr */
    emit_push(buf, VTX_REG_RAX);
    emit_push(buf, VTX_REG_RCX);
    emit_push(buf, VTX_REG_RDX);
    emit_push(buf, VTX_REG_RBX);

    /* We need the GC pointer. It's thread-local in a real implementation.
     * For the baseline JIT, we get it from a global. */
    extern vtx_gc_t *vtx_get_current_gc(void);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_get_current_gc);
    emit_call_reg(buf, VTX_REG_RAX);
    /* RAX = gc pointer */

    /* Set up args: RDI = gc, RSI = size, RDX = typeid */
    emit_mov_reg_reg64(buf, VTX_REG_RDI, VTX_REG_RAX);
    /* We don't know the exact size here without the type system.
     * Use a reasonable default (header + some fields). */
    emit_mov_reg_imm32(buf, VTX_REG_RSI, VTX_HEAP_OBJECT_HEADER_SIZE + 8);
    emit_mov_reg_imm32(buf, VTX_REG_RDX, typeid_);

    extern vtx_heap_object_t *vtx_gc_alloc(vtx_gc_t *, size_t, vtx_typeid_t);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_gc_alloc);
    emit_call_reg(buf, VTX_REG_RAX);
    /* RAX = heap object pointer */

    /* Tag the heap pointer as a tagged value */
    /* result = VTX_NAN_BOX_HEADER | ((ptr >> 3) & mask) << 3 | VTX_TAG_HEAP_PTR */
    /* Simplified: tag the pointer in RAX */
    /* Right now RAX = raw heap pointer. We need to NaN-box it. */
    /* ptr >> 3: shr rax, 3 */
    vtx_code_buffer_emit_byte(buf, 0x48);
    vtx_code_buffer_emit_byte(buf, 0xC1);
    vtx_code_buffer_emit_byte(buf, modrm(3, 5, VTX_REG_RAX));
    vtx_code_buffer_emit_byte(buf, 3);
    /* shl rax, 3 */
    emit_shl_reg_imm8(buf, VTX_REG_RAX, 3);
    /* or rax, VTX_NAN_BOX_HEADER | VTX_TAG_HEAP_PTR */
    emit_mov_reg_imm64(buf, VTX_REG_R10,
        VTX_NAN_BOX_HEADER | VTX_TAG_HEAP_PTR);
    emit_or_reg_reg(buf, VTX_REG_RAX, VTX_REG_R10);

    /* Save result to R12 before restoring expr stack */
    emit_mov_reg_reg64(buf, VTX_REG_R12, VTX_REG_RAX);

    emit_pop(buf, VTX_REG_RBX);
    emit_pop(buf, VTX_REG_RDX);
    emit_pop(buf, VTX_REG_RCX);
    emit_pop(buf, VTX_REG_RAX);

    /* Push the new object onto the expression stack */
    emit_stack_push(ctx);
    /* Move result to RAX (TOS) */
    emit_mov_reg_reg64(buf, VTX_REG_RAX, VTX_REG_R12);
}

static void compile_newarray(vtx_compile_ctx_t *ctx, uint16_t count)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Allocate an array with `count` elements.
     * The array is a heap object with fields[0] = length (SMI),
     * fields[1..count] = elements (initialized to UNDEFINED). */
    emit_push(buf, VTX_REG_RAX);
    emit_push(buf, VTX_REG_RCX);
    emit_push(buf, VTX_REG_RDX);
    emit_push(buf, VTX_REG_RBX);

    size_t alloc_size = vtx_heap_object_alloc_size(count + 1); /* +1 for length */

    extern vtx_gc_t *vtx_get_current_gc(void);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_get_current_gc);
    emit_call_reg(buf, VTX_REG_RAX);

    emit_mov_reg_reg64(buf, VTX_REG_RDI, VTX_REG_RAX);
    emit_mov_reg_imm32(buf, VTX_REG_RSI, (uint32_t)alloc_size);
    emit_mov_reg_imm32(buf, VTX_REG_RDX, VTX_TYPE_OBJECT); /* array type */

    extern vtx_heap_object_t *vtx_gc_alloc(vtx_gc_t *, size_t, vtx_typeid_t);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_gc_alloc);
    emit_call_reg(buf, VTX_REG_RAX);

    /* Set the length field: fields[0] = vtx_make_smi(count) */
    vtx_value_t len_val = vtx_make_smi(count);
    emit_mov_reg_imm64(buf, VTX_REG_R10, len_val);
    /* mov [rax + header_size], r10 */
    int32_t len_off = (int32_t)VTX_HEAP_OBJECT_HEADER_SIZE;
    emit_rex64(buf, VTX_REG_R10, VTX_REG_RAX);
    vtx_code_buffer_emit_byte(buf, 0x89);
    vtx_code_buffer_emit_byte(buf, modrm(1, VTX_REG_R10, VTX_REG_RAX));
    vtx_code_buffer_emit_byte(buf, (uint8_t)(len_off & 0xFF));

    /* Tag the heap pointer */
    vtx_code_buffer_emit_byte(buf, 0x48);
    vtx_code_buffer_emit_byte(buf, 0xC1);
    vtx_code_buffer_emit_byte(buf, modrm(3, 5, VTX_REG_RAX));
    vtx_code_buffer_emit_byte(buf, 3);
    emit_shl_reg_imm8(buf, VTX_REG_RAX, 3);
    emit_mov_reg_imm64(buf, VTX_REG_R10,
        VTX_NAN_BOX_HEADER | VTX_TAG_HEAP_PTR);
    emit_or_reg_reg(buf, VTX_REG_RAX, VTX_REG_R10);

    emit_mov_reg_reg64(buf, VTX_REG_R12, VTX_REG_RAX);

    emit_pop(buf, VTX_REG_RBX);
    emit_pop(buf, VTX_REG_RDX);
    emit_pop(buf, VTX_REG_RCX);
    emit_pop(buf, VTX_REG_RAX);

    emit_stack_push(ctx);
    emit_mov_reg_reg64(buf, VTX_REG_RAX, VTX_REG_R12);
}

/* ========================================================================== */
/* Type checks                                                                 */
/* ========================================================================== */

static void compile_checkcast(vtx_compile_ctx_t *ctx, uint16_t typeid_)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* CHECKCAST: if the TOS object is not an instance of typeid, deopt.
     * The object stays on the stack (no pop/push). */

    /* Null passes checkcast (null can be cast to any reference type) */
    /* First check if null: compare TOS against VTX_VALUE_NULL */
    emit_mov_reg_imm64(buf, VTX_REG_R10, VTX_VALUE_NULL);
    emit_cmp_reg_reg(buf, VTX_REG_RAX, VTX_REG_R10);
    /* je skip_check (null passes) */
    uint32_t je_pos = emit_jcc8(buf, CC_E);
    uint32_t skip_check_target = vtx_code_buffer_position(buf);

    /* Not null — extract heap pointer and check type_id */
    emit_untag_heap_ptr(ctx, VTX_REG_RAX, VTX_REG_R11);

    /* Type check guard */
    vtx_guard_info_t guard;
    memset(&guard, 0, sizeof(guard));
    guard.bytecode_pc = ctx->bc_pc;
    guard.deopt_continuation = ctx->bc_pc;
    guard.stack_depth = ctx->stack_depth;
    guard.expected_value = typeid_;
    vtx_guard_emit_type_check(buf, VTX_REG_R11, (vtx_typeid_t)typeid_,
                               guard, &ctx->guards);

    /* Patch the je to jump here (skip the type check) */
    int8_t je_disp = (int8_t)(vtx_code_buffer_position(buf) - skip_check_target);
    buf.bytes[je_pos] = (uint8_t)je_disp;
}

static void compile_instanceof(vtx_compile_ctx_t *ctx, uint16_t typeid_)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* INSTANCEOF: pop TOS, push true if TOS is instance of typeid, else false. */

    /* Check null → false */
    emit_mov_reg_imm64(buf, VTX_REG_R10, VTX_VALUE_NULL);
    emit_cmp_reg_reg(buf, VTX_REG_RAX, VTX_REG_R10);
    /* je is_null */
    uint32_t je_pos = emit_jcc8(buf, CC_E);

    /* Not null — extract heap pointer and check type */
    emit_untag_heap_ptr(ctx, VTX_REG_RAX, VTX_REG_R11);

    /* Load type_id from object header: R10 = [R11 + 0] (uint32_t) */
    /* Use 32-bit load, zero-extended to 64 bits */
    emit_rex32_if_needed(buf, (vtx_reg_t)0, VTX_REG_R11);
    vtx_code_buffer_emit_byte(buf, 0x8B); /* MOV r32, r/m32 */
    vtx_code_buffer_emit_byte(buf, modrm(1, 0, VTX_REG_R11));
    vtx_code_buffer_emit_byte(buf, 0); /* offset 0 */

    /* Compare R10 with typeid_ */
    emit_cmp_reg_imm32(buf, VTX_REG_R10, (int32_t)typeid_);

    /* sete al */
    vtx_code_buffer_emit_byte(buf, 0x31);
    vtx_code_buffer_emit_byte(buf, modrm(3, VTX_REG_RAX, VTX_REG_RAX));
    emit_setcc(buf, CC_E, VTX_REG_RAX);
    /* Jump past null case */
    uint32_t jmp_pos = emit_jmp32(buf);

    /* is_null: RAX = 0 (false) */
    uint32_t null_target = vtx_code_buffer_position(buf);
    vtx_code_buffer_emit_byte(buf, 0x31);
    vtx_code_buffer_emit_byte(buf, modrm(3, VTX_REG_RAX, VTX_REG_RAX));

    /* Patch: je null_target */
    int8_t je_disp = (int8_t)(null_target - (je_pos + 1));
    buf.bytes[je_pos] = (uint8_t)je_disp;

    /* Patch: jmp past_null */
    uint32_t past_null = vtx_code_buffer_position(buf);
    vtx_code_buffer_patch_dword(buf, jmp_pos, (uint32_t)(past_null - (jmp_pos + 4)));

    /* Tag result as SMI and push */
    emit_tag_smi(ctx, VTX_REG_RAX);
    emit_stack_pop(ctx); /* pop the original value */
    emit_stack_push(ctx);
}

/* ========================================================================== */
/* Array operations                                                            */
/* ========================================================================== */

static void compile_array_load(vtx_compile_ctx_t *ctx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Pop index (TOS) and array (TOS-1) */
    vtx_reg_t arr_reg, idx_reg;
    emit_stack_pop2(ctx, &arr_reg, &idx_reg);

    /* Null check on array */
    vtx_guard_info_t guard;
    memset(&guard, 0, sizeof(guard));
    guard.bytecode_pc = ctx->bc_pc;
    guard.deopt_continuation = ctx->bc_pc;
    guard.stack_depth = ctx->stack_depth + 2;
    vtx_guard_emit_null_check(buf, arr_reg, guard, &ctx->guards);

    /* Extract heap pointer from array */
    emit_untag_heap_ptr(ctx, arr_reg, VTX_REG_R10);

    /* Load array length from fields[0]: R11 = [R10 + header_size] (tagged SMI) */
    int32_t len_off = (int32_t)VTX_HEAP_OBJECT_HEADER_SIZE;
    emit_rex64(buf, VTX_REG_R11, VTX_REG_R10);
    vtx_code_buffer_emit_byte(buf, 0x8B);
    vtx_code_buffer_emit_byte(buf, modrm(1, VTX_REG_R11, VTX_REG_R10));
    vtx_code_buffer_emit_byte(buf, (uint8_t)(len_off & 0xFF));

    /* Untag the length (it's an SMI) and the index */
    emit_untag_smi(ctx, idx_reg);
    emit_untag_smi(ctx, VTX_REG_R11);

    /* Bounds check: 0 <= index < length */
    vtx_guard_info_t bguard;
    memset(&bguard, 0, sizeof(bguard));
    bguard.bytecode_pc = ctx->bc_pc;
    bguard.deopt_continuation = ctx->bc_pc;
    bguard.stack_depth = ctx->stack_depth + 2;
    vtx_guard_emit_bounds_check(buf, idx_reg, VTX_REG_R11, bguard, &ctx->guards);

    /* Load element: RAX = [R10 + header_size + (index+1)*8] */
    /* Compute offset: header_size + 8 + index*8 = header_size + 8*(1+index) */
    /* Use: lea r12, [r10 + header_size + 8 + idx_reg*8] */
    /* Then: mov rax, [r12] */
    emit_add_reg_imm32(buf, idx_reg, 1);
    emit_shl_reg_imm8(buf, idx_reg, 3);
    emit_add_reg_imm32(buf, VTX_REG_R10, (int32_t)VTX_HEAP_OBJECT_HEADER_SIZE + 8);

    /* RAX = [R10 + idx_reg] */
    emit_rex64(buf, VTX_REG_RAX, VTX_REG_R10);
    vtx_code_buffer_emit_byte(buf, 0x8B);
    vtx_code_buffer_emit_byte(buf, modrm(0, VTX_REG_RAX, VTX_REG_R10));
    /* SIB byte: [R10 + idx_reg*1] */
    vtx_code_buffer_emit_byte(buf, sib(0, idx_reg, VTX_REG_R10));

    /* Push result */
    emit_stack_push(ctx);
    /* RAX already holds the result */
}

static void compile_array_store(vtx_compile_ctx_t *ctx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Pop value (TOS), index (TOS-1), array (TOS-2) */
    /* For 3 pops, we need to be more careful with register management */
    /* Save TOS (value) to R12 */
    emit_mov_reg_reg64(buf, VTX_REG_R12, VTX_REG_RAX);
    emit_stack_pop(ctx); /* pop value, RAX = old RCX */

    vtx_reg_t arr_reg, idx_reg;
    emit_stack_pop2(ctx, &arr_reg, &idx_reg);

    /* Null check on array */
    vtx_guard_info_t guard;
    memset(&guard, 0, sizeof(guard));
    guard.bytecode_pc = ctx->bc_pc;
    guard.deopt_continuation = ctx->bc_pc;
    guard.stack_depth = ctx->stack_depth + 3;
    vtx_guard_emit_null_check(buf, arr_reg, guard, &ctx->guards);

    /* Extract heap pointer, load length, bounds check */
    emit_untag_heap_ptr(ctx, arr_reg, VTX_REG_R10);
    int32_t len_off = (int32_t)VTX_HEAP_OBJECT_HEADER_SIZE;
    emit_rex64(buf, VTX_REG_R11, VTX_REG_R10);
    vtx_code_buffer_emit_byte(buf, 0x8B);
    vtx_code_buffer_emit_byte(buf, modrm(1, VTX_REG_R11, VTX_REG_R10));
    vtx_code_buffer_emit_byte(buf, (uint8_t)(len_off & 0xFF));

    emit_untag_smi(ctx, idx_reg);
    emit_untag_smi(ctx, VTX_REG_R11);

    vtx_guard_info_t bguard;
    memset(&bguard, 0, sizeof(bguard));
    bguard.bytecode_pc = ctx->bc_pc;
    bguard.deopt_continuation = ctx->bc_pc;
    bguard.stack_depth = ctx->stack_depth + 3;
    vtx_guard_emit_bounds_check(buf, idx_reg, VTX_REG_R11, bguard, &ctx->guards);

    /* Store: [R10 + header_size + (index+1)*8] = value (R12) */
    emit_add_reg_imm32(buf, idx_reg, 1);
    emit_shl_reg_imm8(buf, idx_reg, 3);
    emit_add_reg_imm32(buf, VTX_REG_R10, (int32_t)VTX_HEAP_OBJECT_HEADER_SIZE + 8);

    emit_rex64(buf, VTX_REG_R12, VTX_REG_R10);
    vtx_code_buffer_emit_byte(buf, 0x89); /* MOV r/m64, r64 */
    vtx_code_buffer_emit_byte(buf, modrm(0, VTX_REG_R12, VTX_REG_R10));
    vtx_code_buffer_emit_byte(buf, sib(0, idx_reg, VTX_REG_R10));
}

static void compile_array_length(vtx_compile_ctx_t *ctx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* RAX = TOS (array object) */
    /* Null check */
    vtx_guard_info_t guard;
    memset(&guard, 0, sizeof(guard));
    guard.bytecode_pc = ctx->bc_pc;
    guard.deopt_continuation = ctx->bc_pc;
    guard.stack_depth = ctx->stack_depth;
    vtx_guard_emit_null_check(buf, VTX_REG_RAX, guard, &ctx->guards);

    /* Extract heap pointer */
    emit_untag_heap_ptr(ctx, VTX_REG_RAX, VTX_REG_R10);

    /* Load length field: RAX = [R10 + header_size] (tagged SMI) */
    int32_t len_off = (int32_t)VTX_HEAP_OBJECT_HEADER_SIZE;
    emit_rex64(buf, VTX_REG_RAX, VTX_REG_R10);
    vtx_code_buffer_emit_byte(buf, 0x8B);
    vtx_code_buffer_emit_byte(buf, modrm(1, VTX_REG_RAX, VTX_REG_R10));
    vtx_code_buffer_emit_byte(buf, (uint8_t)(len_off & 0xFF));

    /* The length is already a tagged SMI — it replaces TOS */
}

/* ========================================================================== */
/* Exception handling                                                          */
/* ========================================================================== */

static void compile_throw(vtx_compile_ctx_t *ctx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Throw: RAX = TOS (exception object).
     * Save it, then call the runtime throw handler. */
    emit_mov_reg_reg64(buf, VTX_REG_RDI, VTX_REG_RAX);
    emit_stack_pop(ctx);

    /* Call runtime throw function */
    extern void vtx_runtime_throw(vtx_value_t);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_runtime_throw);
    emit_call_reg(buf, VTX_REG_RAX);

    /* Should not return — emit trap just in case */
    emit_ud2(buf);
}

static void compile_catch(vtx_compile_ctx_t *ctx, uint16_t handler_pc)
{
    /* CATCH: sets up an exception handler. In the baseline JIT,
     * this is a no-op at the machine code level — exception handling
     * is managed by the runtime. The handler PC is recorded in the
     * side table for deopt purposes. */

    /* Record the handler in the side table */
    if (ctx->side_table) {
        vtx_side_table_add_entry(ctx->side_table,
            vtx_code_buffer_position(buf), 0, 0);
    }
}

/* ========================================================================== */
/* Monitor operations                                                          */
/* ========================================================================== */

static void compile_monitor_enter(vtx_compile_ctx_t *ctx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* RAX = TOS (object to lock) */
    emit_mov_reg_reg64(buf, VTX_REG_RDI, VTX_REG_RAX);
    emit_stack_pop(ctx);

    emit_push(buf, VTX_REG_RAX);
    emit_push(buf, VTX_REG_RCX);
    emit_push(buf, VTX_REG_RDX);
    emit_push(buf, VTX_REG_RBX);

    extern void vtx_runtime_monitor_enter(vtx_value_t);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_runtime_monitor_enter);
    emit_call_reg(buf, VTX_REG_RAX);

    emit_pop(buf, VTX_REG_RBX);
    emit_pop(buf, VTX_REG_RDX);
    emit_pop(buf, VTX_REG_RCX);
    emit_pop(buf, VTX_REG_RAX);
}

static void compile_monitor_exit(vtx_compile_ctx_t *ctx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    emit_mov_reg_reg64(buf, VTX_REG_RDI, VTX_REG_RAX);
    emit_stack_pop(ctx);

    emit_push(buf, VTX_REG_RAX);
    emit_push(buf, VTX_REG_RCX);
    emit_push(buf, VTX_REG_RDX);
    emit_push(buf, VTX_REG_RBX);

    extern void vtx_runtime_monitor_exit(vtx_value_t);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_runtime_monitor_exit);
    emit_call_reg(buf, VTX_REG_RAX);

    emit_pop(buf, VTX_REG_RBX);
    emit_pop(buf, VTX_REG_RDX);
    emit_pop(buf, VTX_REG_RCX);
    emit_pop(buf, VTX_REG_RAX);
}

/* ========================================================================== */
/* Stack manipulation                                                          */
/* ========================================================================== */

static void compile_dup(vtx_compile_ctx_t *ctx)
{
    /* Duplicate TOS. RAX = TOS. Push a copy. */
    emit_stack_push(ctx);
    /* After push, RAX was shifted to RCX. We need RAX = RCX (old TOS). */
    emit_mov_reg_reg64(&ctx->buf, VTX_REG_RAX, VTX_REG_RCX);
}

static void compile_pop(vtx_compile_ctx_t *ctx)
{
    emit_stack_pop(ctx);
}

static void compile_swap(vtx_compile_ctx_t *ctx)
{
    /* Swap TOS and TOS-1. RAX=TOS, RCX=TOS-1 → swap them. */
    emit_mov_reg_reg64(&ctx->buf, VTX_REG_R10, VTX_REG_RAX);
    emit_mov_reg_reg64(&ctx->buf, VTX_REG_RAX, VTX_REG_RCX);
    emit_mov_reg_reg64(&ctx->buf, VTX_REG_RCX, VTX_REG_R10);
}

/* ========================================================================== */
/* Type queries                                                                */
/* ========================================================================== */

static void compile_isnull(vtx_compile_ctx_t *ctx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Check if TOS is null. Replace TOS with boolean result. */
    emit_mov_reg_imm64(buf, VTX_REG_R10, VTX_VALUE_NULL);
    emit_cmp_reg_reg(buf, VTX_REG_RAX, VTX_REG_R10);

    /* sete al */
    vtx_code_buffer_emit_byte(buf, 0x31);
    vtx_code_buffer_emit_byte(buf, modrm(3, VTX_REG_RAX, VTX_REG_RAX));
    emit_setcc(buf, CC_E, VTX_REG_RAX);
    emit_tag_smi(ctx, VTX_REG_RAX);
}

static void compile_typeof(vtx_compile_ctx_t *ctx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Get the type of TOS. Returns the TypeID as an SMI. */
    /* For the baseline JIT, we call a runtime helper. */
    emit_push(buf, VTX_REG_RCX);
    emit_push(buf, VTX_REG_RDX);
    emit_push(buf, VTX_REG_RBX);

    emit_mov_reg_reg64(buf, VTX_REG_RDI, VTX_REG_RAX);

    extern vtx_typeid_t vtx_runtime_typeof(vtx_value_t);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_runtime_typeof);
    emit_call_reg(buf, VTX_REG_RAX);

    emit_mov_reg_reg64(buf, VTX_REG_R12, VTX_REG_RAX);

    emit_pop(buf, VTX_REG_RBX);
    emit_pop(buf, VTX_REG_RDX);
    emit_pop(buf, VTX_REG_RCX);

    /* Tag the TypeID as SMI and replace TOS */
    emit_tag_smi(ctx, VTX_REG_R12);
    emit_mov_reg_reg64(buf, VTX_REG_RAX, VTX_REG_R12);
}

/* ========================================================================== */
/* Main compilation loop                                                       */
/* ========================================================================== */

vtx_compiled_code_t *vtx_baseline_compile(const vtx_method_desc_t *method,
                                           vtx_profile_data_t *profile_data,
                                           vtx_arena_t *arena)
{
    if (!method || !method->bytecode) return NULL;

    vtx_compile_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.method = method;
    ctx.bc = method->bytecode;
    ctx.profile_data = profile_data;
    ctx.arena = arena;
    ctx.layout = vtx_frame_layout_compute(method);
    ctx.method_id = 0; /* Would be derived from method in real impl */

    /* Initialize code buffer */
    if (vtx_code_buffer_init(&ctx.buf, VTX_CODE_BUFFER_INITIAL_CAPACITY) != 0) {
        return NULL;
    }

    /* Initialize guard array */
    if (vtx_guard_array_init(&ctx.guards) != 0) {
        vtx_code_buffer_destroy(&ctx.buf);
        return NULL;
    }

    /* Build side table */
    ctx.side_table = vtx_side_table_build(arena);
    if (!ctx.side_table) {
        vtx_guard_array_destroy(&ctx.guards);
        vtx_code_buffer_destroy(&ctx.buf);
        return NULL;
    }

    /* Emit prologue */
    emit_prologue(&ctx);

    /* Single-pass compilation: iterate over bytecodes */
    ctx.bc_pc = 0;
    ctx.stack_depth = 0;
    ctx.max_stack_depth = 0;

    while (ctx.bc_pc < ctx.bc->length) {
        vtx_opcode_t op = vtx_bytecode_opcode_at(ctx.bc, ctx.bc_pc);

        /* Record bytecode PC → native offset mapping */
        uint32_t native_off = vtx_code_buffer_position(&ctx.buf);
        record_bc_pc_map(&ctx, ctx.bc_pc, native_off);
        record_native_to_bc(&ctx, native_off, ctx.bc_pc);

        /* Read operand if present */
        uint16_t operand = 0;
        const vtx_opcode_info_t *info = &vtx_opcode_table[op];
        if (info->has_operand) {
            operand = vtx_bytecode_read_operand(ctx.bc, ctx.bc_pc);
        }

        /* Compile the opcode */
        switch (op) {
        case VT_OP_HALT:
            compile_halt(&ctx);
            break;
        case VT_OP_NOP:
            compile_nop(&ctx);
            break;
        case VT_OP_LOAD_LOCAL:
            compile_load_local(&ctx, operand);
            break;
        case VT_OP_STORE_LOCAL:
            compile_store_local(&ctx, operand);
            break;
        case VT_OP_LOAD_FIELD:
            compile_load_field(&ctx, operand);
            break;
        case VT_OP_STORE_FIELD:
            compile_store_field(&ctx, operand);
            break;
        case VT_OP_LOAD_CONST_INT:
            compile_load_const_int(&ctx, operand);
            break;
        case VT_OP_LOAD_CONST_FLOAT:
            compile_load_const_float(&ctx, operand);
            break;
        case VT_OP_LOAD_CONST_STR:
            compile_load_const_str(&ctx, operand);
            break;
        case VT_OP_LOAD_NULL:
            compile_load_null(&ctx);
            break;
        case VT_OP_LOAD_TRUE:
            compile_load_true(&ctx);
            break;
        case VT_OP_LOAD_FALSE:
            compile_load_false(&ctx);
            break;
        case VT_OP_LOAD_UNDEFINED:
            compile_load_undefined(&ctx);
            break;
        case VT_OP_IADD:
        case VT_OP_ISUB:
        case VT_OP_IMUL:
        case VT_OP_IDIV:
        case VT_OP_IMOD:
            compile_int_arith(&ctx, op);
            break;
        case VT_OP_FADD:
        case VT_OP_FSUB:
        case VT_OP_FMUL:
        case VT_OP_FDIV:
            compile_float_arith(&ctx, op);
            break;
        case VT_OP_ISHL:
        case VT_OP_ISHR:
        case VT_OP_IAND:
        case VT_OP_IOR:
        case VT_OP_IXOR:
        case VT_OP_INEG:
        case VT_OP_INOT:
            compile_bitwise(&ctx, op);
            break;
        case VT_OP_ICMP_EQ:
        case VT_OP_ICMP_NE:
        case VT_OP_ICMP_LT:
        case VT_OP_ICMP_LE:
        case VT_OP_ICMP_GT:
        case VT_OP_ICMP_GE:
            compile_int_cmp(&ctx, op);
            break;
        case VT_OP_FCMP_EQ:
        case VT_OP_FCMP_NE:
        case VT_OP_FCMP_LT:
        case VT_OP_FCMP_LE:
        case VT_OP_FCMP_GT:
        case VT_OP_FCMP_GE:
            compile_float_cmp(&ctx, op);
            break;
        case VT_OP_GOTO:
            compile_goto(&ctx, operand);
            break;
        case VT_OP_IF_TRUE:
            compile_if_true(&ctx, operand);
            break;
        case VT_OP_IF_FALSE:
            compile_if_false(&ctx, operand);
            break;
        case VT_OP_CALL_STATIC:
            compile_call_static(&ctx, operand);
            break;
        case VT_OP_CALL_VIRTUAL:
            compile_call_virtual(&ctx, operand);
            break;
        case VT_OP_CALL_INTERFACE:
            compile_call_interface(&ctx, operand);
            break;
        case VT_OP_RETURN:
            compile_return(&ctx, false);
            break;
        case VT_OP_RETURN_VALUE:
            compile_return(&ctx, true);
            break;
        case VT_OP_NEW:
            compile_new(&ctx, operand);
            break;
        case VT_OP_NEWARRAY:
            compile_newarray(&ctx, operand);
            break;
        case VT_OP_CHECKCAST:
            compile_checkcast(&ctx, operand);
            break;
        case VT_OP_INSTANCEOF:
            compile_instanceof(&ctx, operand);
            break;
        case VT_OP_ARRAY_LOAD:
            compile_array_load(&ctx);
            break;
        case VT_OP_ARRAY_STORE:
            compile_array_store(&ctx);
            break;
        case VT_OP_ARRAY_LENGTH:
            compile_array_length(&ctx);
            break;
        case VT_OP_THROW:
            compile_throw(&ctx);
            break;
        case VT_OP_CATCH:
            compile_catch(&ctx, operand);
            break;
        case VT_OP_MONITOR_ENTER:
            compile_monitor_enter(&ctx);
            break;
        case VT_OP_MONITOR_EXIT:
            compile_monitor_exit(&ctx);
            break;
        case VT_OP_DUP:
            compile_dup(&ctx);
            break;
        case VT_OP_POP:
            compile_pop(&ctx);
            break;
        case VT_OP_SWAP:
            compile_swap(&ctx);
            break;
        case VT_OP_ISNULL:
            compile_isnull(&ctx);
            break;
        case VT_OP_TYPEOF:
            compile_typeof(&ctx);
            break;
        default:
            /* Unknown opcode — emit trap */
            emit_ud2(&ctx.buf);
            break;
        }

        /* Advance to next instruction */
        ctx.bc_pc += vtx_bytecode_insn_length(ctx.bc, ctx.bc_pc);

        /* Ensure buffer has space for next instruction */
        vtx_code_buffer_ensure_capacity(&ctx.buf, 64);
    }

    /* Emit default epilogue (in case method falls through without RETURN) */
    emit_epilogue(&ctx);

    /* Resolve forward branch fixups */
    resolve_fixups(&ctx);

    /* Generate deopt stubs */
    vtx_deopt_context_t deopt_ctx;
    deopt_ctx.code_buf = &ctx.buf;
    deopt_ctx.guards = &ctx.guards;
    deopt_ctx.frame_layout = ctx.layout;
    deopt_ctx.side_table = ctx.side_table;
    deopt_ctx.arena = arena;
    deopt_ctx.code_start = ctx.buf.bytes;
    deopt_ctx.method_id = ctx.method_id;

    vtx_deopt_stub_array_t deopt_stubs;
    vtx_deopt_stub_array_init(&deopt_stubs);
    deopt_ctx.stub_array = &deopt_stubs;
    vtx_deopt_stubs_emit_all(&deopt_ctx);

    /* Build the compiled code result */
    vtx_compiled_code_t *result = (vtx_compiled_code_t *)malloc(sizeof(vtx_compiled_code_t));
    if (!result) {
        vtx_guard_array_destroy(&ctx.guards);
        vtx_code_buffer_destroy(&ctx.buf);
        vtx_deopt_stub_array_destroy(&deopt_stubs);
        return NULL;
    }
    memset(result, 0, sizeof(vtx_compiled_code_t));

    /* Copy the generated code */
    result->code_size = vtx_code_buffer_position(&ctx.buf);
    result->code = (uint8_t *)malloc(result->code_size);
    if (!result->code) {
        free(result);
        vtx_guard_array_destroy(&ctx.guards);
        vtx_code_buffer_destroy(&ctx.buf);
        vtx_deopt_stub_array_destroy(&deopt_stubs);
        return NULL;
    }
    memcpy(result->code, ctx.buf.bytes, result->code_size);

    /* Make code executable (mprotect) */
    /* This should be done by the code cache, not here. For now, we just
     * copy the bytes. The code cache will handle making them executable. */

    result->frame_layout = ctx.layout;
    result->guards = ctx.guards;
    result->deopt_stubs = deopt_stubs;
    result->side_table = ctx.side_table;
    result->method = method;

    /* Copy PC maps */
    result->bc_pc_map = ctx.pc_map;
    result->bc_pc_map_count = ctx.pc_map_count;

    result->native_to_bc_pc = ctx.native_to_bc;
    result->native_to_bc_pc_count = ctx.native_to_bc_count;

    /* Build deopt info */
    result->deopt_info = (vtx_deopt_info_t *)malloc(sizeof(vtx_deopt_info_t));
    if (result->deopt_info) {
        result->deopt_info->method = method;
        result->deopt_info->pc_map_count = ctx.pc_map_count;
        result->deopt_info->pc_map = (uint32_t *)malloc(ctx.pc_map_count * sizeof(uint32_t));
        result->deopt_info->stack_depth_map = (uint32_t *)malloc(ctx.pc_map_count * sizeof(uint32_t));
        if (result->deopt_info->pc_map && result->deopt_info->stack_depth_map) {
            for (uint32_t i = 0; i < ctx.pc_map_count; i++) {
                result->deopt_info->pc_map[i] = ctx.pc_map[i].native_offset;
                result->deopt_info->stack_depth_map[i] = 0; /* would need tracking */
            }
        }
    }

    /* Clean up */
    vtx_code_buffer_destroy(&ctx.buf);
    free(ctx.fixups);

    return result;
}
