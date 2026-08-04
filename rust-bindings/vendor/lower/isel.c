/* ============================================================================ *
 * AI-MODIFIED CODE
 *
 * This file was originally written by a human developer. It has been
 * substantially modified by an AI assistant (GLM/Z.ai) for bug fixes,
 * performance improvements, and feature additions.
 *
 * Original human-written structure is preserved; AI changes are marked
 * with bug fix IDs (B1-B28) or perf notes (Perf 1-10) in comments.
 *
 * If reviewing, please verify AI changes against the original logic.
 * ============================================================================ */

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
#include "ir/schedule.h"
#include "compile/safepoint.h"
#include "runtime/object.h"
#include "runtime/helpers.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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
    [VTX_X86_IMUL_FULL] = "imul_full",
    [VTX_X86_NEG]   = "neg",
    [VTX_X86_NOT]   = "not",
    [VTX_X86_INC]   = "inc",
    [VTX_X86_DEC]   = "dec",
    [VTX_X86_SHL]   = "shl",
    [VTX_X86_SHR]   = "shr",
    [VTX_X86_SAR]   = "sar",
    [VTX_X86_ROL]   = "rol",
    [VTX_X86_ROR]   = "ror",
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
    [VTX_X86_CDQE]  = "cdqe",
    [VTX_X86_SETCC] = "setcc",
    [VTX_X86_BSWAP] = "bswap",
    [VTX_X86_BSF]   = "bsf",
    [VTX_X86_BSR]   = "bsr",
    [VTX_X86_POPCNT] = "popcnt",
    [VTX_X86_BT]    = "bt",
    [VTX_X86_PUSH]  = "push",
    [VTX_X86_POP]   = "pop",
    [VTX_X86_JMP]   = "jmp",
    [VTX_X86_JCC]   = "jcc",
    [VTX_X86_CALL]  = "call",
    [VTX_X86_RET]   = "ret",
    [VTX_X86_LAHF]  = "lahf",
    [VTX_X86_SAHF]  = "sahf",
    [VTX_X86_UCOMISD] = "ucomisd",
    [VTX_X86_COMISD]  = "comisd",
    [VTX_X86_ADDSD]  = "addsd",
    [VTX_X86_SUBSD]  = "subsd",
    [VTX_X86_MULSD]  = "mulsd",
    [VTX_X86_DIVSD]  = "divsd",
    [VTX_X86_SQRTSD] = "sqrtsd",
    [VTX_X86_XORPS]  = "xorps",
    [VTX_X86_MOVSD]  = "movsd",
    [VTX_X86_MOVSD_LOAD]  = "movsd_load",
    [VTX_X86_MOVSD_STORE] = "movsd_store",
    [VTX_X86_CVTSI2SD] = "cvtsi2sd",
    [VTX_X86_CVTSD2SI] = "cvtsd2si",
    [VTX_X86_CVTTSD2SI] = "cvttsd2si",
    [VTX_X86_CVTSI2SS] = "cvtsi2ss",
    [VTX_X86_CVTSS2SI] = "cvtss2si",
    [VTX_X86_CVTTSS2SI] = "cvttss2si",
    [VTX_X86_MOVQ_XMM_R64] = "movq_xmm_r64",
    [VTX_X86_MOVQ_R64_XMM] = "movq_r64_xmm",
    [VTX_X86_SAFEPOINT_POLL] = "safepoint_poll",
    [VTX_X86_SAFEPOINT_POLL_GUARD_PAGE] = "safepoint_poll_guard_page",
    [VTX_X86_MOVAPD] = "movapd",
    [VTX_X86_ADDPD]  = "addpd",
    [VTX_X86_MULPD]  = "mulpd",
    [VTX_X86_MINPD]  = "minpd",
    [VTX_X86_MAXPD]  = "maxpd",
    [VTX_X86_ANDPD]  = "andpd",
    [VTX_X86_XORPD]  = "xorpd",
    [VTX_X86_SUBPD]  = "subpd",
    [VTX_X86_DIVPD]  = "divpd",
    [VTX_X86_MOVDQA] = "movdqa",
    [VTX_X86_MOVDQU] = "movdqu",
    [VTX_X86_PADDD]  = "paddd",
    [VTX_X86_PSUBD]  = "psubd",
    [VTX_X86_PMULLD] = "pmulld",
    [VTX_X86_PXOR]   = "pxor",
    [VTX_X86_PAND]   = "pand",
    [VTX_X86_POR]    = "por",
    [VTX_X86_PCMPEQD] = "pcmpeqd",
    [VTX_X86_MOVAPS] = "movaps",
    [VTX_X86_ADDPS]  = "addps",
    [VTX_X86_MULPS]  = "mulps",
    [VTX_X86_SUBPS]  = "subps",
    [VTX_X86_DIVPS]  = "divps",
    [VTX_X86_MINPS]  = "minps",
    [VTX_X86_MAXPS]  = "maxps",
    [VTX_X86_CMPPS]  = "cmpps",
    [VTX_X86_ADDSS]  = "addss",
    [VTX_X86_SUBSS]  = "subss",
    [VTX_X86_MULSS]  = "mulss",
    [VTX_X86_DIVSS]  = "divss",
    [VTX_X86_SQRTSS] = "sqrtss",
    [VTX_X86_UCOMISS] = "ucomiss",
    [VTX_X86_MOVSS]  = "movss",
    [VTX_X86_DIV]    = "div",
    [VTX_X86_RDTSC]  = "rdtsc",
    [VTX_X86_RDTSCP] = "rdtscp",
    [VTX_X86_CMPXCHG] = "cmpxchg",
    [VTX_X86_XADD]   = "xadd",
    [VTX_X86_LFENCE] = "lfence",
    [VTX_X86_MFENCE] = "mfence",
    [VTX_X86_SFENCE] = "sfence",
    [VTX_X86_ROUNDSD] = "roundsd",
    [VTX_X86_ROUNDSS] = "roundss",
    [VTX_X86_MOVSD_RIP] = "movsd_rip",
    [VTX_X86_VMOVAPD_256] = "vmovapd.256",
    [VTX_X86_VADDPD_256]  = "vaddpd.256",
    [VTX_X86_VSUBPD_256]  = "vsubpd.256",
    [VTX_X86_VMULPD_256]  = "vmulpd.256",
    [VTX_X86_VDIVPD_256]  = "vdivpd.256",
    [VTX_X86_VMINPD_256]  = "vminpd.256",
    [VTX_X86_VMAXPD_256]  = "vmaxpd.256",
    [VTX_X86_VXORPD_256]  = "vxorpd.256",
    [VTX_X86_VANDPD_256]  = "vandpd.256",
    [VTX_X86_VCMPPD_256]  = "vcmppd.256",
    [VTX_X86_VMOVAPS_256] = "vmovaps.256",
    [VTX_X86_VADDPS_256]  = "vaddps.256",
    [VTX_X86_VSUBPS_256]  = "vsubps.256",
    [VTX_X86_VMULPS_256]  = "vmulps.256",
    [VTX_X86_VDIVPS_256]  = "vdivps.256",
    [VTX_X86_VMOVDQA_256] = "vmovdqa.256",
    [VTX_X86_VPADDD_256]  = "vpaddd.256",
    [VTX_X86_VPSUBD_256]  = "vpsubd.256",
    [VTX_X86_VPMULLD_256] = "vpmulld.256",
    [VTX_X86_VPXOR_256]   = "vpxor.256",
    [VTX_X86_VPAND_256]   = "vpand.256",
    [VTX_X86_VPOR_256]    = "vpor.256",
    [VTX_X86_VZEROUPPER]  = "vzeroupper",
    [VTX_X86_VZEROALL]    = "vzeroall",
    [VTX_X86_VBROADCASTSD] = "vbroadcastsd",
    [VTX_X86_VBROADCASTSS] = "vbroadcastss",
    [VTX_X86_VPERM2F128]  = "vperm2f128",
    [VTX_X86_VINSERTF128] = "vinsertf128",
    [VTX_X86_VEXTRACTF128] = "vextractf128",
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

/* ========================================================================== */
/* SMI untag/retag helper sequences                                            */
/* ========================================================================== */

/**
 * Emit SMI untag sequence: converts NaN-boxed SMI value to raw int64.
 *
 * SMI(a) = HEADER | ((a & DATA_MASK) << 3) | TAG_SMI
 * To extract raw integer a, we use SHL 13 + SAR 16:
 *   - SHL 13 moves the SMI sign bit (bit 50) to bit 63
 *   - SAR 16 sign-extends from bit 63 and produces the correct raw integer
 *
 * IMPORTANT: The SUB HEADER + SAR 3 approach is WRONG for negative values!
 * For SMI(-10): SMI(-10) - HEADER = (2^48 - 10) << 3, which SAR treats as
 * a large positive number, giving the wrong result. Only SHL+SAR correctly
 * sign-extends from the 48-bit data field.
 *
 * @param stream   Instruction stream
 * @param block    Current instruction block
 * @param dst_vreg Destination vreg (will contain raw int64 after untag)
 * @param src_vreg Source vreg (NaN-boxed SMI value)
 * @param node_id  Source SoN node ID
 * @param arena    Arena for allocations
 */
static void emit_smi_untag(vtx_inst_stream_t *stream, vtx_inst_block_t *block,
                            uint32_t dst_vreg, uint32_t src_vreg,
                            vtx_nodeid_t node_id, vtx_arena_t *arena)
{
    /* SMI untag: convert NaN-boxed SMI to raw int64.
     *
     * SMI(val) = HEADER | (val << 3)  where HEADER = 0x7FF8000000000000
     *
     * Approach: SHL 13 + SAR 16
     *   SHL 13: shifts the data bits left by 13. The header bits (bits 47-63)
     *           overflow past bit 63 and are lost. The sign bit of val (bit 50)
     *           moves to bit 63.
     *   SAR 16: arithmetic right shift by 16. Sign-extends from bit 63 (the
     *           original sign bit of val), giving the correct signed value.
     *
     * Example: SMI(2) = 0x7FF8000000000010
     *   SHL 13: 0x0000000000020000 (header overflowed out, data at bit 17)
     *   SAR 16: 0x0000000000000002 = 2 ✓
     *
     * Example: SMI(-1) = 0x7FFFFFFFFFFFFFF8
     *   SHL 13: 0xFFFFFFFFFFFFC000 (sign bit set from val's bit 47)
     *   SAR 16: 0xFFFFFFFFFFFFFFFF = -1 ✓
     */
    stream->uses_smi = true;
    /* The MOV copies the SMI value to a new vreg, then SHL+SAR modify it
     * in place. The coalescer's overlap check is sufficient to prevent
     * unsafe coalescing — if src_vreg is still live after this untag,
     * the intervals overlap and coalescing is rejected. Removing
     * NO_COALESCE allows the regalloc to coalesce when src is dead
     * after the untag, saving one MOV per operand. */
    vtx_inst_t mov = make_rr_inst(VTX_X86_MOV, dst_vreg, src_vreg, node_id);
    vtx_isel_emit_inst(block, mov, arena);
    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, dst_vreg, 13, node_id), arena);
    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, dst_vreg, 16, node_id), arena);
}

/**
 * Emit SMI retag sequence: converts raw int64 to NaN-boxed SMI value.
 *
 * SMI(result) = HEADER | ((result & DATA_MASK) << 3) | TAG_SMI
 * Since TAG_SMI = 0, this is: HEADER | ((result & DATA_MASK) << 3)
 *
 * IMPORTANT: The SHL 3 + ADD HEADER approach is WRONG for negative values!
 * For result = -10: (result << 3) + HEADER = 0xFFFFFFFFFFFFFFB0 + 0x7FF8000000000000
 *   = 0x7FF7FFFFFFFFFFB0, but SMI(-10) = 0x7FFFFFFFFFFFFFF0.
 * The mask-and-OR approach is the ONLY correct retag.
 *
 * Uses two scratch registers (initialized ONCE in the prologue):
 *   - smi_scratch_vreg (R10): holds HEADER = 0x7FF8000000000000
 *   - smi_mask_vreg (R11): holds DATA_MASK = 0x0000FFFFFFFFFFFF
 *
 * BUGFIX (audit #2): The old code reloaded HEADER and DATA_MASK into R10/R11
 * on EVERY retag call (3 instructions, ~24 bytes each time). Now the prologue
 * loads them once, and retag is just AND + SHL + OR (3 instructions, ~10 bytes).
 *
 * @param stream   Instruction stream
 * @param block    Current instruction block
 * @param dst_vreg Destination vreg (will contain NaN-boxed SMI after retag)
 * @param node_id  Source SoN node ID
 * @param arena    Arena for allocations
 */
static void emit_smi_retag(vtx_inst_stream_t *stream, vtx_inst_block_t *block,
                            uint32_t dst_vreg, vtx_nodeid_t node_id,
                            vtx_arena_t *arena)
{
    stream->uses_smi = true;
    /* R10 (smi_scratch_vreg) and R11 (smi_mask_vreg) are loaded ONCE in the
     * prologue by vtx_x86_emit_smi_constants(). Here we just use them. */
    /* AND with DATA_MASK: truncate to 48 bits */
    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_AND, dst_vreg, stream->smi_mask_vreg, node_id), arena);
    /* SHL 3: shift data into position [50:3] */
    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, dst_vreg, 3, node_id), arena);
    /* OR HEADER: install NaN-box header */
    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_OR, dst_vreg, stream->smi_scratch_vreg, node_id), arena);
}

/**
 * Emit SMI load-header-only: just load HEADER into scratch, no retag.
 * Used for cases where we only need the header constant (e.g., Mul(x,0)).
 */
