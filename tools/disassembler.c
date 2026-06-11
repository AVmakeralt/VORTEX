#include "disassembler.h"

#include <stdio.h>
#include <string.h>

/* ========================================================================== */
/* Init/destroy                                                                 */
/* ========================================================================== */

int vtx_disasm_init(vtx_disasm_t *d)
{
    (void)d;
    return 0;
}

void vtx_disasm_destroy(vtx_disasm_t *d)
{
    (void)d;
}

/* ========================================================================== */
/* Single instruction disassembly                                               */
/* ========================================================================== */

size_t vtx_disasm_op(const vtx_bytecode_t *bc, size_t pc,
                      char *buf, size_t bufsize)
{
    return vtx_bytecode_disassemble_op(bc, pc, buf, bufsize);
}

/* ========================================================================== */
/* Full module disassembly                                                      */
/* ========================================================================== */

int vtx_disasm_disassemble(const vtx_bytecode_t *bc, char *out, size_t out_size)
{
    if (bc == NULL || out == NULL || out_size == 0) {
        return -1;
    }

    size_t pos = 0;
    int written;

    /* Header */
    written = snprintf(out + pos, out_size - pos,
                       "; Bytecode module: %zu bytes, %u constants, "
                       "max_locals=%u, max_stack=%u\n",
                       bc->length, bc->constant_count,
                       bc->max_locals, bc->max_stack);
    if (written < 0 || (size_t)written >= out_size - pos) return -1;
    pos += (size_t)written;

    /* Constant pool */
    if (bc->constant_count > 0 && bc->constant_pool != NULL) {
        written = snprintf(out + pos, out_size - pos, "; Constant pool:\n");
        if (written < 0 || (size_t)written >= out_size - pos) return -1;
        pos += (size_t)written;

        for (uint32_t i = 0; i < bc->constant_count; i++) {
            vtx_value_t cv = bc->constant_pool[i];
            if (vtx_is_smi(cv)) {
                written = snprintf(out + pos, out_size - pos,
                                   ";   [%u] smi %ld\n", i, (long)vtx_smi_value(cv));
            } else if (vtx_is_double(cv)) {
                written = snprintf(out + pos, out_size - pos,
                                   ";   [%u] double %g\n", i, vtx_double_value(cv));
            } else if (vtx_is_null(cv)) {
                written = snprintf(out + pos, out_size - pos,
                                   ";   [%u] null\n", i);
            } else if (vtx_is_bool(cv)) {
                written = snprintf(out + pos, out_size - pos,
                                   ";   [%u] bool %s\n", i,
                                   vtx_bool_value(cv) ? "true" : "false");
            } else if (vtx_is_undefined(cv)) {
                written = snprintf(out + pos, out_size - pos,
                                   ";   [%u] undefined\n", i);
            } else if (vtx_is_heap_ptr(cv)) {
                written = snprintf(out + pos, out_size - pos,
                                   ";   [%u] heap_ptr %p\n", i, vtx_heap_ptr(cv));
            } else {
                written = snprintf(out + pos, out_size - pos,
                                   ";   [%u] raw 0x%016lx\n", i, (unsigned long)cv);
            }
            if (written < 0 || (size_t)written >= out_size - pos) return -1;
            pos += (size_t)written;
        }
    }

    /* Instruction stream */
    written = snprintf(out + pos, out_size - pos, "\n; Instructions:\n");
    if (written < 0 || (size_t)written >= out_size - pos) return -1;
    pos += (size_t)written;

    size_t pc = 0;
    char line_buf[256];
    while (pc < bc->length) {
        size_t next_pc = vtx_bytecode_disassemble_op(bc, pc, line_buf, sizeof(line_buf));

        written = snprintf(out + pos, out_size - pos, "%s\n", line_buf);
        if (written < 0 || (size_t)written >= out_size - pos) return -1;
        pos += (size_t)written;

        if (next_pc <= pc) {
            /* Safety: prevent infinite loop on invalid bytecode */
            break;
        }
        pc = next_pc;
    }

    return (int)pos;
}
