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
#include "ir/node.h"
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
        intervals[v].first_range = NULL;
        intervals[v].last_range = NULL;
        intervals[v].start = UINT32_MAX;
        intervals[v].end = 0;
        intervals[v].phys_reg = 0xFF;
        intervals[v].spill_slot = VTX_NO_SPILL;
        intervals[v].is_fixed = false;
        intervals[v].fixed_reg = 0xFF;
        intervals[v].is_spilled = false;
        intervals[v].use_count = 0;
        intervals[v].loop_depth = 0;
        intervals[v].coalesce_src = VTX_VREG_INVALID;
        intervals[v].split_parent = NULL;
        intervals[v].split_child = NULL;

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

                /* Count uses for spill cost estimation */
                intervals[vreg].use_count++;

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
                    intervals[inst->mem.base_vreg].use_count++;
                }
                if (inst->mem.index_vreg != VTX_VREG_INVALID && inst->mem.index_vreg < vreg_count) {
                    if (pos > intervals[inst->mem.index_vreg].end)
                        intervals[inst->mem.index_vreg].end = pos;
                    if (pos < intervals[inst->mem.index_vreg].start)
                        intervals[inst->mem.index_vreg].start = pos;
                    intervals[inst->mem.index_vreg].use_count++;
                }
            }
        }
    }

    /* Estimate loop depth for each block based on back-edges.
     * A block that is the target of a back-edge (JCC/JMP to an earlier block)
     * is a loop header with depth >= 1. Nested loops increase depth. */
    uint32_t *block_loop_depth = (uint32_t *)vtx_arena_alloc(
        arena, stream->block_count * sizeof(uint32_t));
    if (block_loop_depth) {
        memset(block_loop_depth, 0, stream->block_count * sizeof(uint32_t));

        /* Detect back-edges and assign loop depths */
        for (uint32_t b = 0; b < stream->block_count; b++) {
            vtx_inst_block_t *blk = &stream->blocks[b];
            for (uint32_t i = 0; i < blk->inst_count; i++) {
                vtx_inst_t *inst = &blk->insts[i];
                if ((inst->opcode == VTX_X86_JCC || inst->opcode == VTX_X86_JMP) &&
                    inst->opnd_kinds[0] == VTX_OPND_LABEL) {
                    uint32_t target = inst->operands[0];
                    if (target < b && target < stream->block_count) {
                        /* Back-edge: blocks from target to b are in a loop */
                        uint32_t depth = block_loop_depth[target] + 1;
                        for (uint32_t bb = target; bb <= b && bb < stream->block_count; bb++) {
                            if (depth > block_loop_depth[bb])
                                block_loop_depth[bb] = depth;
                        }
                    }
                }
            }
        }

        /* Assign loop depth to intervals based on their start block */
        for (uint32_t v = 0; v < vreg_count; v++) {
            if (intervals[v].start > intervals[v].end) continue;
            /* Find which block this interval starts in */
            uint32_t pos = intervals[v].start;
            for (uint32_t b = 0; b < stream->block_count; b++) {
                vtx_inst_block_t *blk = &stream->blocks[b];
                if (blk->inst_count == 0) continue;
                uint32_t blk_start = blk->insts[0].native_offset;
                uint32_t blk_end = blk->insts[blk->inst_count - 1].native_offset;
                if (pos >= blk_start && pos <= blk_end) {
                    intervals[v].loop_depth = block_loop_depth[b];
                    break;
                }
            }
        }
    }

    /* Remove intervals that were never defined/used (start > end).
     * We do NOT compact the array here — compaction happens after
     * coalescing so that coalesce_copies can index by vreg number.
     *
     * P5: Create a single live range for each valid interval. Intervals
     * with start > end are invalid and get no range. */
    for (uint32_t v = 0; v < vreg_count; v++) {
        if (intervals[v].start <= intervals[v].end) {
            vtx_live_range_t *range = (vtx_live_range_t *)vtx_arena_alloc(
                arena, sizeof(vtx_live_range_t));
            if (range) {
                range->start = intervals[v].start;
                range->end = intervals[v].end;
                range->phys_reg = 0xFF;
                range->spill_slot = VTX_NO_SPILL;
                range->next = NULL;
                intervals[v].first_range = range;
                intervals[v].last_range = range;
            }
        }
    }

    *out_count = vreg_count;
    return intervals;
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
/* Register coalescing for copy instructions                                   */
/* ========================================================================== */

