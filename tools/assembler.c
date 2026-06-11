#include "assembler.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

/* ========================================================================== */
/* Internal: grow buffers                                                       */
/* ========================================================================== */

static int asm_ensure_code_cap(vtx_assembler_t *asm_, size_t needed)
{
    if (asm_->code_len + needed <= asm_->code_cap) {
        return 0;
    }
    size_t new_cap = asm_->code_cap;
    while (new_cap < asm_->code_len + needed) {
        new_cap *= 2;
    }
    uint8_t *new_code = (uint8_t *)realloc(asm_->code, new_cap);
    if (new_code == NULL) return -1;
    asm_->code = new_code;
    asm_->code_cap = new_cap;
    return 0;
}

static int asm_ensure_const_cap(vtx_assembler_t *asm_, uint32_t needed)
{
    if (asm_->const_count + needed <= asm_->const_cap) {
        return 0;
    }
    uint32_t new_cap = asm_->const_cap;
    while (new_cap < asm_->const_count + needed) {
        new_cap *= 2;
    }
    vtx_value_t *new_pool = (vtx_value_t *)realloc(asm_->constant_pool,
                                                     new_cap * sizeof(vtx_value_t));
    if (new_pool == NULL) return -1;
    asm_->constant_pool = new_pool;
    asm_->const_cap = new_cap;
    return 0;
}

/* ========================================================================== */
/* Internal: emit bytecode                                                      */
/* ========================================================================== */

static int asm_emit_byte(vtx_assembler_t *asm_, uint8_t byte)
{
    if (asm_ensure_code_cap(asm_, 1) != 0) return -1;
    asm_->code[asm_->code_len++] = byte;
    return 0;
}

static int asm_emit_u16(vtx_assembler_t *asm_, uint16_t val)
{
    if (asm_ensure_code_cap(asm_, 2) != 0) return -1;
    /* Big-endian */
    asm_->code[asm_->code_len++] = (uint8_t)((val >> 8) & 0xFF);
    asm_->code[asm_->code_len++] = (uint8_t)(val & 0xFF);
    return 0;
}

/* ========================================================================== */
/* Opcode name lookup                                                           */
/* ========================================================================== */

/**
 * Find an opcode by name (case-insensitive). Returns VT_OP_COUNT if not found.
 */
static vtx_opcode_t find_opcode(const char *name)
{
    for (int i = 0; i < VT_OP_COUNT; i++) {
        if (strcasecmp(name, vtx_opcode_table[i].name) == 0) {
            return (vtx_opcode_t)i;
        }
    }
    return VT_OP_COUNT;
}

/* ========================================================================== */
/* Init/destroy                                                                 */
/* ========================================================================== */

int vtx_asm_init(vtx_assembler_t *asm_)
{
    asm_->code = (uint8_t *)malloc(VTX_ASM_INIT_CODE_CAP);
    if (asm_->code == NULL) return -1;

    asm_->constant_pool = (vtx_value_t *)malloc(VTX_ASM_INIT_CONST_CAP * sizeof(vtx_value_t));
    if (asm_->constant_pool == NULL) {
        free(asm_->code);
        return -1;
    }

    asm_->code_cap = VTX_ASM_INIT_CODE_CAP;
    asm_->code_len = 0;
    asm_->const_cap = VTX_ASM_INIT_CONST_CAP;
    asm_->const_count = 0;
    asm_->max_locals = 0;
    asm_->max_stack = 16; /* reasonable default */
    asm_->has_error = false;
    asm_->error_msg[0] = '\0';
    asm_->error_line = 0;

    return 0;
}

void vtx_asm_destroy(vtx_assembler_t *asm_)
{
    if (asm_->code != NULL) {
        free(asm_->code);
        asm_->code = NULL;
    }
    if (asm_->constant_pool != NULL) {
        free(asm_->constant_pool);
        asm_->constant_pool = NULL;
    }
    asm_->code_cap = 0;
    asm_->code_len = 0;
    asm_->const_cap = 0;
    asm_->const_count = 0;
}

/* ========================================================================== */
/* Constant pool                                                                */
/* ========================================================================== */

uint16_t vtx_asm_add_const(vtx_assembler_t *asm_, vtx_value_t value)
{
    /* Check for duplicate */
    for (uint32_t i = 0; i < asm_->const_count; i++) {
        if (asm_->constant_pool[i] == value) {
            return (uint16_t)i;
        }
    }

    if (asm_ensure_const_cap(asm_, 1) != 0) {
        return 0xFFFF; /* error */
    }

    uint16_t idx = (uint16_t)asm_->const_count;
    asm_->constant_pool[asm_->const_count++] = value;
    return idx;
}

/* ========================================================================== */
/* Line processing                                                              */
/* ========================================================================== */

/**
 * Trim leading and trailing whitespace. Returns pointer into the input string.
 */
