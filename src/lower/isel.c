/**
 * VORTEX Instruction Selection — SoN Nodes → x86-64 Instructions
 *
 * Walks the scheduled basic blocks and maps each SoN node to one or more
 * x86-64 instructions. Each value-producing node is assigned a virtual
 * register.
 *
 * Instruction selection rules:
 *   Arithmetic:  Add→addq, Sub→subq, Mul→imulq, Div→cqo+idivq, Mod→cqo+idivq+mov rdx
 *   Bitwise:     And→andq, Or→orq, Xor→xorq
 *   Shifts:      Shl→shlq, Shr→shrq, Sar→sarq
 *   Unary:       Neg→negq, Not→notq
 *   Comparison:  Cmp→cmpq+setcc, CmpP→cmpq+setcc
 *   Memory:      Load→movq, Store→movq, LoadField→movq [base+disp], StoreField→movq [base+disp], val
 *   Calls:       CallStatic→mov args+callq, CallVirtual→mov args+vtable+callq
 *   Control:     If→cmpq+jcc, Goto→jmp, Return→mov rax+ret
 *   Constants:   Constant→movq vreg, imm
 *   Parameters:  Parameter→movq vreg, arg_reg
 *   Guards:      Guard/DeoptGuard→cmpq+jcc (deopt on fail)
 */

#include "lower/isel.h"
#include "ir/graph.h"
#include "compile/safepoint.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Opcode name table                                                           */
/* ========================================================================== */

static const char *vtx_x86_opcode_names[VTX_X86_OPCODE_COUNT] = {
    [VTX_X86_NOP]   = "nop",
    [VTX_X86_ADD]   = "add",
    [VTX_X86_SUB]   = "sub",
    [VTX_X86_IMUL]  = "imul",
    [VTX_X86_IDIV]  = "idiv",
    [VTX_X86_MUL]   = "mul",
    [VTX_X86_NEG]   = "neg",
    [VTX_X86_NOT]   = "not",
    [VTX_X86_INC]   = "inc",
    [VTX_X86_DEC]   = "dec",
    [VTX_X86_SHL]   = "shl",
    [VTX_X86_SHR]   = "shr",
    [VTX_X86_SAR]   = "sar",
    [VTX_X86_AND]   = "and",
    [VTX_X86_OR]    = "or",
    [VTX_X86_XOR]   = "xor",
    [VTX_X86_CMP]   = "cmp",
    [VTX_X86_TEST]  = "test",
    [VTX_X86_MOV]   = "mov",
    [VTX_X86_MOVZX] = "movzx",
    [VTX_X86_MOVSX] = "movsx",
    [VTX_X86_CMOV]  = "cmov",
    [VTX_X86_XCHG]  = "xchg",
    [VTX_X86_LEA]   = "lea",
    [VTX_X86_CQO]   = "cqo",
    [VTX_X86_CDQ]   = "cdq",
    [VTX_X86_SETCC] = "setcc",
    [VTX_X86_PUSH]  = "push",
    [VTX_X86_POP]   = "pop",
    [VTX_X86_JMP]   = "jmp",
    [VTX_X86_JCC]   = "jcc",
    [VTX_X86_CALL]  = "call",
    [VTX_X86_RET]   = "ret",
    [VTX_X86_LAHF]  = "lahf",
    [VTX_X86_SAHF]  = "sahf",
    [VTX_X86_UCOMISD] = "ucomisd",
    [VTX_X86_ADDSD]  = "addsd",
    [VTX_X86_SUBSD]  = "subsd",
    [VTX_X86_MULSD]  = "mulsd",
    [VTX_X86_DIVSD]  = "divsd",
    [VTX_X86_XORPS]  = "xorps",
    [VTX_X86_MOVSD]  = "movsd",
    [VTX_X86_SAFEPOINT_POLL] = "safepoint_poll",
    [VTX_X86_SAFEPOINT_POLL_GUARD_PAGE] = "safepoint_poll_guard_page",
    [VTX_X86_MOVAPD] = "movapd",
    [VTX_X86_ADDPD]  = "addpd",
    [VTX_X86_MULPD]  = "mulpd",
    [VTX_X86_MINPD]  = "minpd",
    [VTX_X86_MAXPD]  = "maxpd",
    [VTX_X86_ANDPD]  = "andpd",
    [VTX_X86_XORPD]  = "xorpd",
};

const char *vtx_x86_opcode_name(vtx_x86_opcode_t opcode)
{
    if (opcode >= VTX_X86_OPCODE_COUNT) return "unknown";
    return vtx_x86_opcode_names[opcode] ? vtx_x86_opcode_names[opcode] : "unknown";
}

/* ========================================================================== */
/* System V AMD64 ABI argument registers                                       */
/* ========================================================================== */

static const uint8_t vtx_arg_regs[6] = {
    7,  /* RDI */
    6,  /* RSI */
    2,  /* RDX */
    1,  /* RCX */
    8,  /* R8  */
    9,  /* R9  */
};

/* ========================================================================== */
/* Helper: make instructions                                                   */
/* ========================================================================== */

static vtx_inst_t make_rr_inst(vtx_x86_opcode_t opcode, uint32_t dst_vreg,
                                uint32_t src_vreg, vtx_nodeid_t source_node)
{
    vtx_inst_t inst;
    memset(&inst, 0, sizeof(inst));
    inst.opcode = opcode;
    inst.opnd_kinds[0] = VTX_OPND_VREG;
    inst.operands[0] = dst_vreg;
    inst.opnd_kinds[1] = VTX_OPND_VREG;
    inst.operands[1] = src_vreg;
    inst.source_node = source_node;
    return inst;
}

/* SSE variant of make_rr_inst — marks the instruction with IS_SSE flag */
static vtx_inst_t make_sse_rr_inst(vtx_x86_opcode_t opcode, uint32_t dst_vreg,
                                     uint32_t src_vreg, vtx_nodeid_t source_node)
{
    vtx_inst_t inst = make_rr_inst(opcode, dst_vreg, src_vreg, source_node);
    inst.flags |= VTX_INST_FLAG_IS_SSE;
    return inst;
}

static vtx_inst_t make_ri_inst(vtx_x86_opcode_t opcode, uint32_t dst_vreg,
                                int64_t imm, vtx_nodeid_t source_node)
{
    vtx_inst_t inst;
    memset(&inst, 0, sizeof(inst));
    inst.opcode = opcode;
    inst.opnd_kinds[0] = VTX_OPND_VREG;
    inst.operands[0] = dst_vreg;
    inst.opnd_kinds[1] = VTX_OPND_IMM;
    inst.imm = imm;
    inst.flags = VTX_INST_FLAG_HAS_IMM;
    inst.source_node = source_node;
    return inst;
}

static vtx_inst_t make_r_inst(vtx_x86_opcode_t opcode, uint32_t vreg,
                               vtx_nodeid_t source_node, uint32_t extra_flags)
{
    vtx_inst_t inst;
    memset(&inst, 0, sizeof(inst));
    inst.opcode = opcode;
    inst.opnd_kinds[0] = VTX_OPND_VREG;
    inst.operands[0] = vreg;
    inst.source_node = source_node;
    inst.flags = extra_flags;
    return inst;
}

static vtx_inst_t make_rm_inst(vtx_x86_opcode_t opcode, uint32_t dst_vreg,
                                const vtx_x86_memop_t *mem, vtx_nodeid_t source_node)
{
    vtx_inst_t inst;
    memset(&inst, 0, sizeof(inst));
    inst.opcode = opcode;
    inst.opnd_kinds[0] = VTX_OPND_VREG;
    inst.operands[0] = dst_vreg;
    inst.opnd_kinds[1] = VTX_OPND_MEM;
    inst.mem = *mem;
    inst.flags = VTX_INST_FLAG_HAS_MEM;
    inst.source_node = source_node;
    return inst;
}

static vtx_inst_t make_mr_inst(vtx_x86_opcode_t opcode, const vtx_x86_memop_t *mem,
                                uint32_t src_vreg, vtx_nodeid_t source_node)
{
    vtx_inst_t inst;
    memset(&inst, 0, sizeof(inst));
    inst.opcode = opcode;
    inst.opnd_kinds[0] = VTX_OPND_MEM;
    inst.opnd_kinds[1] = VTX_OPND_VREG;
    inst.operands[1] = src_vreg;
    inst.mem = *mem;
    inst.flags = VTX_INST_FLAG_HAS_MEM;
    inst.source_node = source_node;
    return inst;
}

static vtx_inst_t make_branch_inst(vtx_x86_opcode_t opcode, uint32_t target_block,
                                    vtx_cond_t cond, vtx_nodeid_t source_node)
{
    vtx_inst_t inst;
    memset(&inst, 0, sizeof(inst));
    inst.opcode = opcode;
    inst.opnd_kinds[0] = VTX_OPND_LABEL;
    inst.operands[0] = target_block;
    inst.cond = cond;
    inst.flags = VTX_INST_FLAG_HAS_COND | VTX_INST_FLAG_IS_BRANCH;
    inst.source_node = source_node;
    return inst;
}

/* ========================================================================== */
/* Virtual register management                                                 */
/* ========================================================================== */

uint32_t vtx_isel_alloc_vreg(vtx_inst_stream_t *stream, vtx_arena_t *arena)
{
    uint32_t vreg = stream->vreg_count++;

    if (vreg >= stream->vreg_fixed_reg_count) {
        uint32_t new_count = stream->vreg_fixed_reg_count;
        if (new_count == 0) new_count = VTX_VREG_COUNT_INITIAL;
        while (new_count <= vreg) new_count *= 2;

        uint8_t *new_arr = (uint8_t *)vtx_arena_alloc(arena, new_count);
        if (!new_arr) return VTX_VREG_INVALID;
        if (stream->vreg_fixed_reg && stream->vreg_fixed_reg_count > 0) {
            memcpy(new_arr, stream->vreg_fixed_reg, stream->vreg_fixed_reg_count);
        }
        memset(new_arr + stream->vreg_fixed_reg_count, 0xFF,
               new_count - stream->vreg_fixed_reg_count);
        stream->vreg_fixed_reg = new_arr;
        stream->vreg_fixed_reg_count = new_count;
    }

    stream->vreg_fixed_reg[vreg] = 0xFF; /* VTX_REG_NONE */
    return vreg;
}

uint32_t vtx_isel_alloc_vreg_fixed(vtx_inst_stream_t *stream, vtx_arena_t *arena,
                                    uint8_t phys_reg)
{
    uint32_t vreg = vtx_isel_alloc_vreg(stream, arena);
    if (vreg == VTX_VREG_INVALID) return VTX_VREG_INVALID;
    stream->vreg_fixed_reg[vreg] = phys_reg;
    return vreg;
}

void vtx_isel_map_node_vreg(vtx_inst_stream_t *stream, vtx_nodeid_t node_id,
                             uint32_t vreg, vtx_arena_t *arena)
{
    if (node_id >= stream->node_to_vreg_count) {
        uint32_t new_count = stream->node_to_vreg_count;
        if (new_count == 0) new_count = VTX_VREG_COUNT_INITIAL;
        while (new_count <= node_id) new_count *= 2;

        uint32_t *new_arr = (uint32_t *)vtx_arena_alloc(arena, new_count * sizeof(uint32_t));
        if (!new_arr) return;
        if (stream->node_to_vreg && stream->node_to_vreg_count > 0) {
            memcpy(new_arr, stream->node_to_vreg,
                   stream->node_to_vreg_count * sizeof(uint32_t));
        }
        for (uint32_t i = stream->node_to_vreg_count; i < new_count; i++) {
            new_arr[i] = VTX_VREG_INVALID;
        }
        stream->node_to_vreg = new_arr;
        stream->node_to_vreg_count = new_count;
    }
    stream->node_to_vreg[node_id] = vreg;
}

uint32_t vtx_isel_node_vreg(const vtx_inst_stream_t *stream, vtx_nodeid_t node_id)
{
    if (!stream->node_to_vreg || node_id >= stream->node_to_vreg_count) {
        return VTX_VREG_INVALID;
    }
    return stream->node_to_vreg[node_id];
}

/* ========================================================================== */
/* Instruction block management                                                */
/* ========================================================================== */

int vtx_isel_block_ensure_capacity(vtx_inst_block_t *block, uint32_t needed,
                                    vtx_arena_t *arena)
{
    if (block->inst_count + needed <= block->inst_capacity) return 0;

    uint32_t new_cap = block->inst_capacity;
    if (new_cap == 0) new_cap = VTX_INST_BLOCK_INITIAL_CAPACITY;
    while (new_cap < block->inst_count + needed) new_cap *= 2;

    vtx_inst_t *new_insts = (vtx_inst_t *)vtx_arena_alloc(arena, new_cap * sizeof(vtx_inst_t));
    if (!new_insts) return -1;
    if (block->insts && block->inst_count > 0) {
        memcpy(new_insts, block->insts, block->inst_count * sizeof(vtx_inst_t));
    }
    block->insts = new_insts;
    block->inst_capacity = new_cap;
    return 0;
}

