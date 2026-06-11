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
        vtx_inst_t inst;
        memset(&inst, 0, sizeof(inst));
        inst.opcode = VTX_X86_MOV;
        inst.opnd_kinds[0] = VTX_OPND_VREG;
        inst.operands[0] = dst;
        inst.opnd_kinds[1] = VTX_OPND_IMM;
        if (node->constval.kind == VTX_TYPE_Int) {
            inst.imm = node->constval.as.int_val;
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

    /* ---- Comparison ---- */
    case VTX_OP_Cmp:
    case VTX_OP_CmpP: {
        if (node->input_count < 2) return -1;
        uint32_t lhs = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t rhs = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (lhs == VTX_VREG_INVALID || rhs == VTX_VREG_INVALID) return -1;
        vtx_isel_emit_inst(block, make_rr_inst(VTX_X86_CMP, lhs, rhs, node_id), arena);
        vtx_inst_t setcc;
        memset(&setcc, 0, sizeof(setcc));
        setcc.opcode = VTX_X86_SETCC;
        setcc.opnd_kinds[0] = VTX_OPND_VREG;
        setcc.operands[0] = dst;
        setcc.cond = node->cond;
        setcc.flags = VTX_INST_FLAG_HAS_COND;
        setcc.source_node = node_id;
        vtx_isel_emit_inst(block, setcc, arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_AND, dst, 0xFF, node_id), arena);
        break;
    }

    /* ---- Memory ---- */
    case VTX_OP_Load: {
        if (node->input_count < 1) return -1;
        uint32_t addr = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (addr == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t mem = { addr, VTX_VREG_INVALID, 1, 0 };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, dst, &mem, node_id), arena);
        break;
    }

    case VTX_OP_Store: {
        if (node->input_count < 2) return -1;
        uint32_t addr = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t val = vtx_isel_node_vreg(stream, node->inputs[1]);
        if (addr == VTX_VREG_INVALID || val == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t mem = { addr, VTX_VREG_INVALID, 1, 0 };
        vtx_isel_emit_inst(block, make_mr_inst(VTX_X86_MOV, &mem, val, node_id), arena);
        break;
    }

    case VTX_OP_LoadField: {
        if (node->input_count < 1) return -1;
        uint32_t obj = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (obj == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t mem = { obj, VTX_VREG_INVALID, 1, (int32_t)node->field_offset };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, dst, &mem, node_id), arena);
        break;
    }

    case VTX_OP_StoreField: {
        if (node->input_count < 2) return -1;
        uint32_t obj = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t val = vtx_isel_node_vreg(stream, node->inputs[1]);
        if (obj == VTX_VREG_INVALID || val == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t mem = { obj, VTX_VREG_INVALID, 1, (int32_t)node->field_offset };
        vtx_isel_emit_inst(block, make_mr_inst(VTX_X86_MOV, &mem, val, node_id), arena);
        break;
    }

    case VTX_OP_LoadIndexed: {
        if (node->input_count < 2) return -1;
        uint32_t base = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t idx = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t dst = ensure_node_vreg(stream, node_id, arena);
        if (base == VTX_VREG_INVALID || idx == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t mem = { base, idx, 8, 0 };
        vtx_isel_emit_inst(block, make_rm_inst(VTX_X86_MOV, dst, &mem, node_id), arena);
        break;
    }

    case VTX_OP_StoreIndexed: {
        if (node->input_count < 3) return -1;
        uint32_t base = vtx_isel_node_vreg(stream, node->inputs[0]);
        uint32_t idx = vtx_isel_node_vreg(stream, node->inputs[1]);
        uint32_t val = vtx_isel_node_vreg(stream, node->inputs[2]);
        if (base == VTX_VREG_INVALID || idx == VTX_VREG_INVALID || val == VTX_VREG_INVALID) return -1;
        vtx_x86_memop_t mem = { base, idx, 8, 0 };
        vtx_isel_emit_inst(block, make_mr_inst(VTX_X86_MOV, &mem, val, node_id), arena);
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
        if (cond_vreg != VTX_VREG_INVALID)
            vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_TEST, cond_vreg, 1, node_id), arena);
        vtx_cond_t jcc_cond = (node->cond != VTX_COND_NEVER) ? node->cond : VTX_COND_NE;
        vtx_isel_emit_inst(block,
            make_branch_inst(VTX_X86_JCC, 0, jcc_cond, node_id), arena);
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
        vtx_inst_t setcc;
        memset(&setcc, 0, sizeof(setcc));
        setcc.opcode = VTX_X86_SETCC;
        setcc.opnd_kinds[0] = VTX_OPND_VREG;
        setcc.operands[0] = dst;
        setcc.cond = VTX_COND_EQ;
        setcc.flags = VTX_INST_FLAG_HAS_COND;
        setcc.source_node = node_id;
        vtx_isel_emit_inst(block, setcc, arena);
        vtx_isel_emit_inst(block, make_ri_inst(VTX_X86_AND, dst, 0xFF, node_id), arena);
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
        if (cond_vreg != VTX_VREG_INVALID) {
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

    case VTX_OP_FrameState:
    case VTX_OP_Start:
    case VTX_OP_End:
    case VTX_OP_LoopEnd:
    case VTX_OP_Switch:
    case VTX_OP_Unwind:
    case VTX_OP_Catch:
    case VTX_OP_Province:
    case VTX_OP_CmpF:
    case VTX_OP_CmpD:
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
                return NULL;
            }
        }
    }

    /* Resolve branch targets from schedule successor info */
    resolve_branch_targets(stream, schedule);

    return stream;
}
