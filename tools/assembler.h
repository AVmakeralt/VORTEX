#ifndef VORTEX_ASSEMBLER_H
#define VORTEX_ASSEMBLER_H

#include "runtime/bytecode.h"
#include "runtime/object.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * VORTEX Bytecode Assembler
 *
 * Text format: one instruction per line.
 *   load_local 0
 *   iadd
 *   return_value
 *   if_true 42
 *
 * Parses each line, emits bytecode. Supports all opcodes.
 * Constant pool entries are added as needed.
 */

/* ========================================================================== */
/* Assembler state                                                              */
/* ========================================================================== */

#define VTX_ASM_INIT_CODE_CAP   256
#define VTX_ASM_INIT_CONST_CAP  32

typedef struct {
    /* Output bytecode buffer */
    uint8_t     *code;           /* dynamically growing code buffer */
    size_t       code_cap;       /* allocated capacity */
    size_t       code_len;       /* current length */

    /* Constant pool */
    vtx_value_t *constant_pool;  /* dynamically growing constant pool */
    uint32_t     const_cap;      /* allocated capacity */
    uint32_t     const_count;    /* current number of constants */

    /* Metadata tracked during assembly */
    uint16_t     max_locals;     /* max local variable index seen + 1 */
    uint16_t     max_stack;      /* estimated max operand stack depth */

    /* Error state */
    bool         has_error;      /* true if any parse error occurred */
    char         error_msg[256]; /* last error message */
    int          error_line;     /* line number of last error */
} vtx_assembler_t;

/* ========================================================================== */
/* API                                                                          */
/* ========================================================================== */

/**
 * Initialize an assembler. Allocates initial buffers.
 * Returns 0 on success, -1 on failure.
 */
int vtx_asm_init(vtx_assembler_t *asm_);

/**
 * Destroy an assembler and free all buffers.
 */
void vtx_asm_destroy(vtx_assembler_t *asm_);

/**
 * Process a single line of assembly text.
 * Parses the line, emits bytecode into the internal buffer.
 * Returns 0 on success, -1 on parse error (sets has_error and error_msg).
 *
 * Line format:
 *   opcode_name              (for opcodes with no operand)
 *   opcode_name operand      (for opcodes with an operand, e.g., load_local 0)
 *   ; comment                (lines starting with ; are ignored)
 *   empty lines              (ignored)
 */
int vtx_asm_line(vtx_assembler_t *asm_, const char *line);

/**
 * Emit the assembled bytecode as a vtx_bytecode_t.
 * The caller must not free the returned code or constant_pool —
 * they are owned by the assembler. Copy them if persistence is needed.
 *
 * Returns a fully populated vtx_bytecode_t.
 */
vtx_bytecode_t vtx_asm_emit(vtx_assembler_t *asm_);

/**
 * Add a constant to the constant pool and return its index.
 * Used internally, but also available for programmatic constant insertion.
 */
uint16_t vtx_asm_add_const(vtx_assembler_t *asm_, vtx_value_t value);

#endif /* VORTEX_ASSEMBLER_H */
