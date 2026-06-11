#include "runtime/bytecode.h"

#include <stdio.h>
#include <string.h>

/* ========================================================================== */
/* Opcode info table                                                           */
/* ========================================================================== */

/* Helper macros for table construction */
#define OP(name, in, out, has_op, op_sz) \
    { #name, (in), (out), (has_op), (op_sz) }

const vtx_opcode_info_t vtx_opcode_table[VT_OP_COUNT] = {
    /* Control */
    OP(VT_OP_HALT,    0, 0, false, 0),
    OP(VT_OP_NOP,     0, 0, false, 0),

    /* Local variable access */
    OP(VT_OP_LOAD_LOCAL,  0, 1, true, 2),
    OP(VT_OP_STORE_LOCAL, 1, 0, true, 2),

    /* Field access */
    OP(VT_OP_LOAD_FIELD,  1, 1, true, 2),   /* pop obj, push obj.field */
    OP(VT_OP_STORE_FIELD, 2, 0, true, 2),   /* pop obj, value; obj.field = value */

    /* Constants */
    OP(VT_OP_LOAD_CONST_INT,   0, 1, true, 2),
    OP(VT_OP_LOAD_CONST_FLOAT, 0, 1, true, 2),
    OP(VT_OP_LOAD_CONST_STR,   0, 1, true, 2),
    OP(VT_OP_LOAD_NULL,        0, 1, false, 0),
    OP(VT_OP_LOAD_TRUE,        0, 1, false, 0),
    OP(VT_OP_LOAD_FALSE,       0, 1, false, 0),
    OP(VT_OP_LOAD_UNDEFINED,   0, 1, false, 0),

    /* Integer arithmetic */
    OP(VT_OP_IADD, 2, 1, false, 0),
    OP(VT_OP_ISUB, 2, 1, false, 0),
    OP(VT_OP_IMUL, 2, 1, false, 0),
    OP(VT_OP_IDIV, 2, 1, false, 0),
    OP(VT_OP_IMOD, 2, 1, false, 0),

    /* Float arithmetic */
    OP(VT_OP_FADD, 2, 1, false, 0),
    OP(VT_OP_FSUB, 2, 1, false, 0),
    OP(VT_OP_FMUL, 2, 1, false, 0),
    OP(VT_OP_FDIV, 2, 1, false, 0),

    /* Bitwise / unary */
    OP(VT_OP_ISHL,  2, 1, false, 0),
    OP(VT_OP_ISHR,  2, 1, false, 0),
    OP(VT_OP_IAND,  2, 1, false, 0),
    OP(VT_OP_IOR,   2, 1, false, 0),
    OP(VT_OP_IXOR,  2, 1, false, 0),
    OP(VT_OP_INEG,  1, 1, false, 0),
    OP(VT_OP_INOT,  1, 1, false, 0),

    /* Integer comparisons */
    OP(VT_OP_ICMP_EQ, 2, 1, false, 0),
    OP(VT_OP_ICMP_NE, 2, 1, false, 0),
    OP(VT_OP_ICMP_LT, 2, 1, false, 0),
    OP(VT_OP_ICMP_LE, 2, 1, false, 0),
    OP(VT_OP_ICMP_GT, 2, 1, false, 0),
    OP(VT_OP_ICMP_GE, 2, 1, false, 0),

    /* Float comparisons */
    OP(VT_OP_FCMP_EQ, 2, 1, false, 0),
    OP(VT_OP_FCMP_NE, 2, 1, false, 0),
    OP(VT_OP_FCMP_LT, 2, 1, false, 0),
    OP(VT_OP_FCMP_LE, 2, 1, false, 0),
    OP(VT_OP_FCMP_GT, 2, 1, false, 0),
    OP(VT_OP_FCMP_GE, 2, 1, false, 0),

    /* Control flow */
    OP(VT_OP_GOTO,     0, 0, true, 2),
    OP(VT_OP_IF_TRUE,  1, 0, true, 2),
    OP(VT_OP_IF_FALSE, 1, 0, true, 2),

    /* Calls */
    OP(VT_OP_CALL_STATIC,    0, 1, true, 2),  /* actual input varies by arg count */
    OP(VT_OP_CALL_VIRTUAL,   0, 1, true, 2),
    OP(VT_OP_CALL_INTERFACE, 0, 1, true, 2),

    /* Returns */
    OP(VT_OP_RETURN,       0, 0, false, 0),
    OP(VT_OP_RETURN_VALUE, 1, 0, false, 0),

    /* Object creation */
    OP(VT_OP_NEW,      0, 1, true, 2),
    OP(VT_OP_NEWARRAY, 1, 1, true, 2),  /* pop size, push array */

    /* Type checks */
    OP(VT_OP_CHECKCAST, 1, 1, true, 2),
    OP(VT_OP_INSTANCEOF,1, 1, true, 2),

    /* Array ops */
    OP(VT_OP_ARRAY_LOAD,  2, 1, false, 0),   /* pop array, index; push value */
    OP(VT_OP_ARRAY_STORE, 3, 0, false, 0),   /* pop array, index, value */
    OP(VT_OP_ARRAY_LENGTH,1, 1, false, 0),   /* pop array, push length */

    /* Exceptions */
    OP(VT_OP_THROW, 1, 0, false, 0),
    OP(VT_OP_CATCH, 0, 1, true, 2),

    /* Monitors */
    OP(VT_OP_MONITOR_ENTER, 1, 0, false, 0),
    OP(VT_OP_MONITOR_EXIT,  1, 0, false, 0),

    /* Stack manipulation */
    OP(VT_OP_DUP,  1, 2, false, 0),
    OP(VT_OP_POP,  1, 0, false, 0),
    OP(VT_OP_SWAP, 2, 2, false, 0),

    /* Type queries */
    OP(VT_OP_ISNULL,  1, 1, false, 0),
    OP(VT_OP_TYPEOF,  1, 1, false, 0),
};

#undef OP

/* ========================================================================== */
/* Bytecode functions                                                          */
/* ========================================================================== */

vtx_opcode_t vtx_bytecode_opcode_at(const vtx_bytecode_t *bc, size_t pc)
{
    VTX_ASSERT(bc != NULL, "bytecode must not be NULL");
    VTX_ASSERT(pc < bc->length, "PC out of bounds");
    return (vtx_opcode_t)bc->code[pc];
}

uint16_t vtx_bytecode_read_operand(const vtx_bytecode_t *bc, size_t pc)
{
    VTX_ASSERT(bc != NULL, "bytecode must not be NULL");
    VTX_ASSERT(pc + 2 < bc->length, "operand bytes out of bounds");

    /* Big-endian: first byte is high byte, second is low byte */
    uint16_t operand = ((uint16_t)bc->code[pc + 1] << 8) | (uint16_t)bc->code[pc + 2];
    return operand;
}

int vtx_bytecode_stack_effect(vtx_opcode_t opcode)
{
    VTX_ASSERT(opcode < VT_OP_COUNT, "invalid opcode");
    const vtx_opcode_info_t *info = &vtx_opcode_table[opcode];
    return (int)info->stack_output_count - (int)info->stack_input_count;
}

size_t vtx_bytecode_insn_length(const vtx_bytecode_t *bc, size_t pc)
{
    VTX_ASSERT(bc != NULL, "bytecode must not be NULL");
    VTX_ASSERT(pc < bc->length, "PC out of bounds");

    vtx_opcode_t opcode = (vtx_opcode_t)bc->code[pc];
    VTX_ASSERT(opcode < VT_OP_COUNT, "invalid opcode");

    const vtx_opcode_info_t *info = &vtx_opcode_table[opcode];
    /* 1 byte for opcode + operand size */
    return 1 + (info->has_operand ? info->operand_size : 0);
}

size_t vtx_bytecode_disassemble_op(const vtx_bytecode_t *bc, size_t pc,
                                   char *buf, size_t bufsize)
{
    VTX_ASSERT(bc != NULL, "bytecode must not be NULL");
    VTX_ASSERT(buf != NULL, "buffer must not be NULL");
    VTX_ASSERT(bufsize > 0, "buffer size must be positive");
    VTX_ASSERT(pc < bc->length, "PC out of bounds");

    vtx_opcode_t opcode = (vtx_opcode_t)bc->code[pc];
    VTX_ASSERT(opcode < VT_OP_COUNT, "invalid opcode");

    const vtx_opcode_info_t *info = &vtx_opcode_table[opcode];

    if (info->has_operand) {
        uint16_t operand = vtx_bytecode_read_operand(bc, pc);
        snprintf(buf, bufsize, "%04zu: %s %u", pc, info->name, operand);
    } else {
        snprintf(buf, bufsize, "%04zu: %s", pc, info->name);
    }

    return pc + vtx_bytecode_insn_length(bc, pc);
}
