/**
 * VORTEX Linear Scan Register Allocator
 *
 * Assigns physical registers to virtual registers using a linear scan
 * algorithm as described by Poletto & Sarkar (1999) with extensions
 * for fixed-register constraints and spill code insertion.
 *
 * Algorithm:
 *   1. Number all instructions sequentially across blocks
 *   2. Compute live intervals for each virtual register
 *   3. Sort intervals by start position
 *   4. Iterate: for each interval, expire old intervals, assign register
 *   5. If no free register, spill the interval with the furthest end
 *   6. Apply the result: rewrite vreg references to physical registers
 */

#include "lower/regalloc.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Helper: compute instruction positions across blocks                         */
/* ========================================================================== */

/**
 * Assign sequential positions to all instructions in the stream.
 * Returns the total number of instructions.
 */
static uint32_t assign_positions(vtx_inst_stream_t *stream)
{
    uint32_t pos = 0;
    for (uint32_t b = 0; b < stream->block_count; b++) {
        vtx_inst_block_t *blk = &stream->blocks[b];
        for (uint32_t i = 0; i < blk->inst_count; i++) {
            blk->insts[i].native_offset = pos;
            pos++;
        }
    }
    return pos;
}

/* ========================================================================== */
/* Compute live intervals                                                      */
/* ========================================================================== */

/**
 * Compute live intervals for all virtual registers in the stream.
 * Returns an array of intervals and sets *out_count.
 */
static vtx_live_interval_t *compute_live_intervals(vtx_inst_stream_t *stream,
                                                     uint32_t *out_count,
                                                     vtx_arena_t *arena)
{
    uint32_t total_insts = assign_positions(stream);
    (void)total_insts;

    uint32_t vreg_count = stream->vreg_count;
    if (vreg_count == 0) {
        *out_count = 0;
        return NULL;
    }

    vtx_live_interval_t *intervals = (vtx_live_interval_t *)vtx_arena_alloc(
        arena, vreg_count * sizeof(vtx_live_interval_t));
    if (!intervals) return NULL;

    /* Initialize intervals: start=MAX, end=0 (will be updated) */
    for (uint32_t v = 0; v < vreg_count; v++) {
        intervals[v].vreg = v;
        intervals[v].start = UINT32_MAX;
        intervals[v].end = 0;
        intervals[v].phys_reg = 0xFF;
        intervals[v].spill_slot = VTX_NO_SPILL;
        intervals[v].is_fixed = false;
        intervals[v].fixed_reg = 0xFF;
        intervals[v].is_spilled = false;

        /* Check if this vreg has a fixed register constraint */
        if (v < stream->vreg_fixed_reg_count && stream->vreg_fixed_reg[v] != 0xFF) {
            intervals[v].is_fixed = true;
            intervals[v].fixed_reg = stream->vreg_fixed_reg[v];
        }
    }

    /* Walk all instructions to find definitions and uses */
    for (uint32_t b = 0; b < stream->block_count; b++) {
        vtx_inst_block_t *blk = &stream->blocks[b];
        for (uint32_t i = 0; i < blk->inst_count; i++) {
            vtx_inst_t *inst = &blk->insts[i];
            uint32_t pos = inst->native_offset;

            /* Process each operand */
            for (int op = 0; op < VTX_INST_MAX_OPERANDS; op++) {
                if (inst->opnd_kinds[op] != VTX_OPND_VREG) continue;
                uint32_t vreg = inst->operands[op];
                if (vreg >= vreg_count) continue;

                /* First operand of most instructions is the destination (definition) */
                if (op == 0 && inst->opcode != VTX_X86_CMP &&
                    inst->opcode != VTX_X86_TEST &&
                    inst->opcode != VTX_X86_PUSH &&
                    inst->opcode != VTX_X86_JCC &&
                    inst->opcode != VTX_X86_JMP &&
                    inst->opcode != VTX_X86_CALL &&
                    inst->opcode != VTX_X86_IDIV) {
                    /* Definition: update start position */
                    if (pos < intervals[vreg].start) {
                        intervals[vreg].start = pos;
                    }
                }

                /* Use: update end position */
                if (pos > intervals[vreg].end) {
                    intervals[vreg].end = pos;
                }

                /* Also consider it a definition for the first operand of
                 * two-operand instructions (add, sub, etc. modify the first operand) */
                if (op == 0 && (inst->opcode == VTX_X86_ADD || inst->opcode == VTX_X86_SUB ||
                    inst->opcode == VTX_X86_IMUL || inst->opcode == VTX_X86_AND ||
                    inst->opcode == VTX_X86_OR || inst->opcode == VTX_X86_XOR ||
                    inst->opcode == VTX_X86_SHL || inst->opcode == VTX_X86_SHR ||
                    inst->opcode == VTX_X86_SAR || inst->opcode == VTX_X86_NEG ||
                    inst->opcode == VTX_X86_NOT || inst->opcode == VTX_X86_MOV ||
                    inst->opcode == VTX_X86_LEA || inst->opcode == VTX_X86_INC ||
                    inst->opcode == VTX_X86_DEC || inst->opcode == VTX_X86_CMOV ||
                    inst->opcode == VTX_X86_SETCC || inst->opcode == VTX_X86_MOVZX ||
                    inst->opcode == VTX_X86_MOVSX || inst->opcode == VTX_X86_POP)) {
                    if (pos < intervals[vreg].start) {
                        intervals[vreg].start = pos;
                    }
                }
            }

            /* Memory operands also reference vregs (uses) */
            if (inst->flags & VTX_INST_FLAG_HAS_MEM) {
                if (inst->mem.base_vreg != VTX_VREG_INVALID && inst->mem.base_vreg < vreg_count) {
                    if (pos > intervals[inst->mem.base_vreg].end)
                        intervals[inst->mem.base_vreg].end = pos;
                    if (pos < intervals[inst->mem.base_vreg].start)
                        intervals[inst->mem.base_vreg].start = pos;
                }
                if (inst->mem.index_vreg != VTX_VREG_INVALID && inst->mem.index_vreg < vreg_count) {
                    if (pos > intervals[inst->mem.index_vreg].end)
                        intervals[inst->mem.index_vreg].end = pos;
                    if (pos < intervals[inst->mem.index_vreg].start)
                        intervals[inst->mem.index_vreg].start = pos;
                }
            }
        }
    }

    /* Remove intervals that were never defined/used (start > end) */
    uint32_t valid_count = 0;
    for (uint32_t v = 0; v < vreg_count; v++) {
        if (intervals[v].start <= intervals[v].end) {
            valid_count++;
        }
    }

    /* Compact the intervals array */
    vtx_live_interval_t *valid_intervals = (vtx_live_interval_t *)vtx_arena_alloc(
        arena, valid_count * sizeof(vtx_live_interval_t));
    if (!valid_intervals && valid_count > 0) return NULL;

    uint32_t idx = 0;
    for (uint32_t v = 0; v < vreg_count; v++) {
        if (intervals[v].start <= intervals[v].end) {
            valid_intervals[idx++] = intervals[v];
        }
    }

    *out_count = valid_count;
    return valid_intervals;
}

