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
#define VTX_VREG_FLAG_XMM     (1u << 1)   /* requires XMM register (float type) */

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

    /* Shifts / Rotates */
    VTX_X86_SHL,        /* shl r/m64, imm8  or  shl r/m64, CL */
    VTX_X86_SHR,        /* shr r/m64, imm8  or  shr r/m64, CL */
    VTX_X86_SAR,        /* sar r/m64, imm8  or  sar r/m64, CL */
    VTX_X86_ROL,        /* rol r/m64, imm8  or  rol r/m64, CL */
    VTX_X86_ROR,        /* ror r/m64, imm8  or  ror r/m64, CL */

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
    VTX_X86_CDQE,       /* sign-extend EAX into RAX (32→64) */

    /* Set byte on condition */
    VTX_X86_SETCC,      /* setcc r/m8 */

    /* Bit manipulation */
    VTX_X86_BSWAP,      /* bswap r64 — byte swap (endianness) */
    VTX_X86_BSF,        /* bsf r64, r/m64 — bit scan forward (ctz) */
    VTX_X86_BSR,        /* bsr r64, r/m64 — bit scan reverse (clz) */
    VTX_X86_POPCNT,     /* popcnt r64, r/m64 — population count (Hamming weight) */
    VTX_X86_BT,         /* bt r/m64, r64/imm8 — bit test */

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

    /* Floating-point operations (SSE/SSE2) */
    VTX_X86_UCOMISD,    /* ucomisd xmm, xmm — unordered compare double */
    VTX_X86_COMISD,     /* comisd xmm, xmm — ordered compare double */
    VTX_X86_ADDSD,      /* addsd xmm, xmm — scalar double add */
    VTX_X86_SUBSD,      /* subsd xmm, xmm — scalar double subtract */
    VTX_X86_MULSD,      /* mulsd xmm, xmm — scalar double multiply */
    VTX_X86_DIVSD,      /* divsd xmm, xmm — scalar double divide */
    VTX_X86_SQRTSD,     /* sqrtsd xmm, xmm — scalar double square root */
    VTX_X86_XORPS,      /* xorps xmm, xmm — bitwise XOR (used for neg) */
    VTX_X86_MOVSD,      /* movsd xmm, xmm — scalar double move (reg-reg) */
    VTX_X86_MOVSD_LOAD, /* movsd xmm, [mem] — scalar double load from memory */
    VTX_X86_MOVSD_STORE,/* movsd [mem], xmm — scalar double store to memory */

    /* Float ↔ Int conversions (SSE2) */
    VTX_X86_CVTSI2SD,   /* cvtsi2sd xmm, r/m64 — int64 → double */
    VTX_X86_CVTSD2SI,   /* cvtsd2si r64, xmm — double → int64 (round toward zero) */
    VTX_X86_CVTTSD2SI,  /* cvttsd2si r64, xmm — double → int64 (truncate) */
    VTX_X86_CVTSI2SS,   /* cvtsi2ss xmm, r/m64 — int64 → float */
    VTX_X86_CVTSS2SI,   /* cvtss2si r64, xmm — float → int64 */
    VTX_X86_CVTTSS2SI,  /* cvttss2si r64, xmm — float → int64 (truncate) */

    /* GPR ↔ XMM bridge (SSE2) */
    VTX_X86_MOVQ_XMM_R64,  /* movq xmm, r64 — move GPR bits into XMM */
    VTX_X86_MOVQ_R64_XMM,  /* movq r64, xmm — move XMM bits into GPR */

    /* Safepoint poll (pseudo-instruction) */
    VTX_X86_SAFEPOINT_POLL,         /* cmpq [vtx_safepoint_flag], 0; jne deopt_stub */
    VTX_X86_SAFEPOINT_POLL_GUARD_PAGE, /* movq rax, [guard_page] — zero-cost poll */

    /* SSE2 packed double operations (for vectorized loops) */
    VTX_X86_MOVAPD,     /* movaps/movapd xmm, xmm — 128-bit aligned move */
    VTX_X86_ADDPD,      /* addpd xmm, xmm — packed double add (2 doubles) */
    VTX_X86_MULPD,      /* mulpd xmm, xmm — packed double multiply (2 doubles) */
    VTX_X86_MINPD,      /* minpd xmm, xmm — packed double min */
    VTX_X86_MAXPD,      /* maxpd xmm, xmm — packed double max */
    VTX_X86_ANDPD,      /* andpd xmm, xmm — packed double bitwise AND */
    VTX_X86_XORPD,      /* xorpd xmm, xmm — packed double bitwise XOR */
    VTX_X86_SUBPD,      /* subpd xmm, xmm — packed double subtract */
    VTX_X86_DIVPD,      /* divpd xmm, xmm — packed double divide */

    /* SSE2 packed integer operations (for vectorized int loops) */
    VTX_X86_MOVDQA,     /* movdqa xmm, xmm — 128-bit aligned integer move */
    VTX_X86_MOVDQU,     /* movdqu xmm, xmm — 128-bit unaligned integer move */
    VTX_X86_PADDD,      /* paddd xmm, xmm — packed 32-bit int add (4 ints) */
    VTX_X86_PSUBD,      /* psubd xmm, xmm — packed 32-bit int subtract */
    VTX_X86_PMULLD,     /* pmulld xmm, xmm — packed 32-bit int multiply (SSE4.1) */
    VTX_X86_PXOR,       /* pxor xmm, xmm — packed integer XOR */
    VTX_X86_PAND,       /* pand xmm, xmm — packed integer AND */
    VTX_X86_POR,        /* por xmm, xmm — packed integer OR */
    VTX_X86_PCMPEQD,    /* pcmpeqd xmm, xmm — packed 32-bit int compare equal */

    /* SSE packed single-precision float operations */
    VTX_X86_MOVAPS,     /* movaps xmm, xmm — 128-bit aligned float move */
    VTX_X86_ADDPS,      /* addps xmm, xmm — packed float add (4 floats) */
    VTX_X86_MULPS,      /* mulps xmm, xmm — packed float multiply */
    VTX_X86_SUBPS,      /* subps xmm, xmm — packed float subtract */
    VTX_X86_DIVPS,      /* divps xmm, xmm — packed float divide */
    VTX_X86_MINPS,      /* minps xmm, xmm — packed float min */
    VTX_X86_MAXPS,      /* maxps xmm, xmm — packed float max */
    VTX_X86_CMPPS,      /* cmpps xmm, xmm, imm8 — packed float compare */

    /* SSE2 scalar single-precision float operations */
    VTX_X86_ADDSS,      /* addss xmm, xmm — scalar float add */
    VTX_X86_SUBSS,      /* subss xmm, xmm — scalar float subtract */
    VTX_X86_MULSS,      /* mulss xmm, xmm — scalar float multiply */
    VTX_X86_DIVSS,      /* divss xmm, xmm — scalar float divide */
    VTX_X86_SQRTSS,     /* sqrtss xmm, xmm — scalar float square root */
    VTX_X86_UCOMISS,    /* ucomiss xmm, xmm — unordered compare float */
    VTX_X86_MOVSS,      /* movss xmm, xmm — scalar float move */

    /* Unsigned division */
    VTX_X86_DIV,        /* div r/m64 — unsigned RDX:RAX / r/m64 */

    /* ---- Timing / Profiling ---- */
    VTX_X86_RDTSC,      /* 0F 31 — read time-stamp counter into EDX:EAX */
    VTX_X86_RDTSCP,     /* 0F 01 F9 — read TSC + TSC_AUX into EDX:EAX, ECX */

    /* ---- Atomics ---- */
    VTX_X86_CMPXCHG,    /* 0F B1 /r — compare RAX with r/m64; if equal, r/m64←r64; else RAX←r/m64 */
    VTX_X86_XADD,       /* 0F C1 /r — exchange and add: temp←r/m64; r/m64←temp+r64; r64←temp */

    /* ---- Memory fences ---- */
    VTX_X86_LFENCE,     /* 0F AE E8 — load fence */
    VTX_X86_MFENCE,     /* 0F AE F0 — memory fence (full) */
    VTX_X86_SFENCE,     /* 0F AE F8 — store fence */

    /* ---- SSE4.1 rounding ---- */
    VTX_X86_ROUNDSD,    /* 66 0F 3A 0B /r ib — round scalar double per imm8 */
    VTX_X86_ROUNDSS,    /* 66 0F 3A 0A /r ib — round scalar float per imm8 */

    /* ---- Constant pool load (RIP-relative MOVSD from literal pool) ---- */
    VTX_X86_MOVSD_RIP,  /* MOVSD xmm, [rip+disp32] — load double from constant pool */

    /* ---- AVX2 VEX-encoded 256-bit packed double ---- */
    VTX_X86_VMOVAPD_256,   /* VEX.256.66.0F 28 /r — 256-bit aligned move */
    VTX_X86_VADDPD_256,    /* VEX.256.66.0F 58 /r — packed double add (4 doubles) */
    VTX_X86_VSUBPD_256,    /* VEX.256.66.0F 5C /r — packed double sub */
    VTX_X86_VMULPD_256,    /* VEX.256.66.0F 59 /r — packed double mul */
    VTX_X86_VDIVPD_256,    /* VEX.256.66.0F 5E /r — packed double div */
    VTX_X86_VMINPD_256,    /* VEX.256.66.0F 5D /r — packed double min */
    VTX_X86_VMAXPD_256,    /* VEX.256.66.0F 5F /r — packed double max */
    VTX_X86_VXORPD_256,    /* VEX.256.66.0F 57 /r — packed double bitwise XOR */
    VTX_X86_VANDPD_256,    /* VEX.256.66.0F 54 /r — packed double bitwise AND */
    VTX_X86_VCMPPD_256,    /* VEX.256.66.0F C2 /r ib — packed double compare */

    /* ---- AVX2 256-bit packed single-precision float ---- */
    VTX_X86_VMOVAPS_256,   /* VEX.256.0F 28 /r — 256-bit aligned float move */
    VTX_X86_VADDPS_256,    /* VEX.256.0F 58 /r — packed float add (8 floats) */
    VTX_X86_VSUBPS_256,    /* VEX.256.0F 5C /r — packed float sub */
    VTX_X86_VMULPS_256,    /* VEX.256.0F 59 /r — packed float mul */
    VTX_X86_VDIVPS_256,    /* VEX.256.0F 5E /r — packed float div */

    /* ---- AVX2 256-bit packed integer ---- */
    VTX_X86_VMOVDQA_256,   /* VEX.256.66.0F 6F /r — 256-bit aligned integer move */
    VTX_X86_VPADDD_256,    /* VEX.256.66.0F FE /r — packed 32-bit int add (8 ints) */
    VTX_X86_VPSUBD_256,    /* VEX.256.66.0F FA /r — packed 32-bit int sub */
    VTX_X86_VPMULLD_256,   /* VEX.256.66.0F38 40 /r — packed 32-bit int mul (SSE4.1/AVX) */
    VTX_X86_VPXOR_256,     /* VEX.256.66.0F EF /r — packed integer XOR */
    VTX_X86_VPAND_256,     /* VEX.256.66.0F DB /r — packed integer AND */
    VTX_X86_VPOR_256,      /* VEX.256.66.0F EB /r — packed integer OR */

    /* ---- AVX2 utility / transition ---- */
    VTX_X86_VZEROUPPER,    /* VEX.128.0F 77 — zero upper 128 bits of YMM0-15 (critical for AVX↔SSE) */
    VTX_X86_VZEROALL,      /* VEX.256.0F 77 — zero all bits of YMM0-15 */

    /* ---- AVX2 lane manipulation / broadcast ---- */
    VTX_X86_VBROADCASTSD,   /* VEX.256.66.0F38 19 /r — broadcast scalar double to 4 doubles */
    VTX_X86_VBROADCASTSS,   /* VEX.128.66.0F38 18 /r — broadcast scalar float to 4 floats */
    VTX_X86_VPERM2F128,    /* VEX.256.66.0F3A 06 /r ib — permute 128-bit lanes */
    VTX_X86_VINSERTF128,   /* VEX.256.66.0F3A 18 /r ib — insert 128-bit lane */
    VTX_X86_VEXTRACTF128,  /* VEX.256.66.0F3A 19 /r ib — extract 128-bit lane */

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
    VTX_OPND_SPILL,     /* spill slot index (value in operand field, for spill reload/store) */
} vtx_opnd_kind_t;

