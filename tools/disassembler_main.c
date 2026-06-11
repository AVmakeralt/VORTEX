/**
 * disassembler_main.c — CLI entry point for the VORTEX bytecode disassembler
 *
 * Reads raw bytecode from a file and outputs human-readable disassembly.
 * Usage: vortex_disasm <bytecode_file>
 *
 * The bytecode file format is simple:
 *   - First 4 bytes: uint32_t code_length (little-endian)
 *   - Next code_length bytes: raw bytecode
 *   - Next 2 bytes: uint16_t max_locals
 *   - Next 2 bytes: uint16_t max_stack
 *   - Next 4 bytes: uint32_t constant_count
 *   - Next constant_count * 8 bytes: constant pool (raw vtx_value_t)
 *
 * For now, this reads the file and disassembles using the built-in
 * bytecode format. A more sophisticated version would read the full
 * module format.
 */

#include "disassembler.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISASM_BUF_SIZE (1024 * 1024)  /* 1 MB */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: vortex_disasm <bytecode_file>\n");
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        fprintf(stderr, "vortex_disasm: cannot open '%s'\n", argv[1]);
        return 1;
    }

    /* Read the entire file */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fprintf(stderr, "vortex_disasm: empty or invalid file\n");
        fclose(f);
        return 1;
    }

    uint8_t *data = (uint8_t *)malloc((size_t)file_size);
    if (data == NULL) {
        fprintf(stderr, "vortex_disasm: out of memory\n");
        fclose(f);
        return 1;
    }

    size_t read_bytes = fread(data, 1, (size_t)file_size, f);
    fclose(f);

    if ((long)read_bytes != file_size) {
        fprintf(stderr, "vortex_disasm: read error\n");
        free(data);
        return 1;
    }

    /* Parse the simple bytecode file format */
    if (read_bytes < 4) {
        fprintf(stderr, "vortex_disasm: file too small\n");
        free(data);
        return 1;
    }

    size_t offset = 0;

    /* Read code_length (4 bytes, little-endian) */
    uint32_t code_length = (uint32_t)data[offset]
                         | ((uint32_t)data[offset + 1] << 8)
                         | ((uint32_t)data[offset + 2] << 16)
                         | ((uint32_t)data[offset + 3] << 24);
    offset += 4;

    if (offset + code_length > read_bytes) {
        fprintf(stderr, "vortex_disasm: code extends beyond file\n");
        free(data);
        return 1;
    }

    const uint8_t *code = data + offset;
    offset += code_length;

    /* Read max_locals (2 bytes) */
    uint16_t max_locals = 0;
    if (offset + 2 <= read_bytes) {
        max_locals = (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
        offset += 2;
    }

    /* Read max_stack (2 bytes) */
    uint16_t max_stack = 0;
    if (offset + 2 <= read_bytes) {
        max_stack = (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
        offset += 2;
    }

    /* Read constant_count (4 bytes) */
    uint32_t const_count = 0;
    vtx_value_t *consts = NULL;
    if (offset + 4 <= read_bytes) {
        const_count = (uint32_t)data[offset]
                    | ((uint32_t)data[offset + 1] << 8)
                    | ((uint32_t)data[offset + 2] << 16)
                    | ((uint32_t)data[offset + 3] << 24);
        offset += 4;

        if (const_count > 0 && offset + const_count * 8 <= read_bytes) {
            consts = (vtx_value_t *)(data + offset);
        }
    }

    /* Build bytecode struct */
    vtx_bytecode_t bc;
    bc.code = code;
    bc.length = code_length;
    bc.constant_pool = consts;
    bc.constant_count = const_count;
    bc.max_locals = max_locals;
    bc.max_stack = max_stack;

    /* Disassemble */
    vtx_disasm_t disasm;
    vtx_disasm_init(&disasm);

    char *out_buf = (char *)malloc(DISASM_BUF_SIZE);
    if (out_buf == NULL) {
        fprintf(stderr, "vortex_disasm: out of memory\n");
        free(data);
        return 1;
    }

    int result = vtx_disasm_disassemble(&bc, out_buf, DISASM_BUF_SIZE);
    if (result >= 0) {
        printf("%s", out_buf);
    } else {
        fprintf(stderr, "vortex_disasm: disassembly failed\n");
    }

    vtx_disasm_destroy(&disasm);
    free(out_buf);
    free(data);
    return (result >= 0) ? 0 : 1;
}