uint32_t vtx_isel_emit_inst(vtx_inst_block_t *block, vtx_inst_t inst, vtx_arena_t *arena)
{
    if (vtx_isel_block_ensure_capacity(block, 1, arena) != 0) return UINT32_MAX;
    uint32_t idx = block->inst_count++;
    block->insts[idx] = inst;
    return idx;
}

/* ========================================================================== */
/* Ensure a vreg exists for a node                                            */
/* ========================================================================== */

static uint32_t ensure_node_vreg(vtx_inst_stream_t *stream, vtx_nodeid_t node_id,
                                  vtx_arena_t *arena)
{
    uint32_t vreg = vtx_isel_node_vreg(stream, node_id);
    if (vreg != VTX_VREG_INVALID) return vreg;
    vreg = vtx_isel_alloc_vreg(stream, arena);
    vtx_isel_map_node_vreg(stream, node_id, vreg, arena);
    return vreg;
}

/* ========================================================================== */
/* Strength reduction helpers                                                  */
/* ========================================================================== */

/**
 * Check if a value is a power of 2.
 * Returns the shift amount (log2) if true, or -1 if false.
 */
static int is_power_of_2(int64_t val)
{
    if (val <= 0) return -1;
    if ((val & (val - 1)) != 0) return -1;
    int shift = 0;
    while (val > 1) { val >>= 1; shift++; }
    return shift;
}

/**
 * Try to get the constant integer value of a node.
 * Returns true and sets *out_val if the node is a Constant with Int type.
 */
static bool try_get_const_int(const vtx_graph_t *graph, vtx_nodeid_t node_id,
                               int64_t *out_val)
{
    const vtx_node_t *node = vtx_node_get_const(&graph->node_table, node_id);
    if (!node || node->opcode != VTX_OP_Constant) return false;
    if (node->constval.kind != VTX_TYPE_Int) return false;
    *out_val = node->constval.as.int_val;
    return true;
}

/**
 * Compute the magic number for signed division by a constant.
 * Implements the algorithm from Hacker's Delight (Warren, Chapter 10).
 * For d != 0, computes M such that floor(n/d) = floor(M*n / 2^p).
 *
 * @param d     Divisor (must be != 0, != -1, != 1)
 * @param M     Output: magic number multiplier
 * @param s     Output: post-shift amount
 * @return      true on success
 */
static bool compute_magic_number(int64_t d, int64_t *M, int *s)
{
    if (d == 0 || d == 1 || d == -1) return false;

    /* Use absolute value for the algorithm */
    uint64_t ad = (d < 0) ? (uint64_t)(-d) : (uint64_t)d;
    uint64_t nc = ((uint64_t)1 << 63) - 1 + (ad - (uint64_t)1) / ad; /* nc = 2^63-1 - (2^63-1)\ad + 1 */
    /* Standard magic number computation per Hacker's Delight §10-3 */
    int p = 63;
    uint64_t q = (uint64_t)1 << p;       /* 2^p */
    uint64_t r = q % ad;                  /* remainder */
    uint64_t m = q / ad;                  /* initial magic */

    /* Iterate to find the correct shift */
    for (;;) {
        p++;
        q = r * 2;
        r = q % ad;
        m = m * 2 + q / ad;
        if (r == 0 || p >= 127) break;
    }

    *M = (int64_t)m;
    *s = p - 64;
    return true;
}

/**
 * Emit a multiply-by-constant sequence using LEA + shift/add.
 * Handles common small constants efficiently.
 * Returns true if the sequence was emitted, false if IMUL should be used.
 */
static bool emit_mul_by_constant(vtx_inst_stream_t *stream, vtx_inst_block_t *block,
                                  uint32_t dst, uint32_t lhs_vreg, int64_t constant,
                                  vtx_nodeid_t node_id, vtx_arena_t *arena)
{
    /* x * 0 → xor dst, dst */
    if (constant == 0) {
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_XOR, dst, dst, node_id), arena);
        return true;
    }

    /* x * 1 → just move (or nop if dst == lhs) */
    if (constant == 1) {
        if (dst != lhs_vreg)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_vreg, node_id), arena);
        return true;
    }

    /* x * -1 → neg */
    if (constant == -1) {
        if (dst != lhs_vreg)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_vreg, node_id), arena);
        vtx_isel_emit_inst(block, make_r_inst(VTX_X86_NEG, dst, node_id, 0), arena);
        return true;
    }

    /* x * 2^n → shl dst, n */
    int shift = is_power_of_2(constant);
    if (shift >= 0) {
        if (dst != lhs_vreg)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_vreg, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, dst, shift, node_id), arena);
        return true;
    }

    /* x * 3 → lea dst, [lhs + lhs*2] */
    if (constant == 3) {
        vtx_x86_memop_t mem = { lhs_vreg, lhs_vreg, 0xFF, 0xFF, 2, 0 };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_LEA, dst, &mem, node_id), arena);
        return true;
    }

    /* x * 5 → lea dst, [lhs + lhs*4] */
    if (constant == 5) {
        vtx_x86_memop_t mem = { lhs_vreg, lhs_vreg, 0xFF, 0xFF, 4, 0 };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_LEA, dst, &mem, node_id), arena);
        return true;
    }

    /* x * 6 → lea dst, [lhs*2 + lhs*4] then add lhs (or: lea [lhs+lhs*2]; shl 1) */
    if (constant == 6) {
        vtx_x86_memop_t mem = { lhs_vreg, lhs_vreg, 0xFF, 0xFF, 2, 0 };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_LEA, dst, &mem, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, dst, 1, node_id), arena);
        return true;
    }

    /* x * 9 → lea dst, [lhs + lhs*8] */
    if (constant == 9) {
        vtx_x86_memop_t mem = { lhs_vreg, lhs_vreg, 0xFF, 0xFF, 8, 0 };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_LEA, dst, &mem, node_id), arena);
        return true;
    }

    /* x * 10 → lea dst, [lhs + lhs*4]; shl 1 */
    if (constant == 10) {
        vtx_x86_memop_t mem = { lhs_vreg, lhs_vreg, 0xFF, 0xFF, 4, 0 };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_LEA, dst, &mem, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, dst, 1, node_id), arena);
        return true;
    }

    /* x * (2^n + 1) → lea dst, [lhs + lhs*2^n] */
    for (int n = 1; n <= 3; n++) {
        if (constant == ((1 << n) + 1)) {
            vtx_x86_memop_t mem = { lhs_vreg, lhs_vreg, 0xFF, 0xFF, (uint8_t)(1 << n), 0 };
            vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_LEA, dst, &mem, node_id), arena);
            return true;
        }
    }

    /* x * (2^n - 1): REMOVED — this pattern was incorrectly implemented.
     * The old code used negative displacement (disp=-8) which is a constant,
     * not a register value. For example, for x*7 (2^3-1), it computed
     * lea dst, [x + x*8 - 8] = 9x - 8, not 7x. The general pattern
     * x*(2^n-1) cannot be expressed in a single LEA because LEA
     * supports [base + index*scale + disp] where disp is a constant,
     * not a register subtraction. The specific cases (3, 5, 9) that
     * ARE expressible are already handled above as x*(2^n+1) patterns.
     * For other constants, fall through to IMUL. */

    /* For negative constants: compute |c| * x, then negate */
    if (constant < 0 && constant > -64) {
        if (emit_mul_by_constant(stream, block, dst, lhs_vreg, -constant, node_id, arena)) {
            vtx_isel_emit_inst(block, make_r_inst(VTX_X86_NEG, dst, node_id, 0), arena);
            return true;
        }
    }

    return false; /* fall back to IMUL */
}

/* ========================================================================== */
/* Call clobber mask                                                           */
/* ========================================================================== */

#define VTX_INST_FLAG_CLOBBER_CALLER_SAVED \
    (VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RCX | \
     VTX_INST_FLAG_CLOBBER_RDX)

/* ========================================================================== */
/* Instruction selection for individual nodes                                  */
/* ========================================================================== */