/* ========================================================================== */
/* Memory operand                                                              */
/* ========================================================================== */

typedef struct {
    uint32_t base_vreg;   /* base virtual register (VTX_VREG_INVALID = none) */
    uint32_t index_vreg;  /* index virtual register (VTX_VREG_INVALID = none) */
    uint8_t  base_phys;   /* physical register for base (0xFF = unassigned) */
    uint8_t  index_phys;  /* physical register for index (0xFF = unassigned) */
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
#define VTX_INST_FLAG_CLOBBER_RSI (1u << 10) /* clobbers RSI (call arg) */
#define VTX_INST_FLAG_CLOBBER_RDI (1u << 11) /* clobbers RDI (call arg) */
#define VTX_INST_FLAG_CLOBBER_R8  (1u << 12) /* clobbers R8 (call arg) */
#define VTX_INST_FLAG_CLOBBER_R9  (1u << 13) /* clobbers R9 (call arg) */
#define VTX_INST_FLAG_CLOBBER_R10 (1u << 14) /* clobbers R10 (call arg) */
#define VTX_INST_FLAG_CLOBBER_R11 (1u << 15) /* clobbers R11 (call arg) */
#define VTX_INST_FLAG_PHI_COPY   (1u << 16) /* MOV inserted for Phi resolution */
#define VTX_INST_FLAG_SPILL_LOAD (1u << 17) /* load from spill slot to register */
#define VTX_INST_FLAG_SPILL_STORE (1u << 18) /* store from register to spill slot */
#define VTX_INST_FLAG_FUSED     (1u << 19) /* P6: CMP+JCC fused pair — scheduler must keep adjacent */
#define VTX_INST_FLAG_IS_SSE    (1u << 20) /* SSE/XMM instruction — uses XMM registers */
#define VTX_INST_FLAG_IS_SAFEPOINT (1u << 21) /* safepoint poll (loop back-edge) */
#define VTX_INST_FLAG_IMPLICIT_NULL (1u << 22) /* implicit null check — SIGSEGV catches null, no explicit TEST+JCC */
#define VTX_INST_FLAG_VALUE_GUARD   (1u << 23) /* value speculation guard — constant fold via guard page */
#define VTX_INST_FLAG_NO_TEST      (1u << 24) /* CMP must NOT be peephole-converted to TEST (NaN-boxed SMI operand) */

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

    /* SMI scratch vreg: fixed to R10, used for NaN-box header constant.
     * Allocated once during isel init, reused for all SMI adjustments.
     * R10 is reserved in VTX_REG_RESERVED_MASK so regalloc won't touch it. */
    uint32_t          smi_scratch_vreg;       /* vreg id fixed to R10 */

    /* SMI mask vreg: fixed to R11, used for VTX_NAN_DATA_MASK constant.
     * Needed for correct SMI retag: (val & DATA_MASK) << 3 | HEADER.
     * R11 is reserved in VTX_REG_RESERVED_MASK so regalloc won't touch it. */
    uint32_t          smi_mask_vreg;          /* vreg id fixed to R11 */

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
