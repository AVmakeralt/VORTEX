#ifndef VORTEX_BYTECODE_H
#define VORTEX_BYTECODE_H

#include <stdint.h>
#include <stddef.h>
#include "vortex_config.h"
#include "runtime/object.h"

/**
 * VORTEX Bytecode Format
 *
 * Variable-length instruction stream. Each instruction consists of:
 *   - 1 byte: opcode
 *   - 0, 1, or 2 bytes: operand (depending on opcode)
 *
 * Operands are stored in big-endian format (2-byte operands).
 * The constant pool contains tagged values indexed by LOAD_CONST_* operands.
 */

/* ========================================================================== */
/* Opcode enumeration                                                          */
/* ========================================================================== */

typedef enum {
    VT_OP_HALT = 0,
    VT_OP_NOP,

    /* Local variable access (2-byte operand: local index) */
    VT_OP_LOAD_LOCAL,
    VT_OP_STORE_LOCAL,

    /* Field access (2-byte operand: field offset) */
    VT_OP_LOAD_FIELD,
    VT_OP_STORE_FIELD,

    /* Constant loading */
    VT_OP_LOAD_CONST_INT,    /* 2-byte operand: constant pool index */
    VT_OP_LOAD_CONST_FLOAT,  /* 2-byte operand: constant pool index */
    VT_OP_LOAD_CONST_STR,    /* 2-byte operand: constant pool index */
    VT_OP_LOAD_NULL,
    VT_OP_LOAD_TRUE,
    VT_OP_LOAD_FALSE,
    VT_OP_LOAD_UNDEFINED,

    /* Integer arithmetic (no operand, pops 2, pushes 1) */
    VT_OP_IADD,
    VT_OP_ISUB,
    VT_OP_IMUL,
    VT_OP_IDIV,
    VT_OP_IMOD,

    /* Float arithmetic */
    VT_OP_FADD,
    VT_OP_FSUB,
    VT_OP_FMUL,
    VT_OP_FDIV,

    /* Bitwise and unary integer operations */
    VT_OP_ISHL,
    VT_OP_ISHR,
    VT_OP_IAND,
    VT_OP_IOR,
    VT_OP_IXOR,
    VT_OP_INEG,
    VT_OP_INOT,

    /* Integer comparisons (pop 2, push bool) */
    VT_OP_ICMP_EQ,
    VT_OP_ICMP_NE,
    VT_OP_ICMP_LT,
    VT_OP_ICMP_LE,
    VT_OP_ICMP_GT,
    VT_OP_ICMP_GE,

    /* Float comparisons */
    VT_OP_FCMP_EQ,
    VT_OP_FCMP_NE,
    VT_OP_FCMP_LT,
    VT_OP_FCMP_LE,
    VT_OP_FCMP_GT,
    VT_OP_FCMP_GE,

    /* Control flow (2-byte operand: target PC) */
    VT_OP_GOTO,
    VT_OP_IF_TRUE,    /* pop 1, branch if truthy */
    VT_OP_IF_FALSE,   /* pop 1, branch if falsy */

    /* Calls (2-byte operand: method index / arg count) */
    VT_OP_CALL_STATIC,    /* 2-byte operand: method index */
    VT_OP_CALL_VIRTUAL,   /* 2-byte operand: method index */
    VT_OP_CALL_INTERFACE, /* 2-byte operand: interface typeid + method index */

    /* Returns */
    VT_OP_RETURN,        /* return void */
    VT_OP_RETURN_VALUE,  /* pop 1, return value */

    /* Object creation (2-byte operand: typeid) */
    VT_OP_NEW,
    VT_OP_NEWARRAY,     /* 2-byte operand: element count from stack or operand */

    /* Type checks */
    VT_OP_CHECKCAST,    /* 2-byte operand: typeid */
    VT_OP_INSTANCEOF,   /* 2-byte operand: typeid */

    /* Array operations */
    VT_OP_ARRAY_LOAD,
    VT_OP_ARRAY_STORE,
    VT_OP_ARRAY_LENGTH,

    /* Exception handling */
    VT_OP_THROW,
    VT_OP_CATCH,            /* 2-byte operand: catch handler PC (catch-all) */
    VT_OP_CATCH_TYPED,      /* 2-byte operand: catch handler PC + 2-byte catch typeid */

    /* Monitor (synchronization) */
    VT_OP_MONITOR_ENTER,
    VT_OP_MONITOR_EXIT,

    /* Stack manipulation */
    VT_OP_DUP,
    VT_OP_POP,
    VT_OP_SWAP,

    /* Type queries */
    VT_OP_ISNULL,
    VT_OP_TYPEOF,

    /* Runtime calls */
    VT_OP_CALL_RUNTIME,    /* call into runtime helper */

    /* Total opcode count */
    VT_OP_COUNT
} vtx_opcode_t;

/* ========================================================================== */
/* Opcode metadata                                                             */
/* ========================================================================== */

/**
 * Per-opcode metadata: name, stack input/output counts, operand info.
 */
typedef struct {
    const char *name;
    uint8_t     stack_input_count;   /* values consumed from operand stack */
    uint8_t     stack_output_count;  /* values produced onto operand stack */
    bool        has_operand;         /* does this opcode have an operand? */
    uint8_t     operand_size;        /* size of operand in bytes (1 or 2) */
} vtx_opcode_info_t;

/**
 * Static const table of opcode info, indexed by vtx_opcode_t.
 */
extern const vtx_opcode_info_t vtx_opcode_table[VT_OP_COUNT];

/* ========================================================================== */
/* Bytecode module                                                             */
/* ========================================================================== */

/**
 * A bytecode module: code + constant pool.
 */
typedef struct {
    const uint8_t *code;            /* bytecode instruction stream */
    size_t         length;          /* length of code in bytes */
    vtx_value_t   *constant_pool;   /* array of constant values */
    uint32_t       constant_count;  /* number of constants */
    uint16_t       max_locals;      /* max local variable slots */
    uint16_t       max_stack;       /* max operand stack depth */
} vtx_bytecode_t;

/**
 * Read the opcode at the given PC.
 */
vtx_opcode_t vtx_bytecode_opcode_at(const vtx_bytecode_t *bc, size_t pc);

/**
 * Read a 2-byte big-endian operand at pc+1.
 * Returns the 16-bit unsigned operand value.
 */
uint16_t vtx_bytecode_read_operand(const vtx_bytecode_t *bc, size_t pc);

/**
 * Compute the net stack effect of an opcode (output_count - input_count).
 * Returns a negative number if the opcode pops more than it pushes.
 */
int vtx_bytecode_stack_effect(vtx_opcode_t opcode);

/**
 * Disassemble one instruction at `pc` into the provided buffer.
 * Returns the PC of the next instruction (pc + instruction length).
 * Writes at most bufsize-1 characters plus a null terminator.
 */
size_t vtx_bytecode_disassemble_op(const vtx_bytecode_t *bc, size_t pc,
                                   char *buf, size_t bufsize);

/**
 * Compute the length (in bytes) of the instruction at `pc`.
 */
size_t vtx_bytecode_insn_length(const vtx_bytecode_t *bc, size_t pc);

#endif /* VORTEX_BYTECODE_H */