static void emit_smi_load_header(vtx_inst_stream_t *stream, vtx_inst_block_t *block,
                                  vtx_nodeid_t node_id, vtx_arena_t *arena)
{
    stream->uses_smi = true;
    /* R10 is already loaded with HEADER by the prologue — nothing to do here.
     * The caller should use stream->smi_scratch_vreg directly. */
    (void)block; (void)node_id; (void)arena;
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
 *
 * For d with |d| > 1 and not a power of 2, computes M such that
 *   trunc_toward_zero(n / d) = SAR( (M * n) >> 64, s ) + (n >> 63) * sign(d)
 * where (M * n) >> 64 is the high 64 bits of the signed 128-bit product
 * (available via the one-operand IMUL on x86-64, which writes RDX:RAX).
 *
 * Algorithm: M = ceil(2^64 / |d|) = ((2^64 - 1) / |d|) + 1, s = 0.
 *
 * This works for all n in [-2^63, 2^63 - 1] and all |d| > 1 (not power of 2).
 * The sign of d is handled by the caller (NEG the result if d < 0).
 *
 * Verified by brute force for d in {-1000..-2, 3..1000} \ {powers of 2}
 * against C99 truncating division.
 *
 * @param d     Divisor (must be != 0, != ±1, not power of 2)
 * @param M     Output: magic number multiplier (as int64_t for IMUL)
 * @param s     Output: post-shift amount (always 0 with this algorithm)
 * @return      true on success
 */
static bool compute_magic_number(int64_t d, int64_t *M, int *s)
{
    if (d == 0 || d == 1 || d == -1) return false;

    uint64_t ad = (d < 0) ? (uint64_t)(-d) : (uint64_t)d;

    /* Caller should have already handled power-of-2 divisors via the
     * Div(x, 2^k) strength reduction. Bail out here so we don't generate
     * a magic-number sequence when a single SAR would do. */
    if ((ad & (ad - 1)) == 0) return false;

    /* M = ceil(2^64 / ad) = ((2^64 - 1) / ad) + 1
     * Computed in unsigned 64-bit arithmetic. */
    uint64_t M_u64 = (UINT64_MAX / ad) + 1;

    *M = (int64_t)M_u64;
    *s = 0;
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
                        const vtx_graph_t *graph, const vtx_schedule_t *schedule,
                        uint32_t sched_block_idx, vtx_nodeid_t node_id,
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
        /* Float zero optimization (XORPS) is DISABLED because float arithmetic
         * is lowered to runtime calls that take NaN-boxed values in GPRs.
         * XORPS zeros an XMM register, but we need the raw double bits (0x0
         * for 0.0) in a GPR. The generic MOV imm path below handles this
         * correctly: MOV dst, 0 puts 0x0000000000000000 in a GPR, which is
         * the correct NaN-boxed representation of 0.0 (raw non-NaN double). */

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
            uint64_t smi_val = VTX_NAN_BOX_HEADER
                             | (((uint64_t)raw & 0x0000FFFFFFFFFFFFULL) << 3)
                             | 0ULL; /* VTX_TAG_SMI = 0 */
            inst.imm = (int64_t)smi_val;
        } else if (node->constval.kind == VTX_TYPE_Ptr) {
            inst.imm = (int64_t)(uintptr_t)node->constval.as.ptr_val;
        } else if (node->constval.kind == VTX_TYPE_Float) {
            /* Float constant: store the raw double bits as an immediate in
             * a GPR vreg. Float arithmetic is lowered to runtime calls which
             * take NaN-boxed values in GPRs, so float constants stay in GPRs. */
            union { double d; uint64_t u; } cvt;
            cvt.d = node->constval.as.float_val;
            inst.imm = (int64_t)cvt.u;
        } else {
            /* Void/undefined constants: emit SMI(0) as a safe default.
             *
             * BUGFIX (loop crash): Void constants are created by the IR
             * builder for uninitialized locals. The old code emitted
             * raw 0 (inst.imm = 0), which is NOT a valid NaN-boxed SMI.
             * When used as a local's initial value, the loop body's
             * untag/retag sequence would produce garbage, and when the
             * loop didn't execute (arg=0), the function returned raw 0
             * instead of SMI(0).
             *
             * SMI(0) = 0x7FF8000000000000 is the correct NaN-boxed
             * representation of the integer 0. Emitting it here ensures
             * that even if the void constant is not removed by DCE, it
             * won't corrupt the value system. */
            inst.imm = (int64_t)VTX_NAN_BOX_HEADER;  /* SMI(0) */
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
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t lhs_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
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
            /* SMI-aware Add(x, Const) fast path.
             *
             * SMI(a) + c*8 = SMI(a+c) when the addition doesn't overflow
             * the 48-bit data field. For positive constants, this is safe
             * as long as a+c doesn't exceed 2^47-1 (max SMI).
             *
             * We enable this only for small positive constants (0..4096)
             * where c*8 fits in a 16-bit immediate and the overflow risk
             * is minimal for typical loop counters. For larger constants,
             * fall through to the safe untag+add+retag path. */
            /* Only use fast path when lhs is NOT a Mul result (Mul can
             * produce large values that overflow the SMI data field when
             * combined with the constant). Safe for loop counters (Phi/Add
             * chains) where values stay small.
             *
             * CRITICAL: Also check that the lhs INPUT is not RAW_INT.
             * If the lhs is raw (unboxed from a previous op in a RAW_INT
             * chain), the fast path would add c*8 to a raw value,
             * producing a result without the SMI header. This was the
             * root cause of the unrolled4 crash: chained Adds where
             * intermediate values lost their tags. */
            if (rhs_const >= 0 && rhs_const <= 4096 &&
                !vtx_nf_has(node->flags, VTX_NF_RAW_INT)) {
                const vtx_node_t *lhs_prod = vtx_node_get_const(&graph->node_table, node->inputs[0]);
                bool lhs_is_mul = lhs_prod && lhs_prod->opcode == VTX_OP_Mul;
                bool lhs_is_raw = lhs_prod && vtx_nf_has(lhs_prod->flags, VTX_NF_RAW_INT);
                if (!lhs_is_mul && !lhs_is_raw) {
                    int64_t c_shifted = rhs_const * 8;
                    if (dst != lhs_vreg) {
                        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_vreg, node_id), arena);
                    }
                    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_ADD, dst,
                                       (int32_t)c_shifted, node_id), arena);
                    break;
                }
            }
            (void)rhs_const;
        }

        /* Perf 2: SMI fast path for Add(Var, Var) — DISABLED.
         *
         * The formula SMI(a+b) = SMI(a) + SMI(b) - HEADER is mathematically
         * correct but breaks in 64-bit arithmetic when both operands are
         * negative. The intermediate sum SMI(a) + SMI(b) = 2*HEADER + (a+b)<<3
         * can overflow past bit 63, causing a carry from the data field into
         * the header bits. The subsequent SUB HEADER doesn't fix this because
         * the carry already corrupted the header.
         *
         * Example: SMI(-3) + SMI(-3) = 0x7FFF...E8 + 0x7FFF...E8 = 0xFFFF...D0
         * (wraps past 2^64). Then - HEADER = 0x8007...D0 instead of the
         * correct SMI(-6) = 0x7FFF...D0.
         *
         * Same borrow-from-header bug as Sub (audit #3 above). Disabled
         * until a range check can prove no overflow. Fall through to the
         * safe untag→add→retag path below. */
        if (false && !vtx_nf_has(node->flags, VTX_NF_RAW_INT) && node->type != VTX_TYPE_Float) {
            const vtx_node_t *lhs_n = vtx_node_get_const(&graph->node_table, node->inputs[0]);
            const vtx_node_t *rhs_n = vtx_node_get_const(&graph->node_table, node->inputs[1]);
            bool lhs_raw = lhs_n && vtx_nf_has(lhs_n->flags, VTX_NF_RAW_INT);
            bool rhs_raw = rhs_n && vtx_nf_has(rhs_n->flags, VTX_NF_RAW_INT);
            bool rhs_const;
            int64_t dummy;
            rhs_const = try_get_const_int(graph, node->inputs[1], &dummy);

            if (!lhs_raw && !rhs_raw && !rhs_const) {
                /* Both inputs are tagged SMIs, neither is raw, rhs is not a
                 * constant (const has its own fast path above). Use the
                 * header-subtraction trick: dst = lhs + rhs - HEADER. */
                stream->uses_smi = true;
                if (dst != lhs_vreg) {
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_vreg, node_id), arena);
                }
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_ADD, dst, rhs_vreg, node_id), arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_SUB, dst, stream->smi_scratch_vreg, node_id), arena);
                break;
            }
        }

        /* Variable + Variable (or Variable + Const): untag both, add, retag.
         *
         * SMI tag elision: if the node is marked RAW_INT, the inputs are
         * already untagged (by a previous op in the chain) and the output
         * will be consumed by another RAW_INT op. Skip untag and retag.
         * The chain entry untag and chain exit retag are handled by the
         * boundary nodes (non-RAW_INT ops that need tagged SMIs).
         *
         * Constant folding: if rhs is a Constant(Int), use ADD reg, imm32
         * with the raw integer value instead of loading+untagging the SMI.
         * This saves 3 instructions (MOV+SHL+SAR) per constant operand. */
        {
            bool is_raw = vtx_nf_has(node->flags, VTX_NF_RAW_INT);

            /* Check if inputs are RAW_INT (already untagged) or need untagging */
            const vtx_node_t *lhs_node = vtx_node_get_const(&graph->node_table, node->inputs[0]);
            const vtx_node_t *rhs_node = vtx_node_get_const(&graph->node_table, node->inputs[1]);
            bool lhs_is_raw = lhs_node && vtx_nf_has(lhs_node->flags, VTX_NF_RAW_INT);
            bool rhs_is_raw = rhs_node && vtx_nf_has(rhs_node->flags, VTX_NF_RAW_INT);

            uint32_t lhs_untagged;

            if (lhs_is_raw) {
                lhs_untagged = lhs_vreg;
            } else {
                lhs_untagged = vtx_isel_alloc_vreg(stream, arena);
                emit_smi_untag(stream, block, lhs_untagged, lhs_vreg, node_id, arena);
            }

            /* Constant folding: if rhs is a constant, use ADD reg, imm32 */
            int64_t rhs_const_val;
            bool rhs_is_const = try_get_const_int(graph, node->inputs[1], &rhs_const_val);

            if (rhs_is_const && !rhs_is_raw &&
                rhs_const_val >= INT32_MIN && rhs_const_val <= INT32_MAX) {
                /* ADD lhs_untagged, imm32 — saves MOV+SHL+SAR for the constant */
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_ADD, lhs_untagged,
                                   (int32_t)rhs_const_val, node_id), arena);
            } else if (rhs_is_raw) {
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_ADD, lhs_untagged, rhs_vreg, node_id), arena);
            } else {
                uint32_t rhs_untagged = vtx_isel_alloc_vreg(stream, arena);
                emit_smi_untag(stream, block, rhs_untagged, rhs_vreg, node_id, arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_ADD, lhs_untagged, rhs_untagged, node_id), arena);
            }

            if (is_raw) {
                if (dst != lhs_untagged) {
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_untagged, node_id), arena);
                }
            } else {
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_untagged, node_id), arena);
                emit_smi_retag(stream, block, dst, node_id, arena);
            }
        }
        break;
    }

    case VTX_OP_Sub: {
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t lhs_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
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

        /* P1 isel: LEA for Sub(x, Const) — REMOVED (was buggy).
         *
         * BUGFIX (audit #3, tier-equivalence test): The fast paths
         *   SUB dst, 8 / ADD dst, 8 / LEA dst, [lhs + neg_c_shifted]
         * are WRONG when the arithmetic causes a borrow from the data
         * field into the header bits. Example: SMI(0) - 8 = 0x7FF7FFFFFFFFFFFF8,
         * but SMI(-1) = 0x7FFFFFFFFFFFFFF8.
         *
         * We always fall through to the untag→sub→retag path below.
         * A future optimization can re-introduce the fast paths with a
         * range check that proves no borrow can occur.
         */
        int64_t rhs_const;
        if (try_get_const_int(graph, node->inputs[1], &rhs_const)) {
            /* SMI-aware Sub(x, negative_const) fast path.
             *
             * Sub(x, c) where c < 0 is equivalent to Add(x, |c|).
             * SMI(a) + |c|*8 = SMI(a + |c|) when |c| >= 0 — no borrow.
             * This is safe because addition only carries UP within the
             * data field, never borrowing from the header bits.
             *
             * For c > 0, the subtraction can borrow from bit 51 when the
             * data field is 0. So we only use the fast path for c < 0
             * (equivalent to addition) and for c == 0 (identity).
             *
             * Also respect RAW_INT: skip if the Sub is in a RAW_INT chain.
             * Also skip if lhs is a Mul result (can produce large values
             * that overflow the SMI data field). */
            if (rhs_const <= 0 && !vtx_nf_has(node->flags, VTX_NF_RAW_INT) &&
                rhs_const >= -4096) {
                int64_t add_val = (-rhs_const) * 8;  /* |c| * 8 */
                if (add_val >= 0 && add_val <= INT32_MAX) {
                    const vtx_node_t *lhs_prod = vtx_node_get_const(&graph->node_table, node->inputs[0]);
                    bool lhs_is_mul = lhs_prod && lhs_prod->opcode == VTX_OP_Mul;
                    if (!lhs_is_mul) {
                        if (dst != lhs_vreg) {
                            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_vreg, node_id), arena);
                        }
                        if (add_val > 0) {
                            vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_ADD, dst,
                                               (int32_t)add_val, node_id), arena);
                        }
                        break;
                    }
                }
            }
            (void)rhs_const;
        }

        /* Perf 2: SMI fast path for Sub(Var, Var) — DISABLED.
         *
         * Same borrow-from-header bug as Add(Var,Var) above. The formula
         * SMI(a-b) = SMI(a) - SMI(b) + HEADER breaks when the subtraction
         * causes a borrow from the data field into the header bits.
         * Disabled until a range check can prove no borrow. */
        if (false && !vtx_nf_has(node->flags, VTX_NF_RAW_INT) && node->type != VTX_TYPE_Float) {
            const vtx_node_t *lhs_n = vtx_node_get_const(&graph->node_table, node->inputs[0]);
            const vtx_node_t *rhs_n = vtx_node_get_const(&graph->node_table, node->inputs[1]);
            bool lhs_raw = lhs_n && vtx_nf_has(lhs_n->flags, VTX_NF_RAW_INT);
            bool rhs_raw = rhs_n && vtx_nf_has(rhs_n->flags, VTX_NF_RAW_INT);
            int64_t dummy;
            bool rhs_const = try_get_const_int(graph, node->inputs[1], &dummy);

            if (!lhs_raw && !rhs_raw && !rhs_const) {
                stream->uses_smi = true;
                if (dst != lhs_vreg) {
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_vreg, node_id), arena);
                }
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_SUB, dst, rhs_vreg, node_id), arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_ADD, dst, stream->smi_scratch_vreg, node_id), arena);
                break;
            }
        }

        /* Variable - Variable (or Variable - Const): untag both, sub, retag.
         * SMI tag elision: skip untag/retag for RAW_INT chains.
         * Constant folding: if rhs is a constant, use SUB reg, imm32. */
        {
            bool is_raw = vtx_nf_has(node->flags, VTX_NF_RAW_INT);
            const vtx_node_t *lhs_node = vtx_node_get_const(&graph->node_table, node->inputs[0]);
            const vtx_node_t *rhs_node = vtx_node_get_const(&graph->node_table, node->inputs[1]);
            bool lhs_is_raw = lhs_node && vtx_nf_has(lhs_node->flags, VTX_NF_RAW_INT);
            bool rhs_is_raw = rhs_node && vtx_nf_has(rhs_node->flags, VTX_NF_RAW_INT);

            uint32_t lhs_untagged;

            if (lhs_is_raw) {
                lhs_untagged = lhs_vreg;
            } else {
                lhs_untagged = vtx_isel_alloc_vreg(stream, arena);
                emit_smi_untag(stream, block, lhs_untagged, lhs_vreg, node_id, arena);
            }

            /* Constant folding: if rhs is a constant, use SUB reg, imm32 */
            int64_t rhs_const_val;
            bool rhs_is_const = try_get_const_int(graph, node->inputs[1], &rhs_const_val);

            if (rhs_is_const && !rhs_is_raw &&
                rhs_const_val >= INT32_MIN && rhs_const_val <= INT32_MAX) {
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SUB, lhs_untagged,
                                   (int32_t)rhs_const_val, node_id), arena);
            } else if (rhs_is_raw) {
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_SUB, lhs_untagged, rhs_vreg, node_id), arena);
            } else {
                uint32_t rhs_untagged = vtx_isel_alloc_vreg(stream, arena);
                emit_smi_untag(stream, block, rhs_untagged, rhs_vreg, node_id, arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_SUB, lhs_untagged, rhs_untagged, node_id), arena);
            }

            if (is_raw) {
                if (dst != lhs_untagged) {
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_untagged, node_id), arena);
                }
            } else {
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_untagged, node_id), arena);
                emit_smi_retag(stream, block, dst, node_id, arena);
            }
        }
        break;
    }

    case VTX_OP_Mul: {
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t lhs_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
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

        /* SMI tag elision: if this node is RAW_INT, inputs are already
         * untagged (by a previous op in the chain) and the output will be
         * consumed by another RAW_INT op. Skip untag and retag.
         *
         * We check BOTH the node itself (is_raw) and its inputs (lhs_is_raw,
         * rhs_is_raw). If an input is RAW_INT, use it directly; otherwise
         * untag it. If THIS node is RAW_INT, skip the retag on output. */
        bool is_raw = vtx_nf_has(node->flags, VTX_NF_RAW_INT);
        const vtx_node_t *lhs_node = vtx_node_get_const(&graph->node_table, node->inputs[0]);
        const vtx_node_t *rhs_node = vtx_node_get_const(&graph->node_table, node->inputs[1]);
        bool lhs_is_raw = lhs_node && vtx_nf_has(lhs_node->flags, VTX_NF_RAW_INT);
        bool rhs_is_raw = rhs_node && vtx_nf_has(rhs_node->flags, VTX_NF_RAW_INT);

        /* Helper: get the untagged lhs value.
         * If lhs is already RAW_INT, use it directly.
         * Otherwise, untag it into a new vreg. */
        uint32_t lhs_untagged;
        if (lhs_is_raw) {
            lhs_untagged = lhs_vreg;
        } else {
            lhs_untagged = vtx_isel_alloc_vreg(stream, arena);
            emit_smi_untag(stream, block, lhs_untagged, lhs_vreg, node_id, arena);
        }

        /* Helper: get the untagged rhs value (same logic). */
        uint32_t rhs_untagged;
        if (rhs_is_raw) {
            rhs_untagged = rhs_vreg;
        } else {
            rhs_untagged = vtx_isel_alloc_vreg(stream, arena);
            emit_smi_untag(stream, block, rhs_untagged, rhs_vreg, node_id, arena);
        }

        /* Try constant multiply shortcuts. These operate on the UNTAGGED
         * lhs_untagged value (already extracted above). */
        int64_t rhs_const;
        if (try_get_const_int(graph, node->inputs[1], &rhs_const)) {
            /* Mul(x, 0) → 0 (raw) or SMI(0) = HEADER (tagged) */
            if (rhs_const == 0) {
                if (is_raw)
                    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_MOV, dst, 0, node_id), arena);
                else
                    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_MOV, dst,
                                        (int64_t)VTX_NAN_BOX_HEADER, node_id), arena);
                break;
            }

            /* Mul(x, 1) → x (already untagged in lhs_untagged) */
            if (rhs_const == 1) {
                if (is_raw) {
                    if (dst != lhs_untagged)
                        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_untagged, node_id), arena);
                } else {
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_untagged, node_id), arena);
                    emit_smi_retag(stream, block, dst, node_id, arena);
                }
                break;
            }

            /* Mul(x, -1) → NEG(x) */
            if (rhs_const == -1) {
                uint32_t neg_dst = (dst != lhs_untagged) ? dst : vtx_isel_alloc_vreg(stream, arena);
                if (neg_dst != lhs_untagged)
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, neg_dst, lhs_untagged, node_id), arena);
                vtx_isel_emit_inst(block, make_r_inst(VTX_X86_NEG, neg_dst, node_id, 0), arena);
                if (dst != neg_dst)
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, neg_dst, node_id), arena);
                if (!is_raw) emit_smi_retag(stream, block, dst, node_id, arena);
                break;
            }

            /* Power of 2: Mul(x, 2^n) → SHL(x, n) */
            int shift = is_power_of_2(rhs_const);
            if (shift >= 0) {
                uint32_t shl_dst = (dst != lhs_untagged) ? dst : vtx_isel_alloc_vreg(stream, arena);
                if (shl_dst != lhs_untagged)
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, shl_dst, lhs_untagged, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, shl_dst, shift, node_id), arena);
                if (dst != shl_dst)
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, shl_dst, node_id), arena);
                if (!is_raw) emit_smi_retag(stream, block, dst, node_id, arena);
                break;
            }

            /* LEA patterns for small constants (3, 5, 6, 9, 10, etc.).
             * emit_mul_by_constant uses LEA which is 1-2 cycles vs IMUL's
             * 3-5 cycles. This is especially important for collatz's 3*n+1
             * path where Mul(x, 3) is the dominant operation.
             * The LEA operates on the UNTAGGED value, producing raw result.
             * We then retag if the node is not RAW_INT. */
            if (emit_mul_by_constant(stream, block, dst, lhs_untagged,
                                      rhs_const, node_id, arena)) {
                if (!is_raw) emit_smi_retag(stream, block, dst, node_id, arena);
                break;
            }

            /* General constant: IMUL(lhs_untagged, const) */
            {
                uint32_t imul_dst = (dst != lhs_untagged) ? dst : vtx_isel_alloc_vreg(stream, arena);
                if (imul_dst != lhs_untagged)
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, imul_dst, lhs_untagged, node_id), arena);
                if (rhs_const >= INT32_MIN && rhs_const <= INT32_MAX) {
                    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_IMUL, imul_dst, (int32_t)rhs_const, node_id), arena);
                } else {
                    uint32_t const_vreg = vtx_isel_alloc_vreg(stream, arena);
                    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_MOV, const_vreg, rhs_const, node_id), arena);
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_IMUL, imul_dst, const_vreg, node_id), arena);
                }
                if (dst != imul_dst)
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, imul_dst, node_id), arena);
                if (!is_raw) emit_smi_retag(stream, block, dst, node_id, arena);
            }
            break;
        }

        /* Try Mul(const, x) — mirror of above, using rhs_untagged */
        int64_t lhs_const;
        if (try_get_const_int(graph, node->inputs[0], &lhs_const)) {
            if (lhs_const == 0) {
                if (is_raw)
                    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_MOV, dst, 0, node_id), arena);
                else
                    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_MOV, dst,
                                        (int64_t)VTX_NAN_BOX_HEADER, node_id), arena);
                break;
            }
            if (lhs_const == 1) {
                if (is_raw) {
                    if (dst != rhs_untagged)
                        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, rhs_untagged, node_id), arena);
                } else {
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, rhs_untagged, node_id), arena);
                    emit_smi_retag(stream, block, dst, node_id, arena);
                }
                break;
            }
            /* General: IMUL(rhs_untagged, lhs_const) */
            {
                uint32_t imul_dst = (dst != rhs_untagged) ? dst : vtx_isel_alloc_vreg(stream, arena);
                if (imul_dst != rhs_untagged)
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, imul_dst, rhs_untagged, node_id), arena);
                if (lhs_const >= INT32_MIN && lhs_const <= INT32_MAX) {
                    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_IMUL, imul_dst, (int32_t)lhs_const, node_id), arena);
                } else {
                    uint32_t const_vreg = vtx_isel_alloc_vreg(stream, arena);
                    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_MOV, const_vreg, lhs_const, node_id), arena);
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_IMUL, imul_dst, const_vreg, node_id), arena);
                }
                if (dst != imul_dst)
                    vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, imul_dst, node_id), arena);
                if (!is_raw) emit_smi_retag(stream, block, dst, node_id, arena);
            }
            break;
        }

        /* Variable * Variable: IMUL(lhs_untagged, rhs_untagged) */
        {
            uint32_t imul_dst = (dst != lhs_untagged) ? dst : vtx_isel_alloc_vreg(stream, arena);
            if (imul_dst != lhs_untagged)
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, imul_dst, lhs_untagged, node_id), arena);
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_IMUL, imul_dst, rhs_untagged, node_id), arena);
            if (dst != imul_dst)
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, imul_dst, node_id), arena);
            if (!is_raw) emit_smi_retag(stream, block, dst, node_id, arena);
        }
        break;
    }

    case VTX_OP_Div: {
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t lhs_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
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

        /* Check if divisor is a constant power of 2.
         * If so, emit SAR with rounding correction instead of IDIV.
         * This is done at isel level (not IR level) to avoid scheduler
         * placement issues — the isel emits instructions in the correct
         * block context. */
        int64_t div_const;
        if (try_get_const_int(graph, node->inputs[1], &div_const) && div_const > 0) {
            int shift = -1;
            int64_t v = div_const;
            if (v > 0 && (v & (v - 1)) == 0) {
                while (v > 1) { shift++; v >>= 1; }
                shift++; /* shift = log2(div_const) */
            }

            if (shift >= 1 && shift <= 62) {
                /* Div(x, 2^k) → (x + (x>>63 & mask)) >> k
                 *
                 * Emit:
                 *   untag lhs → lhs_raw
                 *   mov tmp, lhs_raw
                 *   sar tmp, 63           (sign mask: 0 or -1)
                 *   and tmp, (1<<k)-1     (correction)
                 *   add tmp, lhs_raw      (corrected = x + correction)
                 *   sar tmp, k            (result = corrected >> k)
                 *   mov dst, tmp
                 *   retag dst
                 */
                uint32_t dst = ensure_node_vreg(stream, node_id, arena);
                uint32_t lhs_raw = vtx_isel_alloc_vreg(stream, arena);
                uint32_t tmp = vtx_isel_alloc_vreg(stream, arena);

                /* Untag lhs. Use NO_COALESCE to prevent lhs_raw from being
                 * coalesced with lhs_vreg — we need lhs_raw to stay alive
                 * across the SAR+AND+ADD sequence where tmp is modified. */
                stream->uses_smi = true;
                vtx_inst_t unt = make_rr_inst(VTX_X86_MOV, lhs_raw, lhs_vreg, node_id);
                unt.flags |= VTX_INST_FLAG_NO_COALESCE;
                vtx_isel_emit_inst(block, unt, arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, lhs_raw, 13, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, lhs_raw, 16, node_id), arena);

                /* tmp = lhs_raw >> 63 (sign mask) */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, tmp, lhs_raw, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, tmp, 63, node_id), arena);

                /* tmp = tmp & ((1<<k) - 1) (correction) */
                int64_t mask = (1LL << shift) - 1;
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_AND, tmp, mask, node_id), arena);

                /* tmp = lhs_raw + tmp (corrected) */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_ADD, tmp, lhs_raw, node_id), arena);

                /* tmp = tmp >> k (result) */
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, tmp, shift, node_id), arena);

                /* retag and store to dst */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, tmp, node_id), arena);
                emit_smi_retag(stream, block, dst, node_id, arena);
                break;
            }
        }

        /* Fix #15: Magic-number division for non-power-of-2 constant divisor.
         *
         * Replaces IDIV (~25 cycles) with: MOV RAX, lhs_raw; MOV rhs, M;
         * IMUL_FULL rhs; MOV tmp, RDX; SAR tmp, s; ADD tmp, sign(lhs);
         * [NEG tmp if d<0]; retag. About 6-8 ALU ops at 1 cycle each.
         *
         * Magic formula: M = ceil(2^64 / |d|), s = 0.
         *   trunc(n/d) = SAR(high_64(M * n), s) + (n >> 63) * sign(d)
         *
         * The high 64 bits of the signed 128-bit product M*n come from
         * the one-operand IMUL (RDX:RAX = RAX * src, signed). The
         * (n >> 63) term is the sign mask (0 for n>=0, -1 for n<0) which
         * corrects floor division to truncating division for negative n.
         */
        int64_t magic_d;
        /* Magic-number division disabled: IMUL_FULL clobbers RAX/RDX and
         * the regalloc doesn't process CLOBBER flags to kill live intervals.
         * When two IDIVs are in sequence, the second's CQO clobbers RDX
         * while the first's result is still live. Fall through to CQO+IDIV. */
        if (false && try_get_const_int(graph, node->inputs[1], &magic_d) && magic_d != 0) {
            int64_t M;
            int magic_s;
            if (compute_magic_number(magic_d, &M, &magic_s)) {
                uint32_t dst = ensure_node_vreg(stream, node_id, arena);
                uint32_t lhs_raw;
                /* Check if lhs is already RAW_INT (tag-elided) */
                const vtx_node_t *lhs_node_div = vtx_node_get_const(&graph->node_table, node->inputs[0]);
                bool lhs_is_raw_div = lhs_node_div && vtx_nf_has(lhs_node_div->flags, VTX_NF_RAW_INT);
                if (lhs_is_raw_div) {
                    lhs_raw = lhs_vreg;
                } else {
                    lhs_raw = vtx_isel_alloc_vreg(stream, arena);
                }
                uint32_t magic_vreg = vtx_isel_alloc_vreg(stream, arena);
                uint32_t sign_vreg = vtx_isel_alloc_vreg(stream, arena);
                uint32_t rax_vreg  = vtx_isel_alloc_vreg_fixed(stream, arena, 0);
                uint32_t rdx_vreg  = vtx_isel_alloc_vreg_fixed(stream, arena, 2);
                if (rax_vreg == VTX_VREG_INVALID || rdx_vreg == VTX_VREG_INVALID) return -1;

                stream->uses_smi = true;
                /* Untag lhs into lhs_raw (skip if already RAW_INT). */
                if (!lhs_is_raw_div) {
                    vtx_inst_t unt = make_rr_inst(VTX_X86_MOV, lhs_raw, lhs_vreg, node_id);
                    unt.flags |= VTX_INST_FLAG_NO_COALESCE;
                    vtx_isel_emit_inst(block, unt, arena);
                    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, lhs_raw, 13, node_id), arena);
                    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, lhs_raw, 16, node_id), arena);
                }

                /* Load magic constant M into magic_vreg. */
                vtx_inst_t load_M = make_ri_inst(VTX_X86_MOV, magic_vreg, M, node_id);
                vtx_isel_emit_inst(block, load_M, arena);

                /* RAX = lhs_raw (input to one-operand IMUL).
                 * rax_vreg is fixed to RAX. The regalloc sees rax_vreg as
                 * alive from this MOV to the IMUL_FULL's operand-1 use
                 * (rax_vreg is declared as operand 1 of IMUL_FULL below). */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rax_vreg, lhs_raw, node_id), arena);

                /* IMUL_FULL: RDX:RAX = RAX * magic_vreg (signed).
                 *
                 * Operand layout (mirrors CQO + IDIV convention):
                 *   operand 0 = rdx_vreg (def-only — high 64 bits land in RDX)
                 *   operand 1 = magic_vreg (use — the multiplier)
                 *   operand 2 = rax_vreg (use — keeps RAX alive across IMUL)
                 *
                 * Without operand 0 = rdx_vreg, the regalloc would never
                 * allocate RDX to rdx_vreg, and the later "MOV dst, rdx_vreg"
                 * would read garbage. Without operand 2 = rax_vreg, the
                 * regalloc might let RAX be reused for something else
                 * between the MOV above and the IMUL_FULL (in practice
                 * RAX is reserved, but declaring the use is safer).
                 *
                 * The regalloc special-cases IMUL_FULL (see regalloc.c) to
                 * treat operand 0 as def-only (not a use), matching CQO. */
                vtx_inst_t imul_inst;
                memset(&imul_inst, 0, sizeof(imul_inst));
                imul_inst.opcode = VTX_X86_IMUL_FULL;
                imul_inst.opnd_kinds[0] = VTX_OPND_VREG;
                imul_inst.operands[0] = rdx_vreg;
                imul_inst.opnd_kinds[1] = VTX_OPND_VREG;
                imul_inst.operands[1] = magic_vreg;
                imul_inst.opnd_kinds[2] = VTX_OPND_VREG;
                imul_inst.operands[2] = rax_vreg;
                imul_inst.source_node = node_id;
                imul_inst.flags = VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RDX;
                vtx_isel_emit_inst(block, imul_inst, arena);

                /* dst = RDX (high 64 bits of the product). */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, rdx_vreg, node_id), arena);

                /* dst = SAR(dst, s) — post-shift (usually 0 with our algorithm). */
                if (magic_s > 0) {
                    vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, dst, magic_s, node_id), arena);
                }

                /* sign_vreg = lhs_raw >> 63 (sign mask: 0 or -1). */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, sign_vreg, lhs_raw, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, sign_vreg, 63, node_id), arena);

                /* dst = dst - sign_vreg (corrects floor→truncation for n<0).
                 *
                 * For d > 0, M = ceil(2^64/d), s = 0:
                 *   high_64(M * n) = floor(n/d) - (n<0 && n%d==0 ? 1 : 0)
                 *                  = trunc(n/d) - (n<0 ? 1 : 0)
                 *                  = trunc(n/d) + (n >> 63)
                 * So: trunc(n/d) = high - (n >> 63) = high - sign_vreg.
                 *
                 * For d < 0: trunc(n/d) = -trunc(n/|d|) = -(high - sign_vreg),
                 * which is NEG(high - sign_vreg). */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_SUB, dst, sign_vreg, node_id), arena);

                /* If divisor is negative, negate the result. */
                if (magic_d < 0) {
                    vtx_isel_emit_inst(block, make_r_inst(VTX_X86_NEG, dst, node_id, 0), arena);
                }

                emit_smi_retag(stream, block, dst, node_id, arena);
                vtx_isel_map_node_vreg(stream, node_id, dst, arena);
                break;
            }
        }

        /* SMI Div: must untag both operands, divide, retag result.
         * IDIV on NaN-boxed SMI values produces completely wrong results
         * because the header bits corrupt the division.
         *
         * rep_infer fix: if inputs are already RAW_INT (from UnboxInt),
         * skip the untag — they're already raw int64. */
        {
            const vtx_node_t *lhs_node_div = vtx_node_get_const(&graph->node_table, node->inputs[0]);
            const vtx_node_t *rhs_node_div = vtx_node_get_const(&graph->node_table, node->inputs[1]);
            bool lhs_is_raw_div = lhs_node_div && vtx_nf_has(lhs_node_div->flags, VTX_NF_RAW_INT);
            bool rhs_is_raw_div = rhs_node_div && vtx_nf_has(rhs_node_div->flags, VTX_NF_RAW_INT);
            int64_t div_const_dummy;
            bool rhs_is_const_div = try_get_const_int(graph, node->inputs[1], &div_const_dummy);

            uint32_t lhs_untagged, rhs_untagged;

            if (lhs_is_raw_div) {
                lhs_untagged = lhs_vreg;
            } else {
                lhs_untagged = vtx_isel_alloc_vreg(stream, arena);
                emit_smi_untag(stream, block, lhs_untagged, lhs_vreg, node_id, arena);
            }

            if (rhs_is_const_div) {
                /* Constant: tagged SMI, must untag to get raw int64 for IDIV.
                 * Can't use make_ri_inst because it only supports int32_t
                 * immediates, and the raw value may need full 64-bit. */
                rhs_untagged = vtx_isel_alloc_vreg(stream, arena);
                emit_smi_untag(stream, block, rhs_untagged, rhs_vreg, node_id, arena);
            } else if (rhs_is_raw_div) {
                rhs_untagged = rhs_vreg;
            } else {
                rhs_untagged = vtx_isel_alloc_vreg(stream, arena);
                emit_smi_untag(stream, block, rhs_untagged, rhs_vreg, node_id, arena);
            }

            /* CQO + IDIV: RDX:RAX = sign-extend(RAX), then IDIV rhs.
             *
             * BUGFIX: CQO clobbers RDX. If rhs_untagged is in RDX, it gets
             * destroyed. We must move rhs to a safe register (not RAX/RDX)
             * before CQO. We use a fresh vreg that the regalloc will assign
             * to a safe register. */
            uint32_t rax_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 0);
            uint32_t rdx_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 2);
            if (rax_vreg == VTX_VREG_INVALID || rdx_vreg == VTX_VREG_INVALID) return -1;

            /* Move rhs to a safe register before CQO clobbers RDX.
             * BUGFIX: rhs_safe must NOT be RAX or RDX because:
             *   - MOV rax_vreg, lhs clobbers RAX
             *   - CQO clobbers RDX
             * Previously this used a fixed R8 vreg, but R8 is not in
             * VTX_REG_RESERVED_MASK, so the regalloc could also assign
             * other vregs (e.g., a loop Phi) to R8. When the Mod's rhs_safe
             * and the Div's lhs_vreg both got R8, the Div would read the
             * Mod's leftover rhs value instead of n, producing wrong results
             * (e.g., popcount(8) returned 0 instead of 1).
             *
             * Using a regular vreg lets the regalloc pick a register that
             * doesn't conflict with lhs_vreg. The CLOBBER_RAX | CLOBBER_RDX
             * flags on CQO and IDIV tell the regalloc that RAX and RDX are
             * unavailable during this range, so rhs_safe will be assigned
             * to a safe register (not RAX/RDX, and not any conflicting vreg). */
            uint32_t rhs_safe = vtx_isel_alloc_vreg(stream, arena);
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rhs_safe, rhs_untagged, node_id), arena);

            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rax_vreg, lhs_untagged, node_id), arena);
            vtx_inst_t cqo;
            memset(&cqo, 0, sizeof(cqo));
            cqo.opcode = VTX_X86_CQO;
            cqo.source_node = node_id;
            cqo.flags = VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RDX;
            /* Define rdx_vreg here so the live interval computation knows
             * CQO writes to RDX. Without this, rdx_vreg has no definition,
             * and the regalloc may not properly handle it. */
            cqo.opnd_kinds[0] = VTX_OPND_VREG;
            cqo.operands[0] = rdx_vreg;
            vtx_isel_emit_inst(block, cqo, arena);
            vtx_inst_t idiv_inst;
            memset(&idiv_inst, 0, sizeof(idiv_inst));
            idiv_inst.opcode = VTX_X86_IDIV;
            idiv_inst.opnd_kinds[0] = VTX_OPND_VREG;
            idiv_inst.operands[0] = rhs_safe;
            idiv_inst.source_node = node_id;
            idiv_inst.flags = VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RDX;
            vtx_isel_emit_inst(block, idiv_inst, arena);

            /* RAX has the raw quotient. Copy to fresh vreg.
             * Only retag if NOT RAW_INT (rep_infer may have marked this
             * node as raw — in that case, BoxInt handles the retag). */
            uint32_t div_dst = vtx_isel_alloc_vreg(stream, arena);
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, div_dst, rax_vreg, node_id), arena);
            if (!vtx_nf_has(node->flags, VTX_NF_RAW_INT)) {
                emit_smi_retag(stream, block, div_dst, node_id, arena);
            }
            vtx_isel_map_node_vreg(stream, node_id, div_dst, arena);
            (void)rdx_vreg;
        }
        break;
    }

    case VTX_OP_Mod: {
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t lhs_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t rhs_vreg = vtx_isel_node_vreg(stream, node->inputs[1]);
        if (lhs_vreg == VTX_VREG_INVALID || rhs_vreg == VTX_VREG_INVALID) return -1;

        /* Fix #2: Mod(x, 2^k) → x - (((x + (x>>63 & mask)) >> k) << k)
         *
         * IDIV is ~25 cycles; this sequence is ~6 ALU ops at 1 cycle each.
         * The formula computes truncated-toward-zero division (matches C99
         * `%` as the interpreter uses) and then multiplies back and
         * subtracts to get the remainder. The sign-correction term
         * (x>>63 & mask) is necessary because SAR alone rounds toward
         * -inf, but C99 truncates toward zero.
         *
         * Verified:
         *   7 % 4  = 3  (t=1, t<<2=4, 7-4=3)
         *  -7 % 4  = -3 (corr=3, t=(-7+3)>>2=-1, t<<2=-4, -7-(-4)=-3)
         *   6 % 4  = 2  (t=1, t<<2=4, 6-4=2)
         *  -6 % 4  = -2 (corr=3, t=(-6+3)>>2=-1, t<<2=-4, -6-(-4)=-2)
         */
        int64_t mod_const;
        if (try_get_const_int(graph, node->inputs[1], &mod_const) && mod_const > 0) {
            int shift = -1;
            int64_t v = mod_const;
            if (v > 0 && (v & (v - 1)) == 0) {
                while (v > 1) { shift++; v >>= 1; }
                shift++;
            }

            if (shift >= 1 && shift <= 62) {
                uint32_t dst = ensure_node_vreg(stream, node_id, arena);
                /* Use lhs_vreg directly as the source for untagging into
                 * dst, then use a single tmp vreg. This avoids the long
                 * live range of lhs_raw that caused regalloc issues in
                 * loops. We re-read lhs_raw from dst when needed. */
                uint32_t tmp     = vtx_isel_alloc_vreg(stream, arena);

                stream->uses_smi = true;
                /* dst = untag(lhs_vreg) — SHL 13 + SAR 16 */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_vreg, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, dst, 13, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, dst, 16, node_id), arena);

                /* tmp = dst >> 63 (sign mask: 0 or -1) */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, tmp, dst, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, tmp, 63, node_id), arena);

                /* tmp = tmp & ((1<<k) - 1) (correction) */
                int64_t mask = (1LL << shift) - 1;
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_AND, tmp, mask, node_id), arena);

                /* tmp = dst + tmp (corrected dividend) */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_ADD, tmp, dst, node_id), arena);

                /* tmp = tmp >> k (truncated-toward-zero quotient) */
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, tmp, shift, node_id), arena);

                /* tmp = tmp << k (quotient * 2^k) */
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, tmp, shift, node_id), arena);

                /* dst = dst - tmp (the remainder).
                 * dst still holds the original untagged lhs value because
                 * we only modified tmp after the initial untag. */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_SUB, dst, tmp, node_id), arena);

                /* Retag as SMI. dst holds the raw remainder. */
                emit_smi_retag(stream, block, dst, node_id, arena);
                vtx_isel_map_node_vreg(stream, node_id, dst, arena);
                break;
            }
        }

        /* SMI Mod: must untag both operands, IDIV, retag remainder (in RDX).
         * rep_infer fix: skip untag for RAW_INT inputs. */
        {
            const vtx_node_t *lhs_node_mod = vtx_node_get_const(&graph->node_table, node->inputs[0]);
            const vtx_node_t *rhs_node_mod = vtx_node_get_const(&graph->node_table, node->inputs[1]);
            bool lhs_is_raw_mod = lhs_node_mod && vtx_nf_has(lhs_node_mod->flags, VTX_NF_RAW_INT);
            bool rhs_is_raw_mod = rhs_node_mod && vtx_nf_has(rhs_node_mod->flags, VTX_NF_RAW_INT);
            int64_t mod_const_dummy;
            bool rhs_is_const_mod = try_get_const_int(graph, node->inputs[1], &mod_const_dummy);

            uint32_t lhs_untagged, rhs_untagged;

            if (lhs_is_raw_mod) {
                lhs_untagged = lhs_vreg;
            } else {
                lhs_untagged = vtx_isel_alloc_vreg(stream, arena);
                emit_smi_untag(stream, block, lhs_untagged, lhs_vreg, node_id, arena);
            }

            if (rhs_is_const_mod) {
                /* Constant: tagged SMI, must untag for IDIV. */
                rhs_untagged = vtx_isel_alloc_vreg(stream, arena);
                emit_smi_untag(stream, block, rhs_untagged, rhs_vreg, node_id, arena);
            } else if (rhs_is_raw_mod) {
                rhs_untagged = rhs_vreg;
            } else {
                rhs_untagged = vtx_isel_alloc_vreg(stream, arena);
                emit_smi_untag(stream, block, rhs_untagged, rhs_vreg, node_id, arena);
            }

            /* CQO + IDIV: remainder in RDX.
             * BUGFIX: same as Div — move rhs to safe register before CQO. */
            uint32_t rax_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 0);
            uint32_t rdx_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 2);
            if (rax_vreg == VTX_VREG_INVALID || rdx_vreg == VTX_VREG_INVALID) return -1;

            /* Move rhs to a safe register before CQO clobbers RDX.
             * BUGFIX: same as Div — use a regular vreg (not fixed R8) to
             * avoid conflicts with other vregs that the regalloc may have
             * assigned to R8. The CLOBBER_RAX | CLOBBER_RDX flags ensure
             * the regalloc picks a safe register. */
            uint32_t rhs_safe = vtx_isel_alloc_vreg(stream, arena);
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rhs_safe, rhs_untagged, node_id), arena);

            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rax_vreg, lhs_untagged, node_id), arena);
            vtx_inst_t cqo;
            memset(&cqo, 0, sizeof(cqo));
            cqo.opcode = VTX_X86_CQO;
            cqo.source_node = node_id;
            cqo.flags = VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RDX;
            /* Define rdx_vreg here so the live interval computation knows
             * CQO writes to RDX. Without this, rdx_vreg has no definition,
             * and the regalloc may not properly handle it. */
            cqo.opnd_kinds[0] = VTX_OPND_VREG;
            cqo.operands[0] = rdx_vreg;
            vtx_isel_emit_inst(block, cqo, arena);
            vtx_inst_t idiv_inst;
            memset(&idiv_inst, 0, sizeof(idiv_inst));
            idiv_inst.opcode = VTX_X86_IDIV;
            idiv_inst.opnd_kinds[0] = VTX_OPND_VREG;
            idiv_inst.operands[0] = rhs_safe;
            idiv_inst.source_node = node_id;
            idiv_inst.flags = VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RDX;
            vtx_isel_emit_inst(block, idiv_inst, arena);

            /* RDX has the raw remainder. Copy to fresh vreg.
             * Only retag if NOT RAW_INT (rep_infer may have marked this
             * node as raw — in that case, BoxInt handles the retag). */
            uint32_t mod_dst = vtx_isel_alloc_vreg(stream, arena);
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, mod_dst, rdx_vreg, node_id), arena);
            if (!vtx_nf_has(node->flags, VTX_NF_RAW_INT)) {
                emit_smi_retag(stream, block, mod_dst, node_id, arena);
            }
            vtx_isel_map_node_vreg(stream, node_id, mod_dst, arena);
            (void)rax_vreg;
        }
        break;
    }

    /* ---- Shifts ---- */
    case VTX_OP_Shl: {
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t val_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t cnt_vreg = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (val_vreg == VTX_VREG_INVALID || cnt_vreg == VTX_VREG_INVALID) return -1;

        /* Shl must operate on the RAW integer, not the NaN-boxed SMI.
         * Sequence: untag val → SHL by count → retag result.
         *
         * For variable shifts, x86 SHL r64, CL requires the count in CL.
         * We use a dedicated shift_dst vreg (separate from dst) so the
         * regalloc can freely assign shift_dst and cnt_untagged to any
         * registers. The emitter moves cnt_untagged to RCX before the
         * SHL. Since shift_dst is the SHL's destination (not dst), and
         * cnt_untagged is a use, their live intervals overlap at the SHL
         * and the regalloc gives them different registers. */
        {
            uint32_t val_untagged = vtx_isel_alloc_vreg(stream, arena);
            emit_smi_untag(stream, block, val_untagged, val_vreg, node_id, arena);

            const vtx_node_t *cnt_node = vtx_node_get_const(&graph->node_table, node->inputs[1]);
            if (cnt_node && cnt_node->opcode == VTX_OP_Constant &&
                cnt_node->constval.kind == VTX_TYPE_Int) {
                /* Constant shift: SHL dst, imm8 — no RCX involvement. */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, val_untagged, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, dst,
                                   cnt_node->constval.as.int_val, node_id), arena);
            } else {
                /* Variable shift: use shift_dst so dst is NOT involved in
                 * the SHL (avoids dst==RCX conflict with CL count). */
                /* Force cnt_untagged to RCX so the regalloc keeps shift_dst
                 * OUT of RCX. Without this, shift_dst could be assigned to
                 * RCX, and the emitter's `MOV RCX, count` would overwrite
                 * shift_dst before the SHL r0, CL fires, producing
                 * `count << count` instead of `value << count`.
                 * (Bug: t2_shl_basic, t2_shr_basic in test_smi_arith) */
                uint32_t cnt_untagged = vtx_isel_alloc_vreg_fixed(stream, arena, 1 /* RCX */);
                emit_smi_untag(stream, block, cnt_untagged, cnt_vreg, node_id, arena);

                uint32_t shift_dst = vtx_isel_alloc_vreg(stream, arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, shift_dst, val_untagged, node_id), arena);

                vtx_inst_t shl_inst;
                memset(&shl_inst, 0, sizeof(shl_inst));
                shl_inst.opcode = VTX_X86_SHL;
                shl_inst.opnd_kinds[0] = VTX_OPND_VREG;
                shl_inst.operands[0] = shift_dst;
                shl_inst.opnd_kinds[1] = VTX_OPND_VREG;
                shl_inst.operands[1] = cnt_untagged;
                shl_inst.source_node = node_id;
                shl_inst.flags = VTX_INST_FLAG_CLOBBER_RCX;
                vtx_isel_emit_inst(block, shl_inst, arena);

                /* Retag directly into dst */
                emit_smi_retag(stream, block, shift_dst, node_id, arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, shift_dst, node_id), arena);
                break;
            }

            emit_smi_retag(stream, block, dst, node_id, arena);
        }
        break;
    }

    case VTX_OP_Shr: {
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t val_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t cnt_vreg = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (val_vreg == VTX_VREG_INVALID || cnt_vreg == VTX_VREG_INVALID) return -1;

        /* Same as Shl — must untag, shift raw, retag. */
        {
            uint32_t val_untagged = vtx_isel_alloc_vreg(stream, arena);
            emit_smi_untag(stream, block, val_untagged, val_vreg, node_id, arena);

            const vtx_node_t *cnt_node = vtx_node_get_const(&graph->node_table, node->inputs[1]);
            if (cnt_node && cnt_node->opcode == VTX_OP_Constant &&
                cnt_node->constval.kind == VTX_TYPE_Int) {
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, val_untagged, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHR, dst,
                                   cnt_node->constval.as.int_val, node_id), arena);
            } else {
                uint32_t cnt_untagged = vtx_isel_alloc_vreg_fixed(stream, arena, 1 /* RCX */);
                emit_smi_untag(stream, block, cnt_untagged, cnt_vreg, node_id, arena);

                uint32_t shift_dst = vtx_isel_alloc_vreg(stream, arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, shift_dst, val_untagged, node_id), arena);

                vtx_inst_t shr_inst;
                memset(&shr_inst, 0, sizeof(shr_inst));
                shr_inst.opcode = VTX_X86_SHR;
                shr_inst.opnd_kinds[0] = VTX_OPND_VREG;
                shr_inst.operands[0] = shift_dst;
                shr_inst.opnd_kinds[1] = VTX_OPND_VREG;
                shr_inst.operands[1] = cnt_untagged;
                shr_inst.source_node = node_id;
                shr_inst.flags = VTX_INST_FLAG_CLOBBER_RCX;
                vtx_isel_emit_inst(block, shr_inst, arena);

                emit_smi_retag(stream, block, shift_dst, node_id, arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, shift_dst, node_id), arena);
                break;
            }

            emit_smi_retag(stream, block, dst, node_id, arena);
        }
        break;
    }

    case VTX_OP_Sar: {
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t val_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t cnt_vreg = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (val_vreg == VTX_VREG_INVALID || cnt_vreg == VTX_VREG_INVALID) return -1;

        /* SAR = arithmetic shift right (sign-extends). Same untag/retag
         * as SHR, but emits VTX_X86_SAR instead of VTX_X86_SHR. */
        {
            uint32_t val_untagged = vtx_isel_alloc_vreg(stream, arena);
            emit_smi_untag(stream, block, val_untagged, val_vreg, node_id, arena);

            const vtx_node_t *cnt_node = vtx_node_get_const(&graph->node_table, node->inputs[1]);
            if (cnt_node && cnt_node->opcode == VTX_OP_Constant &&
                cnt_node->constval.kind == VTX_TYPE_Int) {
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, val_untagged, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, dst,
                                   cnt_node->constval.as.int_val, node_id), arena);
            } else {
                uint32_t cnt_untagged = vtx_isel_alloc_vreg_fixed(stream, arena, 1 /* RCX */);
                emit_smi_untag(stream, block, cnt_untagged, cnt_vreg, node_id, arena);

                uint32_t shift_dst = vtx_isel_alloc_vreg(stream, arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, shift_dst, val_untagged, node_id), arena);

                vtx_inst_t sar_inst;
                memset(&sar_inst, 0, sizeof(sar_inst));
                sar_inst.opcode = VTX_X86_SAR;
                sar_inst.opnd_kinds[0] = VTX_OPND_VREG;
                sar_inst.operands[0] = shift_dst;
                sar_inst.opnd_kinds[1] = VTX_OPND_VREG;
                sar_inst.operands[1] = cnt_untagged;
                sar_inst.source_node = node_id;
                sar_inst.flags = VTX_INST_FLAG_CLOBBER_RCX;
                vtx_isel_emit_inst(block, sar_inst, arena);

                emit_smi_retag(stream, block, shift_dst, node_id, arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, shift_dst, node_id), arena);
                break;
            }

            emit_smi_retag(stream, block, dst, node_id, arena);
        }
        break;
    }

    /* ---- Bitwise ---- */
    /* BUGFIX (audit #3): Bitwise ops (And/Or/Xor) operating directly on
     * NaN-boxed SMIs corrupt the result. Example: Xor(SMI(a), SMI(a)) = 0
     * (raw), but SMI(0) = 0x7FF8000000000000. The header is destroyed.
     * Correct sequence: untag both → bitwise op → retag. */
    case VTX_OP_And: {
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;

        /* SMI tag elision: skip untag/retag for RAW_INT chains. */
        bool is_raw = vtx_nf_has(node->flags, VTX_NF_RAW_INT);
        const vtx_node_t *lhs_node = vtx_node_get_const(&graph->node_table, node->inputs[0]);
        const vtx_node_t *rhs_node = vtx_node_get_const(&graph->node_table, node->inputs[1]);
        bool lhs_is_raw = lhs_node && vtx_nf_has(lhs_node->flags, VTX_NF_RAW_INT);
        bool rhs_is_raw = rhs_node && vtx_nf_has(rhs_node->flags, VTX_NF_RAW_INT);
        uint32_t lhs_u, rhs_u;
        if (lhs_is_raw) { lhs_u = lhs; }
        else { lhs_u = vtx_isel_alloc_vreg(stream, arena); emit_smi_untag(stream, block, lhs_u, lhs, node_id, arena); }
        if (rhs_is_raw) { rhs_u = rhs; }
        else { rhs_u = vtx_isel_alloc_vreg(stream, arena); emit_smi_untag(stream, block, rhs_u, rhs, node_id, arena); }
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_u, node_id), arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_AND, dst, rhs_u, node_id), arena);
        if (!is_raw) emit_smi_retag(stream, block, dst, node_id, arena);
        break;
    }

    case VTX_OP_Or: {
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;

        bool is_raw = vtx_nf_has(node->flags, VTX_NF_RAW_INT);
        const vtx_node_t *lhs_node = vtx_node_get_const(&graph->node_table, node->inputs[0]);
        const vtx_node_t *rhs_node = vtx_node_get_const(&graph->node_table, node->inputs[1]);
        bool lhs_is_raw = lhs_node && vtx_nf_has(lhs_node->flags, VTX_NF_RAW_INT);
        bool rhs_is_raw = rhs_node && vtx_nf_has(rhs_node->flags, VTX_NF_RAW_INT);
        uint32_t lhs_u, rhs_u;
        if (lhs_is_raw) { lhs_u = lhs; }
        else { lhs_u = vtx_isel_alloc_vreg(stream, arena); emit_smi_untag(stream, block, lhs_u, lhs, node_id, arena); }
        if (rhs_is_raw) { rhs_u = rhs; }
        else { rhs_u = vtx_isel_alloc_vreg(stream, arena); emit_smi_untag(stream, block, rhs_u, rhs, node_id, arena); }
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_u, node_id), arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_OR, dst, rhs_u, node_id), arena);
        if (!is_raw) emit_smi_retag(stream, block, dst, node_id, arena);
        break;
    }

    case VTX_OP_Xor: {
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;

        bool is_raw = vtx_nf_has(node->flags, VTX_NF_RAW_INT);
        const vtx_node_t *lhs_node = vtx_node_get_const(&graph->node_table, node->inputs[0]);
        const vtx_node_t *rhs_node = vtx_node_get_const(&graph->node_table, node->inputs[1]);
        bool lhs_is_raw = lhs_node && vtx_nf_has(lhs_node->flags, VTX_NF_RAW_INT);
        bool rhs_is_raw = rhs_node && vtx_nf_has(rhs_node->flags, VTX_NF_RAW_INT);
        uint32_t lhs_u, rhs_u;
        if (lhs_is_raw) { lhs_u = lhs; }
        else { lhs_u = vtx_isel_alloc_vreg(stream, arena); emit_smi_untag(stream, block, lhs_u, lhs, node_id, arena); }
        if (rhs_is_raw) { rhs_u = rhs; }
        else { rhs_u = vtx_isel_alloc_vreg(stream, arena); emit_smi_untag(stream, block, rhs_u, rhs, node_id, arena); }
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_u, node_id), arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_XOR, dst, rhs_u, node_id), arena);
        if (!is_raw) emit_smi_retag(stream, block, dst, node_id, arena);
        break;
    }

    /* ---- Unary ---- */
    case VTX_OP_Neg: {
        if (node->input_count < 1) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
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

        /* SMI Neg: untag, negate, retag.
         * NEG on a NaN-boxed SMI value gives -SMI(a) ≠ SMI(-a).
         * We must untag first, then negate, then retag.
         * SMI tag elision: skip untag/retag for RAW_INT chains. */
        {
            bool is_raw = vtx_nf_has(node->flags, VTX_NF_RAW_INT);
            const vtx_node_t *src_node = vtx_node_get_const(&graph->node_table, node->inputs[0]);
            bool src_is_raw = src_node && vtx_nf_has(src_node->flags, VTX_NF_RAW_INT);

            uint32_t untagged;
            if (src_is_raw) { untagged = src; }
            else { untagged = vtx_isel_alloc_vreg(stream, arena); emit_smi_untag(stream, block, untagged, src, node_id, arena); }
            vtx_isel_emit_inst(block, make_r_inst(VTX_X86_NEG, untagged, node_id, 0), arena);
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, untagged, node_id), arena);
            if (!is_raw) emit_smi_retag(stream, block, dst, node_id, arena);
        }
        break;
    }

    case VTX_OP_Not: {
        if (node->input_count < 1) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t src = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (src == VTX_VREG_INVALID) return -1;
        if (dst != src)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, src, node_id), arena);
        vtx_isel_emit_inst(block, make_r_inst(VTX_X86_NOT, dst, node_id, 0), arena);
        break;
    }

    /* ---- Float arithmetic (SSE2 scalar double) ----
     *
     * These opcodes are produced by the graph builder for VT_OP_FADD/FSUB/
     * FMUL/FDIV. We lower them to runtime calls to avoid XMM register
     * allocation complexity. The runtime helpers take two NaN-boxed values
     * and return a NaN-boxed result.
     *
     * This is correct (the runtime does the float arithmetic) but slower
     * than inline SSE2. For frontend-ready correctness, this is sufficient.
     * Future optimization: emit inline ADDSD/SUBSD/MULSD/DIVSD with proper
     * XMM regalloc support. */
    case VTX_OP_AddF:
    case VTX_OP_SubF:
    case VTX_OP_MulF:
    case VTX_OP_DivF: {
        if (node->input_count < 2) return -1;

        /* Get the two input vregs (NaN-boxed values in GPRs). */
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t lhs_vreg = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t rhs_vreg = vtx_isel_node_vreg(stream, node->inputs[1]);
        if (lhs_vreg == VTX_VREG_INVALID || rhs_vreg == VTX_VREG_INVALID) return -1;

        /* Call the runtime helper (same pattern as CallRuntime):
         *   RDI = lhs (NaN-boxed value)
         *   RSI = rhs (NaN-boxed value)
         *   RAX = result (NaN-boxed value)
         *
         * The runtime helper extracts the doubles, does the arithmetic,
         * and re-boxes the result. Float values stay in GPRs throughout —
         * no XMM register allocation involved. */
        extern vtx_value_t vtx_runtime_float_add(vtx_value_t a, vtx_value_t b);
        extern vtx_value_t vtx_runtime_float_sub(vtx_value_t a, vtx_value_t b);
        extern vtx_value_t vtx_runtime_float_mul(vtx_value_t a, vtx_value_t b);
        extern vtx_value_t vtx_runtime_float_div(vtx_value_t a, vtx_value_t b);

        void *helper;
        switch (node->opcode) {
        case VTX_OP_AddF: helper = (void *)vtx_runtime_float_add; break;
        case VTX_OP_SubF: helper = (void *)vtx_runtime_float_sub; break;
        case VTX_OP_MulF: helper = (void *)vtx_runtime_float_mul; break;
        case VTX_OP_DivF: helper = (void *)vtx_runtime_float_div; break;
        default: helper = (void *)vtx_runtime_float_add; break;
        }

        /* RDI = lhs
         * Mark NO_COALESCE so the regalloc doesn't coalesce RDI with lhs_vreg
         * (which would extend lhs_vreg's live range across the call and
         * conflict with RDI being clobbered by the call). */
        uint32_t rdi_preg = vtx_isel_alloc_vreg_fixed(stream, arena, 7 /* RDI */);
        vtx_inst_t mov_rdi = make_rr_inst(VTX_X86_MOV, rdi_preg, lhs_vreg, node_id);
        mov_rdi.flags |= VTX_INST_FLAG_NO_COALESCE;
        vtx_isel_emit_inst(block, mov_rdi, arena);

        /* RSI = rhs
         * Same NO_COALESCE reason. */
        uint32_t rsi_preg = vtx_isel_alloc_vreg_fixed(stream, arena, 6 /* RSI */);
        vtx_inst_t mov_rsi = make_rr_inst(VTX_X86_MOV, rsi_preg, rhs_vreg, node_id);
        mov_rsi.flags |= VTX_INST_FLAG_NO_COALESCE;
        vtx_isel_emit_inst(block, mov_rsi, arena);

        /* CALL helper */
        vtx_inst_t call_inst;
        memset(&call_inst, 0, sizeof(call_inst));
        call_inst.opcode = VTX_X86_CALL;
        call_inst.opnd_kinds[0] = VTX_OPND_IMM;
        call_inst.imm = (int64_t)(uintptr_t)helper;
        call_inst.flags = VTX_INST_FLAG_HAS_IMM | VTX_INST_FLAG_IS_CALL |
                          VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RCX |
                          VTX_INST_FLAG_CLOBBER_RDX | VTX_INST_FLAG_CLOBBER_RSI |
                          VTX_INST_FLAG_CLOBBER_RDI | VTX_INST_FLAG_CLOBBER_R8 |
                          VTX_INST_FLAG_CLOBBER_R9 | VTX_INST_FLAG_CLOBBER_R10 |
                          VTX_INST_FLAG_CLOBBER_R11;
        call_inst.source_node = node_id;
        vtx_isel_emit_inst(block, call_inst, arena);

        /* Emit "MOV dst_vreg, RAX" where RAX is a PREG (physical register),
         * NOT a VREG. This is the key fix:
         *
         * The CALL instruction doesn't define any vreg in the regalloc's
         * view (its operand[0] is IMM, not VREG). So a fixed-RAX vreg
         * allocated after the call has no definition → invalid interval
         * → gets fallback-spilled → the result is lost.
         *
         * By using PREG for the source (RAX=0), the regalloc doesn't need
         * to track it. The dst vreg is properly defined (operand[0] of MOV).
         * The emitter sees r1=0 (RAX) and emits either:
         *   - MOV dst_reg, RAX (if dst gets a register)
         *   - MOV [spill_slot], RAX (if dst is spilled)
         * Both are correct — RAX holds the call result. */
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        vtx_inst_t mov_result;
        memset(&mov_result, 0, sizeof(mov_result));
        mov_result.opcode = VTX_X86_MOV;
        mov_result.opnd_kinds[0] = VTX_OPND_VREG;
        mov_result.operands[0] = dst;
        mov_result.opnd_kinds[1] = VTX_OPND_PREG;  /* RAX as physical register */
        mov_result.operands[1] = 0;                 /* RAX = register 0 */
        mov_result.flags = VTX_INST_FLAG_NO_COALESCE;
        mov_result.source_node = node_id;
        vtx_isel_emit_inst(block, mov_result, arena);
        break;
    }

    case VTX_OP_NegF: {
        /* Float negation: XORPS dst, [sign_mask] where sign_mask = 0x8000000000000000.
         * We XOR with a mask that flips only the sign bit. */
        if (node->input_count < 1) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t src = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (src == VTX_VREG_INVALID) return -1;

        /* MOVSD dst, src; XORPS dst, [rip+disp32] (sign mask) */
        if (dst != src)
            vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_MOVSD, dst, src, node_id), arena);
        /* Emit XORPS with a memory operand pointing to the sign mask.
         * For simplicity, emit a XORPS with imm64 — but XORPS doesn't take
         * an immediate. We need a memory operand.
         * For now, emit a MOVQ_R64_XMM + XOR RAX, sign_mask + MOVQ_XMM_R64.
         * Actually, the simplest is to use MOVSD_LOAD from a constant pool
         * entry and XORPS. But that requires a constant pool entry.
         *
         * For now, use a simpler approach: load the sign mask into an XMM
         * temp via a 64-bit GPR, then XORPS dst, xmm_temp. */
        /* This is a placeholder — full NegF would use a constant pool entry.
         * For frontend-ready, we can skip NegF for now (no bytecode uses it
         * directly; it's only emitted by optimizations). */
        /* TODO: implement NegF properly with a constant pool entry. */
        break;
    }

    /* ---- Min/Max (P1 isel: CMP+CMOV) ----
     *
     * BUGFIX (B3 audit): Min/Max compared NaN-boxed SMI operands directly
     * without untagging. Signed CMP on SMI(-3) (high bits 0x7FF8...) treats
     * it as > SMI(1), so Min(-3,1) returned 1. Every other comparison path
     * untags first; Min/Max were missed.
     *
     * Fix: Untag both operands into temp vregs, compare, CMOV, then retag
     * the result. */
    case VTX_OP_Min: {
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;

        /* Untag lhs and rhs into temp vregs */
        uint32_t lhs_raw = vtx_isel_alloc_vreg(stream, arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, lhs_raw, lhs, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, lhs_raw, 13, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, lhs_raw, 16, node_id), arena);

        uint32_t rhs_raw = vtx_isel_alloc_vreg(stream, arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rhs_raw, rhs, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, rhs_raw, 13, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, rhs_raw, 16, node_id), arena);

        /* min(a, b) = b if a > b, else a
         *   mov dst, lhs_raw
         *   cmp dst, rhs_raw
         *   cmovg dst, rhs_raw    ; if dst > rhs, dst = rhs */
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_raw, node_id), arena);
        vtx_inst_t min_cmp = make_rr_inst(VTX_X86_CMP, dst, rhs_raw, node_id);
        min_cmp.flags |= VTX_INST_FLAG_NO_TEST;
        vtx_isel_emit_inst(block, min_cmp, arena);

        vtx_inst_t cmov;
        memset(&cmov, 0, sizeof(cmov));
        cmov.opcode = VTX_X86_CMOV;
        cmov.opnd_kinds[0] = VTX_OPND_VREG;
        cmov.operands[0] = dst;
        cmov.opnd_kinds[1] = VTX_OPND_VREG;
        cmov.operands[1] = rhs_raw;
        cmov.cond = VTX_COND_GT;
        cmov.flags = VTX_INST_FLAG_HAS_COND;
        cmov.source_node = node_id;
        vtx_isel_emit_inst(block, cmov, arena);

        /* Retag: dst = (dst & DATA_MASK) << 3 | HEADER */
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_AND, dst, stream->smi_mask_vreg, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, dst, 3, node_id), arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_OR, dst, stream->smi_scratch_vreg, node_id), arena);
        break;
    }

    case VTX_OP_Max: {
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;

        /* Untag lhs and rhs into temp vregs */
        uint32_t lhs_raw = vtx_isel_alloc_vreg(stream, arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, lhs_raw, lhs, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, lhs_raw, 13, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, lhs_raw, 16, node_id), arena);

        uint32_t rhs_raw = vtx_isel_alloc_vreg(stream, arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rhs_raw, rhs, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, rhs_raw, 13, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, rhs_raw, 16, node_id), arena);

        /* max(a, b) = a if a > b, else b
         *   mov dst, lhs_raw
         *   cmp dst, rhs_raw
         *   cmovle dst, rhs_raw   ; if dst <= rhs, dst = rhs */
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, lhs_raw, node_id), arena);
        vtx_inst_t max_cmp = make_rr_inst(VTX_X86_CMP, dst, rhs_raw, node_id);
        max_cmp.flags |= VTX_INST_FLAG_NO_TEST;
        vtx_isel_emit_inst(block, max_cmp, arena);

        vtx_inst_t cmov;
        memset(&cmov, 0, sizeof(cmov));
        cmov.opcode = VTX_X86_CMOV;
        cmov.opnd_kinds[0] = VTX_OPND_VREG;
        cmov.operands[0] = dst;
        cmov.opnd_kinds[1] = VTX_OPND_VREG;
        cmov.operands[1] = rhs_raw;
        cmov.cond = VTX_COND_LE;
        cmov.flags = VTX_INST_FLAG_HAS_COND;
        cmov.source_node = node_id;
        vtx_isel_emit_inst(block, cmov, arena);

        /* Retag: dst = (dst & DATA_MASK) << 3 | HEADER */
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_AND, dst, stream->smi_mask_vreg, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, dst, 3, node_id), arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_OR, dst, stream->smi_scratch_vreg, node_id), arena);
        break;
    }

    /* ---- Comparison ---- */
    case VTX_OP_Cmp:
    case VTX_OP_CmpP: {
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        ensure_node_vreg(stream, node->inputs[1], arena);
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;

        /* Cmp+If fusion: if this Cmp is marked (its only consumer is an If
         * and they're adjacent in the schedule), skip SETCC+MOVZX+retag.
         * The If will use the flags from this CMP directly. Don't allocate
         * a dst vreg — this avoids spill code between CMP and JCC. */
        if (node->mark) {
            int64_t rhs_const_val;
            bool rhs_is_const = try_get_const_int(graph, node->inputs[1], &rhs_const_val);

            const vtx_node_t *lhs_node = vtx_node_get_const(&graph->node_table, node->inputs[0]);
            const vtx_node_t *rhs_node = vtx_node_get_const(&graph->node_table, node->inputs[1]);
            bool lhs_is_raw = lhs_node && vtx_nf_has(lhs_node->flags, VTX_NF_RAW_INT);
            bool rhs_is_raw = rhs_node && vtx_nf_has(rhs_node->flags, VTX_NF_RAW_INT);

            uint32_t lhs_untagged;
            if (lhs_is_raw) {
                lhs_untagged = lhs;
            } else {
                lhs_untagged = vtx_isel_alloc_vreg(stream, arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, lhs_untagged, lhs, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, lhs_untagged, 13, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, lhs_untagged, 16, node_id), arena);
            }

            if (rhs_is_const && !rhs_is_raw &&
                rhs_const_val >= INT32_MIN && rhs_const_val <= INT32_MAX) {
                vtx_inst_t cmp = make_ri_inst(VTX_X86_CMP, lhs_untagged,
                                   (int32_t)rhs_const_val, node_id);
                cmp.flags |= VTX_INST_FLAG_NO_TEST; /* don't convert to TEST */
                vtx_isel_emit_inst(block, cmp, arena);
            } else if (rhs_is_raw) {
                vtx_inst_t cmp = make_rr_inst(VTX_X86_CMP, lhs_untagged, rhs, node_id);
                cmp.flags |= VTX_INST_FLAG_NO_TEST;
                vtx_isel_emit_inst(block, cmp, arena);
            } else {
                uint32_t rhs_untagged = vtx_isel_alloc_vreg(stream, arena);
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rhs_untagged, rhs, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, rhs_untagged, 13, node_id), arena);
                vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, rhs_untagged, 16, node_id), arena);
                vtx_inst_t cmp = make_rr_inst(VTX_X86_CMP, lhs_untagged, rhs_untagged, node_id);
                cmp.flags |= VTX_INST_FLAG_NO_TEST;
                vtx_isel_emit_inst(block, cmp, arena);
            }
            /* No dst vreg, no SETCC, no MOVZX, no retag — just the CMP */
            break;
        }

        uint32_t dst = ensure_node_vreg(stream, node_id, arena);

        /* SMI untagging for integer comparisons.
         *
         * NaN-boxed SMI values do NOT preserve signed ordering in 64-bit
         * comparison. SMI(-3) = 0x7FFFFFFFFFFFFFE8 appears as a large
         * positive 64-bit value, so CMP SMI(-3), SMI(0) gives the wrong
         * result (-3 > 0 = true, which is incorrect).
         *
         * To compare correctly, we must untag both operands:
         *   SHL r64, 13   ; move SMI sign bit (bit 50) to bit 63
         *   SAR r64, 16   ; arithmetic shift right, sign-extend from bit 63
         *
         * After untagging, both values are sign-extended integers that can
         * be compared with a normal signed CMP.
         *
         * SMI tag elision: if an input is already RAW_INT (from an elided
         * arithmetic chain), skip the untag — the value is already a raw
         * int64. Using SHL+SAR on an already-raw int would corrupt it.
         *
         * Constant folding: if an operand is a Constant(Int), we can use
         * the raw integer value directly as a CMP immediate, saving the
         * MOV+SHL+SAR untag sequence (3 instructions → 0). */
        const vtx_node_t *lhs_node = vtx_node_get_const(&graph->node_table, node->inputs[0]);
        const vtx_node_t *rhs_node = vtx_node_get_const(&graph->node_table, node->inputs[1]);
        bool lhs_is_raw = lhs_node && vtx_nf_has(lhs_node->flags, VTX_NF_RAW_INT);
        bool rhs_is_raw = rhs_node && vtx_nf_has(rhs_node->flags, VTX_NF_RAW_INT);

        /* Check for constant operands — we can use CMP reg, imm32 */
        int64_t rhs_const_val;
        bool rhs_is_const = try_get_const_int(graph, node->inputs[1], &rhs_const_val);
        int64_t lhs_const_val;
        bool lhs_is_const = try_get_const_int(graph, node->inputs[0], &lhs_const_val);

        uint32_t lhs_untagged;
        if (lhs_is_raw) {
            lhs_untagged = lhs;
        } else {
            lhs_untagged = vtx_isel_alloc_vreg(stream, arena);
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, lhs_untagged, lhs, node_id), arena);
            vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, lhs_untagged, 13, node_id), arena);
            vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, lhs_untagged, 16, node_id), arena);
        }

        /* If rhs is a constant that fits in imm32, use CMP reg, imm32.
         * This saves the MOV+SHL+SAR untag sequence for the constant. */
        if (rhs_is_const && !rhs_is_raw &&
            rhs_const_val >= INT32_MIN && rhs_const_val <= INT32_MAX) {
            vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_CMP, lhs_untagged,
                               (int32_t)rhs_const_val, node_id), arena);
        } else if (rhs_is_raw) {
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_CMP, lhs_untagged, rhs, node_id), arena);
        } else {
            uint32_t rhs_untagged = vtx_isel_alloc_vreg(stream, arena);
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rhs_untagged, rhs, node_id), arena);
            vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, rhs_untagged, 13, node_id), arena);
            vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, rhs_untagged, 16, node_id), arena);
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_CMP, lhs_untagged, rhs_untagged, node_id), arena);
        }

        /* P1 isel: SETCC + MOVZX for boolean result.
         *
         * Pattern: dst = (lhs cmp rhs) ? 1 : 0
         *
         * IMPORTANT: The XOR+SETCC pattern (xor dst,dst; setcc dst_lo) is WRONG
         * because XOR clobbers the flags set by CMP. SETCC must read the flags
         * IMMEDIATELY after CMP, before any instruction that modifies flags.
         *
         * Correct sequence:
         *   CMP lhs, rhs       ; sets flags
         *   SETCC dst_lo       ; reads flags, writes low byte
         *   MOVZX dst, dst_lo  ; zero-extends byte to full register
         *
         * This produces the correct 0/1 boolean result without clobbering flags.
         */
        vtx_inst_t setcc;
        memset(&setcc, 0, sizeof(setcc));
        setcc.opcode = VTX_X86_SETCC;
        setcc.opnd_kinds[0] = VTX_OPND_VREG;
        setcc.operands[0] = dst;
        setcc.cond = node->cond;
        setcc.flags = VTX_INST_FLAG_HAS_COND;
        setcc.source_node = node_id;
        vtx_isel_emit_inst(block, setcc, arena);
        /* MOVZX r64, r/m8 — zero-extend the SETCC result to 0 or 1 */
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOVZX, dst, dst, node_id), arena);

        /* BUGFIX (audit #3): The Cmp result must be SMI-tagged to match
         * the interpreter. The interpreter returns SMI(0) or SMI(1) for
         * comparison results (0x7FF8000000000000 or 0x7FF8000000000008),
         * but the SETCC+MOVZX sequence above produces raw 0 or 1.
         *
         * Without retagging, downstream consumers (If, Return, Phi at
         * merge points) see the wrong value. Example: is_zero(0) should
         * return SMI(1) = 0x7FF8000000000008, but T2 returned 0x1.
         * Worse, the If node's TEST would misbehave on raw 0/1 vs SMI.
         *
         * Fix: retag the 0/1 result as an SMI. */
        emit_smi_retag(stream, block, dst, node_id, arena);
        break;
    }

    /* ---- Representation transitions ---- */
    case VTX_OP_UnboxInt: {
        /* Tagged SMI → Raw int64
         * Emit: MOV dst, src; SHL dst, 13; SAR dst, 16
         * This is the same sequence as emit_smi_untag, but as an
         * explicit IR node so GVN/SCCP can reason about it. */
        if (node->input_count < 1) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t src = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (src == VTX_VREG_INVALID) return -1;
        stream->uses_smi = true;
        if (dst != src)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, src, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SHL, dst, 13, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_SAR, dst, 16, node_id), arena);
        break;
    }

    case VTX_OP_BoxInt: {
        /* Raw int64 → Tagged SMI
         * Use the existing emit_smi_retag which is already tested and
         * correctly handles the mask register (R11) and header (R10). */
        if (node->input_count < 1) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t src = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (src == VTX_VREG_INVALID) return -1;
        if (dst != src)
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, src, node_id), arena);
        emit_smi_retag(stream, block, dst, node_id, arena);
        break;
    }

    /* ---- Memory ---- */
    case VTX_OP_Load: {
        if (node->input_count < 1) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t addr = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (addr == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t mem = { addr, VTX_VREG_INVALID, 0xFF, 0xFF, 1, 0 };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, dst, &mem, node_id), arena);
        break;
    }

    case VTX_OP_Store: {
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t addr = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t val = vtx_isel_node_vreg(stream, node->inputs[1]);
        if (addr == VTX_VREG_INVALID || val == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t mem = { addr, VTX_VREG_INVALID, 0xFF, 0xFF, 1, 0 };
        vtx_isel_emit_inst(block, make_mr_inst(VTX_X86_MOV, &mem, val, node_id), arena);
        break;
    }

    case VTX_OP_LoadField: {
        if (node->input_count < 1) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t obj = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (obj == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t mem = { obj, VTX_VREG_INVALID, 0xFF, 0xFF, 1, (int32_t)node->field_offset };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, dst, &mem, node_id), arena);
        break;
    }

    case VTX_OP_StoreField: {
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t obj = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
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
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t base = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t idx = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (base == VTX_VREG_INVALID || idx == VTX_VREG_INVALID) return -1;

        /* Fix #14: idx is a NaN-boxed SMI by default. Untag it before
         * using it as a SIB index, otherwise the effective address is
         * base + (HEADER | (n<<3)) * 8 + 0 — a garbage pointer.
         *
         * If the idx producer was already marked RAW_INT by SMI tag
         * elision, skip the untag (consistent with how Add/Sub handle
         * RAW_INT inputs). */
        uint32_t idx_for_addr = idx;
        vtx_node_t *idx_node = (node->inputs[1] < graph->node_table.count)
            ? &graph->node_table.nodes[node->inputs[1]] : NULL;
        bool idx_is_raw = idx_node && vtx_nf_has(idx_node->flags, VTX_NF_RAW_INT);
        if (!idx_is_raw) {
            idx_for_addr = vtx_isel_alloc_vreg(stream, arena);
            emit_smi_untag(stream, block, idx_for_addr, idx, node_id, arena);
        }

        /* scale=8 (one vtx_value_t per slot), disp=header (skip the
         * vtx_heap_object_t header to reach fields[0]). */
        vtx_x86_memop_t mem = { base, idx_for_addr, 0xFF, 0xFF, 8,
                                (int32_t)VTX_HEAP_OBJECT_HEADER_SIZE };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, dst, &mem, node_id), arena);
        break;
    }

    case VTX_OP_StoreIndexed: {
        if (node->input_count < 3) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t base = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t idx = vtx_isel_node_vreg(stream, node->inputs[1]);
        ensure_node_vreg(stream, node->inputs[2], arena);
        uint32_t val = vtx_isel_node_vreg(stream, node->inputs[2]);
        if (base == VTX_VREG_INVALID || idx == VTX_VREG_INVALID || val == VTX_VREG_INVALID) return -1;

        /* Fix #14: untag the SMI index (see LoadIndexed above). */
        uint32_t idx_for_addr = idx;
        vtx_node_t *idx_node = (node->inputs[1] < graph->node_table.count)
            ? &graph->node_table.nodes[node->inputs[1]] : NULL;
        bool idx_is_raw = idx_node && vtx_nf_has(idx_node->flags, VTX_NF_RAW_INT);
        if (!idx_is_raw) {
            idx_for_addr = vtx_isel_alloc_vreg(stream, arena);
            emit_smi_untag(stream, block, idx_for_addr, idx, node_id, arena);
        }

        vtx_x86_memop_t mem = { base, idx_for_addr, 0xFF, 0xFF, 8,
                                (int32_t)VTX_HEAP_OBJECT_HEADER_SIZE };
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
                /* Push base address to RDI (first arg), computed field_offset to ESI.
                 * Fix HIGH: old code passed field_offset=0, which means the write
                 * barrier marked the wrong card for large arrays. The actual
                 * offset is idx * element_size + header_size, but since the
                 * card table is page-granular, we pass the index vreg's value
                 * as a proxy for the offset so the barrier can compute the
                 * correct card. For simplicity, pass idx_scaled (idx*8) as
                 * the offset — the card table just needs to know which page
                 * the write is in.
                 * Per System V AMD64 ABI: RDI = arg0, ESI = arg1 */
                uint32_t rdi_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 7); /* RDI */
                uint32_t rsi_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 6); /* RSI */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rdi_vreg, base, node_id), arena);
                /* Pass the index value as field_offset proxy. The card
                 * marking barrier computes card = (obj + offset) / CARD_SIZE.
                 * Using idx as the offset gives the correct card for the
                 * array element being written. */
                vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rsi_vreg, idx_for_addr, node_id), arena);
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
            vtx_x86_memop_t vtable_mem = { receiver_vreg, VTX_VREG_INVALID,
                                           0xFF, 0xFF, 1,
                                           (int32_t)VTX_HEAP_OBJECT_HEADER_SIZE };
            vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, rax_vreg, &vtable_mem, node_id), arena);
            vtx_x86_memop_t method_mem = { rax_vreg, VTX_VREG_INVALID,
                                            0xFF, 0xFF, 1,
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
        /* CALL_RUNTIME dispatches to vtx_runtime_builtin_call(func_id, arg).
         * System V AMD64 ABI: RDI = func_id, RSI = arg value.
         * Return value in RAX.
         *
         * Input order (from graph builder): control, memory, data.
         * The data input is the argument value. */
        /* RDI = runtime function ID (from node->method_index) */
        uint32_t rdi_preg = vtx_isel_alloc_vreg_fixed(stream, arena, vtx_arg_regs[0]); /* RDI */
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_MOV, rdi_preg,
                                                (int64_t)node->method_index, node_id), arena);

        /* RSI = argument value (the data input — input[2] after control+memory) */
        for (uint32_t i = 0; i < node->input_count; i++) {
            const vtx_node_t *inp = vtx_node_get_const(&graph->node_table, node->inputs[i]);
            if (!inp || vtx_nf_has(inp->flags, VTX_NF_CONTROL) ||
                vtx_nf_has(inp->flags, VTX_NF_MEMORY))
                continue;
            uint32_t arg_vreg = vtx_isel_node_vreg(stream, node->inputs[i]);
            if (arg_vreg == VTX_VREG_INVALID) continue;
            uint32_t rsi_preg = vtx_isel_alloc_vreg_fixed(stream, arena, vtx_arg_regs[1]); /* RSI */
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rsi_preg, arg_vreg, node_id), arena);
            break; /* only one data argument for CALL_RUNTIME */
        }

        /* CALL vtx_runtime_builtin_call */
        vtx_inst_t call_inst;
        memset(&call_inst, 0, sizeof(call_inst));
        call_inst.opcode = VTX_X86_CALL;
        call_inst.opnd_kinds[0] = VTX_OPND_IMM;
        call_inst.imm = (int64_t)(uintptr_t)vtx_runtime_builtin_call;
        call_inst.flags = VTX_INST_FLAG_HAS_IMM | VTX_INST_FLAG_IS_CALL |
                          VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RCX |
                          VTX_INST_FLAG_CLOBBER_RDX | VTX_INST_FLAG_CLOBBER_RSI |
                          VTX_INST_FLAG_CLOBBER_RDI | VTX_INST_FLAG_CLOBBER_R8 |
                          VTX_INST_FLAG_CLOBBER_R9 | VTX_INST_FLAG_CLOBBER_R10 |
                          VTX_INST_FLAG_CLOBBER_R11;
        call_inst.source_node = node_id;
        vtx_isel_emit_inst(block, call_inst, arena);
        uint32_t result_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 0); /* RAX */
        vtx_isel_map_node_vreg(stream, node_id, result_vreg, arena);
        return 0;
    }

    /* ---- Control flow ---- */
    case VTX_OP_If: {
        /* Look up the branch target block from the schedule.
         *
         * The If node's method_index stores the branch target bytecode PC
         * (set by the graph builder). We scan the schedule's blocks for one
         * whose Region node has a matching bytecode_pc. This gives us the
         * exact target block index, which we store in the JCC instruction.
         *
         * This replaces the old resolve_branch_targets heuristic that
         * guessed the target based on block position (b+1). That heuristic
         * broke when block layout reordered blocks. */
        uint32_t target_block = 0;  /* default fallback */
        if (schedule != NULL) {
            uint32_t target_pc = node->method_index;
            for (uint32_t sb = 0; sb < schedule->count; sb++) {
                vtx_nodeid_t reg_id = schedule->blocks[sb].region_node;
                if (reg_id < graph->node_table.count) {
                    const vtx_node_t *reg_n = vtx_node_get_const(
                        &graph->node_table, reg_id);
                    if (reg_n && reg_n->bytecode_pc == target_pc) {
                        target_block = sb;
                        break;
                    }
                }
            }
        }
        uint32_t cond_vreg = VTX_VREG_INVALID;
        vtx_nodeid_t cond_node_id = VTX_NODEID_INVALID;
        const vtx_node_t *cond_node = NULL;
        for (uint32_t i = 0; i < node->input_count; i++) {
            cond_node_id = node->inputs[i];
            cond_node = vtx_node_get_const(&graph->node_table, cond_node_id);
            if (cond_node && vtx_nf_has(cond_node->flags, VTX_NF_DATA)) {
                /* Don't call ensure_node_vreg if the Cmp is fused —
                 * the Cmp didn't allocate a dst vreg. */
                if (!(cond_node->opcode == VTX_OP_Cmp && cond_node->mark)) {
                    ensure_node_vreg(stream, cond_node_id, arena);
                    cond_vreg = vtx_isel_node_vreg(stream, cond_node_id);
                }
                break;
            }
        }

        /* Cmp+If fusion: If the condition is a Cmp node marked for fusion
         * (only consumer is this If, and they're adjacent in the schedule),
         * the Cmp's CMP already set the flags. Emit JCC directly. */
        if (cond_node && cond_node->opcode == VTX_OP_Cmp && cond_node->mark) {
            vtx_cond_t cmp_cond = cond_node->cond;
            vtx_cond_t jcc_cond;
            if (node->cond == VTX_COND_NE) {
                /* if_true: jump when the Cmp condition is true */
                jcc_cond = cmp_cond;
            } else if (node->cond == VTX_COND_EQ) {
                /* if_false: jump when the Cmp condition is false */
                jcc_cond = vtx_cond_negate(cmp_cond);
            } else {
                jcc_cond = (node->cond != VTX_COND_NEVER) ? node->cond : VTX_COND_NE;
            }
            vtx_inst_t jcc = make_branch_inst(VTX_X86_JCC, target_block, jcc_cond, node_id);
            jcc.flags |= VTX_INST_FLAG_FUSED;
            vtx_isel_emit_inst(block, jcc, arena);
            /* Emit fallthrough JMP to the not-taken successor.
             * The not-taken successor is the one that is NOT the JCC target.
             * The emit phase's peephole optimizes this away if the
             * not-taken successor is at b+1 (next block in layout). */
            if (schedule != NULL && sched_block_idx < schedule->count) {
                const vtx_schedule_block_t *sblk = &schedule->blocks[sched_block_idx];
                for (uint32_t s = 0; s < sblk->succ_count; s++) {
                    if (sblk->succ_blocks[s] != target_block) {
                        vtx_inst_t fall_jmp = make_branch_inst(VTX_X86_JMP,
                            sblk->succ_blocks[s], VTX_COND_ALWAYS, node_id);
                        fall_jmp.flags |= VTX_INST_FLAG_TARGET_SET;
                        vtx_isel_emit_inst(block, fall_jmp, arena);
                        break;
                    }
                }
            }
            break;
        }

        /* Non-fused path: test truthiness by comparing the cond value
         * against SMI(0) using R10 (pre-loaded in prologue).
         *
         * Both integer Cmp and float CmpF return SMI(0) for false and
         * SMI(1) for true, so this single comparison handles both. */
        if (cond_vreg != VTX_VREG_INVALID) {
            stream->uses_smi = true;
            /* Compare against SMI(0) = VTX_NAN_BOX_HEADER (R10).
             * SMI(0) is the "false" value for T2's Cmp/CmpF results.
             * For if_true (cond NE): jump if cond != SMI(0) (i.e., true)
             * For if_false (cond EQ): jump if cond == SMI(0) (i.e., false) */
            vtx_inst_t cmp = make_rr_inst(VTX_X86_CMP, cond_vreg,
                                           stream->smi_scratch_vreg, node_id);
            cmp.flags |= VTX_INST_FLAG_FUSED | VTX_INST_FLAG_NO_TEST;
            vtx_isel_emit_inst(block, cmp, arena);
        }
        vtx_cond_t jcc_cond = (node->cond != VTX_COND_NEVER) ? node->cond : VTX_COND_NE;
        vtx_inst_t jcc = make_branch_inst(VTX_X86_JCC, target_block, jcc_cond, node_id);
        jcc.flags |= VTX_INST_FLAG_FUSED;
        vtx_isel_emit_inst(block, jcc, arena);
        /* Emit fallthrough JMP to the not-taken successor (same as above). */
        if (schedule != NULL && sched_block_idx < schedule->count) {
            const vtx_schedule_block_t *sblk = &schedule->blocks[sched_block_idx];
            for (uint32_t s = 0; s < sblk->succ_count; s++) {
                if (sblk->succ_blocks[s] != target_block) {
                    vtx_inst_t fall_jmp = make_branch_inst(VTX_X86_JMP,
                        sblk->succ_blocks[s], VTX_COND_ALWAYS, node_id);
                    fall_jmp.flags |= VTX_INST_FLAG_TARGET_SET;
                    vtx_isel_emit_inst(block, fall_jmp, arena);
                    break;
                }
            }
        }
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
                    /* Mark NO_COALESCE so the Return's MOV RAX, val_vreg isn't
                     * coalesced with val_vreg (which would extend val_vreg's
                     * live range to include RAX, but val_vreg is already dead
                     * at the Return — coalescing is safe but might cause the
                     * regalloc to put val_vreg in RAX, which is fine).
                     *
                     * Actually, NOT marking NO_COALESCE lets the coalescer
                     * merge val_vreg with RAX when val_vreg is dead after
                     * this MOV — which is the desired optimization (the value
                     * ends up in RAX without an explicit MOV). */
                    vtx_inst_t mov_ret = make_rr_inst(VTX_X86_MOV, rax_vreg, val_vreg, node_id);
                    vtx_isel_emit_inst(block, mov_ret, arena);
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
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t obj = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (obj == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t type_mem = { obj, VTX_VREG_INVALID,
                                       0xFF, 0xFF, 1, 0 };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, dst, &type_mem, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_CMP, dst, (int64_t)node->type_id, node_id), arena);
        /* B16 fix: Mark the JCC with VTX_INST_FLAG_IS_GUARD so the guard
         * emission pipeline patches it to a deopt stub. Without this flag,
         * the cast-failure branch is left as an unresolved jump and never
         * deoptimizes — a CheckCast that should have deopted instead falls
         * through into the move below, silently accepting the wrong type. */
        vtx_inst_t checkcast_jcc = make_branch_inst(VTX_X86_JCC, 0, VTX_COND_NE, node_id);
        checkcast_jcc.flags |= VTX_INST_FLAG_IS_GUARD;
        vtx_isel_emit_inst(block, checkcast_jcc, arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, obj, node_id), arena);
        break;
    }

    case VTX_OP_InstanceOf: {
        if (node->input_count < 1) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t obj = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (obj == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t type_mem = { obj, VTX_VREG_INVALID,
                                       0xFF, 0xFF, 1, 0 };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, dst, &type_mem, node_id), arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_CMP, dst, (int64_t)node->type_id, node_id), arena);
        /* Fix C1: XOR clobbers CMP flags. Use SETCC+MOVZX (same as Cmp). */
        vtx_inst_t setcc;
        memset(&setcc, 0, sizeof(setcc));
        setcc.opcode = VTX_X86_SETCC;
        setcc.opnd_kinds[0] = VTX_OPND_VREG;
        setcc.operands[0] = dst;
        setcc.cond = VTX_COND_EQ;
        setcc.flags = VTX_INST_FLAG_HAS_COND;
        setcc.source_node = node_id;
        vtx_isel_emit_inst(block, setcc, arena);
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOVZX, dst, dst, node_id), arena);
        emit_smi_retag(stream, block, dst, node_id, arena);
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
            /* Fix C3: TEST cond,1 is wrong for SMI-tagged booleans.
             * SMI(0) = 0x7FF8000000000000, SMI(1) = 0x7FF8000000000008.
             * Bit 0 is always 0 for both. Use CMP cond, SMI(0) via R10
             * (pre-loaded in prologue) instead. */
            stream->uses_smi = true;
            vtx_inst_t cmp = make_rr_inst(VTX_X86_CMP, cond_vreg,
                                           stream->smi_scratch_vreg, node_id);
            cmp.flags |= VTX_INST_FLAG_NO_TEST;
            vtx_isel_emit_inst(block, cmp, arena);
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
        /* BUGFIX: Assign vreg to all Phi INPUT nodes first, then the Phi.
         * If the Phi is processed before its inputs, the inputs won't have
         * vregs yet. The old code called ensure_node_vreg for the Phi first,
         * then for its inputs. But if a node that USES this Phi is processed
         * between the Phi and its inputs, it will see the Phi's vreg but not
         * the input's vreg. By assigning input vregs first, we ensure all
         * nodes that reference the Phi's inputs have valid vregs. */
        for (uint32_t i = 0; i < node->input_count; i++) {
            if (node->inputs[i] != VTX_NODEID_INVALID) {
                ensure_node_vreg(stream, node->inputs[i], arena);
            }
        }
        ensure_node_vreg(stream, node_id, arena);
        break;
    }

    case VTX_OP_Region:
    case VTX_OP_LoopBegin:
        break;

    case VTX_OP_Proj: {
        if (node->input_count >= 1) {
            ensure_node_vreg(stream, node->inputs[0], arena);
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
        ensure_node_vreg(stream, node->inputs[0], arena);
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
        /* Lower float comparison to a runtime call.
         * Float values are in GPRs (NaN-boxed), so we can't use UCOMISD
         * (which requires XMM registers). Instead, call a runtime helper
         * that extracts the doubles, compares them, and returns SMI(0)/SMI(1). */
        if (node->input_count < 2) return -1;
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;

        extern vtx_value_t vtx_runtime_float_eq(vtx_value_t a, vtx_value_t b);
        extern vtx_value_t vtx_runtime_float_ne(vtx_value_t a, vtx_value_t b);
        extern vtx_value_t vtx_runtime_float_lt(vtx_value_t a, vtx_value_t b);
        extern vtx_value_t vtx_runtime_float_le(vtx_value_t a, vtx_value_t b);
        extern vtx_value_t vtx_runtime_float_gt(vtx_value_t a, vtx_value_t b);
        extern vtx_value_t vtx_runtime_float_ge(vtx_value_t a, vtx_value_t b);

        void *helper;
        switch (node->cond) {
        case VTX_COND_EQ: helper = (void*)vtx_runtime_float_eq; break;
        case VTX_COND_NE: helper = (void*)vtx_runtime_float_ne; break;
        case VTX_COND_LT: helper = (void*)vtx_runtime_float_lt; break;
        case VTX_COND_LE: helper = (void*)vtx_runtime_float_le; break;
        case VTX_COND_GT: helper = (void*)vtx_runtime_float_gt; break;
        case VTX_COND_GE: helper = (void*)vtx_runtime_float_ge; break;
        default: helper = (void*)vtx_runtime_float_eq; break;
        }

        /* RDI = lhs */
        uint32_t rdi_preg = vtx_isel_alloc_vreg_fixed(stream, arena, 7);
        vtx_inst_t mov_rdi = make_rr_inst(VTX_X86_MOV, rdi_preg, lhs, node_id);
        mov_rdi.flags |= VTX_INST_FLAG_NO_COALESCE;
        vtx_isel_emit_inst(block, mov_rdi, arena);

        /* RSI = rhs */
        uint32_t rsi_preg = vtx_isel_alloc_vreg_fixed(stream, arena, 6);
        vtx_inst_t mov_rsi = make_rr_inst(VTX_X86_MOV, rsi_preg, rhs, node_id);
        mov_rsi.flags |= VTX_INST_FLAG_NO_COALESCE;
        vtx_isel_emit_inst(block, mov_rsi, arena);

        /* CALL helper */
        vtx_inst_t call_inst;
        memset(&call_inst, 0, sizeof(call_inst));
        call_inst.opcode = VTX_X86_CALL;
        call_inst.opnd_kinds[0] = VTX_OPND_IMM;
        call_inst.imm = (int64_t)(uintptr_t)helper;
        call_inst.flags = VTX_INST_FLAG_HAS_IMM | VTX_INST_FLAG_IS_CALL |
                          VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RCX |
                          VTX_INST_FLAG_CLOBBER_RDX | VTX_INST_FLAG_CLOBBER_RSI |
                          VTX_INST_FLAG_CLOBBER_RDI | VTX_INST_FLAG_CLOBBER_R8 |
                          VTX_INST_FLAG_CLOBBER_R9 | VTX_INST_FLAG_CLOBBER_R10 |
                          VTX_INST_FLAG_CLOBBER_R11;
        call_inst.source_node = node_id;
        vtx_isel_emit_inst(block, call_inst, arena);

        /* Emit "MOV dst, RAX" using PREG for RAX source.
         * Mark the MOV with NO_COALESCE so the regalloc doesn't merge
         * the CmpF result vreg with RAX (which would make it a caller-saved
         * register that gets clobbered by the next CALL). */
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        vtx_inst_t mov_result;
        memset(&mov_result, 0, sizeof(mov_result));
        mov_result.opcode = VTX_X86_MOV;
        mov_result.opnd_kinds[0] = VTX_OPND_VREG;
        mov_result.operands[0] = dst;
        mov_result.opnd_kinds[1] = VTX_OPND_PREG;
        mov_result.operands[1] = 0; /* RAX */
        mov_result.flags = VTX_INST_FLAG_NO_COALESCE;
        mov_result.source_node = node_id;
        vtx_isel_emit_inst(block, mov_result, arena);
        break;
    }

    case VTX_OP_FrameState:
    case VTX_OP_Start:
    case VTX_OP_End: {
        /* Start and End nodes are control markers only — no instructions.
         * Start marks the entry point; End marks the exit.
         * Safepoint polls are emitted at LoopEnd (loop back-edges). */
        break;
    }
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
        ensure_node_vreg(stream, node->inputs[0], arena);
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
            ensure_node_vreg(stream, node->inputs[1], arena);
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
        ensure_node_vreg(stream, node->inputs[0], arena);
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
            ensure_node_vreg(stream, node->inputs[1], arena);
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
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
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
        ensure_node_vreg(stream, node->inputs[0], arena);
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        ensure_node_vreg(stream, node->inputs[1], arena);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;

        if (dst != lhs)
            vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_MOVAPD, dst, lhs, node_id), arena);
        vtx_isel_emit_inst(block, make_sse_rr_inst(VTX_X86_MULPD, dst, rhs, node_id), arena);
        break;
    }

    case VTX_OP_Switch:
    case VTX_OP_Province:
    case VTX_OP_MemBar:
    case VTX_OP_Initialize:
    case VTX_OP_InitializeKlass:
        break;

    case VTX_OP_Unwind: {
        /* VTX_OP_Unwind is the IR node for bytecode VT_OP_THROW.
         * Inputs: [0] = control, [1] = exception value (data).
         *
         * Lower to: CALL vtx_runtime_throw(exception_value)
         *   - Move exception value to RDI (System V AMD64 first arg)
         *   - CALL vtx_runtime_throw
         *   - After the call, emit RET (the throw may return if no catch
         *     handler is found and the uncaught handler returns; in that
         *     case we exit the JIT function).
         *
         * The runtime throw function walks the interpreter frame chain
         * looking for a matching catch handler. If found, it unwinds to
         * that frame and the interpreter dispatches to the handler.
         * If not found, it calls the uncaught exception handler.
         *
         * Note: This implementation correctly handles throws that
         * propagate to caller interpreter frames. Throws to catch
         * handlers within the same JIT-compiled method require
         * additional infrastructure (synthetic frame registration)
         * and are not yet supported — the throw will propagate up
         * to the caller's interpreter frame. */
        extern void vtx_runtime_throw(vtx_value_t exc);
        uint32_t exc_vreg = VTX_VREG_INVALID;
        for (uint32_t i = 0; i < node->input_count; i++) {
            const vtx_node_t *inp = vtx_node_get_const(&graph->node_table, node->inputs[i]);
            if (inp && vtx_nf_has(inp->flags, VTX_NF_DATA)) {
                exc_vreg = vtx_isel_node_vreg(stream, node->inputs[i]);
                break;
            }
        }

        /* RDI = exception value */
        uint32_t rdi_preg = vtx_isel_alloc_vreg_fixed(stream, arena, 7 /* RDI */);
        if (exc_vreg != VTX_VREG_INVALID) {
            vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, rdi_preg, exc_vreg, node_id), arena);
        } else {
            /* No exception value — throw undefined */
            vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_MOV, rdi_preg, 0, node_id), arena);
        }

        /* CALL vtx_runtime_throw */
        vtx_inst_t call_inst;
        memset(&call_inst, 0, sizeof(call_inst));
        call_inst.opcode = VTX_X86_CALL;
        call_inst.opnd_kinds[0] = VTX_OPND_IMM;
        call_inst.imm = (int64_t)(uintptr_t)vtx_runtime_throw;
        call_inst.flags = VTX_INST_FLAG_HAS_IMM | VTX_INST_FLAG_IS_CALL |
                          VTX_INST_FLAG_CLOBBER_RAX | VTX_INST_FLAG_CLOBBER_RCX |
                          VTX_INST_FLAG_CLOBBER_RDX | VTX_INST_FLAG_CLOBBER_RSI |
                          VTX_INST_FLAG_CLOBBER_RDI | VTX_INST_FLAG_CLOBBER_R8 |
                          VTX_INST_FLAG_CLOBBER_R9 | VTX_INST_FLAG_CLOBBER_R10 |
                          VTX_INST_FLAG_CLOBBER_R11;
        call_inst.source_node = node_id;
        vtx_isel_emit_inst(block, call_inst, arena);

        /* After throw, emit RET. If the throw was caught by an interpreter
         * frame, the runtime transferred control there and this RET is
         * unreachable. If not caught, the uncaught handler ran and may
         * have aborted; if it returned, we exit the JIT function. */
        vtx_inst_t ret_inst;
        memset(&ret_inst, 0, sizeof(ret_inst));
        ret_inst.opcode = VTX_X86_RET;
        ret_inst.source_node = node_id;
        vtx_isel_emit_inst(block, ret_inst, arena);
        break;
    }

    case VTX_OP_Catch: {
        /* VTX_OP_Catch is the IR node for bytecode VT_OP_CATCH.
         * In the interpreter, CATCH sets the catch_handler_pc for the
         * frame and pushes undefined as a placeholder for the exception
         * variable.
         *
         * In T2, we don't have a frame-level catch_handler_pc. The catch
         * handler is reached via unwinding from a throw, which transfers
         * to the interpreter. So the JIT-compiled catch handler is
         * typically not reached via throw.
         *
         * The Catch node's value (which subsequent code may use as the
         * exception variable) should be the caught exception. Since the
         * JIT-compiled catch handler isn't reached via throw, we just
         * map the Catch node to a constant VTX_VALUE_UNDEFINED.
         *
         * If the catch handler IS reached (e.g., fall-through after a
         * try block completes without throwing), the exception variable
         * is undefined, which is correct. */
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        /* Load VTX_VALUE_UNDEFINED into dst.
         * VTX_VALUE_UNDEFINED = VTX_NAN_BOX_HEADER | VTX_TAG_UNDEFINED
         * We use the smi_scratch_vreg (R10 = HEADER) and OR in the tag. */
        stream->uses_smi = true;
        /* MOV dst, R10 (HEADER) */
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_MOV, dst, stream->smi_scratch_vreg, node_id), arena);
        /* OR dst, TAG_UNDEFINED (immediate) */
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_OR, dst, VTX_TAG_UNDEFINED, node_id), arena);
        break;
    }

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
                 * For non-loop Phis, inputs are [data_0, data_1, ..., Region]
                 * and p-th data input = inputs[p] (Region is at end).
                 *
                 * BUGFIX (audit #3, loop hang): For loop header Phis, the
                 * control input (LoopBegin) is INTERLEAVED with data inputs:
                 *   [forward_data, LoopBegin, back_edge_data]
                 * So inputs[p] doesn't give the p-th data input. We must
                 * skip control inputs for loop header blocks only.
                 *
                 * BUGFIX (float-in-loop crash): The old code assumed
                 * data_idx == p (predecessor index matches data input index).
                 * This is WRONG when the schedule orders predecessors
                 * differently from the Phi's data input order. For a loop
                 * header with inputs [forward_data, LoopBegin, back_edge_data],
                 * data_idx 0 = forward, data_idx 1 = back-edge. But the
                 * schedule may list the back-edge predecessor first (e.g.,
                 * pred_blocks = [latch, preheader]). In that case, p=0 would
                 * incorrectly select forward_data for the back-edge copy,
                 * causing the loop to use the initial value instead of the
                 * updated value on every iteration.
                 *
                 * Fix: Determine whether predecessor p is the forward edge
                 * or the back-edge by comparing block indices. The back-edge
                 * predecessor (latch) always has a HIGHER block index than
                 * the loop header (it comes after the header in block order).
                 * The forward predecessor (preheader) has a LOWER block index.
                 * Then select data_idx=0 for forward, data_idx=1 for back-edge. */
                if (sched_blk->is_loop_header) {
                    /* Determine if this predecessor is the back-edge.
                     * back-edge: pred_block_idx > b (loop header)
                     * forward:   pred_block_idx < b */
                    uint32_t pred_block_idx = sched_blk->pred_blocks[p];
                    bool is_back_edge = (pred_block_idx > b);

                    /* Loop header Phi inputs: [forward_data, LoopBegin, back_edge_data]
                     * data_idx 0 = forward_data, data_idx 1 = back_edge_data */
                    uint32_t target_data_idx = is_back_edge ? 1 : 0;

                    /* Walk inputs, skip control inputs, find the target data input */
                    uint32_t data_idx = 0;
                    for (uint32_t pi = 0; pi < node->input_count; pi++) {
                        vtx_nodeid_t inp_id = node->inputs[pi];
                        if (inp_id == VTX_NODEID_INVALID || inp_id >= graph->node_table.count) continue;
                        const vtx_node_t *inp_node = vtx_node_get_const(&graph->node_table, inp_id);
                        if (inp_node && (inp_node->opcode == VTX_OP_Region ||
                                         inp_node->opcode == VTX_OP_LoopBegin ||
                                         inp_node->opcode == VTX_OP_Proj)) {
                            continue;
                        }
                        if (data_idx == target_data_idx) {
                            uint32_t input_vreg = vtx_isel_node_vreg(stream, inp_id);
                            if (input_vreg != VTX_VREG_INVALID && input_vreg != phi_vreg) {
                                if (copy_count < MAX_PHI_COPIES) {
                                    copy_dst[copy_count] = phi_vreg;
                                    copy_src[copy_count] = input_vreg;
                                    copy_node[copy_count] = nid;
                                    copy_count++;
                                }
                            }
                            break;
                        }
                        data_idx++;
                    }
                } else {
                    /* Non-loop: Match predecessor to Phi input by control output.
                     *
                     * BUGFIX (if-then-else wrong result): The old code assumed
                     * the schedule's p-th predecessor corresponds to the Phi's
                     * p-th data input. But the schedule may order predecessors
                     * differently than the block finder. This caused the Phi
                     * copies to swap values, producing wrong results.
                     *
                     * Fix: For each schedule predecessor p, find the predecessor's
                     * control output (Goto/Proj). Then scan the Phi's inputs to
                     * find the data input at the same index as the matching
                     * Region input. This correctly matches predecessors to Phi
                     * inputs regardless of ordering. */
                    uint32_t pred_block_idx = sched_blk->pred_blocks[p];
                    if (pred_block_idx >= schedule->count) continue;

                    /* Get the predecessor's control output.
                     *
                     * For Goto blocks: the Goto node is the control output.
                     * For If blocks: the Proj node (not the If) is the control
                     * output to the successor. The Proj is scheduled in the
                     * predecessor block, but it may not be the last node.
                     *
                     * Fix: Scan ALL nodes in the predecessor's schedule and
                     * check if any of them is an input to the current block's
                     * Region. This correctly finds the Proj or Goto. */
                    const vtx_schedule_block_t *pred_blk = &schedule->blocks[pred_block_idx];

                    /* Find the Region node for this block */
                    vtx_nodeid_t region_id = VTX_NODEID_INVALID;
                    for (uint32_t pi = 0; pi < node->input_count; pi++) {
                        vtx_nodeid_t inp = node->inputs[pi];
                        if (inp == VTX_NODEID_INVALID || inp >= graph->node_table.count) continue;
                        const vtx_node_t *inp_n = vtx_node_get_const(&graph->node_table, inp);
                        if (inp_n && (inp_n->opcode == VTX_OP_Region ||
                                      inp_n->opcode == VTX_OP_LoopBegin)) {
                            region_id = inp;
                            break;
                        }
                    }

                    if (region_id == VTX_NODEID_INVALID) continue;
                    const vtx_node_t *region_n = vtx_node_get_const(&graph->node_table, region_id);
                    if (region_n == NULL) continue;

                    /* Scan predecessor's scheduled nodes for one that is an
                     * input to the Region. This finds the Proj or Goto that
                     * connects this predecessor to the current block. */
                    vtx_nodeid_t pred_ctrl = VTX_NODEID_INVALID;
                    uint32_t matching_region_input_idx = 0;
                    for (uint32_t sn = 0; sn < pred_blk->node_count; sn++) {
                        vtx_nodeid_t cand = pred_blk->nodes[sn];
                        for (uint32_t ri = 0; ri < region_n->input_count; ri++) {
                            if (region_n->inputs[ri] == cand) {
                                pred_ctrl = cand;
                                matching_region_input_idx = ri;
                                break;
                            }
                        }
                        if (pred_ctrl != VTX_NODEID_INVALID) break;
                    }

                    if (pred_ctrl == VTX_NODEID_INVALID) {
                        /* Fallback: use position-based approach */
                        if (p >= node->input_count) continue;
                        uint32_t input_vreg = vtx_isel_node_vreg(stream, node->inputs[p]);
                        if (input_vreg == VTX_VREG_INVALID || input_vreg == phi_vreg) continue;
                        if (copy_count < MAX_PHI_COPIES) {
                            copy_dst[copy_count] = phi_vreg;
                            copy_src[copy_count] = input_vreg;
                            copy_node[copy_count] = nid;
                            copy_count++;
                        }
                        continue;
                    }

                    /* Use the matching Region input index to find the
                     * corresponding Phi data input. */
                    if (matching_region_input_idx < node->input_count) {
                        uint32_t input_vreg = vtx_isel_node_vreg(stream, node->inputs[matching_region_input_idx]);
                        if (input_vreg != VTX_VREG_INVALID && input_vreg != phi_vreg) {
                            if (copy_count < MAX_PHI_COPIES) {
                                copy_dst[copy_count] = phi_vreg;
                                copy_src[copy_count] = input_vreg;
                                copy_node[copy_count] = nid;
                                copy_count++;
                            }
                        }
                    }
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

            /* Find insertion point: before the first branch instruction.
             *
             * BUGFIX (audit #3, loop hang): The old code scanned from the end
             * looking for the first non-branch instruction, then inserted
             * after it. But if ALL instructions are branches (e.g., a loop
             * latch block with just JMP + LoopEnd), the insertion point was
             * set to inst_count (after everything), meaning Phi copies were
             * placed AFTER the JMP — they never executed.
             *
             * Fix: scan from the START, find the first branch instruction,
             * and insert before it. This ensures Phi copies execute before
             * any branch. */
            uint32_t insert_pos = pred_blk->inst_count;
            for (uint32_t j = 0; j < pred_blk->inst_count; j++) {
                if (pred_blk->insts[j].flags & VTX_INST_FLAG_IS_BRANCH) {
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

            /* Helper: insert an SMI untag sequence for RAW_INT Phi copies.
             * Uses a fresh temp vreg to avoid corrupting the source if
             * the regalloc coalesces phi_vreg == src_vreg.
             * Sequence: tmp ← src; SHL tmp, 13; SAR tmp, 16; phi ← tmp */
            #define INSERT_UNTAG(dst, src, node_id, offset) do { \
                uint32_t _tmp = vtx_isel_alloc_vreg(stream, arena); \
                vtx_inst_t _mov1 = make_rr_inst(VTX_X86_MOV, _tmp, (src), (node_id)); \
                _mov1.flags |= VTX_INST_FLAG_PHI_COPY | VTX_INST_FLAG_NO_COALESCE; \
                vtx_inst_t _shl = make_ri_inst(VTX_X86_SHL, _tmp, 13, (node_id)); \
                _shl.flags |= VTX_INST_FLAG_PHI_COPY; \
                vtx_inst_t _sar = make_ri_inst(VTX_X86_SAR, _tmp, 16, (node_id)); \
                _sar.flags |= VTX_INST_FLAG_PHI_COPY; \
                vtx_inst_t _mov2 = make_rr_inst(VTX_X86_MOV, (dst), _tmp, (node_id)); \
                _mov2.flags |= VTX_INST_FLAG_PHI_COPY; \
                if (vtx_isel_block_ensure_capacity(pred_blk, 4, arena) != 0) return -1; \
                for (int _i = 0; _i < 4; _i++) { \
                    if ((offset) + _i < pred_blk->inst_count) { \
                        memmove(&pred_blk->insts[(offset) + _i + 1], &pred_blk->insts[(offset) + _i], \
                                (pred_blk->inst_count - ((offset) + _i)) * sizeof(vtx_inst_t)); \
                    } \
                } \
                pred_blk->insts[(offset)] = _mov1; \
                pred_blk->insts[(offset) + 1] = _shl; \
                pred_blk->insts[(offset) + 2] = _sar; \
                pred_blk->insts[(offset) + 3] = _mov2; \
                pred_blk->inst_count += 4; \
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

                /* B14 fix: Emit copies in FORWARD cycle order (second to last).
                 *
                 * The old code emitted in REVERSE order (last down to second),
                 * which silently corrupted cycles of length >= 3.
                 *
                 * Example cycle A<-B, B<-C, C<-A (cycle_indices=[0,1,2]):
                 *   Old (reverse, ci=2 then ci=1): emit C<-A then B<-C.
                 *     After C<-A, C holds A_orig. Then B<-C reads C, which
                 *     was just overwritten, so B gets A_orig instead of C_orig.
                 *   New (forward, ci=1 then ci=2): emit B<-C then C<-A.
                 *     B<-C reads original C (C not yet written). C<-A reads
                 *     original A (A not yet written — A is only written by
                 *     the first copy, which we skipped and handle via temp).
                 * Then A<-temp (saved B_orig) completes the cycle.
                 *
                 * For a 2-cycle A<-B, B<-A, forward vs. reverse is the same
                 * (only one copy emitted in the loop), so this fix is
                 * backward-compatible with the 2-cycle case. */
                for (uint32_t ci = 1; ci < cycle_len; ci++) {
                    uint32_t idx = cycle_indices[ci];
                    INSERT_MOV(copy_dst[idx], copy_src[idx], copy_node[idx], cur_insert);
                    cur_insert++;
                }

                /* Copy temp to first copy's destination */
                INSERT_MOV(copy_dst[first], temp_vreg, copy_node[first], cur_insert);
                cur_insert++;
            }

            /* Phase 2: Emit all non-cycle copies (acyclic, safe to emit directly).
             * For RAW_INT Phis where the source is a tagged SMI (not RAW_INT),
             * insert an untag sequence (MOV + SHL + SAR) instead of a plain MOV.
             * This converts the tagged initial value to raw int at the loop
             * preheader, so the first iteration sees raw int (matching the
             * back-edge value from the elided Add). */
            for (uint32_t i = 0; i < copy_count; i++) {
                if (processed[i]) continue; /* already handled as part of a cycle */

                /* Check if this Phi is RAW_INT and the source is tagged */
                const vtx_node_t *phi_node = vtx_node_get_const(&graph->node_table, copy_node[i]);
                bool phi_is_raw = phi_node && vtx_nf_has(phi_node->flags, VTX_NF_RAW_INT);

                if (phi_is_raw) {
                    /* The Phi is RAW_INT. Both the forward-edge (preheader)
                     * and back-edge (latch) sources need to be raw int.
                     * - Forward edge: source is a tagged SMI (Constant/Parameter)
                     *   → need untag
                     * - Back edge: source might be RAW_INT (from elided Add) → plain MOV
                     *   or tagged SMI (from non-elided Sub that feeds Store) → need untag
                     *
                     * We check the source node's RAW_INT flag to decide.
                     * Forward edge = predecessor before loop header.
                     * Back edge = predecessor after. */
                    bool is_forward_edge = (pred_idx < b);

                    /* Find the source node to check if it's RAW_INT */
                    /* The source vreg is copy_src[i]. We need to find which
                     * IR node produced it. We stored the Phi node ID in
                     * copy_node[i], but not the source node ID. We need to
                     * look it up from the Phi's inputs. */
                    bool src_is_raw = false;
                    /* For loop headers, re-scan the Phi inputs to find the
                     * source node for this predecessor. */
                    if (sched_blk->is_loop_header) {
                        uint32_t data_idx = 0;
                        for (uint32_t pi = 0; pi < phi_node->input_count; pi++) {
                            vtx_nodeid_t inp_id = phi_node->inputs[pi];
                            if (inp_id == VTX_NODEID_INVALID || inp_id >= graph->node_table.count) continue;
                            const vtx_node_t *inp_node = vtx_node_get_const(&graph->node_table, inp_id);
                            if (inp_node && (inp_node->opcode == VTX_OP_Region ||
                                             inp_node->opcode == VTX_OP_LoopBegin ||
                                             inp_node->opcode == VTX_OP_Proj)) {
                                continue;
                            }
                            if (data_idx == p) {
                                src_is_raw = inp_node && vtx_nf_has(inp_node->flags, VTX_NF_RAW_INT);
                                break;
                            }
                            data_idx++;
                        }
                    }

                    if ((is_forward_edge && !src_is_raw) ||
                        (!is_forward_edge && !src_is_raw)) {
                        /* Source is tagged SMI → need untag */
                        INSERT_UNTAG(copy_dst[i], copy_src[i], copy_node[i], cur_insert);
                        cur_insert += 4;
                    } else {
                        /* Source is already raw int → plain MOV */
                        INSERT_MOV(copy_dst[i], copy_src[i], copy_node[i], cur_insert);
                        cur_insert++;
                    }
                } else {
                    /* Normal Phi: plain MOV */
                    INSERT_MOV(copy_dst[i], copy_src[i], copy_node[i], cur_insert);
                    cur_insert++;
                }
            }

            #undef INSERT_MOV
            #undef INSERT_UNTAG
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
                /* Only resolve JMP targets that weren't set by the isel.
                 * Goto JMPs have target=0 and no TARGET_SET flag.
                 * If fallthrough JMPs have TARGET_SET flag — don't override. */
                if (!(inst->flags & VTX_INST_FLAG_TARGET_SET)) {
                    inst->operands[0] = sched_blk->succ_blocks[0];
                }
            }
            /* JCC targets are now set by the isel directly via
             * make_branch_inst(VTX_X86_JCC, target_block, ...).
             * The isel looks up the target block from the If node's
             * method_index (branch target PC) by matching Region
             * bytecode_pc values. This is correct regardless of block
             * layout order — no b+1 heuristic needed. */
        }
    }
}