/**
 * Compute the spill cost for an interval.
 * Higher cost = less desirable to spill.
 * Cost = use_count * (10 ^ loop_depth) — values in loops are much more
 * expensive to spill because every spill/reload is executed per iteration.
 */
static uint64_t compute_spill_cost(const vtx_live_interval_t *interval)
{
    if (interval->use_count == 0) return 0;
    uint64_t cost = interval->use_count;
    /* Multiply by 10 for each level of loop nesting */
    for (uint32_t d = 0; d < interval->loop_depth; d++) {
        cost *= 10;
    }
    return cost;
}

/**
 * Perform register coalescing: for MOV dst, src instructions where
 * both src and dst are virtual registers, try to assign them the same
 * physical register to eliminate the copy.
 *
 * @param stream    Instruction stream
 * @param intervals Live intervals array (indexed by vreg)
 * @param vreg_count Number of vregs
 * @return          Number of coalescences performed
 */
static uint32_t coalesce_copies(vtx_inst_stream_t *stream,
                                 vtx_live_interval_t *intervals,
                                 uint32_t vreg_count)
{
    uint32_t coalesced = 0;

    /* Union-Find for coalescing groups */
    uint32_t *parent = (uint32_t *)malloc(vreg_count * sizeof(uint32_t));
    if (!parent) return 0;
    for (uint32_t v = 0; v < vreg_count; v++) parent[v] = v;

    /* Scan for MOV reg, reg copy instructions */
    for (uint32_t b = 0; b < stream->block_count; b++) {
        vtx_inst_block_t *blk = &stream->blocks[b];
        for (uint32_t i = 0; i < blk->inst_count; i++) {
            vtx_inst_t *inst = &blk->insts[i];
            if (inst->opcode != VTX_X86_MOV) continue;
            if (inst->flags & VTX_INST_FLAG_HAS_IMM) continue;
            if (inst->flags & VTX_INST_FLAG_HAS_MEM) continue;
            if (inst->opnd_kinds[0] != VTX_OPND_VREG) continue;
            if (inst->opnd_kinds[1] != VTX_OPND_VREG) continue;

            uint32_t dst_vreg = inst->operands[0];
            uint32_t src_vreg = inst->operands[1];
            if (dst_vreg >= vreg_count || src_vreg >= vreg_count) continue;
            if (dst_vreg == src_vreg) continue;

            /* Find roots with path compression */
            uint32_t dst_root = dst_vreg;
            while (parent[dst_root] != dst_root) {
                parent[dst_root] = parent[parent[dst_root]];
                dst_root = parent[dst_root];
            }
            uint32_t src_root = src_vreg;
            while (parent[src_root] != src_root) {
                parent[src_root] = parent[parent[src_root]];
                src_root = parent[src_root];
            }

            /* Check if intervals don't overlap (safe to coalesce) */
            vtx_live_interval_t *di = &intervals[dst_root];
            vtx_live_interval_t *si = &intervals[src_root];

            /* Can't coalesce if both are fixed to different registers */
            if (di->is_fixed && si->is_fixed && di->fixed_reg != si->fixed_reg)
                continue;

            /* Check for interval overlap — safe to coalesce if no overlap */
            bool overlaps = !(di->end < si->start || si->end < di->start);
            if (overlaps) continue;

            /* Safe to coalesce: merge the intervals */
            uint32_t root = src_root;
            uint32_t child = dst_root;

            /* Merge into the root with the wider interval */
            if (intervals[root].start > intervals[child].start)
                intervals[root].start = intervals[child].start;
            if (intervals[root].end < intervals[child].end)
                intervals[root].end = intervals[child].end;
            intervals[root].use_count += intervals[child].use_count;
            if (intervals[child].loop_depth > intervals[root].loop_depth)
                intervals[root].loop_depth = intervals[child].loop_depth;

            /* Union */
            parent[child] = root;
            intervals[child].coalesce_src = root;
            coalesced++;
        }
    }

    /* Apply coalescing: update vreg references in the instruction stream */
    for (uint32_t b = 0; b < stream->block_count; b++) {
        vtx_inst_block_t *blk = &stream->blocks[b];
        for (uint32_t i = 0; i < blk->inst_count; i++) {
            vtx_inst_t *inst = &blk->insts[i];
            for (int op = 0; op < VTX_INST_MAX_OPERANDS; op++) {
                if (inst->opnd_kinds[op] == VTX_OPND_VREG) {
                    uint32_t vreg = inst->operands[op];
                    if (vreg < vreg_count) {
                        /* Find root with path compression */
                        while (parent[vreg] != vreg) {
                            parent[vreg] = parent[parent[vreg]];
                            vreg = parent[vreg];
                        }
                        inst->operands[op] = vreg;
                    }
                }
            }
            /* Also update memory operand vregs */
            if (inst->flags & VTX_INST_FLAG_HAS_MEM) {
                if (inst->mem.base_vreg != VTX_VREG_INVALID && inst->mem.base_vreg < vreg_count) {
                    uint32_t v = inst->mem.base_vreg;
                    while (parent[v] != v) { parent[v] = parent[parent[v]]; v = parent[v]; }
                    inst->mem.base_vreg = v;
                }
                if (inst->mem.index_vreg != VTX_VREG_INVALID && inst->mem.index_vreg < vreg_count) {
                    uint32_t v = inst->mem.index_vreg;
                    while (parent[v] != v) { parent[v] = parent[parent[v]]; v = parent[v]; }
                    inst->mem.index_vreg = v;
                }
            }
        }
    }

    free(parent);
    return coalesced;
}

