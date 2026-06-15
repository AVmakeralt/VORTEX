#include "baseline/codegen.h"
#include "codecache/install.h"
#include "runtime/helpers.h"
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
    if (new_cap == 0) new_cap = 256;
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
        if (code->deopt_info->native_offsets) free(code->deopt_info->native_offsets);
        if (code->deopt_info->pc_map) free(code->deopt_info->pc_map);
        if (code->deopt_info->stack_depth_map) free(code->deopt_info->stack_depth_map);
        free(code->deopt_info);
    }
    /* Free polymorphic inline caches */
    if (code->poly_ics) {
        for (uint32_t i = 0; i < code->poly_ic_count; i++) {
            free(code->poly_ics[i]);
        }
        free(code->poly_ics);
        code->poly_ics = NULL;
        code->poly_ic_count = 0;
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

static void emit_test_reg_reg2(vtx_code_buffer_t *buf, vtx_reg_t reg1, vtx_reg_t reg2)
{
    emit_rex64(buf, reg1, reg2);
    vtx_code_buffer_emit_byte(buf, 0x85);
    vtx_code_buffer_emit_byte(buf, modrm(3, reg1, reg2));
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
#define CC_BE 0x6  /* below or equal (CF=1 || ZF=1) */
#define CC_A  0x7  /* above (CF=0 && ZF=0) */
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

    /* Code cache and method registry (optional, for proper code installation) */
    vtx_code_cache_t      *cache;
    vtx_method_registry_t *registry;

    /* Method ID for deopt */
    uint32_t method_id;

    /* Polymorphic inline caches allocated during compilation.
     * Tracked so they can be freed when the method is evicted. */
    vtx_poly_ic_t **poly_ics;
    uint32_t        poly_ic_count;
    uint32_t        poly_ic_capacity;
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
    ctx->pc_map[ctx->pc_map_count].stack_depth = ctx->stack_depth;
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
            /* JMP rel32: also has a 4-byte displacement */
            vtx_code_buffer_patch_dword(&ctx->buf, fixup->patch_position,
                                         (uint32_t)disp);
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

    uint32_t old_depth = ctx->stack_depth;
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

    if (new_depth >= 3 && old_depth > VTX_EXPR_REG_COUNT) {
        /* Load a value from spill into RBX.
         * The deepest value that was in spill before the pop is at
         * spill index (old_depth - VTX_EXPR_REG_COUNT - 1). */
        uint32_t spill_idx = old_depth - VTX_EXPR_REG_COUNT - 1;
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

    uint32_t old_depth = ctx->stack_depth;

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
     * RBX = spill or empty
     *
     * Spill index formula: after popping 2, we need to load values that
     * were in spill before the pop. The Nth spill load (0-indexed) uses
     * spill_idx = old_depth - VTX_EXPR_REG_COUNT - 1 - N.
     * This is because before the pop, the deepest register (RBX) held the
     * value that was at stack position (old_depth - 4) from the bottom,
     * and spill slots held positions 0 through (old_depth - 5). */

    if (new_depth >= 2) {
        /* RAX ← RDX, RCX ← RBX, then load spills into RDX and RBX */
        emit_mov_reg_reg64(&ctx->buf, VTX_REG_RAX, VTX_REG_RDX);
        emit_mov_reg_reg64(&ctx->buf, VTX_REG_RCX, VTX_REG_RBX);
        if (new_depth >= 3 && old_depth > VTX_EXPR_REG_COUNT) {
            /* Load into RDX from spill: deepest value that was below RBX */
            uint32_t spill_idx = old_depth - VTX_EXPR_REG_COUNT - 1;
            int32_t spill_off = vtx_frame_layout_spill_offset(&ctx->layout, spill_idx);
            emit_mov_reg_rbp_offset(&ctx->buf, VTX_REG_RDX, spill_off);
        }
        if (new_depth >= 4 && old_depth > VTX_EXPR_REG_COUNT + 1) {
            /* Load into RBX from spill: next deeper value */
            uint32_t spill_idx = old_depth - VTX_EXPR_REG_COUNT - 2;
            int32_t spill_off = vtx_frame_layout_spill_offset(&ctx->layout, spill_idx);
            emit_mov_reg_rbp_offset(&ctx->buf, VTX_REG_RBX, spill_off);
        }
    } else if (new_depth == 1) {
        /* RAX ← RDX */
        emit_mov_reg_reg64(&ctx->buf, VTX_REG_RAX, VTX_REG_RDX);
        /* RCX ← RBX or load from spill */
        if (old_depth > VTX_EXPR_REG_COUNT) {
            /* Need to load from spill into RCX */
            uint32_t spill_idx = old_depth - VTX_EXPR_REG_COUNT - 1;
            int32_t spill_off = vtx_frame_layout_spill_offset(&ctx->layout, spill_idx);
            emit_mov_reg_rbp_offset(&ctx->buf, VTX_REG_RCX, spill_off);
        } else {
            /* RCX ← RBX */
            emit_mov_reg_reg64(&ctx->buf, VTX_REG_RCX, VTX_REG_RBX);
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
    vtx_code_buffer_t *buf = &ctx->buf;

    /* SMI decoding: raw = (val >> 3) & VTX_NAN_DATA_MASK, then sign-extend.
     *
     * For positive SMI values, the result after SAR+AND is correct.
     * For negative SMI values, the AND mask clears bits [63:48] which
     * contained the sign extension. We need to restore it.
     *
     * Approach: branchless sign extension using arithmetic shift.
     * After AND with VTX_NAN_DATA_MASK (48-bit mask), bit 47 is the sign bit.
     * We can sign-extend by:
     *   1. sar val_reg, 3          -- shift out tag bits
     *   2. shl val_reg, 16         -- shift sign bit to bit 63
     *   3. sar val_reg, 16         -- arithmetic shift back, propagating sign
     *
     * This works because:
     *   - For positive values: bit 47 = 0, so bits [63:48] become 0 after sar
     *   - For negative values: bit 47 = 1, so bits [63:48] become all 1s after sar
     */

    /* Step 1: sar val_reg, 3 — arithmetic shift right by VTX_NAN_DATA_SHIFT */
    emit_sar_reg_imm8(buf, val_reg, VTX_NAN_DATA_SHIFT);

    /* Step 2: and val_reg, VTX_NAN_DATA_MASK — mask out NaN-box header bits */
    emit_mov_reg_imm64(buf, VTX_REG_R10, VTX_NAN_DATA_MASK);
    emit_and_reg_reg(buf, val_reg, VTX_REG_R10);

    /* Step 3: Sign-extend from 48 bits using shift trick:
     *   shl val_reg, 16  — moves bit 47 to bit 63
     *   sar val_reg, 16  — arithmetic shift back, propagating sign from bit 63 */
    emit_shl_reg_imm8(buf, val_reg, 16);
    emit_sar_reg_imm8(buf, val_reg, 16);
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
    emit_rex64(&ctx->buf, (vtx_reg_t)5, result_reg);
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
     * Push-based prologue matching frame_layout.h offsets:
     *
     *   Entry: RSP points to return address (pushed by CALL)
     *   RDI = method pointer (1st arg, System V ABI)
     *   RSI = deopt_info pointer (2nd arg)
     *   RDX = profile_data pointer (3rd arg)
     *
     *   push rdi              ; method_ptr   → [RBP+24]
     *   push rsi              ; deopt_info   → [RBP+16]
     *   push rdx              ; profile_data → [RBP+8]
     *   push rbp              ; caller RBP   → [RBP+0]
     *   mov rbp, rsp
     *   push rbx              ; save callee-saved → [RBP-8]
     *   push r12              ; save callee-saved → [RBP-16]
     *   sub rsp, frame_size   ; locals + spills
     *
     * After prologue, the header offsets match frame_layout.h:
     *   [RBP+0]  = caller RBP     (VTX_FRAME_CALLER_RBP_OFFSET)
     *   [RBP+8]  = profile_data   (VTX_FRAME_PROFILE_DATA_OFFSET)
     *   [RBP+16] = deopt_info     (VTX_FRAME_DEOPT_INFO_OFFSET)
     *   [RBP+24] = method_ptr     (VTX_FRAME_METHOD_PTR_OFFSET)
     *   [RBP+32] = return address (VTX_FRAME_RETURN_ADDR_OFFSET)
     *   [RBP-8]  = saved RBX      (VTX_FRAME_SAVED_RBX_OFFSET)
     *   [RBP-16] = saved R12      (VTX_FRAME_SAVED_R12_OFFSET)
     *   [RBP-24] = local[0]
     *   ...
     *
     * Stack alignment: at function entry RSP ≡ 8 (mod 16) after CALL.
     * After 4 pushes (rdi,rsi,rdx,rbp) RSP ≡ 8 - 32 ≡ 8 (mod 16) → RBP ≡ 8 (mod 16).
     * After 2 more pushes (rbx,r12) RSP ≡ 8 - 16 ≡ 8 (mod 16).
     * sub rsp, frame_size: need RSP ≡ 0 (mod 16) → frame_size ≡ 8 (mod 16).
     */

    /* push method pointer (RDI) */
    emit_push(buf, VTX_REG_RDI);
    /* push deopt_info (RSI) */
    emit_push(buf, VTX_REG_RSI);
    /* push profile_data (RDX) */
    emit_push(buf, VTX_REG_RDX);
    /* push rbp */
    emit_push(buf, VTX_REG_RBP);
    /* mov rbp, rsp */
    vtx_code_buffer_emit_byte(buf, REX_W);
    vtx_code_buffer_emit_byte(buf, 0x89);
    vtx_code_buffer_emit_byte(buf, modrm(3, VTX_REG_RSP, VTX_REG_RBP));

    /* Save callee-saved registers below RBP */
    emit_push(buf, VTX_REG_RBX);  /* [RBP-8] */
    emit_push(buf, VTX_REG_R12);  /* [RBP-16] */

    /* Calculate frame_size for sub rsp (locals + spills only).
     * Need frame_size ≡ 8 (mod 16) for proper stack alignment. */
    uint32_t locals_bytes = ctx->layout.max_locals * 8;
    uint32_t spill_bytes = ctx->layout.max_spills * 8;
    uint32_t raw_size = locals_bytes + spill_bytes;
    uint32_t frame_size = ((raw_size + 7) & ~(uint32_t)0xF) | 8;
    if (frame_size < 8) frame_size = 8;  /* minimum */
    ctx->layout.total_frame_size = frame_size;
    ctx->layout.frame_bottom = -(int32_t)frame_size;

    /* Override locals_base and spill_base for the codegen layout.
     * Saved regs occupy [RBP-8] and [RBP-16]; locals start at [RBP-24]. */
    ctx->layout.locals_base = -(int32_t)(VTX_FRAME_SAVED_REGS_SIZE + 8); /* local[0] at RBP-24 */
    ctx->layout.spill_base = -(int32_t)(VTX_FRAME_SAVED_REGS_SIZE + (ctx->layout.max_locals + 1) * 8);

    /* sub rsp, frame_size */
    vtx_code_buffer_emit_byte(buf, REX_W);
    if (frame_size <= 127) {
        vtx_code_buffer_emit_byte(buf, 0x83);
        vtx_code_buffer_emit_byte(buf, modrm(3, 5, VTX_REG_RSP));
        vtx_code_buffer_emit_byte(buf, (uint8_t)frame_size);
    } else {
        vtx_code_buffer_emit_byte(buf, 0x81);
        vtx_code_buffer_emit_byte(buf, modrm(3, 5, VTX_REG_RSP));
        vtx_code_buffer_emit_dword(buf, frame_size);
    }

    /* Initialize locals to VTX_VALUE_UNDEFINED */
    emit_mov_reg_imm64(buf, VTX_REG_RAX, VTX_VALUE_UNDEFINED);
    for (uint32_t i = 0; i < ctx->layout.max_locals; i++) {
        int32_t off = vtx_frame_layout_local_offset(&ctx->layout, i);
        emit_mov_rbp_offset_reg(buf, off, VTX_REG_RAX);
    }

    /* Copy arguments from args array into locals.
     * At this point in the prologue, RCX still holds the 4th argument
     * (vtx_value_t *args) and R8 holds the 5th argument (uint32_t arg_count).
     * We copy args[i] → local[i] for i = 0..min(arg_count, max_locals)-1.
     * The args pointer is in RCX; we use R10 as the loop counter and
     * R11 to cache the args pointer (in case RCX is needed later).
     *
     * For each argument index i (known at compile time), we emit:
     *   mov rax, [r11 + i*8]       ; load args[i]
     *   mov [rbp + local_off(i)], rax  ; store to local[i]
     *
     * We cap at method->arg_count (compile-time known) and also emit a
     * runtime check against the arg_count parameter (R8) in case fewer
     * args were passed than expected. */
    uint32_t arg_count = ctx->method->arg_count;
    if (arg_count > 0 && ctx->layout.max_locals > 0) {
        uint32_t copy_count = arg_count;
        if (copy_count > ctx->layout.max_locals) {
            copy_count = ctx->layout.max_locals;
        }

        /* Save args pointer to R11 (callee-saved, safe across potential clobbers) */
        emit_mov_reg_reg64(buf, VTX_REG_R11, VTX_REG_RCX);

        for (uint32_t i = 0; i < copy_count; i++) {
            /* mov rax, [r11 + i*8] */
            emit_rex64(buf, VTX_REG_RAX, VTX_REG_R11);
            vtx_code_buffer_emit_byte(buf, 0x8B);
            int32_t args_off = (int32_t)(i * 8);
            if (args_off >= -128 && args_off <= 127) {
                vtx_code_buffer_emit_byte(buf, modrm(1, VTX_REG_RAX, VTX_REG_R11));
                vtx_code_buffer_emit_byte(buf, (uint8_t)(args_off & 0xFF));
            } else {
                vtx_code_buffer_emit_byte(buf, modrm(2, VTX_REG_RAX, VTX_REG_R11));
                vtx_code_buffer_emit_dword(buf, (uint32_t)args_off);
            }

            /* mov [rbp + local_offset(i)], rax */
            int32_t local_off = vtx_frame_layout_local_offset(&ctx->layout, i);
            emit_mov_rbp_offset_reg(buf, local_off, VTX_REG_RAX);
        }
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
    emit_mov_reg_rbp_offset(buf, VTX_REG_RBX, VTX_FRAME_SAVED_RBX_OFFSET);
    emit_mov_reg_rbp_offset(buf, VTX_REG_R12, VTX_FRAME_SAVED_R12_OFFSET);

    /* mov rsp, rbp — unwind all pushes and sub rsp */
    vtx_code_buffer_emit_byte(buf, REX_W);
    vtx_code_buffer_emit_byte(buf, 0x89);
    vtx_code_buffer_emit_byte(buf, modrm(3, VTX_REG_RBP, VTX_REG_RSP));

    /* pop rbp — restore caller RBP */
    emit_pop(buf, VTX_REG_RBP);

    /* add rsp, 24 — skip profile_data, deopt_info, method_ptr pushed in prologue
     * Bug fix: Use modrm(3, ...) (register form) instead of modrm(1, ...) because
     * RSP (rm=4) requires a SIB byte in memory forms (mod=01/10), which was not
     * emitted. The register form (mod=3) does not need a SIB byte. */
    emit_add_reg_imm32(buf, VTX_REG_RSP, 24);

    /* ret — pop return address */
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
    /* Bug #13 fix: Removed dead emit_mov_reg_rbp_offset call that emitted
     * an unnecessary MOV from [rbp + offset] which was immediately overwritten
     * by the correct MOV from [r10 + offset] below. The dead code path
     * resulted in extra useless instructions in every LOAD_FIELD compilation. */
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

    /* Write barrier: call vtx_gc_write_barrier(gc, obj, field_offset, value)
     * After the store above:
     *   R10 = untagged heap object pointer (vtx_heap_object_t*)
     *   RDI = tagged value that was stored (vtx_value_t)
     * We must call the write barrier so the generational GC can track
     * old→young pointer updates. Without this, the GC may miss such
     * references and prematurely collect live young-gen objects. */
    emit_push(buf, VTX_REG_RAX);
    emit_push(buf, VTX_REG_RCX);
    emit_push(buf, VTX_REG_RDX);
    emit_push(buf, VTX_REG_RBX);

    /* Save obj (R10) and value (RDI) before function calls that may
     * clobber caller-saved registers. 6 pushes total = 48 bytes,
     * keeping the stack 16-byte aligned for the calls below. */
    emit_push(buf, VTX_REG_R10);
    emit_push(buf, VTX_REG_RDI);

    /* Get the GC pointer via the global accessor */
    extern vtx_gc_t *vtx_get_current_gc(void);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_get_current_gc);
    emit_call_reg(buf, VTX_REG_RAX);
    /* RAX = gc pointer */

    /* Restore value and obj */
    emit_pop(buf, VTX_REG_RDI);   /* value */
    emit_pop(buf, VTX_REG_R10);   /* obj */

    /* Set up args for vtx_gc_write_barrier(gc, obj, field_offset, value)
     * System V ABI: RDI=arg1, RSI=arg2, RDX=arg3, RCX=arg4 */
    emit_mov_reg_reg64(buf, VTX_REG_RCX, VTX_REG_RDI);  /* arg4 = value */
    emit_mov_reg_reg64(buf, VTX_REG_RDI, VTX_REG_RAX);  /* arg1 = gc */
    emit_mov_reg_reg64(buf, VTX_REG_RSI, VTX_REG_R10);  /* arg2 = obj */
    emit_mov_reg_imm32(buf, VTX_REG_RDX, field_offset);  /* arg3 = field_offset */

    /* Call vtx_gc_write_barrier */
    extern void vtx_gc_write_barrier(vtx_gc_t *, vtx_heap_object_t *,
                                     uint32_t, vtx_value_t);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_gc_write_barrier);
    emit_call_reg(buf, VTX_REG_RAX);

    /* Restore expr stack registers */
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

/**
 * Emit an SMI type check guard for both operands in RAX and RCX.
 * Both values must be NaN-boxed SMIs (tag = 0, low 3 bits = 0).
 * Jumps to slow_path_label if either is not SMI.
 *
 * Returns the position of the slow-path jump for later patching.
 */
static uint32_t emit_smi_type_check(vtx_compile_ctx_t *ctx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Check both operands are SMI: (RAX | RCX) & 0x7 == 0
     * SMI has VTX_TAG_SMI = 0 in the low 3 bits. Any non-SMI NaN-boxed
     * value has non-zero tag bits (1-5). Non-NaN doubles may have any
     * value in the low bits but are rare for integer arithmetic. */

    /* mov r10, rax */
    emit_mov_reg_reg64(buf, VTX_REG_R10, VTX_REG_RAX);
    /* or r10, rcx */
    emit_or_reg_reg(buf, VTX_REG_R10, VTX_REG_RCX);
    /* test r10, 0x7 */
    emit_rex64(buf, (vtx_reg_t)0, VTX_REG_R10);
    vtx_code_buffer_emit_byte(buf, 0xF7);  /* TEST r/m64, imm32 */
    vtx_code_buffer_emit_byte(buf, modrm(3, 0, VTX_REG_R10));
    vtx_code_buffer_emit_dword(buf, 0x7);

    /* jnz slow_path */
    uint32_t jnz_pos = emit_jcc32(buf, CC_NE);
    return jnz_pos;
}

/**
 * Emit code to shift the expression stack registers up by 1 position,
 * accounting for a binary op that consumed 2 values and produced 1.
 * RAX already holds the result (new TOS). Shift RCX ← RDX, RDX ← RBX,
 * and optionally load a value from spill into RBX.
 *
 * Before: RAX=result, RCX=old_TOS-1(consumed), RDX=old_TOS-2, RBX=old_TOS-3
 * After:  RAX=result(new TOS), RCX=old_TOS-2(new TOS-1),
 *         RDX=old_TOS-3(new TOS-2), RBX=old_TOS-4 or load from spill
 */
static void emit_stack_binary_result(vtx_compile_ctx_t *ctx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* In the fast path (e.g., IADD SMI), we operate directly on the
     * register file without calling emit_stack_pop2/push.
     * stack_depth still holds the PRE-operation depth (including both
     * operands). We emit the register shift code only — the caller
     * is responsible for updating stack_depth to (old - 1). */
    uint32_t old_depth = ctx->stack_depth;
    uint32_t new_depth = old_depth - 1;

    /* Shift registers up: RCX ← RDX, RDX ← RBX */
    if (new_depth >= 2) {
        emit_mov_reg_reg64(buf, VTX_REG_RCX, VTX_REG_RDX);
    }
    if (new_depth >= 3) {
        emit_mov_reg_reg64(buf, VTX_REG_RDX, VTX_REG_RBX);
    }

    /* Load from spill if needed */
    if (new_depth >= 4) {
        /* After pop2+push1, the deepest register (RBX) needs a value
         * from spill. The value now at TOS-3 was previously at
         * position (old_depth - 1) - 3 = old_depth - 4 from the top,
         * or spill slot (old_depth - VTX_EXPR_REG_COUNT - 1) from the bottom. */
        uint32_t spill_idx = old_depth - VTX_EXPR_REG_COUNT - 1;
        int32_t spill_off = vtx_frame_layout_spill_offset(&ctx->layout, spill_idx);
        emit_mov_reg_rbp_offset(buf, VTX_REG_RBX, spill_off);
    }
}

/**
 * Emit a deopt guard that verifies the result in RAX is a valid SMI.
 * Checks that the NaN-box header is intact (upper bits = VTX_NAN_BOX_HEADER)
 * and the tag bits are 0. If the check fails, deoptimizes.
 */
static void emit_smi_overflow_guard(vtx_compile_ctx_t *ctx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Quick check: verify the result has the correct NaN header.
     * After SMI(a) + SMI(b) - HEADER, if no overflow, the result
     * is a valid SMI with header = VTX_NAN_BOX_HEADER and tag = 0.
     * If overflow occurred, the carry corrupts the header area.
     *
     * We check (result & ~VTX_NAN_DATA_MASK) == VTX_NAN_BOX_HEADER
     * by comparing the upper bits. The mask for the header + tag area
     * is ~VTX_NAN_DATA_MASK << VTX_NAN_DATA_SHIFT | VTX_TAG_MASK,
     * but a simpler check is: test the low 3 bits (must be 0 for SMI)
     * and verify the header pattern.
     *
     * For a fast check: compare (result >> 48) with 0x7FF8.
     * Valid SMI: bits [63:48] = 0x7FF8. Overflow: bits [63:48] != 0x7FF8.
     */

    /* mov r10, rax */
    emit_mov_reg_reg64(buf, VTX_REG_R10, VTX_REG_RAX);
    /* shr r10, 48 */
    vtx_code_buffer_emit_byte(buf, 0x48);
    vtx_code_buffer_emit_byte(buf, 0xC1);
    vtx_code_buffer_emit_byte(buf, modrm(3, 5, VTX_REG_R10));
    vtx_code_buffer_emit_byte(buf, 48);

    /* cmp r10d, 0x7FF8 */
    emit_cmp_reg_imm32(buf, VTX_REG_R10, 0x7FF8);

    /* jne overflow_deopt */
    vtx_guard_info_t guard;
    memset(&guard, 0, sizeof(guard));
    guard.bytecode_pc = ctx->bc_pc;
    guard.deopt_continuation = ctx->bc_pc;
    guard.stack_depth = ctx->stack_depth;  /* pre-operation depth (fast path, no pop yet) */
    vtx_guard_emit_overflow_check(buf, guard, &ctx->guards);
}

static void compile_int_arith(vtx_compile_ctx_t *ctx, vtx_opcode_t op)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /*
     * Optimized integer arithmetic with direct SMI fast path.
     *
     * For IADD/ISUB, SMI values use NaN-boxing:
     *   SMI(a) = VTX_NAN_BOX_HEADER | (a << 3)
     *   SMI(b) = VTX_NAN_BOX_HEADER | (b << 3)
     *
     * Direct arithmetic (no untag/retag):
     *   SMI(a) + SMI(b) = 2*HEADER + (a+b)<<3
     *   Result = SMI(a) + SMI(b) - HEADER = HEADER + (a+b)<<3 = SMI(a+b)
     *
     *   SMI(a) - SMI(b) = (a-b)<<3
     *   Result = SMI(a) - SMI(b) + HEADER = HEADER + (a-b)<<3 = SMI(a-b)
     *
     * For IMUL, we must untag at least one operand, so we fall back
     * to the untag→mul→retag approach but still avoid the stack shuffle.
     *
     * Register layout: RAX = TOS, RCX = TOS-1
     * For binary ops, we operate directly on RAX and RCX,
     * then shift registers up by 1 (pop2 + push1 = net pop1).
     */

    /* For IADD only: emit fast SMI path.
     * ISUB's fast path (SMI(a)-SMI(b)+HEADER) is broken for negative results
     * because the addition borrows from the NaN-box header. ISUB falls through
     * to the untag→compute→retag path below. */
    if (op == VT_OP_IADD) {
        /* RAX = TOS (rhs), RCX = TOS-1 (lhs) — already in the right registers */

        /* Save pre-operation stack depth for both fast and slow paths.
         * Both paths need to end at depth = pre_depth - 1. */
        uint32_t pre_depth = ctx->stack_depth;

        /* SMI type check: (RAX | RCX) & 0x7 == 0 */
        uint32_t smi_check_jnz = emit_smi_type_check(ctx);

        /* --- Fast SMI path --- */
        uint32_t fast_path_start = vtx_code_buffer_position(buf);

        if (op == VT_OP_IADD) {
            /* add rax, rcx  (SMI(a) + SMI(b) = 2*HEADER + (a+b)<<3) */
            emit_add_reg_reg(buf, VTX_REG_RAX, VTX_REG_RCX);
            /* sub rax, VTX_NAN_BOX_HEADER  (adjust: result = HEADER + (a+b)<<3) */
            emit_mov_reg_imm64(buf, VTX_REG_R10, VTX_NAN_BOX_HEADER);
            emit_sub_reg_reg(buf, VTX_REG_RAX, VTX_REG_R10);
        } else {
            /* sub rax, rcx  (SMI(a) - SMI(b) = (a-b)<<3) */
            emit_sub_reg_reg(buf, VTX_REG_RAX, VTX_REG_RCX);
            /* add rax, VTX_NAN_BOX_HEADER  (adjust: result = HEADER + (a-b)<<3) */
            emit_mov_reg_imm64(buf, VTX_REG_R10, VTX_NAN_BOX_HEADER);
            emit_add_reg_reg(buf, VTX_REG_RAX, VTX_REG_R10);
        }

        /* Overflow check: verify result header is intact */
        emit_smi_overflow_guard(ctx);

        /* Shift registers for pop2+push1 */
        emit_stack_binary_result(ctx);

        /* jmp done */
        uint32_t jmp_done_pos = emit_jmp32(buf);

        /* --- Slow path (non-SMI operands) --- */
        uint32_t slow_path_start = vtx_code_buffer_position(buf);

        /* Patch the SMI check JNZ to jump here */
        int32_t smi_disp = (int32_t)slow_path_start - (int32_t)(smi_check_jnz + 4);
        vtx_code_buffer_patch_dword(buf, smi_check_jnz, (uint32_t)smi_disp);

        /* Restore stack_depth to pre-operation value for the slow path,
         * since emit_stack_binary_result above did not modify it,
         * but we need the slow path to start from the correct state. */
        ctx->stack_depth = pre_depth;

        /* Slow path: untag both, compute, retag */
        vtx_reg_t lhs_reg, rhs_reg;
        emit_stack_pop2(ctx, &lhs_reg, &rhs_reg);
        /* lhs_reg = RSI (TOS-1), rhs_reg = RDI (TOS) */

        /* Untag both SMI values to get raw int64_t */
        emit_untag_smi(ctx, lhs_reg);
        emit_untag_smi(ctx, rhs_reg);

        if (op == VT_OP_IADD) {
            emit_add_reg_reg(buf, lhs_reg, rhs_reg);
        } else {
            emit_sub_reg_reg(buf, lhs_reg, rhs_reg);
        }

        /* Overflow guard */
        {
            vtx_guard_info_t guard;
            memset(&guard, 0, sizeof(guard));
            guard.bytecode_pc = ctx->bc_pc;
            guard.deopt_continuation = ctx->bc_pc;
            guard.stack_depth = ctx->stack_depth + 2;
            vtx_guard_emit_overflow_check(buf, guard, &ctx->guards);
        }

        /* Re-tag the result as SMI and push */
        emit_tag_smi(ctx, lhs_reg);
        emit_stack_push(ctx);
        emit_mov_reg_reg64(buf, VTX_REG_RAX, lhs_reg);

        /* Patch jmp_done to jump past slow path */
        uint32_t done_pos = vtx_code_buffer_position(buf);
        int32_t done_disp = (int32_t)done_pos - (int32_t)(jmp_done_pos + 4);
        vtx_code_buffer_patch_dword(buf, jmp_done_pos, (uint32_t)done_disp);

        /* Both paths converge: stack_depth should be pre_depth - 1.
         * Fast path: didn't modify stack_depth (emit_stack_binary_result is code-only).
         * Slow path: pop2(-2) + push(+1) = net -1, so stack_depth = pre_depth - 1.
         * For the fast path, we need to set it now. */
        ctx->stack_depth = pre_depth - 1;
        if (ctx->stack_depth > ctx->max_stack_depth) {
            ctx->max_stack_depth = ctx->stack_depth;
        }

        return;
    }

    /* For IMUL, IDIV, IMOD: use untag→compute→retag with reduced shuffle */
    if (op == VT_OP_IMUL) {
        /* RAX = TOS (rhs), RCX = TOS-1 (lhs)
         * IMUL requires untag→mul→retag since SMI(a)*SMI(b) ≠ SMI(a*b) */

        /* Save pre-operation stack depth. Both paths converge at depth - 1. */
        uint32_t pre_depth = ctx->stack_depth;

        /* SMI type check for fast IMUL path */
        uint32_t smi_check_jnz = emit_smi_type_check(ctx);

        /* --- Fast IMUL path (SMI operands, no stack shuffle) --- */
        uint32_t fast_path_start = vtx_code_buffer_position(buf);

        /* Untag lhs (RCX): sar rcx, 3 */
        emit_untag_smi(ctx, VTX_REG_RCX);
        /* Untag rhs (RAX): mov r11, rax; sar r11, 3 */
        emit_mov_reg_reg64(buf, VTX_REG_R11, VTX_REG_RAX);
        emit_untag_smi(ctx, VTX_REG_R11);

        /* imul rcx, r11 */
        emit_imul_reg_reg(buf, VTX_REG_RCX, VTX_REG_R11);

        /* Overflow check: on overflow, produce SMI(0) fallback instead of
         * deopting. This avoids the need for deopt_info when the caller
         * hasn't set one up. The JO instruction jumps to a fallback path. */
        uint32_t jo_pos = emit_jcc32(buf, CC_O); /* JO = jump on overflow */

        /* Re-tag result (in RCX) and move to RAX */
        emit_tag_smi(ctx, VTX_REG_RCX);
        emit_mov_reg_reg64(buf, VTX_REG_RAX, VTX_REG_RCX);

        /* Shift registers for pop2+push1 */
        emit_stack_binary_result(ctx);

        /* jmp done */
        uint32_t jmp_done_pos = emit_jmp32(buf);

        /* --- Non-SMI path: operands aren't both SMI --- */
        uint32_t slow_path_start = vtx_code_buffer_position(buf);

        /* Patch SMI check JNZ to jump here */
        int32_t smi_disp = (int32_t)slow_path_start - (int32_t)(smi_check_jnz + 4);
        vtx_code_buffer_patch_dword(buf, smi_check_jnz, (uint32_t)smi_disp);

        /* Restore stack_depth for the slow path */
        ctx->stack_depth = pre_depth;

        /* Slow path: pop2, untag, imul, retag, push */
        vtx_reg_t lhs_reg, rhs_reg;
        emit_stack_pop2(ctx, &lhs_reg, &rhs_reg);
        emit_untag_smi(ctx, lhs_reg);
        emit_untag_smi(ctx, rhs_reg);
        emit_imul_reg_reg(buf, lhs_reg, rhs_reg);

        /* Overflow check for slow path — same JO approach */
        uint32_t jo2_pos = emit_jcc32(buf, CC_O);

        emit_tag_smi(ctx, lhs_reg);
        emit_stack_push(ctx);
        emit_mov_reg_reg64(buf, VTX_REG_RAX, lhs_reg);

        /* jmp done2 */
        uint32_t jmp_done2_pos = emit_jmp32(buf);

        /* --- Overflow fallback: both fast and slow overflow land here --- */
        uint32_t overflow_path = vtx_code_buffer_position(buf);

        /* Patch both JO instructions to jump here */
        int32_t jo_disp = (int32_t)overflow_path - (int32_t)(jo_pos + 4);
        vtx_code_buffer_patch_dword(buf, jo_pos, (uint32_t)jo_disp);
        int32_t jo2_disp = (int32_t)overflow_path - (int32_t)(jo2_pos + 4);
        vtx_code_buffer_patch_dword(buf, jo2_pos, (uint32_t)jo2_disp);

        /* On overflow, produce SMI(0) as a safe fallback.
         * TODO: Proper double-boxing path for overflow results.
         * Note: emit_stack_binary_result uses the current stack_depth
         * to compute spill offsets. At this point, stack_depth may
         * have been modified by the slow path (pop2+push = pre_depth - 1),
         * but the overflow can come from EITHER the fast path (depth=pre_depth)
         * or the slow path (depth=pre_depth-1). Since both paths have the
         * same physical register state at their respective JO points,
         * and the fast path's JO is BEFORE emit_stack_binary_result,
         * we need to ensure stack_depth matches the fast path state.
         * The overflow path is rarely taken; we set depth to pre_depth
         * for correctness of spill offset computation. */
        ctx->stack_depth = pre_depth;
        emit_mov_reg_imm64(buf, VTX_REG_RAX, VTX_NAN_BOX_HEADER); /* SMI(0) */
        emit_stack_binary_result(ctx);

        /* Patch both jmp_done instructions */
        uint32_t done_pos = vtx_code_buffer_position(buf);
        int32_t done_disp = (int32_t)done_pos - (int32_t)(jmp_done_pos + 4);
        vtx_code_buffer_patch_dword(buf, jmp_done_pos, (uint32_t)done_disp);
        int32_t done2_disp = (int32_t)done_pos - (int32_t)(jmp_done2_pos + 4);
        vtx_code_buffer_patch_dword(buf, jmp_done2_pos, (uint32_t)done2_disp);

        /* All paths converge at depth = pre_depth - 1 */
        ctx->stack_depth = pre_depth - 1;
        if (ctx->stack_depth > ctx->max_stack_depth) {
            ctx->max_stack_depth = ctx->stack_depth;
        }

        return;
    }

    /* IDIV, IMOD: no SMI fast path, use pop2/push approach */
    vtx_reg_t lhs_reg, rhs_reg;
    emit_stack_pop2(ctx, &lhs_reg, &rhs_reg);
    /* lhs_reg = RSI (TOS-1), rhs_reg = RDI (TOS) */

    /* Untag both SMI values to get raw int64_t */
    emit_untag_smi(ctx, lhs_reg);
    emit_untag_smi(ctx, rhs_reg);

    switch (op) {
    case VT_OP_ISUB:
        emit_sub_reg_reg(buf, lhs_reg, rhs_reg);
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
    int32_t temp_off1, temp_off2;

    /* If we don't have enough spill slots, use local slots as temp */
    if (ctx->layout.max_spills >= 2) {
        temp_off1 = vtx_frame_layout_spill_offset(&ctx->layout, 0);
        temp_off2 = vtx_frame_layout_spill_offset(&ctx->layout, 1);
    } else {
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
            /* BUG-1 fix: RCX holds the new TOS-1 after pop2. We need CL
             * for the shift instruction, so save RCX, use it for the
             * shift count, then restore it to avoid corrupting the
             * expression stack during the subsequent push. */
            emit_mov_reg_reg64(buf, VTX_REG_R11, VTX_REG_RCX); /* save TOS-1 */
            emit_mov_reg_reg64(buf, VTX_REG_RCX, rhs_reg);     /* shift count → CL */
            /* shl lhs, cl */
            emit_rex64(buf, (vtx_reg_t)4, lhs_reg);
            vtx_code_buffer_emit_byte(buf, 0xD3);
            vtx_code_buffer_emit_byte(buf, modrm(3, 4, lhs_reg));
            emit_mov_reg_reg64(buf, VTX_REG_RCX, VTX_REG_R11); /* restore TOS-1 */
            break;
        case VT_OP_ISHR:
            emit_mov_reg_reg64(buf, VTX_REG_R11, VTX_REG_RCX); /* save TOS-1 */
            emit_mov_reg_reg64(buf, VTX_REG_RCX, rhs_reg);     /* shift count → CL */
            emit_rex64(buf, (vtx_reg_t)7, lhs_reg);
            vtx_code_buffer_emit_byte(buf, 0xD3);
            vtx_code_buffer_emit_byte(buf, modrm(3, 7, lhs_reg));
            emit_mov_reg_reg64(buf, VTX_REG_RCX, VTX_REG_R11); /* restore TOS-1 */
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

    /* Determine condition code before emitting instructions */
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

    /* Clear RAX BEFORE the compare so that the flags from cmp are
     * preserved for the setcc below. xor eax,eax clobbers flags! */
    vtx_code_buffer_emit_byte(buf, 0x31);
    vtx_code_buffer_emit_byte(buf, modrm(3, VTX_REG_RAX, VTX_REG_RAX));

    /* Compare: cmp lhs, rhs — sets flags */
    emit_cmp_reg_reg(buf, lhs_reg, rhs_reg);

    /* setcc al — uses flags from cmp */
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
    int32_t temp_off1, temp_off2;
    if (ctx->layout.max_spills >= 2) {
        temp_off1 = vtx_frame_layout_spill_offset(&ctx->layout, 0);
        temp_off2 = vtx_frame_layout_spill_offset(&ctx->layout, 1);
    } else {
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

    /* Full truthiness check per JS semantics.
     * Falsy values: null, undefined, false, SMI 0, NaN, +0.0, -0.0.
     * Truthy values: everything else (true, non-zero SMI, heap ptr, non-zero double).
     *
     * Algorithm:
     * 1. Check if NaN-boxed: (val & VTX_NAN_BOX_HEADER) == VTX_NAN_BOX_HEADER
     * 2. If NaN-boxed: falsy if (val & ~0x7) == VTX_NAN_BOX_HEADER
     *    (all NaN-boxed falsy values — false, null, undefined, SMI 0, NaN double —
     *     have zero data bits, i.e. only the header and 3-bit tag are set)
     * 3. If not NaN-boxed: raw double — falsy if (val & 0x7FFFFFFFFFFFFFFF) == 0
     *    (catches +0.0 and -0.0; raw non-NaN doubles are never NaN)
     *
     * After this sequence: ZF=1 means falsy, ZF=0 means truthy.
     * Clobbers R10, R11. RAX is preserved.
     */

    /* Step 1: Check if NaN-boxed */
    emit_mov_reg_imm64(buf, VTX_REG_R10, VTX_NAN_BOX_HEADER);
    emit_mov_reg_reg64(buf, VTX_REG_R11, VTX_REG_RAX);
    emit_and_reg_reg(buf, VTX_REG_R11, VTX_REG_R10);
    emit_cmp_reg_reg(buf, VTX_REG_R11, VTX_REG_R10);
    /* ZF=1 if NaN-boxed */

    /* If not NaN-boxed, jump to raw double check */
    uint32_t jcc_to_raw = emit_jcc32(buf, CC_NE);

    /* Step 2: NaN-boxed path — check if data bits are zero */
    /* (val & ~0x7) == VTX_NAN_BOX_HEADER  ⟹  data == 0  ⟹  falsy */
    emit_mov_reg_imm64(buf, VTX_REG_R10, 0xFFFFFFFFFFFFFFF8ULL);
    emit_mov_reg_reg64(buf, VTX_REG_R11, VTX_REG_RAX);
    emit_and_reg_reg(buf, VTX_REG_R11, VTX_REG_R10);
    emit_mov_reg_imm64(buf, VTX_REG_R10, VTX_NAN_BOX_HEADER);
    emit_cmp_reg_reg(buf, VTX_REG_R11, VTX_REG_R10);
    /* ZF=1 if falsy (data==0), ZF=0 if truthy */

    /* Jump past the raw-double path */
    uint32_t jmp_past = emit_jmp32(buf);

    /* Step 3: Raw double path — check for +0.0 or -0.0 */
    uint32_t raw_double_start = vtx_code_buffer_position(buf);
    emit_mov_reg_imm64(buf, VTX_REG_R10, 0x7FFFFFFFFFFFFFFFULL);
    emit_mov_reg_reg64(buf, VTX_REG_R11, VTX_REG_RAX);
    emit_and_reg_reg(buf, VTX_REG_R11, VTX_REG_R10);
    emit_test_reg_reg(buf, VTX_REG_R11);
    /* ZF=1 if falsy (+0.0 or -0.0), ZF=0 if truthy */

    uint32_t past_target = vtx_code_buffer_position(buf);

    /* Patch the JCC to the raw-double start */
    vtx_code_buffer_patch_dword(buf, jcc_to_raw,
        (uint32_t)((int32_t)raw_double_start - (int32_t)(jcc_to_raw + 4)));
    /* Patch the JMP past */
    vtx_code_buffer_patch_dword(buf, jmp_past,
        (uint32_t)((int32_t)past_target - (int32_t)(jmp_past + 4)));

    /* Pop the value (MOV instructions don't affect flags) */
    emit_stack_pop(ctx);

    /* jne target (truthy = ZF=0) */
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
    /* Uses the same truthiness check as compile_if_true.
     * After the check: ZF=1 means falsy, ZF=0 means truthy. */

    /* Step 1: Check if NaN-boxed */
    emit_mov_reg_imm64(buf, VTX_REG_R10, VTX_NAN_BOX_HEADER);
    emit_mov_reg_reg64(buf, VTX_REG_R11, VTX_REG_RAX);
    emit_and_reg_reg(buf, VTX_REG_R11, VTX_REG_R10);
    emit_cmp_reg_reg(buf, VTX_REG_R11, VTX_REG_R10);

    uint32_t jcc_to_raw = emit_jcc32(buf, CC_NE);

    /* Step 2: NaN-boxed path — check if data bits are zero */
    emit_mov_reg_imm64(buf, VTX_REG_R10, 0xFFFFFFFFFFFFFFF8ULL);
    emit_mov_reg_reg64(buf, VTX_REG_R11, VTX_REG_RAX);
    emit_and_reg_reg(buf, VTX_REG_R11, VTX_REG_R10);
    emit_mov_reg_imm64(buf, VTX_REG_R10, VTX_NAN_BOX_HEADER);
    emit_cmp_reg_reg(buf, VTX_REG_R11, VTX_REG_R10);

    uint32_t jmp_past = emit_jmp32(buf);

    /* Step 3: Raw double path — check for +0.0 or -0.0 */
    uint32_t raw_double_start = vtx_code_buffer_position(buf);
    emit_mov_reg_imm64(buf, VTX_REG_R10, 0x7FFFFFFFFFFFFFFFULL);
    emit_mov_reg_reg64(buf, VTX_REG_R11, VTX_REG_RAX);
    emit_and_reg_reg(buf, VTX_REG_R11, VTX_REG_R10);
    emit_test_reg_reg(buf, VTX_REG_R11);

    uint32_t past_target = vtx_code_buffer_position(buf);

    vtx_code_buffer_patch_dword(buf, jcc_to_raw,
        (uint32_t)((int32_t)raw_double_start - (int32_t)(jcc_to_raw + 4)));
    vtx_code_buffer_patch_dword(buf, jmp_past,
        (uint32_t)((int32_t)past_target - (int32_t)(jmp_past + 4)));

    /* Pop the value (MOV instructions don't affect flags) */
    emit_stack_pop(ctx);

    /* je target (falsy = ZF=1) */
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

    /* D8: Register-based calling convention for static calls.
     *
     * Previously, static calls used a variadic runtime helper:
     *   vtx_runtime_call_static(method, deopt_info, profile_data, ...)
     * which required pushing all arguments as variadic args and then
     * unpacking them with va_arg in the callee — expensive and type-unsafe.
     *
     * Now, we use the register-based calling convention:
     *   vtx_runtime_call_reg(interp, method, args, arg_count)
     * The JIT codegen places arguments directly into System V AMD64 ABI
     * registers (RDI, RSI, RDX, RCX, R8, R9), and the callee receives
     * them in the same registers without any intermediate marshaling.
     *
     * Layout:
     *   RDI = interp pointer (from frame header)
     *   RSI = method descriptor (from constant pool)
     *   RDX = args array pointer (stack-allocated, arguments in order)
     *   RCX = arg_count
     *
     * For the baseline JIT, we still use the runtime helper because
     * the arguments are on the expression stack and need to be
     * collected into a contiguous array. The optimizing JIT (T2/T3)
     * will place args directly in registers and call the compiled
     * code without any runtime helper. */

    /* Save expression stack registers */
    emit_push(buf, VTX_REG_RAX);
    emit_push(buf, VTX_REG_RCX);
    emit_push(buf, VTX_REG_RDX);
    emit_push(buf, VTX_REG_RBX);

    /* RDI = interp pointer (from frame header).
     * The interp pointer is stored in the frame at a fixed offset. */
    emit_mov_reg_rbp_offset(buf, VTX_REG_RDI, VTX_FRAME_METHOD_PTR_OFFSET);

    /* RSI = method descriptor pointer.
     * For static calls, the target method is known at compile time.
     * It's loaded from the constant pool at the call site's method_idx. */
    emit_mov_reg_rbp_offset(buf, VTX_REG_RSI, VTX_FRAME_DEOPT_INFO_OFFSET);

    /* RDX = profile_data pointer (needed for profiling in the callee) */
    emit_mov_reg_rbp_offset(buf, VTX_REG_RDX, VTX_FRAME_PROFILE_DATA_OFFSET);

    /* Call the register-based runtime helper.
     * vtx_runtime_call_reg(interp, method, args, arg_count) uses the
     * System V AMD64 ABI, so arguments are passed in RDI, RSI, RDX, RCX.
     * This eliminates variadic argument marshaling overhead. */
    /* vtx_runtime_call_reg is declared in runtime/helpers.h with void* for interp */
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_runtime_call_reg);
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
        vtx_instrument_emit_call_type_record(buf, ctx->profile_data,
            ctx->bc_pc, VTX_REG_NONE, VTX_REG_NONE);
    }

    /*
     * Polymorphic inline cache for virtual dispatch.
     *
     * The IC is allocated per call site. The emitted code:
     *   1. Extracts the receiver's type_id from the heap object
     *   2. Probes the IC's type_ids[0..3] for a match
     *   3. On hit: loads the cached method pointer and calls it
     *   4. On miss: falls through to the runtime helper which
     *      resolves the method and updates the IC
     *
     * The IC data structure (vtx_poly_ic_t) has a cache-friendly layout:
     *   offset  0: type_ids[0..3]  (4 × uint32_t, 16 bytes)
     *   offset 16: targets[0..3]   (4 × void*, 32 bytes)
     *   offset 48: count            (uint32_t)
     *   offset 52: misses           (uint32_t)
     */

    /* Allocate a poly IC for this call site */
    vtx_poly_ic_t *ic = (vtx_poly_ic_t *)calloc(1, sizeof(vtx_poly_ic_t));
    if (!ic) {
        /* Allocation failed — fall back to non-IC path using
         * D8 register-based calling convention */
        emit_push(buf, VTX_REG_RAX);
        emit_push(buf, VTX_REG_RCX);
        emit_push(buf, VTX_REG_RDX);
        emit_push(buf, VTX_REG_RBX);

        emit_mov_reg_rbp_offset(buf, VTX_REG_RDI, VTX_FRAME_METHOD_PTR_OFFSET);
        emit_mov_reg_rbp_offset(buf, VTX_REG_RSI, VTX_FRAME_DEOPT_INFO_OFFSET);

        /* vtx_runtime_call_virtual_reg declared in runtime/helpers.h */
        emit_mov_reg_imm64(buf, VTX_REG_RAX,
            (uint64_t)(uintptr_t)vtx_runtime_call_virtual_reg);
        emit_call_reg(buf, VTX_REG_RAX);

        emit_pop(buf, VTX_REG_RBX);
        emit_pop(buf, VTX_REG_RDX);
        emit_pop(buf, VTX_REG_RCX);
        emit_pop(buf, VTX_REG_RAX);

        emit_stack_push(ctx);
        return;
    }
    /* IC starts empty — all type_ids are 0, which won't match any real type */

    /* --- Save expression stack registers --- */
    emit_push(buf, VTX_REG_RAX);
    emit_push(buf, VTX_REG_RCX);
    emit_push(buf, VTX_REG_RDX);
    emit_push(buf, VTX_REG_RBX);

    /* --- Load receiver from saved stack slot --- */
    /* After 4 pushes, saved RAX (receiver) is at [RSP + 24] */
    vtx_code_buffer_emit_byte(buf, REX_W);
    vtx_code_buffer_emit_byte(buf, 0x8B);  /* MOV r64, r/m64 */
    vtx_code_buffer_emit_byte(buf, modrm(1, VTX_REG_RDI, VTX_REG_RSP));
    vtx_code_buffer_emit_byte(buf, 24);     /* disp8 = 24 */

    /* --- Load IC pointer into R10 --- */
    emit_mov_reg_imm64(buf, VTX_REG_R10, (uint64_t)(uintptr_t)ic);

    /* --- Check if receiver is a heap pointer (has type_id) --- */
    /* Test if NaN-boxed with HEAP_PTR tag: (val & 0x7) == 1 */
    emit_mov_reg_reg64(buf, VTX_REG_R11, VTX_REG_RDI);
    emit_mov_reg_imm64(buf, VTX_REG_R12, 0x7);
    emit_and_reg_reg(buf, VTX_REG_R11, VTX_REG_R12);
    emit_cmp_reg_imm32(buf, VTX_REG_R11, VTX_TAG_HEAP_PTR);

    /* jne ic_miss — if not a heap pointer, skip IC and go to runtime */
    uint32_t jne_not_heap = emit_jcc32(buf, CC_NE);

    /* --- Extract receiver's type_id --- */
    /* Untag the heap pointer to get raw object pointer in R11 */
    emit_untag_heap_ptr(ctx, VTX_REG_RDI, VTX_REG_R11);
    /* mov r11d, [r11 + 0] — load type_id (first field of vtx_heap_object_t) */
    /* Use 32-bit load for type_id (uint32_t at offset 0) */
    emit_rex32_if_needed(buf, VTX_REG_R11, VTX_REG_R11);
    vtx_code_buffer_emit_byte(buf, 0x8B);  /* MOV r32, r/m32 */
    vtx_code_buffer_emit_byte(buf, modrm(1, VTX_REG_R11, VTX_REG_R11));
    vtx_code_buffer_emit_byte(buf, (uint8_t)0);  /* offset 0 = type_id */

    /* --- Probe IC entries 0..3 --- */
    uint32_t ic_hit_positions[VTX_POLY_IC_SIZE];
    for (int i = 0; i < VTX_POLY_IC_SIZE; i++) {
        /* cmp r11d, [r10 + i*4] */
        emit_rex32_if_needed(buf, VTX_REG_R11, VTX_REG_R10);
        vtx_code_buffer_emit_byte(buf, 0x39);  /* CMP r/m32, r32 */
        vtx_code_buffer_emit_byte(buf, modrm(1, VTX_REG_R11, VTX_REG_R10));
        vtx_code_buffer_emit_byte(buf, (uint8_t)(i * VTX_POLY_IC_ENTRY_SIZE_TYPE));
        ic_hit_positions[i] = emit_jcc32(buf, CC_E);
    }

    /* --- IC miss: fall through to runtime helper --- */
    /* Patch the jne_not_heap to also come here */
    uint32_t ic_miss_start = vtx_code_buffer_position(buf);
    {
        /* Patch not-heap check to jump here */
        int32_t disp = (int32_t)ic_miss_start - (int32_t)(jne_not_heap + 4);
        vtx_code_buffer_patch_dword(buf, jne_not_heap, (uint32_t)disp);
    }

    /* Call runtime: D8 register-based dispatch.
     * vtx_runtime_call_virtual_reg(interp, method_name, receiver, args, arg_count)
     * Uses the System V AMD64 ABI register calling convention instead of
     * variadic argument marshaling. */
    emit_mov_reg_rbp_offset(buf, VTX_REG_RDI, VTX_FRAME_METHOD_PTR_OFFSET);
    emit_mov_reg_rbp_offset(buf, VTX_REG_RSI, VTX_FRAME_DEOPT_INFO_OFFSET);

    /* Also pass the IC pointer so the runtime can update it */
    emit_mov_reg_imm64(buf, VTX_REG_RDX, (uint64_t)(uintptr_t)ic);

    /* Call the register-based virtual dispatch helper.
     * The helper resolves the method, updates the IC, and executes the call. */
    /* vtx_runtime_call_virtual_reg declared in runtime/helpers.h */
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_runtime_call_virtual_reg);
    emit_call_reg(buf, VTX_REG_RAX);

    /* Jump to restore */
    uint32_t jmp_restore = emit_jmp32(buf);

    /* --- IC hit handlers --- */
    uint32_t ic_hit_starts[VTX_POLY_IC_SIZE];
    for (int i = 0; i < VTX_POLY_IC_SIZE; i++) {
        ic_hit_starts[i] = vtx_code_buffer_position(buf);

        /* Patch the je for this entry */
        int32_t hit_disp = (int32_t)ic_hit_starts[i] - (int32_t)(ic_hit_positions[i] + 4);
        vtx_code_buffer_patch_dword(buf, ic_hit_positions[i], (uint32_t)hit_disp);

        /* Load the cached method pointer: mov rdi, [r10 + 16 + i*8] */
        emit_rex64(buf, VTX_REG_RDI, VTX_REG_R10);
        vtx_code_buffer_emit_byte(buf, 0x8B);  /* MOV r64, r/m64 */
        int32_t target_off = VTX_POLY_IC_TARGETS_OFFSET + i * VTX_POLY_IC_ENTRY_SIZE_TARGET;
        if (target_off >= -128 && target_off <= 127) {
            vtx_code_buffer_emit_byte(buf, modrm(1, VTX_REG_RDI, VTX_REG_R10));
            vtx_code_buffer_emit_byte(buf, (uint8_t)(target_off & 0xFF));
        } else {
            vtx_code_buffer_emit_byte(buf, modrm(2, VTX_REG_RDI, VTX_REG_R10));
            vtx_code_buffer_emit_dword(buf, (uint32_t)target_off);
        }

        /* Check if the target is NULL (uninitialized IC entry) */
    }

    /* After loading the target, check if it's non-NULL and call it.
     * For simplicity, all 4 hit handlers fall through to a common check. */
    /* cmp rdi, 0 */
    emit_cmp_reg_imm32(buf, VTX_REG_RDI, 0);
    /* je ic_miss — if target is NULL, treat as miss */
    uint32_t je_null_target = emit_jcc32(buf, CC_E);

    /* Target is valid — set up args and call.
     * The callee expects: RDI=method, RSI=deopt_info, RDX=profile_data */
    /* RDI already has the method pointer */
    emit_mov_reg_rbp_offset(buf, VTX_REG_RSI, VTX_FRAME_DEOPT_INFO_OFFSET);
    emit_mov_reg_rbp_offset(buf, VTX_REG_RDX, VTX_FRAME_PROFILE_DATA_OFFSET);

    /* Call the method's compiled code: call [rdi + offsetof(compiled_code)] */
    /* offsetof(vtx_method_desc_t, compiled_code) = 24 (after name:8, signature:8, bytecode:8) */
    emit_rex64(buf, VTX_REG_RAX, VTX_REG_RDI);
    vtx_code_buffer_emit_byte(buf, 0xFF);  /* CALL r/m64 */
    vtx_code_buffer_emit_byte(buf, modrm(2, 2, VTX_REG_RDI));  /* /2 = CALL, mod=2 for disp32 */
    vtx_code_buffer_emit_dword(buf, 24);  /* offset of compiled_code in vtx_method_desc_t */

    /* Jump to restore */
    uint32_t jmp_restore2 = emit_jmp32(buf);

    /* --- Null target handler (IC entry uninitialized) --- */
    {
        uint32_t null_target_start = vtx_code_buffer_position(buf);
        int32_t null_disp = (int32_t)null_target_start - (int32_t)(je_null_target + 4);
        vtx_code_buffer_patch_dword(buf, je_null_target, (uint32_t)null_disp);
    }
    /* Fall through to IC miss path — re-emit the miss code using D8 convention */
    emit_mov_reg_rbp_offset(buf, VTX_REG_RDI, VTX_FRAME_METHOD_PTR_OFFSET);
    emit_mov_reg_rbp_offset(buf, VTX_REG_RSI, VTX_FRAME_DEOPT_INFO_OFFSET);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_runtime_call_virtual_reg);
    emit_call_reg(buf, VTX_REG_RAX);

    /* --- Restore registers --- */
    uint32_t restore_start = vtx_code_buffer_position(buf);

    /* Patch jmp_restore and jmp_restore2 */
    int32_t restore_disp1 = (int32_t)restore_start - (int32_t)(jmp_restore + 4);
    vtx_code_buffer_patch_dword(buf, jmp_restore, (uint32_t)restore_disp1);
    int32_t restore_disp2 = (int32_t)restore_start - (int32_t)(jmp_restore2 + 4);
    vtx_code_buffer_patch_dword(buf, jmp_restore2, (uint32_t)restore_disp2);

    emit_pop(buf, VTX_REG_RBX);
    emit_pop(buf, VTX_REG_RDX);
    emit_pop(buf, VTX_REG_RCX);
    emit_pop(buf, VTX_REG_RAX);

    emit_stack_push(ctx);

    /* Store the IC pointer in the compiled code's metadata for cleanup
     * when the method is evicted from the code cache. */
    if (ctx->poly_ic_count >= ctx->poly_ic_capacity) {
        uint32_t new_cap = ctx->poly_ic_capacity == 0 ? 8 : ctx->poly_ic_capacity * 2;
        vtx_poly_ic_t **new_arr = (vtx_poly_ic_t **)realloc(
            ctx->poly_ics, new_cap * sizeof(vtx_poly_ic_t *));
        if (new_arr) {
            ctx->poly_ics = new_arr;
            ctx->poly_ic_capacity = new_cap;
        }
    }
    if (ctx->poly_ic_count < ctx->poly_ic_capacity) {
        ctx->poly_ics[ctx->poly_ic_count++] = ic;
    } else {
        /* Could not track — free immediately to avoid leak */
        free(ic);
    }
}

static void compile_call_interface(vtx_compile_ctx_t *ctx, uint16_t method_idx)
{
    vtx_code_buffer_t *buf = &ctx->buf;

    /* Interface calls use the same IC mechanism as virtual calls.
     * The key difference is that the runtime helper also checks
     * interface implementation on IC miss. */

    if (ctx->profile_data) {
        vtx_instrument_emit_call_type_record(buf, ctx->profile_data,
            ctx->bc_pc, VTX_REG_NONE, VTX_REG_NONE);
    }

    /* Allocate a poly IC for this interface call site */
    vtx_poly_ic_t *ic = (vtx_poly_ic_t *)calloc(1, sizeof(vtx_poly_ic_t));
    if (!ic) {
        /* Allocation failed — fall back to non-IC path using D8 convention */
        emit_push(buf, VTX_REG_RAX);
        emit_push(buf, VTX_REG_RCX);
        emit_push(buf, VTX_REG_RDX);
        emit_push(buf, VTX_REG_RBX);

        emit_mov_reg_rbp_offset(buf, VTX_REG_RDI, VTX_FRAME_METHOD_PTR_OFFSET);
        emit_mov_reg_rbp_offset(buf, VTX_REG_RSI, VTX_FRAME_DEOPT_INFO_OFFSET);

        /* vtx_runtime_call_interface_reg declared in runtime/helpers.h */
        emit_mov_reg_imm64(buf, VTX_REG_RAX,
            (uint64_t)(uintptr_t)vtx_runtime_call_interface_reg);
        emit_call_reg(buf, VTX_REG_RAX);

        emit_pop(buf, VTX_REG_RBX);
        emit_pop(buf, VTX_REG_RDX);
        emit_pop(buf, VTX_REG_RCX);
        emit_pop(buf, VTX_REG_RAX);

        emit_stack_push(ctx);
        return;
    }

    /* --- Save expression stack registers --- */
    emit_push(buf, VTX_REG_RAX);
    emit_push(buf, VTX_REG_RCX);
    emit_push(buf, VTX_REG_RDX);
    emit_push(buf, VTX_REG_RBX);

    /* --- Load receiver from saved stack slot --- */
    /* After 4 pushes, saved RAX (receiver) is at [RSP + 24] */
    vtx_code_buffer_emit_byte(buf, REX_W);
    vtx_code_buffer_emit_byte(buf, 0x8B);
    vtx_code_buffer_emit_byte(buf, modrm(1, VTX_REG_RDI, VTX_REG_RSP));
    vtx_code_buffer_emit_byte(buf, 24);

    /* --- Load IC pointer into R10 --- */
    emit_mov_reg_imm64(buf, VTX_REG_R10, (uint64_t)(uintptr_t)ic);

    /* --- Check if receiver is a heap pointer --- */
    emit_mov_reg_reg64(buf, VTX_REG_R11, VTX_REG_RDI);
    emit_mov_reg_imm64(buf, VTX_REG_R12, 0x7);
    emit_and_reg_reg(buf, VTX_REG_R11, VTX_REG_R12);
    emit_cmp_reg_imm32(buf, VTX_REG_R11, VTX_TAG_HEAP_PTR);
    uint32_t jne_not_heap = emit_jcc32(buf, CC_NE);

    /* --- Extract receiver's type_id --- */
    emit_untag_heap_ptr(ctx, VTX_REG_RDI, VTX_REG_R11);
    emit_rex32_if_needed(buf, VTX_REG_R11, VTX_REG_R11);
    vtx_code_buffer_emit_byte(buf, 0x8B);
    vtx_code_buffer_emit_byte(buf, modrm(1, VTX_REG_R11, VTX_REG_R11));
    vtx_code_buffer_emit_byte(buf, (uint8_t)0);

    /* --- Probe IC entries --- */
    uint32_t ic_hit_positions[VTX_POLY_IC_SIZE];
    for (int i = 0; i < VTX_POLY_IC_SIZE; i++) {
        emit_rex32_if_needed(buf, VTX_REG_R11, VTX_REG_R10);
        vtx_code_buffer_emit_byte(buf, 0x39);
        vtx_code_buffer_emit_byte(buf, modrm(1, VTX_REG_R11, VTX_REG_R10));
        vtx_code_buffer_emit_byte(buf, (uint8_t)(i * VTX_POLY_IC_ENTRY_SIZE_TYPE));
        ic_hit_positions[i] = emit_jcc32(buf, CC_E);
    }

    /* --- IC miss --- */
    uint32_t ic_miss_start = vtx_code_buffer_position(buf);
    {
        int32_t disp = (int32_t)ic_miss_start - (int32_t)(jne_not_heap + 4);
        vtx_code_buffer_patch_dword(buf, jne_not_heap, (uint32_t)disp);
    }

    emit_mov_reg_rbp_offset(buf, VTX_REG_RDI, VTX_FRAME_METHOD_PTR_OFFSET);
    emit_mov_reg_rbp_offset(buf, VTX_REG_RSI, VTX_FRAME_DEOPT_INFO_OFFSET);

    /* vtx_runtime_call_interface_reg declared in runtime/helpers.h */
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_runtime_call_interface_reg);
    emit_call_reg(buf, VTX_REG_RAX);

    uint32_t jmp_restore = emit_jmp32(buf);

    /* --- IC hit handlers --- */
    for (int i = 0; i < VTX_POLY_IC_SIZE; i++) {
        uint32_t hit_start = vtx_code_buffer_position(buf);
        int32_t hit_disp = (int32_t)hit_start - (int32_t)(ic_hit_positions[i] + 4);
        vtx_code_buffer_patch_dword(buf, ic_hit_positions[i], (uint32_t)hit_disp);

        /* Load cached method pointer */
        emit_rex64(buf, VTX_REG_RDI, VTX_REG_R10);
        vtx_code_buffer_emit_byte(buf, 0x8B);
        int32_t target_off = VTX_POLY_IC_TARGETS_OFFSET + i * VTX_POLY_IC_ENTRY_SIZE_TARGET;
        if (target_off >= -128 && target_off <= 127) {
            vtx_code_buffer_emit_byte(buf, modrm(1, VTX_REG_RDI, VTX_REG_R10));
            vtx_code_buffer_emit_byte(buf, (uint8_t)(target_off & 0xFF));
        } else {
            vtx_code_buffer_emit_byte(buf, modrm(2, VTX_REG_RDI, VTX_REG_R10));
            vtx_code_buffer_emit_dword(buf, (uint32_t)target_off);
        }
    }

    /* Common target check and call */
    emit_cmp_reg_imm32(buf, VTX_REG_RDI, 0);
    uint32_t je_null_target = emit_jcc32(buf, CC_E);

    /* Set up args and call */
    emit_mov_reg_rbp_offset(buf, VTX_REG_RSI, VTX_FRAME_DEOPT_INFO_OFFSET);
    emit_mov_reg_rbp_offset(buf, VTX_REG_RDX, VTX_FRAME_PROFILE_DATA_OFFSET);

    /* call [rdi + 24] — call compiled_code */
    emit_rex64(buf, VTX_REG_RAX, VTX_REG_RDI);
    vtx_code_buffer_emit_byte(buf, 0xFF);
    vtx_code_buffer_emit_byte(buf, modrm(2, 2, VTX_REG_RDI));
    vtx_code_buffer_emit_dword(buf, 24);

    uint32_t jmp_restore2 = emit_jmp32(buf);

    /* Null target handler */
    {
        uint32_t null_start = vtx_code_buffer_position(buf);
        int32_t null_disp = (int32_t)null_start - (int32_t)(je_null_target + 4);
        vtx_code_buffer_patch_dword(buf, je_null_target, (uint32_t)null_disp);
    }
    emit_mov_reg_rbp_offset(buf, VTX_REG_RDI, VTX_FRAME_METHOD_PTR_OFFSET);
    emit_mov_reg_rbp_offset(buf, VTX_REG_RSI, VTX_FRAME_DEOPT_INFO_OFFSET);
    emit_mov_reg_imm64(buf, VTX_REG_RAX,
        (uint64_t)(uintptr_t)vtx_runtime_call_interface_reg);
    emit_call_reg(buf, VTX_REG_RAX);

    /* --- Restore registers --- */
    uint32_t restore_start = vtx_code_buffer_position(buf);
    int32_t rd1 = (int32_t)restore_start - (int32_t)(jmp_restore + 4);
    vtx_code_buffer_patch_dword(buf, jmp_restore, (uint32_t)rd1);
    int32_t rd2 = (int32_t)restore_start - (int32_t)(jmp_restore2 + 4);
    vtx_code_buffer_patch_dword(buf, jmp_restore2, (uint32_t)rd2);

    emit_pop(buf, VTX_REG_RBX);
    emit_pop(buf, VTX_REG_RDX);
    emit_pop(buf, VTX_REG_RCX);
    emit_pop(buf, VTX_REG_RAX);

    emit_stack_push(ctx);

    /* Store the IC pointer in the compiled code's metadata for cleanup
     * when the method is evicted from the code cache. */
    if (ctx->poly_ic_count >= ctx->poly_ic_capacity) {
        uint32_t new_cap = ctx->poly_ic_capacity == 0 ? 8 : ctx->poly_ic_capacity * 2;
        vtx_poly_ic_t **new_arr = (vtx_poly_ic_t **)realloc(
            ctx->poly_ics, new_cap * sizeof(vtx_poly_ic_t *));
        if (new_arr) {
            ctx->poly_ics = new_arr;
            ctx->poly_ic_capacity = new_cap;
        }
    }
    if (ctx->poly_ic_count < ctx->poly_ic_capacity) {
        ctx->poly_ics[ctx->poly_ic_count++] = ic;
    } else {
        /* Could not track — free immediately to avoid leak */
        free(ic);
    }
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
    /* Tag the heap pointer as a NaN-boxed value.
     * Encoding: VTX_NAN_BOX_HEADER | (ptr >> 3 << 3) | VTX_TAG_HEAP_PTR
     * The >>3 then <<3 clears the low 3 bits making room for the tag,
     * then OR in the NaN header and HEAP_PTR tag. */
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
    buf->bytes[je_pos] = (uint8_t)je_disp;
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

    /* Compare RAX (type_id) with typeid_ */
    emit_cmp_reg_imm32(buf, VTX_REG_RAX, (int32_t)typeid_);

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
    buf->bytes[je_pos] = (uint8_t)je_disp;

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
    vtx_code_buffer_emit_byte(buf, modrm(0, VTX_REG_RAX, (vtx_reg_t)4)); /* rm=4 signals SIB */
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
    vtx_code_buffer_emit_byte(buf, modrm(0, VTX_REG_R12, (vtx_reg_t)4)); /* rm=4 signals SIB */
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

    vtx_code_buffer_t *buf = &ctx->buf;

    /* Record the handler in the side table */
    if (ctx->side_table) {
        vtx_side_table_add_entry(ctx->side_table,
            vtx_code_buffer_position(buf), 0, 0);
    }
    (void)handler_pc;
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
                                           vtx_arena_t *arena,
                                           vtx_code_cache_t *cache,
                                           vtx_method_registry_t *registry)
{
    if (!method || !method->bytecode) return NULL;

    vtx_compile_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.method = method;
    ctx.bc = method->bytecode;
    ctx.profile_data = profile_data;
    ctx.arena = arena;
    ctx.cache = cache;
    ctx.registry = registry;
    ctx.layout = vtx_frame_layout_compute(method);
    ctx.method_id = method->vtable_index != 0xFFFFFFFF ?
                     method->vtable_index : (uint32_t)(uintptr_t)method;

    /* If the method_id is absurdly large (e.g., from a truncated pointer
     * address), use a simple monotonic counter instead. This prevents
     * vtx_method_registry_add from trying to allocate a huge array. */
    if (ctx.method_id > 100000) {
        static uint32_t next_method_id = 1;
        ctx.method_id = next_method_id++;
    }

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

    if (ctx.cache && ctx.registry) {
        /* Install through the code cache's proper installation path.
         * This handles: allocating cache space, copying code, mprotect to
         * executable, registering in the method registry, and updating the
         * method's code pointer atomically. */
        bool installed = vtx_install_method(ctx.cache, ctx.registry,
            method, ctx.method_id,
            ctx.buf.bytes, result->code_size,
            ctx.side_table,
            NULL,  /* reloc_table: baseline JIT has no external call relocations */
            NULL, 0,  /* no dependency type info for baseline JIT */
            NULL, 0,  /* no dependency shape info for baseline JIT */
            ctx.arena,
            ctx.poly_ics, ctx.poly_ic_count);
        if (installed) {
            /* Code is now in the cache; set entry_point to the installed code.
             * Do NOT set result->code to the cache memory — it's not malloc'd
             * and must not be freed by vtx_compiled_code_destroy().
             * The side_table ownership was transferred to the compiled_method. */
            result->code = NULL;
            result->entry_point = method->compiled_code;
            result->side_table = NULL; /* ownership transferred to cache */
        } else {
            /* Installation failed — fall back to malloc+memcpy */
            result->code = (uint8_t *)malloc(result->code_size);
            if (!result->code) {
                free(result);
                vtx_guard_array_destroy(&ctx.guards);
                vtx_code_buffer_destroy(&ctx.buf);
                vtx_deopt_stub_array_destroy(&deopt_stubs);
                return NULL;
            }
            memcpy(result->code, ctx.buf.bytes, result->code_size);
            result->side_table = ctx.side_table;
        }
    } else {
        /* No code cache available — fall back to malloc+memcpy.
         * The caller must handle making the code executable and installing
         * the method's code pointer. */
        result->code = (uint8_t *)malloc(result->code_size);
        if (!result->code) {
            free(result);
            vtx_guard_array_destroy(&ctx.guards);
            vtx_code_buffer_destroy(&ctx.buf);
            vtx_deopt_stub_array_destroy(&deopt_stubs);
            return NULL;
        }
        memcpy(result->code, ctx.buf.bytes, result->code_size);
        result->side_table = ctx.side_table;
    }

    result->frame_layout = ctx.layout;
    result->guards = ctx.guards;
    result->deopt_stubs = deopt_stubs;
    /* side_table is set above: NULL if installed via cache (ownership
     * transferred to compiled_method), or ctx.side_table if malloc'd. */
    result->method = method;

    /* Copy PC maps */
    result->bc_pc_map = ctx.pc_map;
    result->bc_pc_map_count = ctx.pc_map_count;

    result->native_to_bc_pc = ctx.native_to_bc;
    result->native_to_bc_pc_count = ctx.native_to_bc_count;

    /* Build deopt info — BS-2 fix: store both native offsets AND bytecode PCs */
    result->deopt_info = (vtx_deopt_info_t *)malloc(sizeof(vtx_deopt_info_t));
    if (result->deopt_info) {
        result->deopt_info->method = method;
        result->deopt_info->pc_map_count = ctx.pc_map_count;
        result->deopt_info->native_offsets = (uint32_t *)malloc(ctx.pc_map_count * sizeof(uint32_t));
        result->deopt_info->pc_map = (uint32_t *)malloc(ctx.pc_map_count * sizeof(uint32_t));
        result->deopt_info->stack_depth_map = (uint32_t *)malloc(ctx.pc_map_count * sizeof(uint32_t));
        if (result->deopt_info->native_offsets && result->deopt_info->pc_map && result->deopt_info->stack_depth_map) {
            for (uint32_t i = 0; i < ctx.pc_map_count; i++) {
                result->deopt_info->native_offsets[i] = ctx.pc_map[i].native_offset;
                result->deopt_info->pc_map[i] = ctx.pc_map[i].bytecode_pc;
                result->deopt_info->stack_depth_map[i] = ctx.pc_map[i].stack_depth;
            }
        }
    }

    /* Clean up */
    vtx_code_buffer_destroy(&ctx.buf);
    free(ctx.fixups);

    /* Transfer poly IC ownership to the result so they can be freed
     * when the compiled method is evicted or the code is destroyed.
     * If the code was installed via the cache, the compiled_method_t
     * takes ownership; otherwise the compiled_code_t owns them. */
    if (ctx.cache && ctx.registry && result->entry_point != NULL) {
        /* Code was installed — poly_ics will be transferred to
         * compiled_method_t via vtx_install_method. Free our copy
         * of the pointer array (the ICs themselves are now owned
         * by compiled_method_t). */
        free(ctx.poly_ics);
    } else {
        /* Code was NOT installed via cache — the result owns the ICs.
         * Store them in the result for later cleanup. */
        result->poly_ics = ctx.poly_ics;
        result->poly_ic_count = ctx.poly_ic_count;
    }

    return result;
}