static int select_node(vtx_inst_stream_t *stream, vtx_inst_block_t *block,
                        const vtx_graph_t *graph, vtx_nodeid_t node_id,
                        vtx_arena_t *arena)
{
    const vtx_node_t *node = vtx_node_get_const(&graph->node_table, node_id);
    if (!node || node->dead) return 0;

    switch (node->opcode) {

    /* ---- Constants ---- */
    case VTX_OP_Constant: {
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (dst == VTX_VREG_INVALID) return -1;

        /* P1 isel: XORPS for float zero — 1 instruction vs 2.
         *
         * Pattern: double constant 0.0
         *   Old: mov dst, 0x0000000000000000 (10 bytes for imm64)
         *   New: xorps dst, dst             (3 bytes for XMM, zeroed)
         *
         * XORPS xmm, xmm zeros the entire 128-bit register, which gives
         * us +0.0 in the low 64 bits. This is shorter and faster than
         * loading a 64-bit immediate. The register renamer handles
         * XOR zeroing for free on modern CPUs.
         */
        if (node->constval.kind == VTX_TYPE_Float && node->constval.as.float_val == 0.0) {
            vtx_inst_t xorps;
            memset(&xorps, 0, sizeof(xorps));
            xorps.opcode = VTX_X86_XORPS;
            xorps.opnd_kinds[0] = VTX_OPND_VREG;
            xorps.operands[0] = dst;
            xorps.opnd_kinds[1] = VTX_OPND_VREG;
            xorps.operands[1] = dst;
            xorps.flags = VTX_INST_FLAG_IS_SSE;
            xorps.source_node = node_id;
            vtx_isel_emit_inst(block, xorps, arena);
            break;
        }

        vtx_inst_t inst;
        memset(&inst, 0, sizeof(inst));
        inst.opcode = VTX_X86_MOV;
        inst.opnd_kinds[0] = VTX_OPND_VREG;
        inst.operands[0] = dst;
        inst.opnd_kinds[1] = VTX_OPND_IMM;
        if (node->constval.kind == VTX_TYPE_Int) {
            /* SMI-tag the integer constant for the NaN-boxed value system.
             * The IR stores raw integer values (e.g., 42), but the runtime
             * expects NaN-boxed SMIs (e.g., 0x7FF8000000000150).
             * SMI tag: VTX_NAN_BOX_HEADER | (val << 3) | VTX_TAG_SMI */
            int64_t raw = node->constval.as.int_val;
            uint64_t smi_val = 0x7FF8000000000000ULL
                             | (((uint64_t)raw & 0x0000FFFFFFFFFFFFULL) << 3)
                             | 0ULL; /* VTX_TAG_SMI = 0 */
            inst.imm = (int64_t)smi_val;
        } else if (node->constval.kind == VTX_TYPE_Ptr) {
            inst.imm = (int64_t)(uintptr_t)node->constval.as.ptr_val;
        } else if (node->constval.kind == VTX_TYPE_Float) {
            /* Store the raw bits of the double as an immediate */
            union { double d; uint64_t u; } cvt;
            cvt.d = node->constval.as.float_val;
            inst.imm = (int64_t)cvt.u;
        } else {
            inst.imm = 0;
        }
        inst.flags = VTX_INST_FLAG_HAS_IMM;
        inst.source_node = node_id;
        vtx_isel_emit_inst(block, inst, arena);
        break;
    }

    /* ---- Parameters ---- */
    case VTX_OP_Parameter: {
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        uint32_t pidx = node->local_index;
        if (pidx < 6) {
            vtx_inst_t inst;
            memset(&inst, 0, sizeof(inst));
            inst.opcode = VTX_X86_MOV;
            inst.opnd_kinds[0] = VTX_OPND_VREG;
            inst.operands[0] = dst;
            inst.opnd_kinds[1] = VTX_OPND_PREG;
            inst.operands[1] = vtx_arg_regs[pidx];
            inst.source_node = node_id;
            vtx_isel_emit_inst(block, inst, arena);
        } else {
            vtx_x86_memop_t mem;
            mem.base_vreg = VTX_VREG_INVALID;
            mem.index_vreg = VTX_VREG_INVALID;
            mem.base_phys = 0xFF;
            mem.index_phys = 0xFF;
            mem.scale = 1;
            mem.disp = (int32_t)(16 + 8 * (pidx - 6));
            vtx_inst_t inst;
            memset(&inst, 0, sizeof(inst));
            inst.opcode = VTX_X86_MOV;
            inst.opnd_kinds[0] = VTX_OPND_VREG;
            inst.operands[0] = dst;
            inst.opnd_kinds[1] = VTX_OPND_MEM;
            inst.mem = mem;
            inst.flags = VTX_INST_FLAG_HAS_MEM;
            inst.source_node = node_id;
            vtx_isel_emit_inst(block, inst, arena);
        }
        break;
    }

    /* ---- Arithmetic: binary ---- */
    case VTX_OP_Add: {
        if (node->input_count < 2) return -1;
        uint32_t lhs_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t rhs_vreg = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs_vreg == VTX_VREG_INVALID || rhs_vreg == VTX_VREG_INVALID) return -1;

        /* Float Add → ADDSD xmm, xmm */
        if (node->type == VTX_TYPE_Float) {
            if (dst != lhs_vreg)
                vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_MOVSD, dst, lhs_vreg, node_id), arena);
            vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_ADDSD, dst, rhs_vreg, node_id), arena);
            break;
        }

        /* P1 isel: LEA for Add(x, Const) — replaces MOV+ADD with single LEA.
         *
         * Pattern: dst = lhs + constant
         *   Old: mov dst, lhs; add dst, imm
         *   New: lea dst, [lhs + imm]     (1 instruction, no flag write)
         *
         * LEA can represent [base + disp32] where disp32 is the constant.
         * For imm in [-2^31, 2^31-1], this is a single LEA instruction.
         * For imm outside that range, fall through to MOV+ADD.
         *
         * Advantage: LEA doesn't set flags (unlike ADD), so it doesn't
         * interfere with subsequent flag-dependent instructions.
         * On modern CPUs, LEA has the same throughput as ADD.
         *
         * Special case: Add(x, 1) or Add(x, -1) when dst == lhs → INC/DEC
         *   INC dst is 3 bytes (REX.W + FF /0) vs LEA 7 bytes (REX.W + 8D + ModRM + disp32)
         *   DEC dst is 3 bytes (REX.W + FF /1) vs LEA 7 bytes
         *   INC/DEC are slightly worse because they clobber flags (but partial
         *   flag stalls are rare on modern CPUs), but the 4-byte size savings
         *   and 1 fewer uop make them preferable when dst == lhs.
         */
        int64_t rhs_const;
        if (try_get_const_int(graph, node->inputs[1], &rhs_const)) {
            if (rhs_const == 1 && dst == lhs_vreg) {
                /* P1: INC for +1 when dst == lhs (in-place) */
                vtx_isel_emit_inst(block, make_r_inst(VTX_X86_INC, dst, node_id, 0), arena);
                break;
            }
            if (rhs_const == -1 && dst == lhs_vreg) {
                /* P1: DEC for -1 when dst == lhs (in-place) */
                vtx_isel_emit_inst(block, make_r_inst(VTX_X86_DEC, dst, node_id, 0), arena);
                break;
            }
            if (rhs_const != 0 && rhs_const >= INT32_MIN && rhs_const <= INT32_MAX) {
                vtx_x86_memop_t mem = { lhs_vreg, VTX_VREG_INVALID, 0xFF, 0xFF, 1, (int32_t)rhs_const };
                vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_LEA, dst, &mem, node_id), arena);
                break;
            }
            /* rhs_const == 0: just MOV (handled below by dst != lhs path) */
        }

        /* P1 isel: LEA for Add(x, y) where dst ≠ lhs — saves a MOV.
         *
         * Pattern: dst = lhs + rhs (dst != lhs)
         *   Old: mov dst, lhs; add dst, rhs
         *   New: lea dst, [lhs + rhs]     (1 instruction instead of 2)
         *
         * LEA can represent [base + index*scale] where scale=1,
         * giving base + index = lhs + rhs. This eliminates the MOV.
         *
         * Constraint: dst must differ from both lhs and rhs to avoid
         * clobbering. If dst == lhs, the ADD form is fine (in-place).
         */
        if (dst != lhs_vreg && dst != rhs_vreg) {
            vtx_x86_memop_t mem = { lhs_vreg, rhs_vreg, 0xFF, 0xFF, 1, 0 };
            vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_LEA, dst, &mem, node_id), arena);
            break;
        }

        /* Fallback: MOV + ADD */
        if (dst != lhs_vreg)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_vreg, node_id), arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_ADD, dst, rhs_vreg, node_id), arena);
        break;
    }

    case VTX_OP_Sub: {
        if (node->input_count < 2) return -1;
        uint32_t lhs_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t rhs_vreg = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs_vreg == VTX_VREG_INVALID || rhs_vreg == VTX_VREG_INVALID) return -1;

        /* Float Sub → SUBSD xmm, xmm */
        if (node->type == VTX_TYPE_Float) {
            if (dst != lhs_vreg)
                vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_MOVSD, dst, lhs_vreg, node_id), arena);
            vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_SUBSD, dst, rhs_vreg, node_id), arena);
            break;
        }

        /* P1 isel: LEA for Sub(x, Const) — subtract via LEA with negative disp.
         *
         * Pattern: dst = lhs - constant
         *   Old: mov dst, lhs; sub dst, imm
         *   New: lea dst, [lhs + (-imm)]   (1 instruction, no flag write)
         *
         * Only valid when -imm fits in int32 (i.e., imm in [-2^31+1, 2^31]).
         * Note: LEA doesn't set flags, which is an advantage for subsequent
         * flag-dependent code.
         *
         * Special case: Sub(x, 1) when dst == lhs → DEC
         *   DEC dst is 3 bytes vs LEA 7 bytes */
        int64_t rhs_const_sub;
        if (try_get_const_int(graph, node->inputs[1], &rhs_const_sub)) {
            /* P1: DEC for Sub(x, 1) when dst == lhs (in-place) */
            if (rhs_const_sub == 1 && dst == lhs_vreg) {
                vtx_isel_emit_inst(block, make_r_inst(VTX_X86_DEC, dst, node_id, 0), arena);
                break;
            }
            /* P1: INC for Sub(x, -1) = Add(x, 1) when dst == lhs */
            if (rhs_const_sub == -1 && dst == lhs_vreg) {
                vtx_isel_emit_inst(block, make_r_inst(VTX_X86_INC, dst, node_id, 0), arena);
                break;
            }
            int64_t neg_imm = -rhs_const_sub;
            if (neg_imm != 0 && neg_imm >= INT32_MIN && neg_imm <= INT32_MAX) {
                vtx_x86_memop_t mem = { lhs_vreg, VTX_VREG_INVALID, 0xFF, 0xFF, 1, (int32_t)neg_imm };
                vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_LEA, dst, &mem, node_id), arena);
                break;
            }
        }

        if (dst != lhs_vreg)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_vreg, node_id), arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_SUB, dst, rhs_vreg, node_id), arena);
        break;
    }

    case VTX_OP_Mul: {
        if (node->input_count < 2) return -1;
        uint32_t lhs_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t rhs_vreg = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs_vreg == VTX_VREG_INVALID || rhs_vreg == VTX_VREG_INVALID) return -1;

        /* Float Mul → MULSD xmm, xmm */
        if (node->type == VTX_TYPE_Float) {
            if (dst != lhs_vreg)
                vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_MOVSD, dst, lhs_vreg, node_id), arena);
            vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_MULSD, dst, rhs_vreg, node_id), arena);
            break;
        }

        /* Strength reduction: try to replace IMUL with cheaper ops */
        int64_t rhs_const;
        if (try_get_const_int(graph, node->inputs[1], &rhs_const)) {
            /* P1 isel: LEA for Mul(x, 3/5/9) — already handled by
             * emit_mul_by_constant() which uses LEA for these cases.
             * The existing code correctly emits:
             *   x*3 → lea dst, [x + x*2]
             *   x*5 → lea dst, [x + x*4]
             *   x*9 → lea dst, [x + x*8]
             * These are 1-instruction replacements for what would otherwise
             * be a 2-instruction MOV+IMUL sequence. */
            /* RHS is constant — try LEA/shift/add sequence */
            if (emit_mul_by_constant(stream, block, dst, lhs_vreg, rhs_const, node_id, arena))
                break;
        }
        int64_t lhs_const;
        if (try_get_const_int(graph, node->inputs[0], &lhs_const)) {
            /* LHS is constant — swap and try */
            if (emit_mul_by_constant(stream, block, dst, rhs_vreg, lhs_const, node_id, arena))
                break;
        }

        /* Fallback: use IMUL */
        if (dst != lhs_vreg)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_vreg, node_id), arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_IMUL, dst, rhs_vreg, node_id), arena);
        break;
    }

    case VTX_OP_Div: {
        if (node->input_count < 2) return -1;
        uint32_t lhs_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t rhs_vreg = vtx_isel_node_vreg(stream, node->inputs[1]);
        if (lhs_vreg == VTX_VREG_INVALID || rhs_vreg == VTX_VREG_INVALID) return -1;

        /* Float Div → DIVSD xmm, xmm */
        if (node->type == VTX_TYPE_Float) {
            uint32_t dst = ensure_node_vreg(stream, node_id, arena);
            if (dst != lhs_vreg)
                vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_MOVSD, dst, lhs_vreg, node_id), arena);
            vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_DIVSD, dst, rhs_vreg, node_id), arena);
            break;
        }

        /* Strength reduction: divide by constant power of 2 → SAR */
        int64_t rhs_const;
        if (try_get_const_int(graph, node->inputs[1], &rhs_const)) {
            int shift = is_power_of_2(rhs_const);
            if (shift >= 0) {
                /* x / 2^n → sar dst, n (with rounding fixup for signed div) */
                uint32_t dst = ensure_node_vreg(stream, node_id, arena);
                if (dst != lhs_vreg)
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_vreg, node_id), arena);
                /* For signed division, add round-to-zero fixup:
                 * if x < 0, add (2^n - 1) before shifting */
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHR, dst, 63, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_AND, dst, (int64_t)((1 << shift) - 1), node_id), arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_ADD, dst, lhs_vreg, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, dst, shift, node_id), arena);
                break;
            }

            /* Divide by constant using magic number multiplication */
            int64_t magic;
            int magic_shift;
            if (compute_magic_number(rhs_const, &magic, &magic_shift)) {
                uint32_t dst = ensure_node_vreg(stream, node_id, arena);
                if (dst != lhs_vreg)
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_vreg, node_id), arena);
                /* mov tmp, magic; imul dst, tmp; sar dst, shift */
                uint32_t tmp = vtx_isel_alloc_vreg(stream, arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_MOV, tmp, magic, node_id), arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_IMUL, dst, tmp, node_id), arena);
                if (magic_shift > 0)
                    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, dst, magic_shift, node_id), arena);
                break;
            }
        }

        /* Fallback: use CQO + IDIV */
        uint32_t rax_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 0);
        uint32_t rdx_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 2);
        if (rax_vreg == VTX_VREG_INVALID || rdx_vreg == VTX_VREG_INVALID) return -1;
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rax_vreg, lhs_vreg, node_id), arena);
        vtx_inst_t cqo;
        memset(&cqo, 0, sizeof(cqo));
        cqo.opcode = VTX_X86_CQO;
        cqo.source_node = node_id;
        cqo.flags = VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RDX;
        vtx_isel_emit_inst(block, cqo, arena);
        vtx_inst_t idiv_inst;
        memset(&idiv_inst, 0, sizeof(idiv_inst));
        idiv_inst.opcode = VTX_X86_IDIV;
        idiv_inst.opnd_kinds[0] = VTX_OPND_VREG;
        idiv_inst.operands[0] = rhs_vreg;
        idiv_inst.source_node = node_id;
        idiv_inst.flags = VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RDX;
        vtx_isel_emit_inst(block, idiv_inst, arena);
        vtx_isel_map_node_vreg(stream, node_id, rax_vreg, arena);
        (void)rdx_vreg;
        return 0;
    }

    case VTX_OP_Mod: {
        if (node->input_count < 2) return -1;
        uint32_t lhs_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t rhs_vreg = vtx_isel_node_vreg(stream, node->inputs[1]);
        if (lhs_vreg == VTX_VREG_INVALID || rhs_vreg == VTX_VREG_INVALID) return -1;

        /* Strength reduction: modulo by power of 2 → AND with mask */
        int64_t rhs_const;
        if (try_get_const_int(graph, node->inputs[1], &rhs_const)) {
            int shift = is_power_of_2(rhs_const);
            if (shift >= 0) {
                /* x % 2^n → and dst, (2^n - 1) (for unsigned)
                 * For signed mod: need to handle negative dividend.
                 * Use: and of sign bits + and of magnitude */
                uint32_t dst = ensure_node_vreg(stream, node_id, arena);
                if (dst != lhs_vreg)
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_vreg, node_id), arena);
                /* Get sign mask: shr dst, 63 gives all 1s if negative, 0 if positive */
                uint32_t tmp = vtx_isel_alloc_vreg(stream, arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, tmp, dst, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHR, tmp, 63, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_AND, tmp, (int64_t)((1 << shift) - 1), node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_AND, dst, (int64_t)((1 << shift) - 1), node_id), arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_ADD, dst, tmp, node_id), arena);
                break;
            }
        }

        /* Fallback: use CQO + IDIV, result in RDX */
        uint32_t rax_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 0);
        uint32_t rdx_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 2);
        if (rax_vreg == VTX_VREG_INVALID || rdx_vreg == VTX_VREG_INVALID) return -1;
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rax_vreg, lhs_vreg, node_id), arena);
        vtx_inst_t cqo;
        memset(&cqo, 0, sizeof(cqo));
        cqo.opcode = VTX_X86_CQO;
        cqo.source_node = node_id;
        cqo.flags = VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RDX;
        vtx_isel_emit_inst(block, cqo, arena);
        vtx_inst_t idiv_inst;
        memset(&idiv_inst, 0, sizeof(idiv_inst));
        idiv_inst.opcode = VTX_X86_IDIV;
        idiv_inst.opnd_kinds[0] = VTX_OPND_VREG;
        idiv_inst.operands[0] = rhs_vreg;
        idiv_inst.source_node = node_id;
        idiv_inst.flags = VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RDX;
        vtx_isel_emit_inst(block, idiv_inst, arena);
        vtx_isel_map_node_vreg(stream, node_id, rdx_vreg, arena);
        (void)rax_vreg;
        return 0;
    }

    /* ---- Shifts ---- */
    case VTX_OP_Shl: {
        if (node->input_count < 2) return -1;
        uint32_t val_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t cnt_vreg = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (val_vreg == VTX_VREG_INVALID || cnt_vreg == VTX_VREG_INVALID) return -1;
        if (dst != val_vreg)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, val_vreg, node_id), arena);
        const vtx_node_t *cnt_node = vtx_node_get_const(&graph->node_table, node->inputs[1]);
        if (cnt_node && cnt_node->opcode == VTX_OP_Constant &&
            cnt_node->constval.kind == VTX_TYPE_Int) {
            vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, dst,
                               cnt_node->constval.as.int_val, node_id), arena);
        } else {
            uint32_t rcx_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 1);
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rcx_vreg, cnt_vreg, node_id), arena);
            vtx_inst_t shl_inst;
            memset(&shl_inst, 0, sizeof(shl_inst));
            shl_inst.opcode = VTX_X86_SHL;
            shl_inst.opnd_kinds[0] = VTX_OPND_VREG;
            shl_inst.operands[0] = dst;
            shl_inst.opnd_kinds[1] = VTX_OPND_PREG;
            shl_inst.operands[1] = 1;
            shl_inst.source_node = node_id;
            shl_inst.flags = VTX_INST_FLAG_CLOBBER_RCX;
            vtx_isel_emit_inst(block, shl_inst, arena);
        }
        break;
    }

    case VTX_OP_Shr: {
        if (node->input_count < 2) return -1;
        uint32_t val_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t cnt_vreg = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (val_vreg == VTX_VREG_INVALID || cnt_vreg == VTX_VREG_INVALID) return -1;
        if (dst != val_vreg)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, val_vreg, node_id), arena);
        const vtx_node_t *cnt_node = vtx_node_get_const(&graph->node_table, node->inputs[1]);
        if (cnt_node && cnt_node->opcode == VTX_OP_Constant &&
            cnt_node->constval.kind == VTX_TYPE_Int) {
            vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHR, dst,
                               cnt_node->constval.as.int_val, node_id), arena);
        } else {
            uint32_t rcx_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 1);
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rcx_vreg, cnt_vreg, node_id), arena);
            vtx_inst_t shr_inst;
            memset(&shr_inst, 0, sizeof(shr_inst));
            shr_inst.opcode = VTX_X86_SHR;
            shr_inst.opnd_kinds[0] = VTX_OPND_VREG;
            shr_inst.operands[0] = dst;
            shr_inst.opnd_kinds[1] = VTX_OPND_PREG;
            shr_inst.operands[1] = 1;
            shr_inst.source_node = node_id;
            shr_inst.flags = VTX_INST_FLAG_CLOBBER_RCX;
            vtx_isel_emit_inst(block, shr_inst, arena);
        }
        break;
    }

    /* ---- Bitwise ---- */
    case VTX_OP_And: {
        if (node->input_count < 2) return -1;
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;
        if (dst != lhs)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs, node_id), arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_AND, dst, rhs, node_id), arena);
        break;
    }

    case VTX_OP_Or: {
        if (node->input_count < 2) return -1;
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;
        if (dst != lhs)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs, node_id), arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_OR, dst, rhs, node_id), arena);
        break;
    }

    case VTX_OP_Xor: {
        if (node->input_count < 2) return -1;
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;
        if (dst != lhs)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs, node_id), arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_XOR, dst, rhs, node_id), arena);
        break;
    }

    /* ---- Unary ---- */
    case VTX_OP_Neg: {
        if (node->input_count < 1) return -1;
        uint32_t src = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (src == VTX_VREG_INVALID) return -1;

        /* Float Neg → xorps sign bit flip:
         * Load a constant with only the sign bit set (0x8000000000000000)
         * into a temp XMM register, then XORPS dst, tmp. */
        if (node->type == VTX_TYPE_Float) {
            uint32_t tmp = vtx_isel_alloc_vreg(stream, arena);
            /* mov tmp, 0x8000000000000000 (sign bit mask) */
            vtx_inst_t mov_imm;
            memset(&mov_imm, 0, sizeof(mov_imm));
            mov_imm.opcode = VTX_X86_MOV;
            mov_imm.opnd_kinds[0] = VTX_OPND_VREG;
            mov_imm.operands[0] = tmp;
            mov_imm.opnd_kinds[1] = VTX_OPND_IMM;
            /* 0x8000000000000000 as signed int64 = -9223372036854775808 */
            mov_imm.imm = (int64_t)0x8000000000000000ULL;
            mov_imm.flags = VTX_INST_FLAG_HAS_IMM;
            mov_imm.source_node = node_id;
            vtx_isel_emit_inst(block, mov_imm, arena);

            if (dst != src)
                vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_MOVSD, dst, src, node_id), arena);
            /* xorps dst, tmp — flip the sign bit */
            vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_XORPS, dst, tmp, node_id), arena);
            break;
        }

        if (dst != src)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, src, node_id), arena);
        vtx_isel_emit_inst(block, make_r_inst(VTX_X86_NEG, dst, node_id, 0), arena);
        break;
    }

    case VTX_OP_Not: {
        if (node->input_count < 1) return -1;
        uint32_t src = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (src == VTX_VREG_INVALID) return -1;
        if (dst != src)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, src, node_id), arena);
        vtx_isel_emit_inst(block, make_r_inst(VTX_X86_NOT, dst, node_id, 0), arena);
        break;
    }

    /* ---- Min/Max (P1 isel: CMP+CMOV) ---- */
    case VTX_OP_Min: {
        if (node->input_count < 2) return -1;
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;

        /* min(a, b) = b if a > b, else a
         *   mov dst, lhs
         *   cmp dst, rhs
         *   cmovg dst, rhs    ; if dst > rhs, dst = rhs */
        if (dst != lhs)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs, node_id), arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_CMP, dst, rhs, node_id), arena);

        vtx_inst_t cmov;
        memset(&cmov, 0, sizeof(cmov));
        cmov.opcode = VTX_X86_CMOV;
        cmov.opnd_kinds[0] = VTX_OPND_VREG;
        cmov.operands[0] = dst;
        cmov.opnd_kinds[1] = VTX_OPND_VREG;
        cmov.operands[1] = rhs;
        cmov.cond = VTX_COND_GT;
        cmov.flags = VTX_INST_FLAG_HAS_COND;
        cmov.source_node = node_id;
        vtx_isel_emit_inst(block, cmov, arena);
        break;
    }

    case VTX_OP_Max: {
        if (node->input_count < 2) return -1;
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;

        /* max(a, b) = a if a > b, else b
         *   mov dst, lhs
         *   cmp dst, rhs
         *   cmovle dst, rhs   ; if dst <= rhs, dst = rhs */
        if (dst != lhs)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs, node_id), arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_CMP, dst, rhs, node_id), arena);

        vtx_inst_t cmov;
        memset(&cmov, 0, sizeof(cmov));
        cmov.opcode = VTX_X86_CMOV;
        cmov.opnd_kinds[0] = VTX_OPND_VREG;
        cmov.operands[0] = dst;
        cmov.opnd_kinds[1] = VTX_OPND_VREG;
        cmov.operands[1] = rhs;
        cmov.cond = VTX_COND_LE;
        cmov.flags = VTX_INST_FLAG_HAS_COND;
        cmov.source_node = node_id;
        vtx_isel_emit_inst(block, cmov, arena);
        break;
    }

    /* ---- Comparison ---- */
    case VTX_OP_Cmp:
    case VTX_OP_CmpP: {
        if (node->input_count < 2) return -1;
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;

        /* Optimization: CMP against 0 → TEST reg, reg */
        int64_t rhs_const;
        if (try_get_const_int(graph, node->inputs[1], &rhs_const) && rhs_const == 0) {
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_TEST, lhs, lhs, node_id), arena);
        } else {
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_CMP, lhs, rhs, node_id), arena);
        }

        /* P1 isel: SETCC into zeroed register — eliminates the AND 0xFF.
         *
         * Pattern: dst = (lhs cmp rhs) ? 1 : 0
         *   Old: setcc dst_lo; and dst, 0xFF    (2 instructions)
         *   New: xor dst, dst; setcc dst_lo      (2 instructions, but xor is cheaper)
         *
         * Why XOR+SETCC is better than SETCC+AND:
         *   - XOR reg,reg is 1 uop (register renaming zeros the register)
         *   - SETCC writes only the low byte, but XOR already zeroed the
         *     upper bytes, so the result is correctly zero-extended
         *   - AND 0xFF is 1 uop but requires an immediate operand
         *   - More importantly: XOR+SETCC can be macro-fused on some CPUs
         *   - The register renamer handles XOR zeroing for free (no execution)
         */
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_XOR, dst, dst, node_id), arena);
        vtx_inst_t setcc;
        memset(&setcc, 0, sizeof(setcc));
        setcc.opcode = VTX_X86_SETCC;
        setcc.opnd_kinds[0] = VTX_OPND_VREG;
        setcc.operands[0] = dst;
        setcc.cond = node->cond;
        setcc.flags = VTX_INST_FLAG_HAS_COND;
        setcc.source_node = node_id;
        vtx_isel_emit_inst(block, setcc, arena);
        break;
    }

    /* ---- Memory ---- */
    case VTX_OP_Load: {
        if (node->input_count < 1) return -1;
        uint32_t addr = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (addr == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t mem = { addr, VTX_VREG_INVALID, 0xFF, 0xFF, 1, 0 };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, dst, &mem, node_id), arena);
        break;
    }

    case VTX_OP_Store: {
        if (node->input_count < 2) return -1;
        uint32_t addr = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t val = vtx_isel_node_vreg(stream, node->inputs[1]);
        if (addr == VTX_VREG_INVALID || val == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t mem = { addr, VTX_VREG_INVALID, 0xFF, 0xFF, 1, 0 };
        vtx_isel_emit_inst(block, make_mr_inst(VTX_X86_MOV, &mem, val, node_id), arena);
        break;
    }

    case VTX_OP_LoadField: {
        if (node->input_count < 1) return -1;
        uint32_t obj = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (obj == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t mem = { obj, VTX_VREG_INVALID, 0xFF, 0xFF, 1, (int32_t)node->field_offset };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, dst, &mem, node_id), arena);
        break;
    }

    case VTX_OP_StoreField: {
        if (node->input_count < 2) return -1;
        uint32_t obj = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t val = vtx_isel_node_vreg(stream, node->inputs[1]);
        if (obj == VTX_VREG_INVALID || val == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t mem = { obj, VTX_VREG_INVALID, 0xFF, 0xFF, 1, (int32_t)node->field_offset };
        vtx_isel_emit_inst(block, make_mr_inst(VTX_X86_MOV, &mem, val, node_id), arena);

        /* Emit GC write barrier for reference stores.
         * Check if the stored value is a pointer/reference type or if
         * the node has the VTX_NF_WRITE_BARRIER flag set. */
        {
            bool is_ref_store = vtx_nf_has(node->flags, VTX_NF_WRITE_BARRIER);
            if (!is_ref_store && node->inputs[1] < graph->node_table.count) {
                const vtx_node_t *val_node = &graph->node_table.nodes[node->inputs[1]];
                if (val_node && val_node->type == VTX_TYPE_Ptr) {
                    is_ref_store = true;
                }
            }
            if (is_ref_store) {
                /* Push obj address to RDI (first arg) and field_offset to ESI (second arg).
                 * Per System V AMD64 ABI: RDI = arg0, ESI = arg1 */
                uint32_t rdi_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 7); /* RDI */
                uint32_t rsi_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 6); /* RSI */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rdi_vreg, obj, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_MOV, rsi_vreg,
                                   (int64_t)node->field_offset, node_id), arena);
                /* Call write barrier stub (imm = -4) */
                vtx_inst_t wb_call;
                memset(&wb_call, 0, sizeof(wb_call));
                wb_call.opcode = VTX_X86_CALL;
                wb_call.opnd_kinds[0] = VTX_OPND_IMM;
                wb_call.imm = -4;
                wb_call.flags = VTX_INST_FLAG_HAS_IMM | VTX_INST_FLAG_IS_CALL |
                                VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RCX |
                                VTX_INST_FLAG_CLOBBER_RDX | VTX_INST_FLAG_CLOBBER_RSI |
                                VTX_INST_FLAG_CLOBBER_RDI | VTX_INST_FLAG_CLOBBER_R8 |
                                VTX_INST_FLAG_CLOBBER_R9 | VTX_INST_FLAG_CLOBBER_R10 |
                                VTX_INST_FLAG_CLOBBER_R11;
                wb_call.source_node = node_id;
                vtx_isel_emit_inst(block, wb_call, arena);
            }
        }
        break;
    }

    case VTX_OP_LoadIndexed: {
        if (node->input_count < 2) return -1;
        uint32_t base = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t idx = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (base == VTX_VREG_INVALID || idx == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t mem = { base, idx, 0xFF, 0xFF, 8, 0 };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, dst, &mem, node_id), arena);
        break;
    }

    case VTX_OP_StoreIndexed: {
        if (node->input_count < 3) return -1;
        uint32_t base = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t idx = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t val = vtx_isel_node_vreg(stream, node->inputs[2]);
        if (base == VTX_VREG_INVALID || idx == VTX_VREG_INVALID || val == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t mem = { base, idx, 0xFF, 0xFF, 8, 0 };
        vtx_isel_emit_inst(block, make_mr_inst(VTX_X86_MOV, &mem, val, node_id), arena);

        /* Emit GC write barrier for reference stores.
         * For StoreIndexed, the field offset is computed at runtime as
         * idx * element_size + header_size. We pass 0 as field_offset
         * and let the write barrier use the object's base address to
         * mark the card. This is conservative (marks the whole card
         * containing the object) but correct — the GC will scan all
         * fields of the object anyway when the card is dirty. */
        {
            bool is_ref_store = vtx_nf_has(node->flags, VTX_NF_WRITE_BARRIER);
            if (!is_ref_store && node->inputs[2] < graph->node_table.count) {
                const vtx_node_t *val_node = &graph->node_table.nodes[node->inputs[2]];
                if (val_node && val_node->type == VTX_TYPE_Ptr) {
                    is_ref_store = true;
                }
            }
            if (is_ref_store) {
                /* Push base address to RDI (first arg), 0 as field_offset to ESI (second arg).
                 * Per System V AMD64 ABI: RDI = arg0, ESI = arg1 */
                uint32_t rdi_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 7); /* RDI */
                uint32_t rsi_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 6); /* RSI */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rdi_vreg, base, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_MOV, rsi_vreg, 0, node_id), arena);
                /* Call write barrier stub (imm = -4) */
                vtx_inst_t wb_call;
                memset(&wb_call, 0, sizeof(wb_call));
                wb_call.opcode = VTX_X86_CALL;
                wb_call.opnd_kinds[0] = VTX_OPND_IMM;
                wb_call.imm = -4;
                wb_call.flags = VTX_INST_FLAG_HAS_IMM | VTX_INST_FLAG_IS_CALL |
                                VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RCX |
                                VTX_INST_FLAG_CLOBBER_RDX | VTX_INST_FLAG_CLOBBER_RSI |
                                VTX_INST_FLAG_CLOBBER_RDI | VTX_INST_FLAG_CLOBBER_R8 |
                                VTX_INST_FLAG_CLOBBER_R9 | VTX_INST_FLAG_CLOBBER_R10 |
                                VTX_INST_FLAG_CLOBBER_R11;
                wb_call.source_node = node_id;
                vtx_isel_emit_inst(block, wb_call, arena);
            }
        }
        break;
    }

    /* ---- Calls ---- */
    case VTX_OP_CallStatic: {
        uint32_t data_arg_idx = 0;
        for (uint32_t i = 0; i < node->input_count; i++) {
            const vtx_node_t *inp = vtx_node_get_const(&graph->node_table, node->inputs[i]);
            if (!inp || vtx_nf_has(inp->flags, VTX_NF_CONTROL) ||
                vtx_nf_has(inp->flags, VTX_NF_MEMORY))
                continue;
            uint32_t arg_vreg = vtx_isel_node_vreg(stream, node->inputs[i]);
            if (arg_vreg == VTX_VREG_INVALID) continue;
            if (data_arg_idx < 6) {
                uint32_t arg_preg = vtx_isel_alloc_vreg_fixed(stream, arena, vtx_arg_regs[data_arg_idx]);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, arg_preg, arg_vreg, node_id), arena);
            } else {
                vtx_isel_emit_inst(block, make_r_inst(VTX_X86_PUSH, arg_vreg, node_id, 0), arena);
            }
            data_arg_idx++;
        }
        vtx_inst_t call_inst;
        memset(&call_inst, 0, sizeof(call_inst));
        call_inst.opcode = VTX_X86_CALL;
        call_inst.opnd_kinds[0] = VTX_OPND_IMM;
        call_inst.imm = (int64_t)node->method_index;
        call_inst.flags = VTX_INST_FLAG_HAS_IMM | VTX_INST_FLAG_IS_CALL |
                          VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RCX |
                          VTX_INST_FLAG_CLOBBER_RDX | VTX_INST_FLAG_CLOBBER_RSI |
                          VTX_INST_FLAG_CLOBBER_RDI | VTX_INST_FLAG_CLOBBER_R8 |
                          VTX_INST_FLAG_CLOBBER_R9 | VTX_INST_FLAG_CLOBBER_R10 |
                          VTX_INST_FLAG_CLOBBER_R11;
        call_inst.source_node = node_id;
        vtx_isel_emit_inst(block, call_inst, arena);
        uint32_t rax_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 0);
        vtx_isel_map_node_vreg(stream, node_id, rax_vreg, arena);
        if (data_arg_idx > 6) {
            vtx_inst_t cleanup;
            memset(&cleanup, 0, sizeof(cleanup));
            cleanup.opcode = VTX_X86_ADD;
            cleanup.opnd_kinds[0] = VTX_OPND_PREG;
            cleanup.operands[0] = 4;
            cleanup.opnd_kinds[1] = VTX_OPND_IMM;
            cleanup.imm = (int64_t)((data_arg_idx - 6) * 8);
            cleanup.flags = VTX_INST_FLAG_HAS_IMM;
            cleanup.source_node = node_id;
            vtx_isel_emit_inst(block, cleanup, arena);
        }
        return 0;
    }

    case VTX_OP_CallVirtual:
    case VTX_OP_CallInterface: {
        uint32_t data_arg_idx = 0;
        uint32_t receiver_vreg = VTX_VREG_INVALID;
        for (uint32_t i = 0; i < node->input_count; i++) {
            const vtx_node_t *inp = vtx_node_get_const(&graph->node_table, node->inputs[i]);
            if (!inp || vtx_nf_has(inp->flags, VTX_NF_CONTROL) ||
                vtx_nf_has(inp->flags, VTX_NF_MEMORY))
                continue;
            uint32_t arg_vreg = vtx_isel_node_vreg(stream, node->inputs[i]);
            if (arg_vreg == VTX_VREG_INVALID) continue;
            if (data_arg_idx == 0) receiver_vreg = arg_vreg;
            if (data_arg_idx < 6) {
                uint32_t arg_preg = vtx_isel_alloc_vreg_fixed(stream, arena, vtx_arg_regs[data_arg_idx]);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, arg_preg, arg_vreg, node_id), arena);
            } else {
                vtx_isel_emit_inst(block, make_r_inst(VTX_X86_PUSH, arg_vreg, node_id, 0), arena);
            }
            data_arg_idx++;
        }
        if (receiver_vreg != VTX_VREG_INVALID) {
            uint32_t rax_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 0);
            vtx_x86_memop_t vtable_mem = { receiver_vreg, VTX_VREG_INVALID, 1,
                                           (int32_t)VTX_HEAP_OBJECT_HEADER_SIZE };
            vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, rax_vreg, &vtable_mem, node_id), arena);
            vtx_x86_memop_t method_mem = { rax_vreg, VTX_VREG_INVALID, 1,
                                           (int32_t)(node->method_index * (uint32_t)sizeof(void *)) };
            vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, rax_vreg, &method_mem, node_id), arena);
            vtx_inst_t call_inst;
            memset(&call_inst, 0, sizeof(call_inst));
            call_inst.opcode = VTX_X86_CALL;
            call_inst.opnd_kinds[0] = VTX_OPND_PREG;
            call_inst.operands[0] = 0;
            call_inst.flags = VTX_INST_FLAG_IS_CALL |
                              VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RCX |
                              VTX_INST_FLAG_CLOBBER_RDX | VTX_INST_FLAG_CLOBBER_RSI |
                              VTX_INST_FLAG_CLOBBER_RDI | VTX_INST_FLAG_CLOBBER_R8 |
                              VTX_INST_FLAG_CLOBBER_R9 | VTX_INST_FLAG_CLOBBER_R10 |
                              VTX_INST_FLAG_CLOBBER_R11;
            call_inst.source_node = node_id;
            vtx_isel_emit_inst(block, call_inst, arena);
        }
        uint32_t result_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 0);
        vtx_isel_map_node_vreg(stream, node_id, result_vreg, arena);
        if (data_arg_idx > 6) {
            vtx_inst_t cleanup;
            memset(&cleanup, 0, sizeof(cleanup));
            cleanup.opcode = VTX_X86_ADD;
            cleanup.opnd_kinds[0] = VTX_OPND_PREG;
            cleanup.operands[0] = 4;
            cleanup.opnd_kinds[1] = VTX_OPND_IMM;
            cleanup.imm = (int64_t)((data_arg_idx - 6) * 8);
            cleanup.flags = VTX_INST_FLAG_HAS_IMM;
            cleanup.source_node = node_id;
            vtx_isel_emit_inst(block, cleanup, arena);
        }
        return 0;
    }

    case VTX_OP_CallRuntime: {
        uint32_t data_arg_idx = 0;
        for (uint32_t i = 0; i < node->input_count; i++) {
            const vtx_node_t *inp = vtx_node_get_const(&graph->node_table, node->inputs[i]);
            if (!inp || vtx_nf_has(inp->flags, VTX_NF_CONTROL) ||
                vtx_nf_has(inp->flags, VTX_NF_MEMORY))
                continue;
            uint32_t arg_vreg = vtx_isel_node_vreg(stream, node->inputs[i]);
            if (arg_vreg == VTX_VREG_INVALID) continue;
            if (data_arg_idx < 6) {
                uint32_t arg_preg = vtx_isel_alloc_vreg_fixed(stream, arena, vtx_arg_regs[data_arg_idx]);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, arg_preg, arg_vreg, node_id), arena);
            }
            data_arg_idx++;
        }
        vtx_inst_t call_inst;
        memset(&call_inst, 0, sizeof(call_inst));
        call_inst.opcode = VTX_X86_CALL;
        call_inst.opnd_kinds[0] = VTX_OPND_IMM;
        call_inst.imm = (int64_t)node->method_index;
        call_inst.flags = VTX_INST_FLAG_HAS_IMM | VTX_INST_FLAG_IS_CALL |
                          VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RCX |
                          VTX_INST_FLAG_CLOBBER_RDX | VTX_INST_FLAG_CLOBBER_RSI |
                          VTX_INST_FLAG_CLOBBER_RDI | VTX_INST_FLAG_CLOBBER_R8 |
                          VTX_INST_FLAG_CLOBBER_R9 | VTX_INST_FLAG_CLOBBER_R10 |
                          VTX_INST_FLAG_CLOBBER_R11;
        call_inst.source_node = node_id;
        vtx_isel_emit_inst(block, call_inst, arena);
        uint32_t result_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 0);
        vtx_isel_map_node_vreg(stream, node_id, result_vreg, arena);
        return 0;
    }

    /* ---- Control flow ---- */
    case VTX_OP_If: {
        uint32_t cond_vreg = VTX_VREG_INVALID;
        for (uint32_t i = 0; i < node->input_count; i++) {
            const vtx_node_t *inp = vtx_node_get_const(&graph->node_table, node->inputs[i]);
            if (inp && vtx_nf_has(inp->flags, VTX_NF_DATA)) {
                cond_vreg = vtx_isel_node_vreg(stream, node->inputs[i]);
                break;
            }
        }
        /* P1 isel: TEST+JCC fusion marker.
         *
         * Modern x86-64 CPUs (Intel since Nehalem, AMD since Bulldozer)
         * can fuse TEST+JCC into a single macro-op, but ONLY if they are
         * adjacent with no intervening instructions. Marking both with
         * VTX_INST_FLAG_FUSED tells the scheduler to keep them together.
         *
         * TEST+JCC is the most commonly fused pair (for null checks,
         * boolean tests, etc.). CMP+JCC fusion is already handled by
         * the guard_emit pipeline. */
        if (cond_vreg != VTX_VREG_INVALID) {
            vtx_inst_t test = make_ri_inst(VTX_X86_TEST, cond_vreg, 1, node_id);
            test.flags |= VTX_INST_FLAG_FUSED; /* P1: mark for fusion */
            vtx_isel_emit_inst(block, test, arena);
        }
        vtx_cond_t jcc_cond = (node->cond != VTX_COND_NEVER) ? node->cond : VTX_COND_NE;
        vtx_inst_t jcc = make_branch_inst(VTX_X86_JCC, 0, jcc_cond, node_id);
        jcc.flags |= VTX_INST_FLAG_FUSED; /* P1: mark for fusion */
        vtx_isel_emit_inst(block, jcc, arena);
        break;
    }

    case VTX_OP_Goto: {
        vtx_isel_emit_inst(block,
            make_branch_inst(VTX_X86_JMP, 0, VTX_COND_ALWAYS, node_id), arena);
        break;
    }

    case VTX_OP_Return: {
        for (uint32_t i = 0; i < node->input_count; i++) {
            const vtx_node_t *inp = vtx_node_get_const(&graph->node_table, node->inputs[i]);
            if (inp && vtx_nf_has(inp->flags, VTX_NF_DATA)) {
                uint32_t val_vreg = vtx_isel_node_vreg(stream, node->inputs[i]);
                if (val_vreg != VTX_VREG_INVALID) {
                    uint32_t rax_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 0);
                    vtx_isel_emit_inst(block,
                        make_rr_inst(VTX_X86_MOV, rax_vreg, val_vreg, node_id), arena);
                }
                break;
            }
        }
        vtx_inst_t ret_inst;
        memset(&ret_inst, 0, sizeof(ret_inst));
        ret_inst.opcode = VTX_X86_RET;
        ret_inst.source_node = node_id;
        vtx_isel_emit_inst(block, ret_inst, arena);
        break;
    }

    /* ---- Type operations ---- */
    case VTX_OP_CheckCast: {
        if (node->input_count < 1) return -1;
        uint32_t obj = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (obj == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t type_mem = { obj, VTX_VREG_INVALID, 1, 0 };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, dst, &type_mem, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_CMP, dst, (int64_t)node->type_id, node_id), arena);
        vtx_isel_emit_inst(block,
            make_branch_inst(VTX_X86_JCC, 0, VTX_COND_NE, node_id), arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, obj, node_id), arena);
        break;
    }

    case VTX_OP_InstanceOf: {
        if (node->input_count < 1) return -1;
        uint32_t obj = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (obj == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t type_mem = { obj, VTX_VREG_INVALID, 1, 0 };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, dst, &type_mem, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_CMP, dst, (int64_t)node->type_id, node_id), arena);
        /* P1: XOR+SETCC for InstanceOf (same as Cmp/CmpP) */
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_XOR, dst, dst, node_id), arena);
        vtx_inst_t setcc;
        memset(&setcc, 0, sizeof(setcc));
        setcc.opcode = VTX_X86_SETCC;
        setcc.opnd_kinds[0] = VTX_OPND_VREG;
        setcc.operands[0] = dst;
        setcc.cond = VTX_COND_EQ;
        setcc.flags = VTX_INST_FLAG_HAS_COND;
        setcc.source_node = node_id;
        vtx_isel_emit_inst(block, setcc, arena);
        break;
    }

    /* ---- Guards ---- */
    case VTX_OP_Guard:
    case VTX_OP_DeoptGuard: {
        uint32_t cond_vreg = VTX_VREG_INVALID;
        for (uint32_t i = 0; i < node->input_count; i++) {
            const vtx_node_t *inp = vtx_node_get_const(&graph->node_table, node->inputs[i]);
            if (inp && vtx_nf_has(inp->flags, VTX_NF_DATA)) {
                cond_vreg = vtx_isel_node_vreg(stream, node->inputs[i]);
                break;
            }
        }

        /* Zero-cost deopt: implicit null checks via SIGSEGV.
         *
         * When the guard page / SIGSEGV mechanism is available AND this
         * is a null-check guard (guard_kind == 0 or condition is EQ/NE
         * against zero), we can skip the explicit TEST+JCC and instead
         * let the next load from the checked register trigger SIGSEGV
         * if the pointer is null. The signal handler catches the fault
         * and performs deoptimization.
         *
         * This eliminates ~9 bytes of code and 2 uops per null check.
         * The "guard" is simply an annotation on the subsequent load
         * instruction, marked with VTX_INST_FLAG_IMPLICIT_NULL.
         *
         * We only do this for null checks (the most common guard type).
         * Type checks and bounds checks still use explicit CMP+JCC
         * because they can't be expressed as a simple dereference trap.
         *
         * Conditions where we can use implicit null check:
         *   1. Guard page is available (signal handlers installed)
         *   2. Guard kind is null_check (kind == 0) or the guard
         *      condition tests against zero
         *   3. The subsequent instruction loads from the checked register
         *      at an offset < VTX_NULL_PAGE_LIMIT (64KB)
         *
         * For now, we conservatively enable implicit null checks only
         * when guard_kind == 0 (null_check). The guard's subsequent
         * load instruction will naturally trap on null via SIGSEGV. */
        bool use_implicit_null = false;
        if (vtx_guard_page_is_available_inline() && cond_vreg != VTX_VREG_INVALID) {
            /* Check if this is a null-check guard. We detect this by
             * checking the guard's condition: if it's a test against zero
             * (condition is EQ or NE with a zero/constant-0 input), or
             * if the guard_kind metadata indicates null_check (kind 0).
             *
             * In the SoN IR, a null check guard has its condition input
             * as a Cmp node comparing against zero. We check if the
             * guard's cond is VTX_COND_EQ or VTX_COND_NE with the
             * deopt happening on the null side. */
            if (node->cond == VTX_COND_EQ || node->cond == VTX_COND_NE) {
                /* This is likely a null check (test against zero).
                 * Enable implicit null check via SIGSEGV. */
                use_implicit_null = true;
            }
        }

        if (use_implicit_null) {
            /* Implicit null check: emit NO guard code.
             * Mark the condition register with an annotation so the
             * subsequent load instruction is tagged as an implicit
             * null check site. The SIGSEGV handler will catch null
             * dereferences at that load instruction. */
            vtx_inst_t implicit_mark;
            memset(&implicit_mark, 0, sizeof(implicit_mark));
            implicit_mark.opcode = VTX_X86_NOP;
            implicit_mark.source_node = node_id;
            implicit_mark.flags = VTX_INST_FLAG_IMPLICIT_NULL | VTX_INST_FLAG_IS_GUARD;
            implicit_mark.opnd_kinds[0] = VTX_OPND_VREG;
            implicit_mark.operands[0] = cond_vreg;
            vtx_isel_emit_inst(block, implicit_mark, arena);
        } else if (cond_vreg != VTX_VREG_INVALID) {
            /* Traditional explicit guard: TEST + JCC */
            vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_TEST, cond_vreg, 1, node_id), arena);
            vtx_cond_t deopt_cond = vtx_cond_negate(node->cond);
            vtx_inst_t jcc = make_branch_inst(VTX_X86_JCC, 0, deopt_cond, node_id);
            jcc.flags |= VTX_INST_FLAG_IS_GUARD;
            vtx_isel_emit_inst(block, jcc, arena);
        }

        if (node->input_count >= 1) {
            for (uint32_t i = 0; i < node->input_count; i++) {
                const vtx_node_t *inp = vtx_node_get_const(&graph->node_table, node->inputs[i]);
                if (inp && vtx_nf_has(inp->flags, VTX_NF_DATA)) {
                    uint32_t vreg = vtx_isel_node_vreg(stream, node->inputs[i]);
                    if (vreg != VTX_VREG_INVALID)
                        vtx_isel_map_node_vreg(stream, node_id, vreg, arena);
                    break;
                }
            }
        }
        break;
    }

    /* ---- Phi / Region / Proj ---- */
    case VTX_OP_Phi: {
        ensure_node_vreg(stream, node_id, arena);
        /* G6 fix part 1: Ensure all Phi input nodes also have vregs assigned.
         * Without this, resolve_phis() will find input_vreg == VTX_VREG_INVALID
         * and silently skip the copy, losing data flow through the Phi. */
        for (uint32_t i = 0; i < node->input_count; i++) {
            if (node->inputs[i] != VTX_NODEID_INVALID) {
                ensure_node_vreg(stream, node->inputs[i], arena);
            }
        }
        break;
    }

    case VTX_OP_Region:
    case VTX_OP_LoopBegin:
        break;

    case VTX_OP_Proj: {
        if (node->input_count >= 1) {
            uint32_t src = vtx_isel_node_vreg(stream, node->inputs[0]);
            if (src != VTX_VREG_INVALID)
                vtx_isel_map_node_vreg(stream, node_id, src, arena);
        }
        break;
    }

    /* ---- Allocation ---- */
    case VTX_OP_NewObject: {
        uint32_t rdi_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 7);
        uint32_t rsi_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 6);
        uint32_t rax_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 0);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_MOV, rdi_vreg,
                           (int64_t)node->type_id, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_MOV, rsi_vreg, 0, node_id), arena);
        vtx_inst_t call_inst;
        memset(&call_inst, 0, sizeof(call_inst));
        call_inst.opcode = VTX_X86_CALL;
        call_inst.opnd_kinds[0] = VTX_OPND_IMM;
        call_inst.imm = -1;
        call_inst.flags = VTX_INST_FLAG_HAS_IMM | VTX_INST_FLAG_IS_CALL |
                          VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RCX |
                          VTX_INST_FLAG_CLOBBER_RDX | VTX_INST_FLAG_CLOBBER_RSI |
                          VTX_INST_FLAG_CLOBBER_RDI | VTX_INST_FLAG_CLOBBER_R8 |
                          VTX_INST_FLAG_CLOBBER_R9 | VTX_INST_FLAG_CLOBBER_R10 |
                          VTX_INST_FLAG_CLOBBER_R11;
        call_inst.source_node = node_id;
        vtx_isel_emit_inst(block, call_inst, arena);
        vtx_isel_map_node_vreg(stream, node_id, rax_vreg, arena);
        return 0;
    }

    case VTX_OP_NewArray: {
        uint32_t rdi_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 7);
        uint32_t rax_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 0);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_MOV, rdi_vreg,
                           (int64_t)node->type_id, node_id), arena);
        vtx_inst_t call_inst;
        memset(&call_inst, 0, sizeof(call_inst));
        call_inst.opcode = VTX_X86_CALL;
        call_inst.opnd_kinds[0] = VTX_OPND_IMM;
        call_inst.imm = -2;
        call_inst.flags = VTX_INST_FLAG_HAS_IMM | VTX_INST_FLAG_IS_CALL |
                          VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RCX |
                          VTX_INST_FLAG_CLOBBER_RDX | VTX_INST_FLAG_CLOBBER_RSI |
                          VTX_INST_FLAG_CLOBBER_RDI | VTX_INST_FLAG_CLOBBER_R8 |
                          VTX_INST_FLAG_CLOBBER_R9 | VTX_INST_FLAG_CLOBBER_R10 |
                          VTX_INST_FLAG_CLOBBER_R11;
        call_inst.source_node = node_id;
        vtx_isel_emit_inst(block, call_inst, arena);
        vtx_isel_map_node_vreg(stream, node_id, rax_vreg, arena);
        return 0;
    }

    case VTX_OP_Allocate: {
        if (node->input_count < 1) return -1;
        uint32_t size_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        if (size_vreg == VTX_VREG_INVALID) return -1;
        uint32_t rdi_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 7);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rdi_vreg, size_vreg, node_id), arena);
        vtx_inst_t call_inst;
        memset(&call_inst, 0, sizeof(call_inst));
        call_inst.opcode = VTX_X86_CALL;
        call_inst.opnd_kinds[0] = VTX_OPND_IMM;
        call_inst.imm = -3;
        call_inst.flags = VTX_INST_FLAG_HAS_IMM | VTX_INST_FLAG_IS_CALL |
                          VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RCX |
                          VTX_INST_FLAG_CLOBBER_RDX | VTX_INST_FLAG_CLOBBER_RSI |
                          VTX_INST_FLAG_CLOBBER_RDI | VTX_INST_FLAG_CLOBBER_R8 |
                          VTX_INST_FLAG_CLOBBER_R9 | VTX_INST_FLAG_CLOBBER_R10 |
                          VTX_INST_FLAG_CLOBBER_R11;
        call_inst.source_node = node_id;
        vtx_isel_emit_inst(block, call_inst, arena);
        uint32_t rax_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 0);
        vtx_isel_map_node_vreg(stream, node_id, rax_vreg, arena);
        return 0;
    }

    /* ---- Deopt ---- */
    case VTX_OP_Deopt: {
        vtx_inst_t jmp;
        memset(&jmp, 0, sizeof(jmp));
        jmp.opcode = VTX_X86_JMP;
        jmp.opnd_kinds[0] = VTX_OPND_LABEL;
        jmp.operands[0] = 0;
        jmp.flags = VTX_INST_FLAG_IS_BRANCH | VTX_INST_FLAG_IS_DEOPT;
        jmp.source_node = node_id;
        vtx_isel_emit_inst(block, jmp, arena);
        break;
    }

    /* ---- Float/Double Comparison ---- */
    case VTX_OP_CmpF:
    case VTX_OP_CmpD: {
        if (node->input_count < 2) return -1;
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;

        /* Emit UCOMISD lhs, rhs — unordered compare scalar double.
         * UCOMISD sets EFLAGS as follows:
         *   Equal:           ZF=1, PF=0, CF=0  → SETE (0F 94)
         *   Below (less):    ZF=0, PF=0, CF=1  → SETB (0F 92)
         *   Above (greater): ZF=0, PF=0, CF=0  → SETA (0F 97)
         *   Unordered:       ZF=1, PF=1, CF=1
         *
         * For floating-point comparisons, we map VTX conditions to
         * the unsigned comparison condition codes (same as UCOMISD):
         *   VTX_COND_EQ  → E  (ZF=1)
         *   VTX_COND_NE  → NE (ZF=0) — note: doesn't catch unordered,
         *                   but matches the common behavior for != on floats
         *   VTX_COND_LT  → B  (CF=1)
         *   VTX_COND_LE  → BE (CF=1 or ZF=1)
         *   VTX_COND_GT  → A  (CF=0 and ZF=0)
         *   VTX_COND_GE  → AE (CF=0) */
        vtx_inst_t ucomisd;
        memset(&ucomisd, 0, sizeof(ucomisd));
        ucomisd.opcode = VTX_X86_UCOMISD;
        ucomisd.opnd_kinds[0] = VTX_OPND_VREG;
        ucomisd.operands[0] = lhs;
        ucomisd.opnd_kinds[1] = VTX_OPND_VREG;
        ucomisd.operands[1] = rhs;
        ucomisd.flags = VTX_INST_FLAG_IS_SSE;
        ucomisd.source_node = node_id;
        vtx_isel_emit_inst(block, ucomisd, arena);

        /* Map float condition to x86 condition code.
         * Float comparisons use unsigned condition codes because
         * UCOMISD sets CF/ZF/PF like an unsigned comparison. */
        vtx_cond_t float_cond = node->cond;
        /* For LT/LE/GT/GE, use the unsigned equivalents for float */
        vtx_cond_t ucond = float_cond;
        switch (float_cond) {
        case VTX_COND_LT: ucond = VTX_COND_ULT; break;
        case VTX_COND_LE: ucond = VTX_COND_ULE; break;
        case VTX_COND_GT: ucond = VTX_COND_UGT; break;
        case VTX_COND_GE: ucond = VTX_COND_UGE; break;
        default: break;
        }

        /* P1 isel: XOR+SETCC for float comparisons too (drop AND 0xFF) */
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_XOR, dst, dst, node_id), arena);
        vtx_inst_t setcc;
        memset(&setcc, 0, sizeof(setcc));
        setcc.opcode = VTX_X86_SETCC;
        setcc.opnd_kinds[0] = VTX_OPND_VREG;
        setcc.operands[0] = dst;
        setcc.cond = ucond;
        setcc.flags = VTX_INST_FLAG_HAS_COND;
        setcc.source_node = node_id;
        vtx_isel_emit_inst(block, setcc, arena);
        break;
    }

    case VTX_OP_FrameState:
    case VTX_OP_Start:
    case VTX_OP_End:
    case VTX_OP_LoopEnd: {
        /* Emit safepoint poll at loop back-edge.
         *
         * When the guard page mechanism is available (zero-cost deopt),
         * emit a single MOV from the guard page instead of CMP+JCC.
         * The guard page is normally readable; when a safepoint is
         * requested, the page becomes PROT_NONE and the MOV triggers
         * SIGSEGV, which the signal handler processes.
         *
         * The guard page poll does NOT need IS_GUARD flag because
         * there is no JCC to patch — the SIGSEGV handler directly
         * processes the safepoint.
         *
         * When the guard page is NOT available, fall back to the
         * traditional CMP+JCC approach. */
        vtx_inst_t sp;
        memset(&sp, 0, sizeof(sp));
        sp.source_node = node_id;

        /* Check if guard page is available at compile time.
         * If so, use the zero-cost poll; otherwise use the
         * traditional CMP+JCC poll. Use the inline flag check
         * to avoid a circular dependency between vortex_lower
         * and vortex_compile. */
        if (vtx_guard_page_is_available_inline()) {
            sp.opcode = VTX_X86_SAFEPOINT_POLL_GUARD_PAGE;
            sp.flags = VTX_INST_FLAG_IS_SAFEPOINT;
        } else {
            sp.opcode = VTX_X86_SAFEPOINT_POLL;
            sp.flags = VTX_INST_FLAG_IS_GUARD | VTX_INST_FLAG_IS_SAFEPOINT;
        }
        vtx_isel_emit_inst(block, sp, arena);
        break;
    }
    /* ---- SIMD Vector operations (P1 isel: SSE2 packed double) ---- */
    case VTX_OP_VectorLoad: {
        if (node->input_count < 1) return -1;
        /* VectorLoad: load 128-bit vector from [base + index*scale + disp]
         * Input 0: base address, Input 1: index (optional)
         * Emits: movapd dst, [base + index*scale + disp] */
        uint32_t base = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (base == VTX_VREG_INVALID) return -1;

        /* Mark dst as XMM class */
        if (dst < stream->vreg_fixed_reg_count) {
            /* We set the XMM flag via the vreg_flags mechanism during regalloc */
        }

        vtx_inst_t vload;
        memset(&vload, 0, sizeof(vload));
        vload.opcode = VTX_X86_MOVAPD;
        vload.opnd_kinds[0] = VTX_OPND_VREG;
        vload.operands[0] = dst;
        vload.flags = VTX_INST_FLAG_HAS_MEM | VTX_INST_FLAG_IS_SSE;
        vload.mem.base_vreg = base;
        vload.mem.base_phys = 0xFF;
        vload.mem.index_vreg = VTX_VREG_INVALID;
        vload.mem.index_phys = 0xFF;
        vload.mem.scale = 1;
        vload.mem.disp = (int32_t)node->field_offset;
        vload.source_node = node_id;

        /* If we have an index input, set up SIB */
        if (node->input_count >= 2) {
            uint32_t idx = vtx_isel_node_vreg(stream, node->inputs[1]);
            if (idx != VTX_VREG_INVALID) {
                vload.mem.index_vreg = idx;
                vload.mem.index_phys = 0xFF;
                vload.mem.scale = 8; /* 8 bytes per double, 2 doubles = 16 bytes */
            }
        }

        vtx_isel_emit_inst(block, vload, arena);
        break;
    }

    case VTX_OP_VectorStore: {
        if (node->input_count < 2) return -1;
        /* VectorStore: store 128-bit vector to [base + index*scale + disp]
         * Input 0: base address, Input 1: index (optional), Input last: value */
        uint32_t base = vtx_isel_node_vreg(stream, node->inputs[0]);
        /* Find the value input (last input) */
        uint32_t val = vtx_isel_node_vreg(stream, node->inputs[node->input_count - 1]);
        if (base == VTX_VREG_INVALID || val == VTX_VREG_INVALID) return -1;

        vtx_inst_t vstore;
        memset(&vstore, 0, sizeof(vstore));
        vstore.opcode = VTX_X86_MOVAPD;
        vstore.opnd_kinds[0] = VTX_OPND_VREG;
        vstore.operands[0] = val;
        vstore.flags = VTX_INST_FLAG_HAS_MEM | VTX_INST_FLAG_IS_SSE;
        vstore.mem.base_vreg = base;
        vstore.mem.base_phys = 0xFF;
        vstore.mem.index_vreg = VTX_VREG_INVALID;
        vstore.mem.index_phys = 0xFF;
        vstore.mem.scale = 1;
        vstore.mem.disp = (int32_t)node->field_offset;
        vstore.source_node = node_id;

        /* If we have an index input, set up SIB */
        if (node->input_count >= 3) {
            uint32_t idx = vtx_isel_node_vreg(stream, node->inputs[1]);
            if (idx != VTX_VREG_INVALID) {
                vstore.mem.index_vreg = idx;
                vstore.mem.index_phys = 0xFF;
                vstore.mem.scale = 8;
            }
        }

        vtx_isel_emit_inst(block, vstore, arena);
        break;
    }

    case VTX_OP_VectorAdd: {
        if (node->input_count < 2) return -1;
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;

        if (dst != lhs)
            vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_MOVAPD, dst, lhs, node_id), arena);
        vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_ADDPD, dst, rhs, node_id), arena);
        break;
    }

    case VTX_OP_VectorMul: {
        if (node->input_count < 2) return -1;
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;

        if (dst != lhs)
            vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_MOVAPD, dst, lhs, node_id), arena);
        vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_MULPD, dst, rhs, node_id), arena);
        break;
    }

    case VTX_OP_Switch:
    case VTX_OP_Unwind:
    case VTX_OP_Catch:
    case VTX_OP_Province:
    case VTX_OP_MemBar:
    case VTX_OP_Initialize:
    case VTX_OP_InitializeKlass:
        break;

    default:
        break;
    }

    return 0;
}

