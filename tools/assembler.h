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
 * Two modes:
 *   (1) Single-line mode (legacy): vtx_asm_line() emits bytecode directly.
 *       Branch targets must be literal PCs.
 *   (2) Program mode (new): vtx_asm_program() runs a 2-pass assembler over
 *       a multi-line string. Supports labels and pseudo-instructions.
 *
 * Labels:
 *   loop_start:
 *     load_local 0
 *     if_true loop_start      ; label resolved to PC in pass 2
 *
 * Pseudo-instructions (program mode only):
 *   .max_locals  N            ; declare max_locals (default: auto-detect)
 *   .max_stack   N            ; declare max_stack (default: 32)
 *   .const       int    42    ; add int constant, returns next const index
 *   .const       float  3.14  ; add float constant
 *   .const       str    "hi"  ; add string constant (stored as heap ptr)
 *   .method      name  sig    ; declare method name + signature (metadata)
 *   .arg_count   N            ; set method arg_count (for CALL_STATIC)
 *
 * Comments: lines starting with ';' are ignored. Inline comments after
 * operands are NOT supported (use a separate line).
 */

/* ========================================================================== */
/* Assembler state                                                              */
/* ========================================================================== */

#define VTX_ASM_INIT_CODE_CAP   256
#define VTX_ASM_INIT_CONST_CAP  32
#define VTX_ASM_MAX_LABELS      64
#define VTX_ASM_MAX_LABEL_NAME  32
#define VTX_ASM_MAX_FIXUPS      128

typedef struct {
    char     name[VTX_ASM_MAX_LABEL_NAME];
    uint16_t pc;        /* PC of the label (set in pass 1) */
    bool     defined;   /* true if the label was defined */
} vtx_asm_label_t;

typedef struct {
    uint16_t fixup_pc;        /* PC of the operand to patch (the byte AFTER opcode) */
    uint16_t label_idx;       /* index into labels[] of the target label */
    uint16_t branch_insn_pc;  /* PC of the branch opcode itself (for diagnostics) */
} vtx_asm_fixup_t;

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
    bool         max_stack_set;  /* true if .max_stack was used */
    bool         max_locals_set; /* true if .max_locals was used */

    /* Method metadata (for CALL_STATIC etc.) */
    char         method_name[64];
    char         method_sig[64];
    uint16_t     arg_count;

    /* Labels and fixups (for program mode) */
    vtx_asm_label_t labels[VTX_ASM_MAX_LABELS];
    uint32_t         label_count;
    vtx_asm_fixup_t  fixups[VTX_ASM_MAX_FIXUPS];
    uint32_t         fixup_count;

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

/**
 * Assemble a multi-line program text into bytecode.
 *
 * Supports:
 *   - Labels: "name:" defines a label at the current PC.
 *   - Label references: "goto loop_start" jumps to the label.
 *   - Pseudo-instructions starting with '.': .max_locals, .max_stack,
 *     .const, .method, .arg_count.
 *   - Comments: lines starting with ';' are ignored.
 *   - Empty lines are ignored.
 *
 * On success, returns 0 and the assembler's code/constant_pool/max_locals/
 * max_stack fields are populated (use vtx_asm_emit() to extract them).
 * On failure, returns -1 and asm_->has_error is set with error_msg/error_line.
 *
 * Calling this resets the assembler state (clears any previous code/labels).
 */
int vtx_asm_program(vtx_assembler_t *asm_, const char *text);

/**
 * Look up a label by name. Returns its PC, or 0xFFFF if not found.
 * Only valid after vtx_asm_program() has completed.
 */
uint16_t vtx_asm_label_pc(const vtx_assembler_t *asm_, const char *name);

#endif /* VORTEX_ASSEMBLER_H */
