/**
 * VORTEX Relocations and Fixups
 *
 * Records and applies relocations to the emitted code buffer.
 * After all code is emitted, relocations are applied in a single pass
 * to patch forward references, call targets, and absolute addresses.
 */

#include "lower/reloc.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_reloc_table_init(vtx_reloc_table_t *table, vtx_arena_t *arena)
{
    if (!table) return -1;
    table->count = 0;
    table->capacity = VTX_RELOC_INITIAL_CAPACITY;
    table->entries = (vtx_reloc_t *)vtx_arena_alloc(arena,
                        table->capacity * sizeof(vtx_reloc_t));
    if (!table->entries) {
        table->capacity = 0;
        return -1;
    }
    memset(table->entries, 0, table->capacity * sizeof(vtx_reloc_t));
    return 0;
}

void vtx_reloc_table_destroy(vtx_reloc_table_t *table)
{
    if (!table) return;
    /* Entries are arena-allocated, no individual free needed */
    table->entries = NULL;
    table->count = 0;
    table->capacity = 0;
}

/* ========================================================================== */
/* Add relocation entries                                                      */
/* ========================================================================== */

static uint32_t ensure_capacity(vtx_reloc_table_t *table, vtx_arena_t *arena)
{
    if (table->count < table->capacity) return 0;

    uint32_t new_cap = table->capacity * 2;
    vtx_reloc_t *new_entries = (vtx_reloc_t *)vtx_arena_alloc(arena,
        new_cap * sizeof(vtx_reloc_t));
    if (!new_entries) return UINT32_MAX;

    if (table->entries && table->count > 0) {
        memcpy(new_entries, table->entries, table->count * sizeof(vtx_reloc_t));
    }
    table->entries = new_entries;
    table->capacity = new_cap;
    return 0;
}

uint32_t vtx_reloc_add(vtx_reloc_table_t *table,
                        vtx_reloc_kind_t kind,
                        uint32_t offset,
                        uint32_t target_offset,
                        uint64_t target_address,
                        uint32_t symbol,
                        int32_t addend,
                        vtx_arena_t *arena)
{
    if (!table) return UINT32_MAX;
    if (ensure_capacity(table, arena) == UINT32_MAX) return UINT32_MAX;

    uint32_t idx = table->count++;
    vtx_reloc_t *entry = &table->entries[idx];
    entry->kind = kind;
    entry->offset = offset;
    entry->target_offset = target_offset;
    entry->target_address = target_address;
    entry->symbol = symbol;
    entry->addend = addend;
    return idx;
}

uint32_t vtx_reloc_add_branch(vtx_reloc_table_t *table,
                               uint32_t patch_offset,
                               uint32_t source_offset,
                               uint32_t target_offset,
                               vtx_arena_t *arena)
{
    /* For a branch: the displacement is relative to the instruction after
     * the branch. For a 6-byte JCC (0F 8x + 4 bytes disp), the
     * displacement is relative to source_offset + 6.
     * For a 5-byte JMP (E9 + 4 bytes disp), relative to source_offset + 5.
     * We use target_offset - source_offset - instruction_length.
     * The addend handles the instruction length offset. */
    return vtx_reloc_add(table, VTX_RELOC_REL32, patch_offset,
                          target_offset, 0, 0, 0, arena);
}

uint32_t vtx_reloc_add_call(vtx_reloc_table_t *table,
                             uint32_t patch_offset,
                             uint64_t target_address,
                             vtx_arena_t *arena)
{
    return vtx_reloc_add(table, VTX_RELOC_REL32, patch_offset,
                          0, target_address, 0, 0, arena);
}

uint32_t vtx_reloc_add_deopt_handler(vtx_reloc_table_t *table,
                                      uint32_t patch_offset,
                                      uint64_t handler_addr,
                                      vtx_arena_t *arena)
{
    return vtx_reloc_add(table, VTX_RELOC_ABS64, patch_offset,
                          0, handler_addr, 0, 0, arena);
}

