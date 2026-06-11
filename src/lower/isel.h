#ifndef VORTEX_LOWER_ISEL_H
#define VORTEX_LOWER_ISEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/node.h"
#include "ir/schedule.h"
#include "runtime/arena.h"

/**
 * VORTEX Instruction Selection — SoN Nodes → x86-64 Instructions
 *
 * Walks the scheduled basic blocks and maps each SoN node to one or more
 * x86-64 instructions. Each value-producing node is assigned a virtual
 * register. The resulting instruction stream uses virtual registers that
 * will be resolved by the register allocator.
 *
 * Special handling:
 *   - Div/Mod: uses CQO + IDIV which clobbers RAX/RDX
 *   - Calls: argument registers per System V AMD64 ABI
 *   - Memory: base+index+scale+displacement addressing
 *   - Guards: compare + conditional jump (patched by guard_emit)
 */

/* ========================================================================== */
/* Virtual register                                                            */
/* ========================================================================== */

#define VTX_VREG_INVALID ((uint32_t)0xFFFFFFFF)
#define VTX_VREG_COUNT_INITIAL 256

/* Per-vreg metadata flags */
#define VTX_VREG_FLAG_NONE    0u
#define VTX_VREG_FLAG_FIXED   (1u << 0)   /* fixed to a physical register */

/* ========================================================================== */
/* x86-64 instruction opcodes                                                  */
/* ========================================================================== */

typedef enum {
    VTX_X86_NOP = 0,

    /* Arithmetic */
    VTX_X86_ADD,        /* add r/m64, r64  or  add r/m64, imm32  or  add r/m64, imm8 */
    VTX_X86_SUB,        /* sub r/m64, r64  or  sub r/m64, imm */
    VTX_X86_IMUL,       /* imul r64, r/m64 */
    VTX_X86_IDIV,       /* idiv r/m64  (RDX:RAX / r/m64) */
    VTX_X86_MUL,        /* mul r/m64  (RDX:RAX * r/m64, unsigned) */
    VTX_X86_NEG,        /* neg r/m64 */
    VTX_X86_NOT,        /* not r/m64 */
    VTX_X86_INC,        /* inc r/m64 */
    VTX_X86_DEC,        /* dec r/m64 */

    /* Shifts */
    VTX_X86_SHL,        /* shl r/m64, imm8  or  shl r/m64, CL */
    VTX_X86_SHR,        /* shr r/m64, imm8  or  shr r/m64, CL */
    VTX_X86_SAR,        /* sar r/m64, imm8  or  sar r/m64, CL */

    /* Bitwise */
    VTX_X86_AND,        /* and r/m64, r64  or  and r/m64, imm */
    VTX_X86_OR,         /* or  r/m64, r64  or  or  r/m64, imm */
    VTX_X86_XOR,        /* xor r/m64, r64  or  xor r/m64, imm */

    /* Comparison / test */
    VTX_X86_CMP,        /* cmp r/m64, r64  or  cmp r/m64, imm */
    VTX_X86_TEST,       /* test r/m64, r64  or  test r/m64, imm */

    /* Data movement */
    VTX_X86_MOV,        /* mov r/m64, r64  or  mov r64, r/m64  or  mov r64, imm64  or  mov r/m64, imm32 */
    VTX_X86_MOVZX,      /* movzx r64, r/m32 */
    VTX_X86_MOVSX,      /* movsx r64, r/m32 */
    VTX_X86_CMOV,       /* cmovcc r64, r/m64 */
    VTX_X86_XCHG,       /* xchg r/m64, r64 */
    VTX_X86_LEA,        /* lea r64, m */

    /* Sign extension */
    VTX_X86_CQO,        /* sign-extend RAX into RDX:RAX */
    VTX_X86_CDQ,        /* sign-extend EAX into EDX:EAX */

    /* Set byte on condition */
    VTX_X86_SETCC,      /* setcc r/m8 */

    /* Stack */
    VTX_X86_PUSH,       /* push r64  or  push imm8  or  push imm32 */
    VTX_X86_POP,        /* pop r64 */

    /* Control flow */
    VTX_X86_JMP,        /* jmp rel32  or  jmp rel8 */
    VTX_X86_JCC,        /* jcc rel32  or  jcc rel8 */
    VTX_X86_CALL,       /* call rel32 */
    VTX_X86_RET,        /* ret */

    /* Flag operations */
    VTX_X86_LAHF,       /* load flags into AH */
    VTX_X86_SAHF,       /* store AH into flags */

    VTX_X86_OPCODE_COUNT
} vtx_x86_opcode_t;

/* ========================================================================== */
/* Operand kinds                                                              */
/* ========================================================================== */

typedef enum {
    VTX_OPND_NONE = 0,
    VTX_OPND_VREG,      /* virtual register (value in operand field) */
    VTX_OPND_PREG,      /* physical register (value in operand field, vtx_reg_t) */
    VTX_OPND_IMM,       /* immediate (value in imm field) */
    VTX_OPND_MEM,       /* memory (address in mem field) */
    VTX_OPND_LABEL,     /* branch target label = block index (value in operand field) */
} vtx_opnd_kind_t;

/* ========================================================================== */
/* Memory operand                                                              */
/* ========================================================================== */

typedef struct {
    uint32_t base_vreg;   /* base virtual register (VTX_VREG_INVALID = none) */
    uint32_t index_vreg;  /* index virtual register (VTX_VREG_INVALID = none) */
    uint8_t  scale;       /* 1, 2, 4, or 8 */
    int32_t  disp;        /* displacement */
} vtx_x86_memop_t;

/* ========================================================================== */
/* Instruction                                                                 */
/* ========================================================================== */

