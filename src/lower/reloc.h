#ifndef VORTEX_LOWER_RELOC_H
#define VORTEX_LOWER_RELOC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "runtime/arena.h"

/**
 * VORTEX Relocations and Fixups
 *
 * Handles fixups for:
 *   - Forward branches (patched after target is emitted)
 *   - Call targets (patched after callee is compiled)
 *   - Constant pool references (RIP-relative)
 *   - Deopt handler addresses (absolute)
 *
 * Relocations are recorded during code emission and applied in a final
 * pass once all code positions are known.
 */

/* ========================================================================== */
/* Relocation kinds                                                            */
/* ========================================================================== */

typedef enum {
    VTX_RELOC_REL32 = 0,   /* 32-bit relative (for jumps, calls) */
    VTX_RELOC_ABS64,       /* 64-bit absolute address */
    VTX_RELOC_RIP_REL32,   /* RIP-relative 32-bit (for constant pool, globals) */
} vtx_reloc_kind_t;

/* ========================================================================== */
/* Relocation entry                                                            */
/* ========================================================================== */

typedef struct {
    vtx_reloc_kind_t kind;          /* relocation type */
    uint32_t         offset;        /* offset in code buffer where fixup goes */
    uint32_t         target_offset; /* target offset in code buffer (for intra-code relocations) */
    uint64_t         target_address;/* absolute target address (for ABS64) */
    uint32_t         symbol;        /* symbol index (for external references) */
    int32_t          addend;        /* additional offset to add */
} vtx_reloc_t;

/* ========================================================================== */
/* Relocation table                                                            */
/* ========================================================================== */

#define VTX_RELOC_INITIAL_CAPACITY 64

typedef struct {
    vtx_reloc_t *entries;
    uint32_t     count;
    uint32_t     capacity;
} vtx_reloc_table_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize a relocation table.
 * Returns 0 on success, -1 on failure.
 */
int vtx_reloc_table_init(vtx_reloc_table_t *table, vtx_arena_t *arena);

/**
 * Destroy a relocation table.
 */
void vtx_reloc_table_destroy(vtx_reloc_table_t *table);

/* ========================================================================== */
/* Relocation management                                                       */
/* ========================================================================== */

/**
 * Add a relocation entry to the table.
 * Returns the index of the new entry, or UINT32_MAX on failure.
 *
 * @param table          Relocation table
 * @param kind           Relocation type
 * @param offset         Offset in code buffer where fixup goes
 * @param target_offset  Target offset (for REL32)
 * @param target_address Absolute target address (for ABS64)
 * @param symbol         Symbol index (0 = none)
 * @param addend         Additional offset
 * @param arena          Arena for allocation
 */
uint32_t vtx_reloc_add(vtx_reloc_table_t *table,
                        vtx_reloc_kind_t kind,
                        uint32_t offset,
                        uint32_t target_offset,
                        uint64_t target_address,
                        uint32_t symbol,
                        int32_t addend,
                        vtx_arena_t *arena);

/**
 * Apply all relocations to the code buffer.
 *
 * For REL32: patch the 32-bit value at `offset` with
 *   (target_offset - offset - 4 + addend)
 *
 * For ABS64: patch the 64-bit value at `offset` with
 *   (target_address + addend)
 *
 * For RIP_REL32: patch the 32-bit value at `offset` with
 *   (target_offset - (offset + 4) + addend)
 *
 * @param table       Relocation table
 * @param code_buffer Code buffer to patch
 * @param code_size   Size of the code buffer
 * @return            0 on success, -1 on failure
 */
int vtx_reloc_apply_all(vtx_reloc_table_t *table, uint8_t *code_buffer,
                         uint32_t code_size);

/**
 * Add a forward branch relocation.
 * Convenience function for the common case of a JCC/JMP that targets
 * a not-yet-emitted location.
 *
 * @param table          Relocation table
 * @param patch_offset   Offset of the 32-bit displacement to patch
 * @param source_offset  Offset of the branch instruction start
 * @param target_offset  Offset of the branch target (0 = unknown, will be set later)
 * @param arena          Arena
 */
uint32_t vtx_reloc_add_branch(vtx_reloc_table_t *table,
                               uint32_t patch_offset,
                               uint32_t source_offset,
                               uint32_t target_offset,
                               vtx_arena_t *arena);

/**
 * Add a call target relocation.
 *
 * @param table          Relocation table
 * @param patch_offset   Offset of the 32-bit displacement
 * @param target_address Absolute address of the callee
 * @param arena          Arena
 */
uint32_t vtx_reloc_add_call(vtx_reloc_table_t *table,
                             uint32_t patch_offset,
                             uint64_t target_address,
                             vtx_arena_t *arena);

/**
 * Add a deopt handler address relocation (absolute 64-bit).
 *
 * @param table          Relocation table
 * @param patch_offset   Offset of the 64-bit address to patch
 * @param handler_addr   Address of the deopt handler
 * @param arena          Arena
 */
uint32_t vtx_reloc_add_deopt_handler(vtx_reloc_table_t *table,
                                      uint32_t patch_offset,
                                      uint64_t handler_addr,
                                      vtx_arena_t *arena);

#endif /* VORTEX_LOWER_RELOC_H */