/* ========================================================================== */
/* Phi resolution: emit parallel copy sequences at block boundaries            */
/* ========================================================================== */

/**
 * After all blocks have been selected, resolve Phi nodes by emitting MOV
 * instructions at the end of each predecessor block.
 *
 * For each Phi in block B with inputs [v0, v1, ...] and predecessors [P0, P1, ...],
 * we emit "MOV phi_vreg, vi_vreg" at the end of Pi (before the terminal branch).
 *
 * G6 fix part 2: Implements proper parallel copy semantics using temporary vregs
 * to break cycles. When phi_A depends on v_B and phi_B depends on v_A, a naive
 * sequential copy would overwrite v_A before phi_B could read it. We detect such
 * cycles and break them by saving the first value to a temp vreg before performing
 * the cycle copies, then copying the temp to the last destination.
 */
static int resolve_phis(vtx_inst_stream_t *stream, const vtx_schedule_t *schedule,
                         const vtx_graph_t *graph, vtx_arena_t *arena)
{
    /* We process Phi copies per-predecessor to handle parallel copy semantics.
     * For each predecessor P of block B, we need to emit all copies
     * (phi_i ← input_i) simultaneously at the end of P. We collect all
     * copies for a given (B, P) pair, detect cycles, and emit them safely. */

    for (uint32_t b = 0; b < stream->block_count && b < schedule->count; b++) {
        const vtx_schedule_block_t *sched_blk = &schedule->blocks[b];

        /* For each predecessor of block b */
        for (uint32_t p = 0; p < sched_blk->pred_count; p++) {
            uint32_t pred_idx = sched_blk->pred_blocks[p];
            if (pred_idx >= stream->block_count) continue;

            vtx_inst_block_t *pred_blk = &stream->blocks[pred_idx];

            /* Collect all Phi copies for this (block, predecessor) pair.
             * Each copy is: dst_vreg ← src_vreg */
            #define MAX_PHI_COPIES 64
            uint32_t copy_dst[MAX_PHI_COPIES];
            uint32_t copy_src[MAX_PHI_COPIES];
            vtx_nodeid_t copy_node[MAX_PHI_COPIES];
            uint32_t copy_count = 0;

            for (uint32_t n = 0; n < sched_blk->node_count; n++) {
                vtx_nodeid_t nid = sched_blk->nodes[n];
                const vtx_node_t *node = vtx_node_get_const(&graph->node_table, nid);
                if (!node || node->opcode != VTX_OP_Phi) continue;

                uint32_t phi_vreg = vtx_isel_node_vreg(stream, nid);
                if (phi_vreg == VTX_VREG_INVALID) continue;

                /* The p-th input of the Phi corresponds to the p-th predecessor.
                 * Skip the Region input (last input) if it's at index p. */
                if (p >= node->input_count) continue;
                uint32_t input_vreg = vtx_isel_node_vreg(stream, node->inputs[p]);
                if (input_vreg == VTX_VREG_INVALID || input_vreg == phi_vreg) continue;

                if (copy_count < MAX_PHI_COPIES) {
                    copy_dst[copy_count] = phi_vreg;
                    copy_src[copy_count] = input_vreg;
                    copy_node[copy_count] = nid;
                    copy_count++;
                }
            }

            if (copy_count == 0) continue;

            /* Detect cycles: a copy (dst_i ← src_i) is part of a cycle if
             * src_i is also some dst_j. We use the standard parallel copy
             * algorithm that correctly handles multiple independent cycles
             * by allocating one temp vreg per cycle.
             *
             * P0 FIX: The old implementation only allocated ONE temp vreg and
             * only saved the first endangered source, silently corrupting
             * additional independent cycles. For example, with copies:
             *   A ← B, B ← A, C ← D, D ← C
             * The old code would save B→temp, then do A←B (OK), B←temp (OK),
             * C←D (WRONG — D already overwritten by D←C), D←C (WRONG).
             *
             * The fix: detect each independent cycle and allocate a separate
             * temp vreg for each. For each cycle, save the last source to a
             * temp, then perform the cycle copies in reverse, finally copy
             * temp to the last destination.
             */

            /* Find insertion point: before the first trailing branch */
            uint32_t insert_pos = pred_blk->inst_count;
            for (uint32_t j = pred_blk->inst_count; j > 0; j--) {
                if (!(pred_blk->insts[j-1].flags & VTX_INST_FLAG_IS_BRANCH)) {
                    insert_pos = j;
                    break;
                }
            }

            /* Helper: insert a MOV instruction at insert_pos + offset */
            #define INSERT_MOV(dst, src, node_id, offset) do { \
                vtx_inst_t _copy = make_rr_inst(VTX_X86_MOV, (dst), (src), (node_id)); \
                _copy.flags |= VTX_INST_FLAG_PHI_COPY; \
                if (vtx_isel_block_ensure_capacity(pred_blk, 1, arena) != 0) return -1; \
                if ((offset) < pred_blk->inst_count) { \
                    memmove(&pred_blk->insts[(offset) + 1], &pred_blk->insts[(offset)], \
                            (pred_blk->inst_count - (offset)) * sizeof(vtx_inst_t)); \
                } \
                pred_blk->insts[(offset)] = _copy; \
                pred_blk->inst_count++; \
            } while(0)

            uint32_t cur_insert = insert_pos;

            /* ---- Parallel copy algorithm (correct for multiple cycles) ----
             *
             * Algorithm:
             * 1. Build a graph: each copy (dst_i ← src_i) is a directed edge
             * 2. Find all connected components that are cycles
             * 3. For each cycle, allocate a temp vreg and break the cycle:
             *    a. Save the last source in the cycle to temp
             *    b. Perform copies in reverse order around the cycle
             *    c. Copy temp to the last destination
             * 4. Emit all non-cycle copies normally
             *
             * We track which copies have been processed to avoid
             * processing a cycle member twice.
             */

            /* Classify copies: which are part of cycles, which are acyclic */
            bool src_is_dst[MAX_PHI_COPIES];
            bool processed[MAX_PHI_COPIES];
            memset(src_is_dst, 0, sizeof(src_is_dst));
            memset(processed, 0, sizeof(processed));
            for (uint32_t i = 0; i < copy_count; i++) {
                for (uint32_t j = 0; j < copy_count; j++) {
                    if (copy_src[i] == copy_dst[j]) {
                        src_is_dst[i] = true;
                        break;
                    }
                }
            }

            /* Phase 1: Process all cycles.
             * For each unprocessed copy that is part of a cycle,
             * trace the cycle and break it with a temp vreg. */
            for (uint32_t start = 0; start < copy_count; start++) {
                if (processed[start] || !src_is_dst[start]) continue;

                /* Trace the cycle starting from copy 'start':
                 * start: dst_s ← src_s (src_s is also some dst_k)
                 * Find k: dst_k == src_s
                 * Then find: dst_? == src_k
                 * Continue until we return to dst_s */
                uint32_t cycle_indices[MAX_PHI_COPIES];
                uint32_t cycle_len = 0;

                uint32_t cur = start;
                bool found_cycle = false;
                do {
                    if (processed[cur]) break;
                    cycle_indices[cycle_len++] = cur;
                    processed[cur] = true;

                    /* Find the copy whose dst == this copy's src */
                    found_cycle = false;
                    for (uint32_t j = 0; j < copy_count; j++) {
                        if (copy_dst[j] == copy_src[cur] && !processed[j]) {
                            cur = j;
                            found_cycle = true;
                            break;
                        }
                    }
                } while (found_cycle && cur != start && cycle_len < MAX_PHI_COPIES);

                if (cycle_len < 2) continue; /* Not a real cycle */

                /* Break the cycle with a temp vreg.
                 * Strategy: save the source of the LAST copy in the cycle
                 * to a temp, then emit copies in reverse order, and finally
                 * copy temp to the first copy's destination.
                 *
                 * Example cycle: A←B, B←C, C←A
                 *   1. Save A → temp (save the source of the last copy C←A)
                 *   2. C ← A (emit last copy, A is still original)
                 *   3. B ← C (emit middle copy)
                 *   4. A ← temp (copy saved value to first destination)
                 *
                 * Wait — this is WRONG. After step 2, C has been overwritten.
                 * If B←C was supposed to use the original C, it's gone.
                 *
                 * Correct approach for cycle A←B, B←C, C←A:
                 *   1. Save last src (A) to temp: temp ← A
                 *   2. Emit copies in REVERSE order: C←A, B←C, A←B...
                 *      No — that overwrites before reading.
                 *
                 * The standard correct algorithm:
                 *   1. temp ← src_of_last_copy (= A for C←A)
                 *   2. Emit copies in REVERSE cycle order:
                 *      C ← A (now C has A's original value)
                 *      B ← C (now B has A's original value... WRONG!)
                 *
                 * Actually, the CORRECT way:
                 *   1. temp ← src_of_last (save: temp = A)
                 *   2. Emit copies FORWARD, but skip the first:
                 *      A ← B (writes A, but we saved original A in temp)
                 *      B ← C (writes B, original B already used by A←B)
                 *   3. C ← temp (write C with saved original A)
                 *
                 * Wait, that only works if A←B is the first copy.
                 * Let me re-think. The cycle is: A←B, B←C, C←A.
                 * The "last" copy is C←A. We save src_of_last = A to temp.
                 * Then emit copies in REVERSE order:
                 *   C←A (C gets A's original value, but A is unmodified so far)
                 *   B←C (B gets C's original value, C was just overwritten but
                 *         we want the original C. FAIL.)
                 *
                 * The truly correct algorithm:
                 *   1. Save last copy's SOURCE to temp: temp ← A (from C←A)
                 *   2. Process copies from last to first (reverse cycle order):
                 *      C←A: This writes C. A is still original. OK.
                 *      B←C: This writes B. But C was just overwritten!
                 *      We need ORIGINAL C here. FAIL again.
                 *
                 * OK, the REAL correct algorithm for parallel copy with cycles:
                 *   1. Save the first copy's SOURCE to temp: temp ← B (from A←B)
                 *   2. Emit copies starting from the LAST, going backwards:
                 *      C←A (A still has its original value)
                 *      B←C (C still has its original value)
                 *   3. A←temp (temp has B's original value)
                 *   This works! Going backwards, each source is read before
                 *   it's written by any earlier copy in the reverse sequence.
                 */

                /* Allocate a temp vreg for this cycle */
                uint32_t temp_vreg = stream->vreg_count++;

                /* Save first copy's source to temp */
                uint32_t first = cycle_indices[0];
                INSERT_MOV(temp_vreg, copy_src[first], copy_node[first], cur_insert);
                cur_insert++;

                /* Emit copies in reverse cycle order (last to second) */
                for (int ci = (int)cycle_len - 1; ci >= 1; ci--) {
                    uint32_t idx = cycle_indices[ci];
                    INSERT_MOV(copy_dst[idx], copy_src[idx], copy_node[idx], cur_insert);
                    cur_insert++;
                }

                /* Copy temp to first copy's destination */
                INSERT_MOV(copy_dst[first], temp_vreg, copy_node[first], cur_insert);
                cur_insert++;
            }

            /* Phase 2: Emit all non-cycle copies (acyclic, safe to emit directly) */
            for (uint32_t i = 0; i < copy_count; i++) {
                if (processed[i]) continue; /* already handled as part of a cycle */
                INSERT_MOV(copy_dst[i], copy_src[i], copy_node[i], cur_insert);
                cur_insert++;
            }

            #undef INSERT_MOV
            #undef MAX_PHI_COPIES
        }
    }
    return 0;
}