#define VTX_INST_MAX_OPERANDS 3
#define VTX_INST_FLAG_NONE        0u
#define VTX_INST_FLAG_HAS_IMM     (1u << 0)  /* immediate field is valid */
#define VTX_INST_FLAG_HAS_MEM     (1u << 1)  /* memory operand is valid */
#define VTX_INST_FLAG_HAS_COND    (1u << 2)  /* condition code is valid */
#define VTX_INST_FLAG_CLOBBER_RAX (1u << 3)  /* clobbers RAX (idiv etc.) */
#define VTX_INST_FLAG_CLOBBER_RDX (1u << 4)  /* clobbers RDX (idiv etc.) */
#define VTX_INST_FLAG_CLOBBER_RCX (1u << 5)  /* clobbers RCX (variable shifts) */
#define VTX_INST_FLAG_IS_CALL     (1u << 6)  /* is a call instruction */
#define VTX_INST_FLAG_IS_GUARD    (1u << 7)  /* is a guard check (needs deopt) */
#define VTX_INST_FLAG_IS_DEOPT    (1u << 8)  /* is a deopt stub entry */
#define VTX_INST_FLAG_IS_BRANCH   (1u << 9)  /* is a branch (needs reloc) */

typedef struct {
    vtx_x86_opcode_t opcode;
    vtx_opnd_kind_t  opnd_kinds[VTX_INST_MAX_OPERANDS];
    uint32_t         operands[VTX_INST_MAX_OPERANDS]; /* vreg id / preg / label */
    int64_t          imm;          /* immediate value */
    vtx_x86_memop_t  mem;          /* memory operand */
    vtx_cond_t       cond;         /* condition code for JCC/SETCC/CMOV */
    uint32_t         flags;        /* VTX_INST_FLAG_* bitmask */

    /* Tracking: which SoN node produced this instruction */
    vtx_nodeid_t     source_node;

    /* Filled during emission: native code offset of this instruction */
    uint32_t         native_offset;
} vtx_inst_t;

/* ========================================================================== */
/* Instruction block (one per schedule block)                                  */
/* ========================================================================== */

#define VTX_INST_BLOCK_INITIAL_CAPACITY 64

typedef struct {
    vtx_inst_t *insts;
    uint32_t    inst_count;
    uint32_t    inst_capacity;
    uint32_t    block_id;          /* corresponds to schedule block index */
} vtx_inst_block_t;

/* ========================================================================== */
/* Instruction stream                                                          */
/* ========================================================================== */

typedef struct {
    vtx_inst_block_t *blocks;
    uint32_t          block_count;
    uint32_t          block_capacity;

    /* Virtual register info */
    uint32_t          vreg_count;            /* next vreg id to assign */
    uint32_t         *node_to_vreg;          /* NodeID → vreg mapping (arena) */
    uint32_t          node_to_vreg_count;    /* size of node_to_vreg array */

    /* Per-vreg: fixed register constraint (VTX_REG_NONE = no constraint) */
    uint8_t          *vreg_fixed_reg;        /* vreg → vtx_reg_t if VTX_VREG_FLAG_FIXED */
    uint32_t          vreg_fixed_reg_count;  /* size of vreg_fixed_reg array */

    /* Reference to the graph for node lookups */
    const vtx_schedule_t *schedule;
} vtx_inst_stream_t;

/* ========================================================================== */
/* Instruction selection entry point                                           */
/* ========================================================================== */

/**
 * Select x86-64 instructions from the scheduled SoN graph.
 *
 * Walks each block in the schedule, mapping each SoN node to x86-64
 * instructions. Value-producing nodes are assigned virtual registers.
 * Guard/DeoptGuard nodes emit compare + conditional jump instructions.
 *
 * @param schedule  The scheduled SoN graph
 * @param arena     Arena for allocations (node_to_vreg, blocks, instructions)
 * @return          Instruction stream, or NULL on failure
 */
vtx_inst_stream_t *vtx_isel_select(const vtx_schedule_t *schedule,
                                     const vtx_graph_t *graph,
                                     vtx_arena_t *arena);

/**
 * Get the virtual register assigned to a node.
 * Returns VTX_VREG_INVALID if the node has no vreg.
 */
uint32_t vtx_isel_node_vreg(const vtx_inst_stream_t *stream, vtx_nodeid_t node_id);

/**
 * Allocate a new virtual register in the stream.
 * Returns the new vreg id, or VTX_VREG_INVALID on failure.
 */
uint32_t vtx_isel_alloc_vreg(vtx_inst_stream_t *stream, vtx_arena_t *arena);

/**
 * Allocate a new virtual register fixed to a specific physical register.
 * Returns the new vreg id, or VTX_VREG_INVALID on failure.
 */
uint32_t vtx_isel_alloc_vreg_fixed(vtx_inst_stream_t *stream, vtx_arena_t *arena,
                                    uint8_t phys_reg);

/**
 * Map a node to a virtual register (overwriting any previous mapping).
 */
void vtx_isel_map_node_vreg(vtx_inst_stream_t *stream, vtx_nodeid_t node_id,
                             uint32_t vreg, vtx_arena_t *arena);

/**
 * Append an instruction to a block.
 * Returns the instruction index, or UINT32_MAX on failure.
 */
uint32_t vtx_isel_emit_inst(vtx_inst_block_t *block, vtx_inst_t inst, vtx_arena_t *arena);

/**
 * Ensure a block has capacity for at least `needed` more instructions.
 * Returns 0 on success, -1 on failure.
 */
int vtx_isel_block_ensure_capacity(vtx_inst_block_t *block, uint32_t needed, vtx_arena_t *arena);

/**
 * Get the human-readable name for an x86 opcode.
 */
const char *vtx_x86_opcode_name(vtx_x86_opcode_t opcode);

#endif /* VORTEX_LOWER_ISEL_H */