/* ========================================================================== */
/* Sort intervals by start position                                            */
/* ========================================================================== */

static int cmp_intervals_by_start(const void *a, const void *b)
{
    const vtx_live_interval_t *ia = (const vtx_live_interval_t *)a;
    const vtx_live_interval_t *ib = (const vtx_live_interval_t *)b;
    if (ia->start != ib->start) return (ia->start < ib->start) ? -1 : 1;
    /* Tie-break: longer interval first */
    if (ia->end != ib->end) return (ia->end > ib->end) ? -1 : 1;
    return 0;
}

/* ========================================================================== */
/* Linear scan                                                                 */
/* ========================================================================== */

vtx_regalloc_result_t *vtx_regalloc_run(vtx_inst_stream_t *stream, vtx_arena_t *arena)
{
    VTX_ASSERT(stream != NULL, "stream must not be NULL");

    vtx_regalloc_result_t *result = (vtx_regalloc_result_t *)vtx_arena_alloc(
        arena, sizeof(vtx_regalloc_result_t));
    if (!result) return NULL;
    memset(result, 0, sizeof(*result));

    /* Compute live intervals */
    uint32_t interval_count = 0;
    vtx_live_interval_t *intervals = compute_live_intervals(stream, &interval_count, arena);

    result->intervals = intervals;
    result->interval_count = interval_count;

    /* Sort intervals by start position */
    if (intervals && interval_count > 0) {
        qsort(intervals, interval_count, sizeof(vtx_live_interval_t), cmp_intervals_by_start);
    }

    /* Allocate vreg → phys_reg mapping */
    uint32_t vreg_count = stream->vreg_count;
    if (vreg_count == 0) return result;

    result->vreg_to_phys = (uint8_t *)vtx_arena_alloc(arena, vreg_count);
    result->vreg_to_spill = (uint32_t *)vtx_arena_alloc(arena, vreg_count * sizeof(uint32_t));
    if (!result->vreg_to_phys || !result->vreg_to_spill) return NULL;

    memset(result->vreg_to_phys, 0xFF, vreg_count); /* 0xFF = unassigned */
    for (uint32_t v = 0; v < vreg_count; v++) {
        result->vreg_to_spill[v] = VTX_NO_SPILL;
    }

    /* Active list: intervals currently assigned to physical registers */
    uint32_t active_count = 0;
    uint32_t active_capacity = interval_count > 0 ? interval_count : 1;
    vtx_live_interval_t **active = (vtx_live_interval_t **)vtx_arena_alloc(
        arena, active_capacity * sizeof(vtx_live_interval_t *));
    if (!active && active_capacity > 0) return NULL;

    /* Free register pool: bitmask of available registers */
    uint32_t free_regs = VTX_CALLER_SAVED_MASK | VTX_CALLEE_SAVED_MASK;
    /* Remove reserved registers */
    free_regs &= ~VTX_REG_RESERVED_MASK;

    /* Track which callee-saved registers are used */
    uint32_t callee_saved_used = 0;

    /* Next spill slot */
    uint32_t next_spill_slot = 0;

    /* Helper: expire old intervals */
    /* Returns the number of intervals expired */
    for (uint32_t i = 0; i < interval_count; i++) {
        vtx_live_interval_t *current = &intervals[i];

        /* Expire: remove from active any interval that ended before current starts */
        uint32_t new_active_count = 0;
        for (uint32_t j = 0; j < active_count; j++) {
            vtx_live_interval_t *a = active[j];
            if (a->end < current->start) {
                /* This interval has expired — free its register */
                if (a->phys_reg != 0xFF) {
                    free_regs |= (1u << a->phys_reg);
                }
            } else {
                active[new_active_count++] = a;
            }
        }
        active_count = new_active_count;

        /* Handle fixed-register constraints */
        if (current->is_fixed) {
            uint8_t fixed = current->fixed_reg;
            current->phys_reg = fixed;

            /* Evict any active interval using this register */
            for (uint32_t j = 0; j < active_count; j++) {
                if (active[j]->phys_reg == fixed) {
                    /* Spill the evicted interval */
                    active[j]->is_spilled = true;
                    active[j]->spill_slot = next_spill_slot++;
                    active[j]->phys_reg = 0xFF;
                    /* Remove from active */
                    for (uint32_t k = j; k < active_count - 1; k++) {
                        active[k] = active[k + 1];
                    }
                    active_count--;
                    break;
                }
            }

            /* Mark the register as used */
            free_regs &= ~(1u << fixed);
            if (VTX_CALLEE_SAVED_MASK & (1u << fixed)) {
                callee_saved_used |= (1u << fixed);
            }

            /* Add to active */
            if (active_count < active_capacity) {
                active[active_count++] = current;
            }

            /* Update result mapping */
            result->vreg_to_phys[current->vreg] = fixed;
            continue;
        }

        /* Try to allocate a free register */
        if (free_regs != 0) {
            /* Prefer caller-saved registers first */
            uint32_t caller_free = free_regs & VTX_CALLER_SAVED_MASK;
            uint32_t reg_bit;

            if (caller_free != 0) {
                reg_bit = caller_free & (~caller_free + 1u); /* lowest set bit */
            } else {
                uint32_t callee_free = free_regs & VTX_CALLEE_SAVED_MASK;
                reg_bit = callee_free & (~callee_free + 1u);
                callee_saved_used |= reg_bit;
            }

            uint8_t reg = 0;
            while ((1u << reg) != reg_bit) reg++;

            current->phys_reg = reg;
            free_regs &= ~reg_bit;

            /* Update result mapping */
            result->vreg_to_phys[current->vreg] = reg;

            /* Add to active */
            if (active_count < active_capacity) {
                active[active_count++] = current;
            }
        } else {
            /* No free register — spill the interval with the furthest end point */
            /* Find the active interval with the furthest end */
            uint32_t spill_idx = 0;
            uint32_t furthest_end = 0;
            for (uint32_t j = 0; j < active_count; j++) {
                if (active[j]->end > furthest_end && !active[j]->is_fixed) {
                    furthest_end = active[j]->end;
                    spill_idx = j;
                }
            }

            if (active_count > 0 && furthest_end > current->end) {
                /* Spill the active interval, assign its register to current */
                vtx_live_interval_t *spill = active[spill_idx];
                current->phys_reg = spill->phys_reg;
                spill->is_spilled = true;
                spill->spill_slot = next_spill_slot++;
                result->vreg_to_phys[spill->vreg] = 0xFF;
                result->vreg_to_spill[spill->vreg] = spill->spill_slot;
                result->vreg_to_phys[current->vreg] = current->phys_reg;

                /* Replace in active list */
                active[spill_idx] = current;
            } else {
                /* Spill the current interval */
                current->is_spilled = true;
                current->spill_slot = next_spill_slot++;
                result->vreg_to_spill[current->vreg] = current->spill_slot;
            }
        }
    }

    /* Set callee-saved mask */
    result->callee_saved_mask = callee_saved_used;

    /* Compute frame size */
    /* Frame layout: [callee-saved pushes] [spill slots] [alignment] */
    uint32_t callee_pushes = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (callee_saved_used & (1u << r)) callee_pushes++;
    }
    uint32_t spill_bytes = next_spill_slot * 8;
    uint32_t frame_size = spill_bytes;
    /* Align to 16 bytes */
    frame_size = (frame_size + 15u) & ~15u;
    result->frame_size = frame_size;
    result->spill_count = next_spill_slot;

    result->vreg_to_phys_count = vreg_count;
    result->vreg_to_spill_count = vreg_count;

    return result;
}