static const char *trim(const char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/**
 * Skip past the opcode name to the operand.
 * Returns pointer to the character after the opcode name (possibly whitespace).
 * Kept for future label/jump-target assembly support.
 */
static inline const char *skip_opcode(const char *s)
    __attribute__((unused));

static inline const char *skip_opcode(const char *s)
{
    while (*s && !isspace((unsigned char)*s)) s++;
    return s;
}

int vtx_asm_line(vtx_assembler_t *asm_, const char *line)
{
    static int line_number = 0;
    line_number++;

    const char *p = trim(line);

    /* Skip empty lines and comments */
    if (*p == '\0' || *p == ';') {
        return 0;
    }

    /* Extract the opcode name */
    char opname[64];
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < (int)sizeof(opname) - 1) {
        opname[i++] = *p++;
    }
    opname[i] = '\0';

    /* Look up the opcode */
    vtx_opcode_t opcode = find_opcode(opname);
    if (opcode == VT_OP_COUNT) {
        snprintf(asm_->error_msg, sizeof(asm_->error_msg),
                 "unknown opcode: '%s'", opname);
        asm_->error_line = line_number;
        asm_->has_error = true;
        return -1;
    }

    const vtx_opcode_info_t *info = &vtx_opcode_table[opcode];

    /* Emit the opcode byte */
    if (asm_emit_byte(asm_, (uint8_t)opcode) != 0) {
        snprintf(asm_->error_msg, sizeof(asm_->error_msg),
                 "code buffer overflow");
        asm_->error_line = line_number;
        asm_->has_error = true;
        return -1;
    }

    /* Handle operand */
    if (info->has_operand) {
        /* Skip whitespace to operand */
        p = trim(p);

        if (*p == '\0') {
            snprintf(asm_->error_msg, sizeof(asm_->error_msg),
                     "opcode '%s' requires an operand", opname);
            asm_->error_line = line_number;
            asm_->has_error = true;
            return -1;
        }

        /* Parse the operand */
        uint16_t operand_val = 0;

        /* Special handling for const-pool opcodes: the operand is a value,
         * not a pool index. We add it to the pool and use the index. */
        if (opcode == VT_OP_LOAD_CONST_INT) {
            /* Parse integer value */
            char *endptr = NULL;
            long long val = strtoll(p, &endptr, 0);
            if (endptr == p) {
                snprintf(asm_->error_msg, sizeof(asm_->error_msg),
                         "invalid integer operand: '%s'", p);
                asm_->error_line = line_number;
                asm_->has_error = true;
                return -1;
            }
            if (val < VTX_SMI_MIN || val > VTX_SMI_MAX) {
                snprintf(asm_->error_msg, sizeof(asm_->error_msg),
                         "integer operand out of SMI range: %lld", val);
                asm_->error_line = line_number;
                asm_->has_error = true;
                return -1;
            }
            uint16_t pool_idx = vtx_asm_add_const(asm_, vtx_make_smi((int64_t)val));
            if (pool_idx == 0xFFFF) return -1;
            operand_val = pool_idx;
        } else if (opcode == VT_OP_LOAD_CONST_FLOAT) {
            char *endptr = NULL;
            double val = strtod(p, &endptr);
            if (endptr == p) {
                snprintf(asm_->error_msg, sizeof(asm_->error_msg),
                         "invalid float operand: '%s'", p);
                asm_->error_line = line_number;
                asm_->has_error = true;
                return -1;
            }
            uint16_t pool_idx = vtx_asm_add_const(asm_, vtx_make_double(val));
            if (pool_idx == 0xFFFF) return -1;
            operand_val = pool_idx;
        } else if (opcode == VT_OP_LOAD_CONST_STR) {
            /* String operand: the rest of the line is the string value */
            const char *str_start = p;
            size_t slen = strlen(str_start);
            /* For simplicity, store the string pointer as a heap pointer constant */
            /* In a real assembler, we'd intern the string. Here we just add a
             * pointer constant. For testing, this is sufficient. */
            uint16_t pool_idx = vtx_asm_add_const(asm_, vtx_make_heap_ptr(
                (void *)(uintptr_t)0xDEAD0000)); /* placeholder */
            (void)str_start; (void)slen;
            operand_val = pool_idx;
        } else {
            /* Standard numeric operand */
            char *endptr = NULL;
            unsigned long val = strtoul(p, &endptr, 0);
            if (endptr == p) {
                snprintf(asm_->error_msg, sizeof(asm_->error_msg),
                         "invalid operand: '%s'", p);
                asm_->error_line = line_number;
                asm_->has_error = true;
                return -1;
            }
            if (val > 0xFFFF) {
                snprintf(asm_->error_msg, sizeof(asm_->error_msg),
                         "operand out of range: %lu", val);
                asm_->error_line = line_number;
                asm_->has_error = true;
                return -1;
            }
            operand_val = (uint16_t)val;
        }

        /* Emit operand based on size */
        if (info->operand_size == 2) {
            if (asm_emit_u16(asm_, operand_val) != 0) return -1;
        } else if (info->operand_size == 1) {
            if (asm_emit_byte(asm_, (uint8_t)(operand_val & 0xFF)) != 0) return -1;
        }

        /* Track max_locals for load_local/store_local */
        if (opcode == VT_OP_LOAD_LOCAL || opcode == VT_OP_STORE_LOCAL) {
            uint16_t local_idx = operand_val;
            if (local_idx + 1 > asm_->max_locals) {
                asm_->max_locals = local_idx + 1;
            }
        }
    }

    return 0;
}

/* ========================================================================== */
/* Emit final bytecode                                                          */
/* ========================================================================== */

vtx_bytecode_t vtx_asm_emit(vtx_assembler_t *asm_)
{
    vtx_bytecode_t bc;
    bc.code = asm_->code;
    bc.length = asm_->code_len;
    bc.constant_pool = asm_->constant_pool;
    bc.constant_count = asm_->const_count;
    bc.max_locals = asm_->max_locals;
    bc.max_stack = asm_->max_stack;
    return bc;
}