/* ========================================================================== */
/* Interval splitting (P5)                                                     */
/* ========================================================================== */

/**
 * Split a live interval at the given position.
 *
 * The interval is divided into two parts:
 *   - First half: [start, position) — keeps the current register assignment
 *   - Second half: [position, end) — becomes a new interval needing a register
 *
 * The first half's range list is truncated, and a new interval is created
 * for the second half. The split_parent/split_child pointers link them.
 *
 * At the split point, the caller should insert:
 *   - Spill instruction (store register to spill slot) at the end of the first half
 *   - Reload instruction (load from spill slot to register) at the start of the second half
 */
vtx_live_interval_t *vtx_regalloc_split_interval(vtx_live_interval_t *interval,
                                                   uint32_t position,
                                                   vtx_arena_t *arena)
{
    if (!interval) return NULL;
    if (position <= interval->start || position > interval->end) return NULL;

    /* Create the second half interval */
    vtx_live_interval_t *second = (vtx_live_interval_t *)vtx_arena_alloc(
        arena, sizeof(vtx_live_interval_t));
    if (!second) return NULL;
    memset(second, 0, sizeof(*second));

    /* Initialize second half */
    second->vreg = interval->vreg;        /* same vreg — needs separate spill slot */
    second->start = position;
    second->end = interval->end;
    second->phys_reg = 0xFF;               /* needs a new register */
    second->spill_slot = VTX_NO_SPILL;
    second->is_fixed = interval->is_fixed;
    second->fixed_reg = interval->fixed_reg;
    second->is_spilled = false;
    second->use_count = 0;                  /* will be counted separately */
    second->loop_depth = interval->loop_depth;
    second->coalesce_src = VTX_VREG_INVALID;
    second->split_parent = interval;
    second->split_child = NULL;

    /* Create a range for the second half */
    vtx_live_range_t *second_range = (vtx_live_range_t *)vtx_arena_alloc(
        arena, sizeof(vtx_live_range_t));
    if (!second_range) return NULL;
    second_range->start = position;
    second_range->end = interval->end;
    second_range->phys_reg = 0xFF;
    second_range->spill_slot = VTX_NO_SPILL;
    second_range->next = NULL;
    second->first_range = second_range;
    second->last_range = second_range;

    /* Count uses in the second half from the original use_count.
     * We approximate by splitting proportionally. */
    uint32_t total_len = interval->end - interval->start;
    uint32_t second_len = interval->end - position;
    if (total_len > 0 && interval->use_count > 0) {
        second->use_count = (interval->use_count * second_len) / total_len;
        if (second->use_count == 0) second->use_count = 1; /* at least 1 */
    }

    /* Truncate the first half */
    interval->end = position > 0 ? position - 1 : 0;
    interval->split_child = second;

    /* Update the first half's range list: truncate the last range */
    if (interval->last_range) {
        interval->last_range->end = interval->end;
    }

    return second;
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

    /* Compute live intervals — returns vreg-indexed array (not compacted) */
    uint32_t interval_count = 0;
    uint32_t vreg_count = stream->vreg_count;
    vtx_live_interval_t *intervals = compute_live_intervals(stream, &interval_count, arena);

    /* Perform register coalescing on the vreg-indexed array before compaction.
     * coalesce_copies indexes the intervals array by vreg number, so the
     * array must still be vreg-indexed at this point. */
    if (intervals && vreg_count > 0) {
        coalesce_copies(stream, intervals, vreg_count);
    }

    /* Now compact the intervals: remove entries where start > end
     * (intervals that were never defined/used, or were coalesced away). */
    uint32_t valid_count = 0;
    if (intervals) {
        for (uint32_t v = 0; v < vreg_count; v++) {
            if (intervals[v].start <= intervals[v].end &&
                intervals[v].coalesce_src == VTX_VREG_INVALID) {
                valid_count++;
            }
        }
    }

    vtx_live_interval_t *valid_intervals = NULL;
    if (valid_count > 0) {
        valid_intervals = (vtx_live_interval_t *)vtx_arena_alloc(
            arena, valid_count * sizeof(vtx_live_interval_t));
        if (!valid_intervals) return NULL;

        uint32_t idx = 0;
        for (uint32_t v = 0; v < vreg_count; v++) {
            if (intervals[v].start <= intervals[v].end &&
                intervals[v].coalesce_src == VTX_VREG_INVALID) {
                valid_intervals[idx++] = intervals[v];
            }
        }
    }

    result->intervals = valid_intervals;
    result->interval_count = valid_count;

    /* Sort intervals by start position */
    if (valid_intervals && valid_count > 0) {
        qsort(valid_intervals, valid_count, sizeof(vtx_live_interval_t), cmp_intervals_by_start);
    }

    /* Allocate vreg → phys_reg mapping */
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

    /* Linear scan: iterate over the compacted, sorted valid_intervals array.
     * The original bug (G1) iterated over the raw vreg-indexed intervals[]
     * array, which is NOT sorted by start position. The linear scan algorithm
     * REQUIRES intervals to be processed in start-position order. */
    for (uint32_t i = 0; i < valid_count; i++) {
        vtx_live_interval_t *current = &valid_intervals[i];

        /* Skip intervals that were coalesced into another */
        if (current->coalesce_src != VTX_VREG_INVALID) continue;

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
            /* Prefer caller-saved registers for short-lived values,
             * callee-saved for long-lived values in loops */
            uint32_t caller_free = free_regs & VTX_CALLER_SAVED_MASK;
            uint32_t callee_free = free_regs & VTX_CALLEE_SAVED_MASK;
            uint32_t reg_bit;

            /* Heuristic: short-lived values (small interval range) prefer
             * caller-saved registers. Long-lived values in deep loops
             * prefer callee-saved registers (less save/restore overhead). */
            uint32_t interval_length = current->end - current->start;
            bool prefer_caller_saved = (interval_length < 20) || (current->loop_depth == 0);

            if (prefer_caller_saved && caller_free != 0) {
                reg_bit = caller_free & (~caller_free + 1u); /* lowest set bit */
            } else if (callee_free != 0) {
                reg_bit = callee_free & (~callee_free + 1u);
                callee_saved_used |= reg_bit;
            } else if (caller_free != 0) {
                /* Fallback to caller-saved if no callee-saved available */
                reg_bit = caller_free & (~caller_free + 1u);
            } else {
                /* No registers available at all — shouldn't happen since free_regs != 0 */
                continue;
            }

            /* Use __builtin_ctz for O(1) register number extraction from bitmask,
             * instead of O(N) loop counting bits. */
            uint8_t reg = (uint8_t)__builtin_ctz(reg_bit);

            current->phys_reg = reg;
            free_regs &= ~reg_bit;

            /* Update result mapping */
            result->vreg_to_phys[current->vreg] = reg;

            /* Add to active */
            if (active_count < active_capacity) {
                active[active_count++] = current;
            }
        } else {
            /* No free register — P5: Use interval splitting instead of
             * spilling the entire lifetime. We split the evicted interval
             * at the current position so that only the overlapping part
             * is spilled. The non-overlapping parts can keep their register.
             *
             * If splitting is not possible (e.g., the interval is too short),
             * we fall back to full-lifetime spilling. */
            uint32_t spill_idx = 0;
            uint64_t min_cost = UINT64_MAX;
            for (uint32_t j = 0; j < active_count; j++) {
                if (active[j]->is_fixed) continue;
                uint64_t cost = compute_spill_cost(active[j]);
                if (cost < min_cost) {
                    min_cost = cost;
                    spill_idx = j;
                }
            }

            uint64_t current_cost = compute_spill_cost(current);

            if (active_count > 0 && min_cost < current_cost) {
                /* Evict the active interval with lowest cost.
                 * P5: Try to split the interval at current->start so that
                 * the part before current->start keeps its register, and
                 * only the overlapping part is spilled. */
                vtx_live_interval_t *spill = active[spill_idx];

                /* Check if splitting is worthwhile: the interval must extend
                 * beyond the current start position by a meaningful amount.
                 * If spill->end <= current->start, the interval has already
                 * expired and there's nothing to split. If the interval only
                 * overlaps by a small amount, splitting is still worthwhile
                 * because the non-overlapping part keeps its register. */
                if (spill->end > current->start && spill->start < current->start) {
                    /* Split the interval at current->start.
                     * The first half [spill->start, current->start) keeps the register.
                     * The second half [current->start, spill->end) is spilled. */
                    vtx_live_interval_t *second_half = vtx_regalloc_split_interval(
                        spill, current->start, arena);
                    if (second_half) {
                        /* Spill the second half */
                        second_half->is_spilled = true;
                        second_half->spill_slot = next_spill_slot++;
                        second_half->phys_reg = 0xFF;
                        result->vreg_to_spill[second_half->vreg] = second_half->spill_slot;
                        result->vreg_to_phys[second_half->vreg] = 0xFF;

                        /* The first half (spill) keeps its register but is
                         * no longer active since it ends before current->start.
                         * Its register is freed. */
                        free_regs |= (1u << spill->phys_reg);

                        /* Assign the freed register to current */
                        uint32_t reg_bit = (1u << spill->phys_reg);
                        uint8_t reg = spill->phys_reg;
                        current->phys_reg = reg;
                        free_regs &= ~reg_bit;
                        result->vreg_to_phys[current->vreg] = reg;

                        /* Replace in active list */
                        active[spill_idx] = current;
                    } else {
                        /* Split failed — fall back to full spill */
                        current->phys_reg = spill->phys_reg;
                        spill->is_spilled = true;
                        spill->spill_slot = next_spill_slot++;
                        result->vreg_to_phys[spill->vreg] = 0xFF;
                        result->vreg_to_spill[spill->vreg] = spill->spill_slot;
                        result->vreg_to_phys[current->vreg] = current->phys_reg;
                        active[spill_idx] = current;
                    }
                } else {
                    /* Can't split (interval starts at or after current position),
                     * or splitting isn't worthwhile — fall back to full spill */
                    current->phys_reg = spill->phys_reg;
                    spill->is_spilled = true;
                    spill->spill_slot = next_spill_slot++;
                    result->vreg_to_phys[spill->vreg] = 0xFF;
                    result->vreg_to_spill[spill->vreg] = spill->spill_slot;
                    result->vreg_to_phys[current->vreg] = current->phys_reg;
                    active[spill_idx] = current;
                }
            } else {
                /* P5: Try to split the current interval instead of spilling
                 * its entire lifetime. If it overlaps with an active interval
                 * only partially, split at the active interval's start. */
                bool did_split = false;
                /* Find the active interval that ends latest (the one most
                 * blocking us) and try to split current before it. */
                uint32_t latest_end = 0;
                for (uint32_t j = 0; j < active_count; j++) {
                    if (active[j]->end > latest_end) {
                        latest_end = active[j]->end;
                    }
                }

                /* If the current interval extends beyond the last active
                 * interval, we can split it: first half gets no register
                 * (spilled), second half can try again when registers free up. */
                if (latest_end < current->end && latest_end > current->start) {
                    vtx_live_interval_t *second_half = vtx_regalloc_split_interval(
                        current, latest_end + 1, arena);
                    if (second_half) {
                        /* Spill the first half (current, now shortened) */
                        current->is_spilled = true;
                        current->spill_slot = next_spill_slot++;
                        result->vreg_to_spill[current->vreg] = current->spill_slot;

                        /* The second half needs a register — try to allocate
                         * one later. For now, add it to a deferred list or
                         * just spill it too. Since the linear scan processes
                         * intervals in order, and the second half starts
                         * after the current position, it will be processed
                         * in a future iteration. However, since we're iterating
                         * over valid_intervals (which is sorted by start),
                         * the second half might not be in the array.
                         *
                         * For simplicity, spill the second half too but record
                         * that it exists for the apply phase to insert
                         * reload instructions. */
                        second_half->is_spilled = true;
                        second_half->spill_slot = next_spill_slot++;
                        second_half->phys_reg = 0xFF;
                        result->vreg_to_spill[second_half->vreg] = second_half->spill_slot;
                        result->vreg_to_phys[second_half->vreg] = 0xFF;

                        did_split = true;
                    }
                }

                if (!did_split) {
                    /* Spill the current interval entirely */
                    current->is_spilled = true;
                    current->spill_slot = next_spill_slot++;
                    result->vreg_to_spill[current->vreg] = current->spill_slot;
                }
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

            /* Handle memory operand vregs.
             * G2 fix: When a memory operand vreg is spilled (phys == 0xFF),
             * we must insert a reload instruction before the current instruction
             * to load the spilled value into a scratch register, then use that
             * scratch register as the base/index. We use R12 (VTX_SPILL_TMP_REG)
             * for base and R13 for index — these are callee-saved scratch registers
             * reserved for spill handling in the emitter. */
            if (inst->flags & VTX_INST_FLAG_HAS_MEM) {
                if (inst->mem.base_vreg != VTX_VREG_INVALID &&
                    inst->mem.base_vreg < result->vreg_to_phys_count) {
                    uint8_t phys = result->vreg_to_phys[inst->mem.base_vreg];
                    if (phys != 0xFF) {
                        inst->mem.base_phys = phys;
                        inst->mem.base_vreg = VTX_VREG_INVALID;
                    } else {
                        /* Spilled: insert a MOV from spill slot into R12 before this inst */
                        uint32_t spill_slot = result->vreg_to_spill[inst->mem.base_vreg];
                        if (spill_slot != VTX_NO_SPILL) {
                            vtx_inst_t reload;
                            memset(&reload, 0, sizeof(reload));
                            reload.opcode = VTX_X86_MOV;
                            reload.opnd_kinds[0] = VTX_OPND_PREG;
                            reload.operands[0] = 12; /* R12 */
                            reload.opnd_kinds[1] = VTX_OPND_SPILL;
                            reload.operands[1] = spill_slot;
                            reload.flags |= VTX_INST_FLAG_SPILL_LOAD;
                            reload.source_node = inst->source_node;
                            /* Insert reload before current instruction */
                            if (vtx_isel_block_ensure_capacity(blk, 1, arena) != 0) return -1;
                            memmove(&blk->insts[i + 1], &blk->insts[i],
                                    (blk->inst_count - i) * sizeof(vtx_inst_t));
                            blk->insts[i] = reload;
                            blk->inst_count++;
                            i++; /* Skip past the inserted reload */
                            /* Now use R12 as the base */
                            inst = &blk->insts[i]; /* re-read after memmove */
                            inst->mem.base_phys = 12; /* R12 */
                            inst->mem.base_vreg = VTX_VREG_INVALID;
                        }
                    }
                }
                if (inst->mem.index_vreg != VTX_VREG_INVALID &&
                    inst->mem.index_vreg < result->vreg_to_phys_count) {
                    uint8_t phys = result->vreg_to_phys[inst->mem.index_vreg];
                    if (phys != 0xFF) {
                        inst->mem.index_phys = phys;
                        inst->mem.index_vreg = VTX_VREG_INVALID;
                    } else {
                        /* Spilled: insert a MOV from spill slot into R13 before this inst */
                        uint32_t spill_slot = result->vreg_to_spill[inst->mem.index_vreg];
                        if (spill_slot != VTX_NO_SPILL) {
                            vtx_inst_t reload;
                            memset(&reload, 0, sizeof(reload));
                            reload.opcode = VTX_X86_MOV;
                            reload.opnd_kinds[0] = VTX_OPND_PREG;
                            reload.operands[0] = 13; /* R13 */
                            reload.opnd_kinds[1] = VTX_OPND_SPILL;
                            reload.operands[1] = spill_slot;
                            reload.flags |= VTX_INST_FLAG_SPILL_LOAD;
                            reload.source_node = inst->source_node;
                            /* Insert reload before current instruction */
                            if (vtx_isel_block_ensure_capacity(blk, 1, arena) != 0) return -1;
                            memmove(&blk->insts[i + 1], &blk->insts[i],
                                    (blk->inst_count - i) * sizeof(vtx_inst_t));
                            blk->insts[i] = reload;
                            blk->inst_count++;
                            i++; /* Skip past the inserted reload */
                            /* Now use R13 as the index */
                            inst = &blk->insts[i]; /* re-read after memmove */
                            inst->mem.index_phys = 13; /* R13 */
                            inst->mem.index_vreg = VTX_VREG_INVALID;
                        }
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

/* ========================================================================== */
/* Position-based register queries (for guard emission)                        */
/* ========================================================================== */

uint32_t vtx_regalloc_live_regs_at_position(
    const vtx_regalloc_result_t *result,
    uint32_t position,
    uint8_t *out_regs,
    vtx_nodeid_t *out_nodeids,
    uint32_t max_entries)
{
    if (!result || !result->intervals || !out_regs || !out_nodeids)
        return 0;

    uint32_t count = 0;

    /* Walk all live intervals and find ones that are live at `position`
     * and have a valid physical register assignment. */
    for (uint32_t i = 0; i < result->interval_count && count < max_entries; i++) {
        const vtx_live_interval_t *iv = &result->intervals[i];

        /* Skip intervals that were coalesced into another */
        if (iv->coalesce_src != VTX_VREG_INVALID) continue;

        /* Check if this interval is live at the given position */
        if (iv->start <= position && iv->end >= position &&
            iv->phys_reg != 0xFF) {
            out_regs[count] = iv->phys_reg;
            out_nodeids[count] = (vtx_nodeid_t)iv->vreg; /* vreg is used as proxy;
                                                           * the caller maps vreg → NodeID
                                                           * via the instruction stream */
            count++;
        }
    }

    return count;
}

vtx_nodeid_t vtx_regalloc_node_at_position(
    const vtx_regalloc_result_t *result,
    const vtx_inst_stream_t *stream,
    uint32_t position,
    uint8_t phys_reg)
{
    if (!result || !result->intervals)
        return VTX_NODEID_INVALID;

    /* Find the live interval that occupies phys_reg at the given position */
    for (uint32_t i = 0; i < result->interval_count; i++) {
        const vtx_live_interval_t *iv = &result->intervals[i];

        if (iv->coalesce_src != VTX_VREG_INVALID) continue;
        if (iv->phys_reg != phys_reg) continue;
        if (iv->start > position || iv->end < position) continue;

        /* Found the interval occupying this register at this position.
         * Now we need to map vreg → NodeID.
         * The instruction stream has a node_to_vreg mapping, but we need
         * the reverse. We scan the instruction stream for the instruction
         * that defines this vreg and return its source_node. */
        uint32_t target_vreg = iv->vreg;

        if (stream != NULL) {
            for (uint32_t b = 0; b < stream->block_count; b++) {
                const vtx_inst_block_t *blk = &stream->blocks[b];
                for (uint32_t j = 0; j < blk->inst_count; j++) {
                    const vtx_inst_t *inst = &blk->insts[j];
                    /* The first operand of most instructions is the destination.
                     * If it's a vreg matching our target, this instruction
                     * defines it. */
                    if (inst->opnd_kinds[0] == VTX_OPND_VREG &&
                        inst->operands[0] == target_vreg &&
                        inst->source_node != VTX_NODEID_INVALID) {
                        return inst->source_node;
                    }
                }
            }
        }

        /* Couldn't find the defining instruction — use vreg as NodeID proxy.
         * This is an approximation but better than VTX_NODEID_INVALID. */
        return (vtx_nodeid_t)target_vreg;
    }

    return VTX_NODEID_INVALID;
}