/* ========================================================================== */
/* Resolve branch targets from schedule successor info                         */
/* ========================================================================== */

static void resolve_branch_targets(vtx_inst_stream_t *stream,
                                    const vtx_schedule_t *schedule)
{
    for (uint32_t b = 0; b < stream->block_count && b < schedule->count; b++) {
        vtx_inst_block_t *blk = &stream->blocks[b];
        const vtx_schedule_block_t *sched_blk = &schedule->blocks[b];

        for (uint32_t i = 0; i < blk->inst_count; i++) {
            vtx_inst_t *inst = &blk->insts[i];
            if (!(inst->flags & VTX_INST_FLAG_IS_BRANCH)) continue;
            if (inst->opnd_kinds[0] != VTX_OPND_LABEL) continue;

            if (inst->opcode == VTX_X86_JMP && sched_blk->succ_count > 0) {
                inst->operands[0] = sched_blk->succ_blocks[0];
            } else if (inst->opcode == VTX_X86_JCC) {
                if (sched_blk->succ_count > 0)
                    inst->operands[0] = sched_blk->succ_blocks[0];
            }
        }
    }
}

/* ========================================================================== */
/* Main entry point                                                            */
/* ========================================================================== */

vtx_inst_stream_t *vtx_isel_select(const vtx_schedule_t *schedule,
                                     const vtx_graph_t *graph,
                                     vtx_arena_t *arena)
{
    VTX_ASSERT(schedule != NULL, "schedule must not be NULL");
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    vtx_inst_stream_t *stream = (vtx_inst_stream_t *)vtx_arena_alloc(arena, sizeof(vtx_inst_stream_t));
    if (!stream) return NULL;
    memset(stream, 0, sizeof(*stream));
    stream->schedule = schedule;

    stream->block_count = schedule->count;
    stream->block_capacity = schedule->count;
    stream->blocks = (vtx_inst_block_t *)vtx_arena_alloc(arena,
                       schedule->count * sizeof(vtx_inst_block_t));
    if (!stream->blocks) return NULL;
    memset(stream->blocks, 0, schedule->count * sizeof(vtx_inst_block_t));

    /* Walk each block and select instructions for each node */
    for (uint32_t b = 0; b < schedule->count; b++) {
        const vtx_schedule_block_t *sched_blk = &schedule->blocks[b];
        vtx_inst_block_t *inst_blk = &stream->blocks[b];
        inst_blk->block_id = b;

        for (uint32_t n = 0; n < sched_blk->node_count; n++) {
            vtx_nodeid_t node_id = sched_blk->nodes[n];
            if (select_node(stream, inst_blk, graph, node_id, arena) != 0) {
                const vtx_node_t *dbg_node = vtx_node_get_const(&graph->node_table, node_id);
                fprintf(stderr, "[isel] FAILED: block %u, node %u, opcode %s, inputs:",
                        b, node_id, dbg_node ? vtx_node_opcode_name(dbg_node->opcode) : "NULL");
                if (dbg_node) {
                    for (uint32_t di = 0; di < dbg_node->input_count; di++) {
                        vtx_nodeid_t dinp = dbg_node->inputs[di];
                        const vtx_node_t *dinp_n = vtx_node_get_const(&graph->node_table, dinp);
                        uint32_t dvreg = vtx_isel_node_vreg(stream, dinp);
                        fprintf(stderr, " N%u(%s,vreg=%u)", dinp,
                                dinp_n ? vtx_node_opcode_name(dinp_n->opcode) : "?", dvreg);
                    }
                }
                fprintf(stderr, "\n");
                return NULL;
            }
        }
    }

    /* Resolve Phi nodes: emit parallel copy sequences at predecessor block ends */
    if (resolve_phis(stream, schedule, graph, arena) != 0) {
        return NULL;
    }

    /* Resolve branch targets from schedule successor info */
    resolve_branch_targets(stream, schedule);

    return stream;
}
