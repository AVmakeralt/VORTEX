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

#include "deopt/side_table.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Lifecycle                                                                  */
/* ========================================================================== */

vtx_side_table_t *vtx_side_table_build(vtx_arena_t *arena)
{
    (void)arena;  /* struct is always malloc'd so it outlives the arena */
    vtx_side_table_t *table = calloc(1, sizeof(vtx_side_table_t));
    if (!table) return NULL;

    memset(table, 0, sizeof(*table));

    table->entry_capacity = VTX_SIDE_TABLE_INITIAL_CAPACITY;
    table->entries = calloc(table->entry_capacity,
                             sizeof(vtx_side_table_entry_t));
    if (!table->entries) {
        free(table);
        return NULL;
    }
    table->entry_count = 0;

    table->frame_state_capacity = VTX_SIDE_TABLE_INITIAL_CAPACITY;
    table->frame_states = calloc(table->frame_state_capacity,
                                  sizeof(vtx_frame_state_t *));
    if (!table->frame_states) {
        free(table->entries);
        free(table);
        return NULL;
    }
    table->frame_state_count = 0;

    return table;
}

void vtx_side_table_destroy(vtx_side_table_t *table)
{
    if (!table) return;

    /* Free register maps (each is separately allocated) */
    for (uint32_t i = 0; i < table->entry_count; i++) {
        free(table->entries[i].register_map);
    }
    free(table->entries);
    free(table->frame_states);

    /* The table struct is always malloc'd (see vtx_side_table_build),
     * so we free it here. */
    free(table);
}

/* ========================================================================== */
/* Entry management                                                           */
/* ========================================================================== */

uint32_t vtx_side_table_add_entry(vtx_side_table_t *table,
                                   uint32_t native_pc_offset,
                                   uint32_t frame_state_index,
                                   uint32_t flags,
                                   uint32_t bytecode_pc)
{
    if (!table) return UINT32_MAX;

    /* DEOPT-007 fix: VTX_ASSERT is a no-op in release builds, so the
     * ordering invariant was unenforced. Replace with a real check that
     * rejects out-of-order entries (returns UINT32_MAX). Out-of-order
     * entries would break the binary search in vtx_side_table_lookup. */
    if (table->entry_count > 0) {
        uint32_t last_pc = table->entries[table->entry_count - 1].native_pc_offset;
        if (native_pc_offset < last_pc) {
            /* Out of order — refuse to add. This indicates a bug in the
             * codegen (emitting side-table entries out of PC order). */
            VTX_ASSERT(native_pc_offset >= last_pc,
                       "side table entries must be added in increasing PC order");
            return UINT32_MAX;
        }
    }

    /* Grow if needed */
    if (table->entry_count >= table->entry_capacity) {
        uint32_t new_cap = table->entry_capacity * 2;
        vtx_side_table_entry_t *new_entries = realloc(
            table->entries, (size_t)new_cap * sizeof(vtx_side_table_entry_t));
        if (!new_entries) return UINT32_MAX;
        memset(new_entries + table->entry_capacity, 0,
               (size_t)(new_cap - table->entry_capacity) * sizeof(vtx_side_table_entry_t));
        table->entries = new_entries;
        table->entry_capacity = new_cap;
    }

    uint32_t idx = table->entry_count++;
    vtx_side_table_entry_t *entry = &table->entries[idx];
    entry->native_pc_offset = native_pc_offset;
    entry->frame_state_index = frame_state_index;
    entry->register_map = NULL;
    entry->register_map_count = 0;
    entry->flags = flags;
    /* OSR-5: store the bytecode_pc so OSR entry lookup can match by
     * loop_header_pc instead of picking the first OSR-flagged entry. */
    entry->bytecode_pc = bytecode_pc;

    return idx;
}

int vtx_side_table_add_register(vtx_side_table_t *table,
                                 uint32_t register_number,
                                 vtx_nodeid_t node_id)
{
    if (!table || table->entry_count == 0) return -1;

    vtx_side_table_entry_t *entry = &table->entries[table->entry_count - 1];

    /* Grow register map if needed */
    uint32_t new_count = entry->register_map_count + 1;
    vtx_reg_map_entry_t *new_map = realloc(
        entry->register_map,
        (size_t)new_count * sizeof(vtx_reg_map_entry_t));
    if (!new_map) return -1;

    entry->register_map = new_map;
    entry->register_map[entry->register_map_count].register_number = register_number;
    entry->register_map[entry->register_map_count].node_id = node_id;
    entry->register_map_count = new_count;

    return 0;
}

/* ========================================================================== */
/* FrameState management                                                      */
/* ========================================================================== */

uint32_t vtx_side_table_add_frame_state(vtx_side_table_t *table,
                                         vtx_frame_state_t *fs)
{
    if (!table) return UINT32_MAX;

    /* Grow if needed */
    if (table->frame_state_count >= table->frame_state_capacity) {
        uint32_t new_cap = table->frame_state_capacity * 2;
        vtx_frame_state_t **new_fs = realloc(
            table->frame_states,
            (size_t)new_cap * sizeof(vtx_frame_state_t *));
        if (!new_fs) return UINT32_MAX;
        table->frame_states = new_fs;
        table->frame_state_capacity = new_cap;
    }

    uint32_t idx = table->frame_state_count++;
    table->frame_states[idx] = fs;
    return idx;
}

vtx_frame_state_t *vtx_side_table_get_frame_state(
    const vtx_side_table_t *table, uint32_t index)
{
    if (!table || index >= table->frame_state_count) return NULL;
    return table->frame_states[index];
}

/* ========================================================================== */
/* Lookup                                                                     */
/* ========================================================================== */

