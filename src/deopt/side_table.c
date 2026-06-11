#include "deopt/side_table.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Lifecycle                                                                  */
/* ========================================================================== */

vtx_side_table_t *vtx_side_table_build(vtx_arena_t *arena)
{
    vtx_side_table_t *table;
    if (arena) {
        table = vtx_arena_alloc(arena, sizeof(vtx_side_table_t));
    } else {
        table = calloc(1, sizeof(vtx_side_table_t));
    }
    if (!table) return NULL;

    memset(table, 0, sizeof(*table));

    table->entry_capacity = VTX_SIDE_TABLE_INITIAL_CAPACITY;
    table->entries = calloc(table->entry_capacity,
                             sizeof(vtx_side_table_entry_t));
    if (!table->entries) {
        if (!arena) free(table);
        return NULL;
    }
    table->entry_count = 0;

    table->frame_state_capacity = VTX_SIDE_TABLE_INITIAL_CAPACITY;
    table->frame_states = calloc(table->frame_state_capacity,
                                  sizeof(vtx_frame_state_t *));
    if (!table->frame_states) {
        free(table->entries);
        if (!arena) free(table);
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

    /* Note: we don't free 'table' itself because it might be arena-allocated.
     * The caller is responsible for knowing whether to free the table struct. */
}

/* ========================================================================== */
/* Entry management                                                           */
/* ========================================================================== */

uint32_t vtx_side_table_add_entry(vtx_side_table_t *table,
                                   uint32_t native_pc_offset,
                                   uint32_t frame_state_index,
                                   uint32_t flags)
{
    if (!table) return UINT32_MAX;

    /* Validate ordering: entries must be added in increasing PC order */
    if (table->entry_count > 0) {
        uint32_t last_pc = table->entries[table->entry_count - 1].native_pc_offset;
        VTX_ASSERT(native_pc_offset >= last_pc,
                   "side table entries must be added in increasing PC order");
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