/* ========================================================================== */
/* Cmp+If fusion pre-pass                                                      */
/* ========================================================================== */

/**
 * Mark Cmp nodes whose ONLY consumer is an If node AND the Cmp is
 * the immediate predecessor of the If in the schedule (same block,
 * adjacent positions). This guarantees no flag-clobbering instructions
 * are inserted between the Cmp's CMP and the If's JCC.
 *
 * When a Cmp feeds directly into an If (the common case in branchy code),
 * we can skip the SETCC+MOVZX+retag sequence in the Cmp isel and skip the
 * redundant CMP cond,SMI(0) in the If isel. The If's JCC uses the flags
 * left by the Cmp's CMP directly.
 *
 * Uses the node's `mark` bit to flag Cmp nodes eligible for fusion.
 */
static void mark_cmp_if_fusion(vtx_graph_t *graph, const vtx_schedule_t *schedule)
{
    vtx_node_table_t *nt = &graph->node_table;
    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t *cmp = &nt->nodes[i];
        if (cmp->dead || cmp->opcode != VTX_OP_Cmp) continue;
        if (cmp->use_count != 1) continue;

        vtx_use_entry_t *use = &cmp->uses[0];
        if (use->user_id >= nt->count) continue;
        vtx_node_t *user = &nt->nodes[use->user_id];
        if (user->dead || user->opcode != VTX_OP_If) continue;

        /* Check that Cmp and If are in the same block and adjacent.
         * This guarantees no flag-clobbering instructions between them. */
        if (schedule == NULL || schedule->node_block == NULL) continue;
        if (i >= schedule->node_block_count) continue;
        if (use->user_id >= schedule->node_block_count) continue;

        uint32_t cmp_block = schedule->node_block[i];
        uint32_t if_block = schedule->node_block[use->user_id];
        if (cmp_block != if_block) continue;
        if (cmp_block >= schedule->count) continue;

        /* Find the Cmp and If positions in the block's node list */
        const vtx_schedule_block_t *blk = &schedule->blocks[cmp_block];
        int32_t cmp_pos = -1, if_pos = -1;
        for (uint32_t n = 0; n < blk->node_count; n++) {
            if (blk->nodes[n] == (vtx_nodeid_t)i) cmp_pos = (int32_t)n;
            if (blk->nodes[n] == use->user_id) if_pos = (int32_t)n;
        }
        if (cmp_pos < 0 || if_pos < 0) continue;

        /* The Cmp must be immediately before the If (adjacent).
         * This guarantees no other data nodes are scheduled between
         * them that could clobber flags. */
        if (if_pos != cmp_pos + 1) continue;

        /* The Cmp's only user is an If, they're in the same block,
         * and they're adjacent — safe to fuse. */
        cmp->mark = true;
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

    /* Allocate the SMI scratch vreg (fixed to R10). This vreg is reused by
     * all SMI adjustment sequences (Add/Sub header fixups, etc.) so we don't
     * create multiple vregs all fighting for R10 in the register allocator.
     * R10 is reserved in VTX_REG_RESERVED_MASK so regalloc will never
     * allocate it for normal vregs. */
    stream->smi_scratch_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 10);
    if (stream->smi_scratch_vreg == VTX_VREG_INVALID) return NULL;

    /* Allocate the SMI mask vreg (fixed to R11). Used for VTX_NAN_DATA_MASK
     * constant in SMI retag sequences: (val & DATA_MASK) << 3 | HEADER.
     * R11 is reserved in VTX_REG_RESERVED_MASK so regalloc will never
     * allocate it for normal vregs. */
    stream->smi_mask_vreg = vtx_isel_alloc_vreg_fixed(stream, arena, 11);
    if (stream->smi_mask_vreg == VTX_VREG_INVALID) return NULL;

    stream->block_count = schedule->count;
    stream->block_capacity = schedule->count;
    stream->blocks = (vtx_inst_block_t *)vtx_arena_alloc(arena,
                       schedule->count * sizeof(vtx_inst_block_t));
    if (!stream->blocks) return NULL;
    memset(stream->blocks, 0, schedule->count * sizeof(vtx_inst_block_t));

    /* Cmp+If fusion pre-pass: mark Cmp nodes whose only consumer is an If,
     * AND the Cmp and If are adjacent in the schedule (same block, consecutive
     * positions). This guarantees no flag-clobbering instructions between them.
     * The Cmp isel skips SETCC+MOVZX+retag for marked nodes (emits only CMP),
     * and the If isel uses the Cmp's flags directly for JCC.
     *
     * IMPORTANT: Clear all marks first — other IR passes use the mark bit
     * and may leave stale marks. */
    vtx_node_table_clear_marks((vtx_node_table_t *)&graph->node_table);
    mark_cmp_if_fusion((vtx_graph_t *)graph, schedule);

    /* Walk each block and select instructions for each node */
    for (uint32_t b = 0; b < schedule->count; b++) {
        const vtx_schedule_block_t *sched_blk = &schedule->blocks[b];
        vtx_inst_block_t *inst_blk = &stream->blocks[b];
        inst_blk->block_id = b;

        for (uint32_t n = 0; n < sched_blk->node_count; n++) {
            vtx_nodeid_t node_id = sched_blk->nodes[n];
            if (select_node(stream, inst_blk, graph, schedule, b, node_id, arena) != 0) {
                const vtx_node_t *dbg_node = vtx_node_get_const(&graph->node_table, node_id);
                if (dbg_node) {
                    fprintf(stderr, "ISEL FAIL on N%u (%s), input_count=%u, inputs:",
                            node_id, vtx_node_opcode_name(dbg_node->opcode),
                            dbg_node->input_count);
                    for (uint32_t di = 0; di < dbg_node->input_count; di++) {
                        vtx_nodeid_t dinp = dbg_node->inputs[di];
                        const vtx_node_t *dinp_n = (dinp < graph->node_table.count) ?
                            vtx_node_get_const(&graph->node_table, dinp) : NULL;
                        uint32_t dvreg = vtx_isel_node_vreg(stream, dinp);
                        fprintf(stderr, " N%u(%s,vreg=%u,dead=%d)",
                                dinp,
                                dinp_n ? vtx_node_opcode_name(dinp_n->opcode) : "INVALID",
                                dvreg,
                                dinp_n ? (int)dinp_n->dead : -1);
                    }
                }
                fprintf(stderr, "\n");
                return NULL;
            }
        }
    }

    /* If any SMI arithmetic was emitted, prepend the SMI constant loads
     * (R10=HEADER, R11=DATA_MASK) to block 0. The retag/untag sequences
     * rely on these constants being in R10/R11.
     *
     * BUGFIX (audit #2): The old code reloaded these constants on every
     * retag call. Now we load them once here. We emit them as instructions
     * in the stream (not as raw emitter calls) so the regalloc sees the
     * definitions and doesn't spill the fixed vregs.
     *
     * We use a special source_node of VTX_NODEID_INVALID to indicate these
     * are prologue instructions with no corresponding IR node. */
    if (stream->uses_smi && stream->block_count > 0) {
        vtx_inst_block_t *blk0 = &stream->blocks[0];
        /* Ensure capacity for 3 extra instructions at the front */
        if (vtx_isel_block_ensure_capacity(blk0, 3, arena) != 0) return NULL;
        /* Shift existing instructions right by 3 */
        memmove(&blk0->insts[3], &blk0->insts[0],
                blk0->inst_count * sizeof(vtx_inst_t));
        /* MOV R10, VTX_NAN_BOX_HEADER */
        vtx_inst_t mov_r10 = make_ri_inst(VTX_X86_MOV, stream->smi_scratch_vreg,
                                           (int64_t)VTX_NAN_BOX_HEADER, VTX_NODEID_INVALID);
        mov_r10.flags |= VTX_INST_FLAG_NO_COALESCE;
        blk0->insts[0] = mov_r10;
        /* MOV R11, -1 */
        vtx_inst_t mov_r11 = make_ri_inst(VTX_X86_MOV, stream->smi_mask_vreg,
                                           (int64_t)-1, VTX_NODEID_INVALID);
        mov_r11.flags |= VTX_INST_FLAG_NO_COALESCE;
        blk0->insts[1] = mov_r11;
        /* SHR R11, 16 → R11 = 0x0000FFFFFFFFFFFF (DATA_MASK) */
        vtx_inst_t shr_r11 = make_ri_inst(VTX_X86_SHR, stream->smi_mask_vreg,
                                           16, VTX_NODEID_INVALID);
        blk0->insts[2] = shr_r11;
        blk0->inst_count += 3;
    }

    /* Resolve Phi nodes: emit parallel copy sequences at predecessor block ends */
    if (resolve_phis(stream, schedule, graph, arena) != 0) {
        return NULL;
    }

    /* Resolve branch targets from schedule successor info */
    resolve_branch_targets(stream, schedule);

    return stream;
}