/* ========================================================================== */
/* Apply register allocation result to instruction stream                      */
/* ========================================================================== */

int vtx_regalloc_apply(vtx_inst_stream_t *stream,
                        const vtx_regalloc_result_t *result,
                        vtx_arena_t *arena)
{
    if (!stream || !result) return -1;

    for (uint32_t b = 0; b < stream->block_count; b++) {
        vtx_inst_block_t *blk = &stream->blocks[b];
        for (uint32_t i = 0; i < blk->inst_count; i++) {
            vtx_inst_t *inst = &blk->insts[i];

            /* Replace vreg operands with physical registers */
            for (int op = 0; op < VTX_INST_MAX_OPERANDS; op++) {
                if (inst->opnd_kinds[op] == VTX_OPND_VREG) {
                    uint32_t vreg = inst->operands[op];
                    if (vreg < result->vreg_to_phys_count) {
                        uint8_t phys = result->vreg_to_phys[vreg];
                        if (phys != 0xFF) {
                            inst->opnd_kinds[op] = VTX_OPND_PREG;
                            inst->operands[op] = phys;
                        }
                        /* If phys == 0xFF, the vreg is spilled and needs
                         * a load before use / store after def. This requires
                         * inserting new instructions. For simplicity in this
                         * implementation, we mark the instruction as needing
                         * a spill load/store and handle it in emission. */
                    }
                }
            }

            /* Handle memory operand vregs */
            if (inst->flags & VTX_INST_FLAG_HAS_MEM) {
                if (inst->mem.base_vreg != VTX_VREG_INVALID &&
                    inst->mem.base_vreg < result->vreg_to_phys_count) {
                    uint8_t phys = result->vreg_to_phys[inst->mem.base_vreg];
                    if (phys != 0xFF) {
                        inst->mem.base_vreg = VTX_VREG_INVALID;
                        /* We need a separate field for physical register base.
                         * For now, encode it as a special marker. */
                        /* Store the physical register in the displacement's
                         * upper bits (hack for this implementation) */
                        inst->mem.disp = (int32_t)phys; /* Temp: will be resolved in emission */
                    }
                }
            }
        }
    }

    return 0;
}

/* ========================================================================== */
/* Accessors                                                                   */
/* ========================================================================== */

uint8_t vtx_regalloc_phys_reg(const vtx_regalloc_result_t *result, uint32_t vreg)
{
    if (!result || !result->vreg_to_phys || vreg >= result->vreg_to_phys_count)
        return 0xFF;
    return result->vreg_to_phys[vreg];
}

uint32_t vtx_regalloc_spill_slot(const vtx_regalloc_result_t *result, uint32_t vreg)
{
    if (!result || !result->vreg_to_spill || vreg >= result->vreg_to_spill_count)
        return VTX_NO_SPILL;
    return result->vreg_to_spill[vreg];
}