/* ========================================================================== */
/* Apply all relocations                                                      */
/* ========================================================================== */

/**
 * Read a 32-bit little-endian value from a buffer.
 */
static int32_t read_i32(const uint8_t *buf, uint32_t offset)
{
    return (int32_t)((uint32_t)buf[offset] |
                     ((uint32_t)buf[offset + 1] << 8) |
                     ((uint32_t)buf[offset + 2] << 16) |
                     ((uint32_t)buf[offset + 3] << 24));
}

/**
 * Write a 32-bit little-endian value to a buffer.
 */
static void write_i32(uint8_t *buf, uint32_t offset, int32_t value)
{
    uint32_t v = (uint32_t)value;
    buf[offset]     = (uint8_t)(v & 0xFF);
    buf[offset + 1] = (uint8_t)((v >> 8) & 0xFF);
    buf[offset + 2] = (uint8_t)((v >> 16) & 0xFF);
    buf[offset + 3] = (uint8_t)((v >> 24) & 0xFF);
}

/**
 * Write a 64-bit little-endian value to a buffer.
 */
static void write_i64(uint8_t *buf, uint32_t offset, int64_t value)
{
    uint64_t v = (uint64_t)value;
    for (int i = 0; i < 8; i++) {
        buf[offset + i] = (uint8_t)((v >> (i * 8)) & 0xFF);
    }
}

int vtx_reloc_apply_all(vtx_reloc_table_t *table, uint8_t *code_buffer,
                         uint32_t code_size)
{
    if (!table || !code_buffer) return -1;

    for (uint32_t i = 0; i < table->count; i++) {
        const vtx_reloc_t *reloc = &table->entries[i];

        /* Bounds check */
        if (reloc->offset >= code_size) {
            VTX_ASSERT(false, "relocation offset out of bounds");
            continue;
        }

        switch (reloc->kind) {

        case VTX_RELOC_REL32: {
            /* 32-bit relative displacement.
             * For intra-code: disp = target_offset - (offset + 4) + addend
             * For external:   disp = (int32_t)(target_address - (code_base + offset + 4))
             */
            if (reloc->target_offset != 0 || reloc->target_address == 0) {
                /* Intra-code relocation */
                if (reloc->offset + 4 > code_size) continue;
                int32_t disp = (int32_t)reloc->target_offset -
                               (int32_t)(reloc->offset + 4) +
                               reloc->addend;
                write_i32(code_buffer, reloc->offset, disp);
            } else {
                /* External call relocation — target_address is absolute.
                 * We need the code buffer's base address to compute the
                 * relative displacement. This is set up at install time. */
                if (reloc->offset + 4 > code_size) continue;
                /* For now, store the absolute address as a placeholder.
                 * The actual relative displacement will be computed when
                 * the code is installed at its final address. */
                int64_t addr = (int64_t)reloc->target_address;
                int64_t src = (int64_t)(uintptr_t)code_buffer + reloc->offset + 4;
                int32_t disp = (int32_t)(addr - src + reloc->addend);
                write_i32(code_buffer, reloc->offset, disp);
            }
            break;
        }

        case VTX_RELOC_ABS64: {
            /* 64-bit absolute address */
            if (reloc->offset + 8 > code_size) continue;
            write_i64(code_buffer, reloc->offset,
                      (int64_t)(reloc->target_address + (uint64_t)reloc->addend));
            break;
        }

        case VTX_RELOC_RIP_REL32: {
            /* RIP-relative 32-bit displacement.
             * disp = target_offset - (offset + 4) + addend
             * (RIP points to the next instruction, which is offset + 4
             *  after the displacement field) */
            if (reloc->offset + 4 > code_size) continue;
            int32_t disp = (int32_t)reloc->target_offset -
                           (int32_t)(reloc->offset + 4) +
                           reloc->addend;
            write_i32(code_buffer, reloc->offset, disp);
            break;
        }
        }
    }

    return 0;
}
