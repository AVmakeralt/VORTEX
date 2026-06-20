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
 *
 * Accepts both the full enum name (e.g., "VT_OP_LOAD_CONST_INT") and the
 * bare opcode name without the "VT_OP_" prefix (e.g., "LOAD_CONST_INT" or
 * "load_const_int"). This makes assembly text much more readable.
 */
static vtx_opcode_t find_opcode(const char *name)
{
    /* First, try a direct case-insensitive match against the full names. */
    for (int i = 0; i < VT_OP_COUNT; i++) {
        if (strcasecmp(name, vtx_opcode_table[i].name) == 0) {
            return (vtx_opcode_t)i;
        }
    }
    /* Then, try matching without the "VT_OP_" prefix. */
    const char *prefix = "VT_OP_";
    size_t plen = strlen(prefix);
    for (int i = 0; i < VT_OP_COUNT; i++) {
        const char *full = vtx_opcode_table[i].name;
        if (strncasecmp(full, prefix, plen) == 0) {
            if (strcasecmp(name, full + plen) == 0) {
                return (vtx_opcode_t)i;
            }
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
    asm_->max_stack_set = false;
    asm_->max_locals_set = false;
    asm_->method_name[0] = '\0';
    asm_->method_sig[0] = '\0';
    asm_->arg_count = 0;
    asm_->label_count = 0;
    asm_->fixup_count = 0;
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
    /* Use a per-call counter instead of a global static so multiple assemblers
     * can run concurrently without line-number cross-talk. */
    int line_number = ++asm_->error_line;

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

/* ========================================================================== */
/* Program mode: multi-line text with labels + pseudo-instructions              */
/* ========================================================================== */

/* Set error state with a printf-style message. */
static void asm_error(vtx_assembler_t *asm_, int line, const char *msg) {
    asm_->has_error = true;
    asm_->error_line = line;
    snprintf(asm_->error_msg, sizeof(asm_->error_msg), "line %d: %s", line, msg);
}

/* Find or create a label by name. Returns its index, or -1 on error. */
static int32_t asm_label_index(vtx_assembler_t *asm_, const char *name) {
    /* Search existing labels */
    for (uint32_t i = 0; i < asm_->label_count; i++) {
        if (strncmp(asm_->labels[i].name, name, VTX_ASM_MAX_LABEL_NAME) == 0) {
            return (int32_t)i;
        }
    }
    /* Create a new (forward-reference) label */
    if (asm_->label_count >= VTX_ASM_MAX_LABELS) {
        return -1;
    }
    uint32_t idx = asm_->label_count++;
    strncpy(asm_->labels[idx].name, name, VTX_ASM_MAX_LABEL_NAME - 1);
    asm_->labels[idx].name[VTX_ASM_MAX_LABEL_NAME - 1] = '\0';
    asm_->labels[idx].pc = 0;
    asm_->labels[idx].defined = false;
    return (int32_t)idx;
}

/* Reset assembler state for a fresh program-mode assembly. */
static void asm_reset(vtx_assembler_t *asm_) {
    asm_->code_len = 0;
    asm_->const_count = 0;
    asm_->max_locals = 0;
    asm_->max_stack = 16;
    asm_->max_stack_set = false;
    asm_->max_locals_set = false;
    asm_->method_name[0] = '\0';
    asm_->method_sig[0] = '\0';
    asm_->arg_count = 0;
    asm_->label_count = 0;
    asm_->fixup_count = 0;
    asm_->has_error = false;
    asm_->error_msg[0] = '\0';
    asm_->error_line = 0;
}

/* Process one line in program mode. Returns 0 on success, -1 on error.
 * Line may be: empty, comment, label definition, pseudo-instruction,
 * or regular opcode + optional operand (which may be a label name
 * for branch instructions). */
static int asm_program_line(vtx_assembler_t *asm_, const char *line, int line_no)
{
    const char *p = trim(line);

    /* Skip empty lines and comments */
    if (*p == '\0' || *p == ';') {
        return 0;
    }

    /* Check for label definition: "name:" */
    {
        char label_name[VTX_ASM_MAX_LABEL_NAME];
        const char *colon = strchr(p, ':');
        if (colon != NULL) {
            size_t name_len = (size_t)(colon - p);
            /* Trim trailing whitespace before colon */
            while (name_len > 0 && isspace((unsigned char)p[name_len - 1])) {
                name_len--;
            }
            /* Trim leading whitespace (already trimmed, but be safe) */
            size_t start = 0;
            while (start < name_len && isspace((unsigned char)p[start])) {
                start++;
            }
            name_len -= start;
            if (name_len > 0 && name_len < VTX_ASM_MAX_LABEL_NAME) {
                memcpy(label_name, p + start, name_len);
                label_name[name_len] = '\0';
                /* Verify it's a valid identifier (starts with letter or _,
                 * contains only alnum or _) */
                bool valid = (isalpha((unsigned char)label_name[0]) || label_name[0] == '_');
                for (size_t i = 1; i < name_len && valid; i++) {
                    if (!isalnum((unsigned char)label_name[i]) && label_name[i] != '_') {
                        valid = false;
                    }
                }
                if (valid) {
                    int32_t idx = asm_label_index(asm_, label_name);
                    if (idx < 0) {
                        asm_error(asm_, line_no, "too many labels");
                        return -1;
                    }
                    if (asm_->labels[idx].defined) {
                        asm_error(asm_, line_no, "label already defined");
                        return -1;
                    }
                    asm_->labels[idx].defined = true;
                    asm_->labels[idx].pc = (uint16_t)asm_->code_len;
                    /* Continue processing the rest of the line after the colon,
                     * in case there's an instruction on the same line. */
                    p = trim(colon + 1);
                    if (*p == '\0' || *p == ';') {
                        return 0;
                    }
                    /* fall through to instruction parsing */
                }
            }
        }
    }

    /* Check for pseudo-instruction (starts with '.') */
    if (*p == '.') {
        char opname[32];
        int i = 0;
        while (*p && !isspace((unsigned char)*p) && i < (int)sizeof(opname) - 1) {
            opname[i++] = *p++;
        }
        opname[i] = '\0';
        p = trim(p);

        if (strcasecmp(opname, ".max_locals") == 0) {
            char *end = NULL;
            long v = strtol(p, &end, 0);
            if (end == p || v < 0 || v > 65535) {
                asm_error(asm_, line_no, "invalid .max_locals operand");
                return -1;
            }
            asm_->max_locals = (uint16_t)v;
            asm_->max_locals_set = true;
            return 0;
        }
        if (strcasecmp(opname, ".max_stack") == 0) {
            char *end = NULL;
            long v = strtol(p, &end, 0);
            if (end == p || v < 0 || v > 65535) {
                asm_error(asm_, line_no, "invalid .max_stack operand");
                return -1;
            }
            asm_->max_stack = (uint16_t)v;
            asm_->max_stack_set = true;
            return 0;
        }
        if (strcasecmp(opname, ".arg_count") == 0) {
            char *end = NULL;
            long v = strtol(p, &end, 0);
            if (end == p || v < 0 || v > 65535) {
                asm_error(asm_, line_no, "invalid .arg_count operand");
                return -1;
            }
            asm_->arg_count = (uint16_t)v;
            return 0;
        }
        if (strcasecmp(opname, ".method") == 0) {
            /* .method name signature */
            char name[64], sig[64];
            if (sscanf(p, "%63s %63s", name, sig) != 2) {
                asm_error(asm_, line_no, ".method requires name and signature");
                return -1;
            }
            strncpy(asm_->method_name, name, sizeof(asm_->method_name) - 1);
            strncpy(asm_->method_sig, sig, sizeof(asm_->method_sig) - 1);
            return 0;
        }
        if (strcasecmp(opname, ".const") == 0) {
            /* .const int 42 | .const float 3.14 | .const str hello */
            char type[16];
            char value[128];
            if (sscanf(p, "%15s %127s", type, value) != 2) {
                asm_error(asm_, line_no, ".const requires type and value");
                return -1;
            }
            if (strcasecmp(type, "int") == 0) {
                char *end = NULL;
                long long v = strtoll(value, &end, 0);
                if (end == value) {
                    asm_error(asm_, line_no, "invalid int constant");
                    return -1;
                }
                if (v < VTX_SMI_MIN || v > VTX_SMI_MAX) {
                    asm_error(asm_, line_no, "int constant out of SMI range");
                    return -1;
                }
                vtx_asm_add_const(asm_, vtx_make_smi((int64_t)v));
                return 0;
            }
            if (strcasecmp(type, "float") == 0) {
                char *end = NULL;
                double v = strtod(value, &end);
                if (end == value) {
                    asm_error(asm_, line_no, "invalid float constant");
                    return -1;
                }
                vtx_asm_add_const(asm_, vtx_make_double(v));
                return 0;
            }
            if (strcasecmp(type, "str") == 0) {
                /* Store the string pointer as a heap pointer constant.
                 * Note: this allocates a string on the heap that is never
                 * freed; for testing/assembling this is acceptable. */
                size_t slen = strlen(value);
                char *str = (char *)malloc(slen + 1);
                if (str == NULL) {
                    asm_error(asm_, line_no, "out of memory for string constant");
                    return -1;
                }
                memcpy(str, value, slen + 1);
                vtx_asm_add_const(asm_, vtx_make_heap_ptr(str));
                return 0;
            }
            asm_error(asm_, line_no, "unknown .const type");
            return -1;
        }
        asm_error(asm_, line_no, "unknown pseudo-instruction");
        return -1;
    }

    /* Regular instruction: parse opcode + optional operand. */
    char opname[64];
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < (int)sizeof(opname) - 1) {
        opname[i++] = *p++;
    }
    opname[i] = '\0';

    vtx_opcode_t opcode = find_opcode(opname);
    if (opcode == VT_OP_COUNT) {
        asm_error(asm_, line_no, "unknown opcode");
        return -1;
    }
    const vtx_opcode_info_t *info = &vtx_opcode_table[opcode];

    /* Emit opcode byte */
    if (asm_emit_byte(asm_, (uint8_t)opcode) != 0) {
        asm_error(asm_, line_no, "code buffer overflow");
        return -1;
    }

    if (!info->has_operand) {
        return 0;
    }

    /* Parse operand */
    p = trim(p);
    if (*p == '\0') {
        asm_error(asm_, line_no, "missing operand");
        return -1;
    }

    /* For branch instructions (GOTO, IF_TRUE, IF_FALSE), the operand may
     * be either a label name or a literal PC. We record a fixup for label
     * references; literal PCs are emitted directly. */
    if (opcode == VT_OP_GOTO || opcode == VT_OP_IF_TRUE || opcode == VT_OP_IF_FALSE) {
        /* Try to parse as a number first. */
        char *end = NULL;
        long v = strtol(p, &end, 0);
        if (end != p) {
            /* Literal PC */
            if (v < 0 || v > 0xFFFF) {
                asm_error(asm_, line_no, "branch target out of range");
                return -1;
            }
            if (asm_emit_u16(asm_, (uint16_t)v) != 0) {
                asm_error(asm_, line_no, "code buffer overflow");
                return -1;
            }
        } else {
            /* Label reference — record fixup, emit placeholder. */
            char label_name[VTX_ASM_MAX_LABEL_NAME];
            int li = 0;
            while (*p && !isspace((unsigned char)*p) && li < (int)sizeof(label_name) - 1) {
                label_name[li++] = *p++;
            }
            label_name[li] = '\0';
            int32_t idx = asm_label_index(asm_, label_name);
            if (idx < 0) {
                asm_error(asm_, line_no, "too many labels");
                return -1;
            }
            if (asm_->fixup_count >= VTX_ASM_MAX_FIXUPS) {
                asm_error(asm_, line_no, "too many fixups");
                return -1;
            }
            uint16_t fixup_pc = (uint16_t)asm_->code_len;
            /* Emit placeholder 0x0000 — will be patched in pass 2. */
            if (asm_emit_u16(asm_, 0) != 0) {
                asm_error(asm_, line_no, "code buffer overflow");
                return -1;
            }
            asm_->fixups[asm_->fixup_count].fixup_pc = fixup_pc;
            asm_->fixups[asm_->fixup_count].label_idx = (uint16_t)idx;
            asm_->fixups[asm_->fixup_count].branch_insn_pc = (uint16_t)(fixup_pc - 1);
            asm_->fixup_count++;
        }
        return 0;
    }

    /* Non-branch operand: const-pool opcodes get special handling. */
    if (opcode == VT_OP_LOAD_CONST_INT) {
        char *end = NULL;
        long long v = strtoll(p, &end, 0);
        if (end == p) {
            asm_error(asm_, line_no, "invalid int operand");
            return -1;
        }
        if (v < VTX_SMI_MIN || v > VTX_SMI_MAX) {
            asm_error(asm_, line_no, "int out of SMI range");
            return -1;
        }
        uint16_t idx = vtx_asm_add_const(asm_, vtx_make_smi((int64_t)v));
        if (idx == 0xFFFF) {
            asm_error(asm_, line_no, "const pool full");
            return -1;
        }
        if (asm_emit_u16(asm_, idx) != 0) {
            asm_error(asm_, line_no, "code buffer overflow");
            return -1;
        }
    } else if (opcode == VT_OP_LOAD_CONST_FLOAT) {
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p) {
            asm_error(asm_, line_no, "invalid float operand");
            return -1;
        }
        uint16_t idx = vtx_asm_add_const(asm_, vtx_make_double(v));
        if (idx == 0xFFFF) {
            asm_error(asm_, line_no, "const pool full");
            return -1;
        }
        if (asm_emit_u16(asm_, idx) != 0) {
            asm_error(asm_, line_no, "code buffer overflow");
            return -1;
        }
    } else {
        /* Generic numeric operand */
        char *end = NULL;
        unsigned long v = strtoul(p, &end, 0);
        if (end == p) {
            asm_error(asm_, line_no, "invalid operand");
            return -1;
        }
        if (v > 0xFFFF) {
            asm_error(asm_, line_no, "operand out of range");
            return -1;
        }
        if (info->operand_size == 2) {
            if (asm_emit_u16(asm_, (uint16_t)v) != 0) {
                asm_error(asm_, line_no, "code buffer overflow");
                return -1;
            }
        } else if (info->operand_size == 1) {
            if (asm_emit_byte(asm_, (uint8_t)(v & 0xFF)) != 0) {
                asm_error(asm_, line_no, "code buffer overflow");
                return -1;
            }
        }
        /* Track max_locals */
        if (opcode == VT_OP_LOAD_LOCAL || opcode == VT_OP_STORE_LOCAL) {
            if ((uint16_t)v + 1 > asm_->max_locals) {
                asm_->max_locals = (uint16_t)v + 1;
            }
        }
    }

    return 0;
}

int vtx_asm_program(vtx_assembler_t *asm_, const char *text)
{
    if (asm_ == NULL || text == NULL) return -1;

    /* Reset for a fresh assembly. */
    asm_reset(asm_);

    /* Single-pass assembly with forward-reference fixups.
     * We process each line, recording label definitions as we encounter
     * them and queuing fixups for forward references. After all lines
     * are processed, we patch the fixups. */
    const char *line_start = text;
    int line_no = 0;
    while (*line_start != '\0') {
        line_no++;
        /* Find end of line */
        const char *line_end = line_start;
        while (*line_end != '\0' && *line_end != '\n') {
            line_end++;
        }
        /* Copy line into a buffer */
        size_t line_len = (size_t)(line_end - line_start);
        char line_buf[512];
        if (line_len >= sizeof(line_buf)) {
            line_len = sizeof(line_buf) - 1;
        }
        memcpy(line_buf, line_start, line_len);
        line_buf[line_len] = '\0';

        if (asm_program_line(asm_, line_buf, line_no) != 0) {
            return -1;
        }

        if (*line_end == '\0') break;
        line_start = line_end + 1;
    }

    /* Pass 2: patch all forward-reference fixups. */
    for (uint32_t i = 0; i < asm_->fixup_count; i++) {
        uint32_t li = asm_->fixups[i].label_idx;
        if (li >= asm_->label_count) {
            asm_error(asm_, 0, "internal: fixup label index out of range");
            return -1;
        }
        if (!asm_->labels[li].defined) {
            snprintf(asm_->error_msg, sizeof(asm_->error_msg),
                     "undefined label: '%s'", asm_->labels[li].name);
            asm_->has_error = true;
            return -1;
        }
        uint16_t target = asm_->labels[li].pc;
        uint16_t fpc = asm_->fixups[i].fixup_pc;
        if ((size_t)fpc + 1 >= asm_->code_len) {
            asm_error(asm_, 0, "internal: fixup PC out of range");
            return -1;
        }
        asm_->code[fpc]     = (uint8_t)((target >> 8) & 0xFF);
        asm_->code[fpc + 1] = (uint8_t)(target & 0xFF);
    }

    /* If max_stack wasn't explicitly set, use the abstract interpreter
     * provided by the bytecode module. */
    if (!asm_->max_stack_set) {
        vtx_bytecode_t bc = vtx_asm_emit(asm_);
        uint32_t computed = vtx_bytecode_compute_max_stack(&bc, asm_->max_locals);
        if (computed > asm_->max_stack) {
            asm_->max_stack = (uint16_t)computed;
        }
    }

    return 0;
}

uint16_t vtx_asm_label_pc(const vtx_assembler_t *asm_, const char *name)
{
    for (uint32_t i = 0; i < asm_->label_count; i++) {
        if (strncmp(asm_->labels[i].name, name, VTX_ASM_MAX_LABEL_NAME) == 0) {
            return asm_->labels[i].pc;
        }
    }
    return 0xFFFF;
}