uint32_t vtx_side_table_lookup(const vtx_side_table_t *table,
                                uint32_t native_pc_offset)
{
    if (!table || table->entry_count == 0) return UINT32_MAX;

    /* Binary search: find the largest native_pc_offset <= native_pc_offset.
     * The entries array is sorted by native_pc_offset. */
    uint32_t lo = 0;
    uint32_t hi = table->entry_count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (table->entries[mid].native_pc_offset <= native_pc_offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    /* lo is now the first entry with native_pc_offset > target,
     * so lo - 1 is the last entry with native_pc_offset <= target. */
    if (lo == 0) {
        /* No entry with native_pc_offset <= target */
        return UINT32_MAX;
    }

    return table->entries[lo - 1].frame_state_index;
}

const vtx_side_table_entry_t *vtx_side_table_lookup_entry(
    const vtx_side_table_t *table,
    uint32_t native_pc_offset)
{
    if (!table || table->entry_count == 0) return NULL;

    /* Binary search (same as above) */
    uint32_t lo = 0;
    uint32_t hi = table->entry_count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (table->entries[mid].native_pc_offset <= native_pc_offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo == 0) return NULL;

    return &table->entries[lo - 1];
}

/* OSR-23 + OSR-5 fix: dedicated OSR entry lookup.
 *
 * The generic vtx_side_table_lookup returns the entry with the largest
 * native_pc_offset <= target, which is correct for deopt (state at or
 * before the PC) but WRONG for OSR entry lookup — it could return a
 * non-OSR entry (e.g., a safepoint) near the requested PC.
 *
 * This function only returns entries that:
 *   1. Have the VTX_STF_OSR_ENTRY flag set (OSR-23 fix), AND
 *   2. Match the requested bytecode_pc (OSR-5 fix — picks the right
 *      loop header when a method has multiple OSR entry points).
 *
 * Returns NULL if no matching entry is found.
 *
 * We use a linear scan because the number of OSR entry points per
 * method is typically small (one per loop header — usually <10).
 * Binary search by native_pc_offset doesn't help here because we're
 * filtering by bytecode_pc, not by native PC.
 */
const vtx_side_table_entry_t *vtx_side_table_lookup_osr_entry(
    const vtx_side_table_t *table,
    uint32_t bytecode_pc)
{
    if (!table || table->entry_count == 0) return NULL;

    for (uint32_t i = 0; i < table->entry_count; i++) {
        const vtx_side_table_entry_t *entry = &table->entries[i];
        if ((entry->flags & VTX_STF_OSR_ENTRY) &&
            entry->bytecode_pc == bytecode_pc) {
            return entry;
        }
    }

    return NULL;
}

const vtx_side_table_entry_t *vtx_side_table_get_entry(
    const vtx_side_table_t *table, uint32_t index)
{
    if (!table || index >= table->entry_count) return NULL;
    return &table->entries[index];
}

uint32_t vtx_side_table_entry_count(const vtx_side_table_t *table)
{
    return table ? table->entry_count : 0;
}

/* ========================================================================== */
/* Register map lookup                                                        */
/* ========================================================================== */

vtx_nodeid_t vtx_side_table_find_register(const vtx_side_table_t *table,
                                            uint32_t native_pc_offset,
                                            uint32_t register_number)
{
    const vtx_side_table_entry_t *entry =
        vtx_side_table_lookup_entry(table, native_pc_offset);
    if (!entry) return VTX_NODEID_INVALID;

    for (uint32_t i = 0; i < entry->register_map_count; i++) {
        if (entry->register_map[i].register_number == register_number) {
            return entry->register_map[i].node_id;
        }
    }

    return VTX_NODEID_INVALID;
}

const vtx_reg_map_entry_t *vtx_side_table_get_register_map(
    const vtx_side_table_t *table,
    uint32_t native_pc_offset,
    uint32_t *out_count)
{
    const vtx_side_table_entry_t *entry =
        vtx_side_table_lookup_entry(table, native_pc_offset);
    if (!entry) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    if (out_count) *out_count = entry->register_map_count;
    return entry->register_map;
}

/* ========================================================================== */
/* Safepoint recording                                                         */
/* ========================================================================== */

int vtx_side_table_record_safepoint(vtx_side_table_t *table,
                                     uint32_t native_pc_offset,
                                     const uint32_t *root_node_ids,
                                     uint32_t root_count)
{
    if (!table) return -1;

    /* Add a side table entry at the safepoint PC offset with the
     * VTX_STF_SAFEPPOINT flag. Use frame_state_index = UINT32_MAX
     * as a sentinel since safepoints don't necessarily have an
     * associated FrameState — the GC root map is the primary data.
     * OSR-5: bytecode_pc = UINT32_MAX since safepoints are not OSR
     * entry points. */
    uint32_t entry_idx = vtx_side_table_add_entry(table,
                                                    native_pc_offset,
                                                    UINT32_MAX,
                                                    VTX_STF_SAFEPPOINT,
                                                    UINT32_MAX);
    if (entry_idx == UINT32_MAX) return -1;

    /* Record each GC root as a register map entry. We store the
     * NodeID in the node_id field and use VTX_REG_NONE (0xFF) as
     * the register_number to indicate that the root's location is
     * identified by NodeID rather than a physical register.
     * This supports stack-allocated roots where the register
     * allocator has spilled the value. */
    for (uint32_t i = 0; i < root_count; i++) {
        /* Use a special register number to indicate "NodeID-based root".
         * 0xFFFFFFFF means the root is identified by node_id alone;
         * the GC will look up the node's location from the register
         * map or spill slots. */
        if (vtx_side_table_add_register(table, 0xFFFFFFFF, root_node_ids[i]) != 0) {
            return -1;
        }
    }

    return 0;
}
