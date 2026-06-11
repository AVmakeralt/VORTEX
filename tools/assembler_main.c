/**
 * assembler_main.c — CLI entry point for the VORTEX bytecode assembler
 *
 * Reads assembly text from stdin or a file, emits bytecode.
 * Usage: vortex_asm [input_file]
 *   If no input_file, reads from stdin.
 *   Outputs raw bytecode to stdout or a file.
 */

#include "assembler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_BUF_SIZE 1024

int main(int argc, char **argv)
{
    FILE *input = stdin;
    bool close_input = false;

    if (argc > 1) {
        input = fopen(argv[1], "r");
        if (input == NULL) {
            fprintf(stderr, "vortex_asm: cannot open '%s'\n", argv[1]);
            return 1;
        }
        close_input = true;
    }

    vtx_assembler_t asm_;
    if (vtx_asm_init(&asm_) != 0) {
        fprintf(stderr, "vortex_asm: failed to initialize assembler\n");
        if (close_input) fclose(input);
        return 1;
    }

    char line[LINE_BUF_SIZE];
    int line_num = 0;
    while (fgets(line, sizeof(line), input) != NULL) {
        line_num++;

        /* Remove trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }

        if (vtx_asm_line(&asm_, line) != 0) {
            fprintf(stderr, "vortex_asm: error on line %d: %s\n",
                    line_num, asm_.error_msg);
            vtx_asm_destroy(&asm_);
            if (close_input) fclose(input);
            return 1;
        }
    }

    if (asm_.has_error) {
        fprintf(stderr, "vortex_asm: error: %s\n", asm_.error_msg);
        vtx_asm_destroy(&asm_);
        if (close_input) fclose(input);
        return 1;
    }

    vtx_bytecode_t bc = vtx_asm_emit(&asm_);

    /* Output a summary */
    fprintf(stderr, "vortex_asm: assembled %zu bytes, %u constants, "
            "max_locals=%u, max_stack=%u\n",
            bc.length, bc.constant_count, bc.max_locals, bc.max_stack);

    /* Print disassembly to stdout */
    printf("; Assembled bytecode\n");
    printf("; %zu bytes, %u constants, max_locals=%u, max_stack=%u\n\n",
           bc.length, bc.constant_count, bc.max_locals, bc.max_stack);

    if (bc.constant_count > 0 && bc.constant_pool != NULL) {
        printf("; Constant pool:\n");
        for (uint32_t i = 0; i < bc.constant_count; i++) {
            vtx_value_t cv = bc.constant_pool[i];
            if (vtx_is_smi(cv)) {
                printf(";   [%u] smi %ld\n", i, (long)vtx_smi_value(cv));
            } else if (vtx_is_double(cv)) {
                printf(";   [%u] double %g\n", i, vtx_double_value(cv));
            } else if (vtx_is_null(cv)) {
                printf(";   [%u] null\n", i);
            } else if (vtx_is_bool(cv)) {
                printf(";   [%u] bool %s\n", i, vtx_bool_value(cv) ? "true" : "false");
            } else {
                printf(";   [%u] raw 0x%016lx\n", i, (unsigned long)cv);
            }
        }
        printf("\n");
    }

    /* Print instructions */
    size_t pc = 0;
    char buf[256];
    while (pc < bc.length) {
        size_t next_pc = vtx_bytecode_disassemble_op(&bc, pc, buf, sizeof(buf));
        printf("%s\n", buf);
        if (next_pc <= pc) break;
        pc = next_pc;
    }

    vtx_asm_destroy(&asm_);
    if (close_input) fclose(input);
    return 0;
}
