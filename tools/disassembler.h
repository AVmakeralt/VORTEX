#ifndef VORTEX_DISASSEMBLER_H
#define VORTEX_DISASSEMBLER_H

#include "runtime/bytecode.h"

#include <stddef.h>

/**
 * VORTEX Bytecode Disassembler
 *
 * Converts vtx_bytecode_t → human-readable text output.
 */

/* ========================================================================== */
/* Disassembler state                                                           */
/* ========================================================================== */

typedef struct {
    /* No mutable state needed — the disassembler is stateless.
     * The struct exists for API consistency and future extensibility
     * (e.g., symbol table for label resolution). */
    int unused;
} vtx_disasm_t;

/* ========================================================================== */
/* API                                                                          */
/* ========================================================================== */

/**
 * Initialize the disassembler.
 * Returns 0 on success, -1 on failure.
 */
int vtx_disasm_init(vtx_disasm_t *d);

/**
 * Destroy the disassembler.
 */
void vtx_disasm_destroy(vtx_disasm_t *d);

/**
 * Disassemble an entire bytecode module into a text buffer.
 *
 * @param bc        Bytecode module to disassemble
 * @param out       Output buffer for text
 * @param out_size  Size of the output buffer in bytes
 * @return          Number of bytes written (excluding null terminator),
 *                  or -1 on error
 */
int vtx_disasm_disassemble(const vtx_bytecode_t *bc, char *out, size_t out_size);

/**
 * Disassemble a single instruction at the given PC.
 * Writes the disassembly into buf (at most bufsize-1 chars + null).
 * Returns the PC of the next instruction.
 *
 * This is a convenience wrapper around vtx_bytecode_disassemble_op.
 */
size_t vtx_disasm_op(const vtx_bytecode_t *bc, size_t pc,
                      char *buf, size_t bufsize);

#endif /* VORTEX_DISASSEMBLER_H */
