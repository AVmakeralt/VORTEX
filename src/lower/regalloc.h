#ifndef VORTEX_LOWER_REGALLOC_H
#define VORTEX_LOWER_REGALLOC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "lower/isel.h"
#include "runtime/arena.h"

/**
 * VORTEX Linear Scan Register Allocator
 *
 * Assigns physical registers to virtual registers using a linear scan
 * algorithm. Computes live intervals for each virtual register, sorts
 * by start point, and iteratively assigns physical registers.
 *
 * Register allocation policy:
 *   - Caller-saved first: RAX, RCX, RDX, RSI, RDI, R8-R11
 *   - Callee-saved next: RBX, R12-R15 (with save/restore)
 *   - RBP/RSP reserved (never assigned)
 *   - Spill slots at fixed offsets from RBP
 *
 * When no free register is available, the interval with the furthest
 * end point is evicted (spilled).
 */

/* ========================================================================== */
/* Physical register sets                                                      */
/* ========================================================================== */

/* Number of allocatable physical registers (excludes RSP, RBP) */
#define VTX_PHYS_REG_COUNT 14

/* Caller-saved registers (first choice for allocation) */
#define VTX_CALLER_SAVED_COUNT 9
static const uint8_t vtx_caller_saved_regs[VTX_CALLER_SAVED_COUNT] = {
    0,  /* RAX */
    1,  /* RCX */
    2,  /* RDX */
    6,  /* RSI */
    7,  /* RDI */
    8,  /* R8  */
    9,  /* R9  */
    10, /* R10 */
    11, /* R11 */
};

/* Callee-saved registers (require save/restore) */
#define VTX_CALLEE_SAVED_COUNT 5
static const uint8_t vtx_callee_saved_regs[VTX_CALLEE_SAVED_COUNT] = {
    3,  /* RBX */
    12, /* R12 */
    13, /* R13 */
    14, /* R14 */
    15, /* R15 */
};

/* Bitmask for reserved registers */
#define VTX_REG_RESERVED_MASK ((1u << 4) | (1u << 5))  /* RSP=4, RBP=5 */

/* Bitmask for caller-saved registers */
#define VTX_CALLER_SAVED_MASK \
    ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 6) | (1u << 7) | \
     (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11))

/* Bitmask for callee-saved registers */
#define VTX_CALLEE_SAVED_MASK \
    ((1u << 3) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15))

/* No spill slot sentinel */
#define VTX_NO_SPILL ((uint32_t)0xFFFFFFFF)

/* ========================================================================== */
/* Live interval                                                               */
/* ========================================================================== */

typedef struct {
    uint32_t vreg;         /* virtual register */
    uint32_t start;        /* first instruction position (inclusive) */
    uint32_t end;          /* last instruction position (inclusive) */
    uint8_t  phys_reg;     /* assigned physical register (0xFF = unassigned) */
    uint32_t spill_slot;   /* assigned spill slot (VTX_NO_SPILL = none) */
    bool     is_fixed;     /* true if vreg is fixed to a physical register */
    uint8_t  fixed_reg;    /* the fixed physical register (if is_fixed) */
    bool     is_spilled;   /* true if this interval was spilled */
    uint32_t use_count;    /* number of uses of this vreg (for spill cost) */
    uint32_t loop_depth;   /* estimated loop nesting depth (for spill cost) */
    uint32_t coalesce_src; /* vreg this was coalesced from (VTX_VREG_INVALID = none) */
} vtx_live_interval_t;

/* ========================================================================== */
/* Register allocation result                                                  */
/* ========================================================================== */

typedef struct {
    /* Mapping: vreg → physical register (0xFF = not assigned / spilled) */
    uint8_t  *vreg_to_phys;        /* array indexed by vreg */
    uint32_t  vreg_to_phys_count;

    /* Mapping: vreg → spill slot index (VTX_NO_SPILL = not spilled) */
    uint32_t *vreg_to_spill;       /* array indexed by vreg */
    uint32_t  vreg_to_spill_count;

    /* Total number of spill slots used */
    uint32_t  spill_count;

    /* Bitmask of callee-saved registers that need save/restore */
    uint32_t  callee_saved_mask;

    /* Total frame size in bytes (locals + spills + alignment) */
    uint32_t  frame_size;

    /* Number of live intervals */
    uint32_t  interval_count;
    vtx_live_interval_t *intervals; /* array of live intervals (arena) */
} vtx_regalloc_result_t;

/* ========================================================================== */
/* Register allocator entry point                                              */
/* ========================================================================== */

/**
 * Run the linear scan register allocator on the instruction stream.
 *
 * Computes live intervals, assigns physical registers, and inserts
 * spill/fill code as needed.
 *
 * @param stream  The instruction stream (with virtual registers)
 * @param arena   Arena for allocations
 * @return        Register allocation result, or NULL on failure
 */
vtx_regalloc_result_t *vtx_regalloc_run(vtx_inst_stream_t *stream, vtx_arena_t *arena);

/**
 * Apply the register allocation result to the instruction stream.
 * Replaces virtual register references with physical register references.
 * Inserts spill/fill instructions where needed.
 *
 * @param stream  The instruction stream to modify
 * @param result  The register allocation result
 * @param arena   Arena for new instructions
 * @return        0 on success, -1 on failure
 */
int vtx_regalloc_apply(vtx_inst_stream_t *stream,
                        const vtx_regalloc_result_t *result,
                        vtx_arena_t *arena);

/**
 * Get the physical register assigned to a virtual register.
 * Returns 0xFF (VTX_REG_NONE) if not assigned.
 */
uint8_t vtx_regalloc_phys_reg(const vtx_regalloc_result_t *result, uint32_t vreg);

/**
 * Get the spill slot assigned to a virtual register.
 * Returns VTX_NO_SPILL if not spilled.
 */
uint32_t vtx_regalloc_spill_slot(const vtx_regalloc_result_t *result, uint32_t vreg);

/**
 * Find the NodeID that is currently in a given physical register at a
 * given instruction position.
 *
 * Uses the register allocator's live interval data to determine which
 * vreg (and thus which SoN node) occupies the physical register at
 * the specified instruction position.
 *
 * @param result   Register allocation result
 * @param stream   Instruction stream (for vreg → NodeID reverse mapping)
 * @param position Instruction position (sequential instruction index)
 * @param phys_reg Physical register number to look up
 * @return         NodeID occupying the register, or VTX_NODEID_INVALID
 */
vtx_nodeid_t vtx_regalloc_node_at_position(
    const vtx_regalloc_result_t *result,
    const vtx_inst_stream_t *stream,
    uint32_t position,
    uint8_t phys_reg);

/**
 * Get the set of physical registers that are live at a given instruction
 * position, along with their defining NodeIDs.
 *
 * @param result       Register allocation result
 * @param position     Instruction position (sequential instruction index)
 * @param out_regs     Output array of physical register numbers (caller-allocated)
 * @param out_nodeids  Output array of NodeIDs (caller-allocated, parallel to out_regs)
 * @param max_entries  Maximum number of entries the output arrays can hold
 * @return             Number of live register entries written
 */
uint32_t vtx_regalloc_live_regs_at_position(
    const vtx_regalloc_result_t *result,
    uint32_t position,
    uint8_t *out_regs,
    vtx_nodeid_t *out_nodeids,
    uint32_t max_entries);

#endif /* VORTEX_LOWER_REGALLOC_H */
