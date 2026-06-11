#ifndef VORTEX_DEOPT_SIDE_TABLE_H
#define VORTEX_DEOPT_SIDE_TABLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/node.h"
#include "deopt/frame_state.h"
#include "runtime/arena.h"

/**
 * VORTEX Side Table — Native PC → FrameState Mapping
 *
 * The side table maps native PC offsets to FrameState indices, enabling
 * deoptimization to reconstruct interpreter state at any point in compiled
 * code. It is stored separately from the compiled code (not embedded in
 * the instruction stream) to avoid polluting the instruction cache.
 *
 * Structure:
 *   - A sorted array of (native_pc_offset, frame_state_index) tuples.
 *   - Binary search for lookup: O(log n) where n is the number of entries.
 *   - A register map: for each entry, which registers hold which NodeIDs.
 *
 * The side table is built during code emission (each guard, call, and
 * allocation point gets an entry). At deopt time, the runtime binary-
 * searches the table for the native PC to find the FrameState and
 * register map, then reconstructs the interpreter frame.
 */

/* ========================================================================== */
/* Register map entry                                                         */
/* ========================================================================== */

/**
 * A single entry in the register map: which register holds which NodeID.
 * The register number is architecture-specific (e.g., RAX=0, RCX=1, etc.
 * on x86-64).
 */
typedef struct {
    uint32_t     register_number; /* architecture-specific register number */
    vtx_nodeid_t node_id;         /* the NodeID whose value is in this register */
} vtx_reg_map_entry_t;

/* ========================================================================== */
/* Side table entry                                                           */
/* ========================================================================== */

/**
 * A single entry in the side table: maps a native PC offset to a FrameState
 * and provides the register map for reconstructing values.
 */
typedef struct {
    uint32_t              native_pc_offset;   /* offset from code start */
    uint32_t              frame_state_index;  /* index into the FrameState array */
    vtx_reg_map_entry_t  *register_map;       /* register map entries (arena-allocated) */
    uint32_t              register_map_count;  /* number of entries in register map */
    uint32_t              flags;              /* bit flags for this entry */
} vtx_side_table_entry_t;

/* Entry flags */
#define VTX_STF_CALL_SITE      (1u << 0)  /* entry is at a call site */
#define VTX_STF_GUARD          (1u << 1)  /* entry is at a guard check */
#define VTX_STF_SAFEPPOINT     (1u << 2)  /* entry is at a safepoint */
#define VTX_STF_ALLOCATION     (1u << 3)  /* entry is at an allocation point */
#define VTX_STF_OSR_ENTRY      (1u << 4)  /* entry is an OSR entry point */

/* ========================================================================== */
/* Side table                                                                 */
/* ========================================================================== */

#define VTX_SIDE_TABLE_INITIAL_CAPACITY 64

/**
 * The side table: a sorted array of entries mapping native PC offsets
 * to FrameState indices and register maps.
 */
typedef struct {
    vtx_side_table_entry_t *entries;       /* sorted array of entries */
    uint32_t                entry_count;
    uint32_t                entry_capacity;

    /* The FrameState array: indexed by frame_state_index from entries.
     * The actual FrameState structures are stored elsewhere (arena-allocated
     * during compilation), but we keep an array of pointers for indexed access. */
    vtx_frame_state_t     **frame_states;  /* array of FrameState pointers */
    uint32_t                frame_state_count;
    uint32_t                frame_state_capacity;
} vtx_side_table_t;

/* ========================================================================== */
/* Lifecycle                                                                  */
/* ========================================================================== */

/**
 * Build a new side table. If arena is non-NULL, entries are allocated from it.
 * If arena is NULL, entries are allocated with malloc.
 * Returns a new side table, or NULL on failure.
 */
vtx_side_table_t *vtx_side_table_build(vtx_arena_t *arena);

/**
 * Destroy a side table and free all memory.
 * Only frees malloc'd tables (not arena-allocated ones).
 */
void vtx_side_table_destroy(vtx_side_table_t *table);

/* ========================================================================== */
/* Entry management                                                           */
/* ========================================================================== */

/**
 * Add an entry to the side table. Entries must be added in increasing
 * native_pc_offset order (this is natural since code is emitted sequentially).
 * Returns the index of the new entry, or UINT32_MAX on failure.
 *
 * @param table             The side table
 * @param native_pc_offset  Native PC offset of the deopt point
 * @param frame_state_index Index into the FrameState array
 * @param flags             Bit flags for this entry
 * @return Entry index, or UINT32_MAX on failure
 */
uint32_t vtx_side_table_add_entry(vtx_side_table_t *table,
                                   uint32_t native_pc_offset,
                                   uint32_t frame_state_index,
                                   uint32_t flags);

/**
 * Add a register map entry to the most recently added side table entry.
 * Returns 0 on success, -1 on failure.
 */
int vtx_side_table_add_register(vtx_side_table_t *table,
                                 uint32_t register_number,
                                 vtx_nodeid_t node_id);

/* ========================================================================== */
/* FrameState management                                                      */
/* ========================================================================== */

/**
 * Register a FrameState with the side table. Returns the index.
 * The FrameState pointer is stored; it must remain valid for the
 * lifetime of the side table.
 */
uint32_t vtx_side_table_add_frame_state(vtx_side_table_t *table,
                                         vtx_frame_state_t *fs);

/**
 * Get a FrameState by index.
 */
vtx_frame_state_t *vtx_side_table_get_frame_state(
    const vtx_side_table_t *table, uint32_t index);

/* ========================================================================== */
/* Lookup                                                                     */
/* ========================================================================== */

/**
 * Look up the side table entry for a given native PC offset.
 * Uses binary search on the sorted entries array.
 *
 * If an exact match is not found, returns the entry with the largest
 * native_pc_offset that is <= the given pc. This is correct because
 * the side table entry at or before the PC describes the state at that point.
 *
 * Returns the frame_state_index, or UINT32_MAX if no entry exists
 * at or before the given PC.
 */
uint32_t vtx_side_table_lookup(const vtx_side_table_t *table,
                                uint32_t native_pc_offset);

/**
 * Look up the full side table entry for a given native PC offset.
 * Same semantics as vtx_side_table_lookup, but returns the full entry.
 * Returns NULL if no entry is found.
 */
const vtx_side_table_entry_t *vtx_side_table_lookup_entry(
    const vtx_side_table_t *table,
    uint32_t native_pc_offset);

/**
 * Get a side table entry by its index.
 */
const vtx_side_table_entry_t *vtx_side_table_get_entry(
    const vtx_side_table_t *table, uint32_t index);

/**
 * Get the number of entries in the side table.
 */
uint32_t vtx_side_table_entry_count(const vtx_side_table_t *table);

/* ========================================================================== */
/* Register map lookup                                                        */
/* ========================================================================== */

/**
 * Look up the NodeID held in a specific register at a given native PC.
 * Returns VTX_NODEID_INVALID if the register is not mapped at that PC.
 */
vtx_nodeid_t vtx_side_table_find_register(const vtx_side_table_t *table,
                                            uint32_t native_pc_offset,
                                            uint32_t register_number);

/**
 * Get the full register map for a given native PC offset.
 * Sets *out_count to the number of register map entries.
 * Returns a pointer to the first entry, or NULL if no entry found.
 */
const vtx_reg_map_entry_t *vtx_side_table_get_register_map(
    const vtx_side_table_t *table,
    uint32_t native_pc_offset,
    uint32_t *out_count);

#endif /* VORTEX_DEOPT_SIDE_TABLE_H */
