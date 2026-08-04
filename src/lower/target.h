/* ========================================================================== */
/* Target Description — Retargetable RegAlloc Interface                       */
/* ========================================================================== */
/*
 * lower/target.h — Abstract target description for retargetable regalloc.
 *
 * The regalloc queries this interface to learn about the target's register
 * file, calling conventions, and operand constraints. Each backend (x86-64,
 * ARM64, RISC-V) provides a concrete implementation.
 *
 * This is a polymorphic interface: the regalloc holds a TargetDescription*
 * and calls virtual methods. The concrete target is selected at runtime,
 * enabling cross-compilation (e.g., compile for ARM64 on an x86 host).
 *
 * Architecture:
 *
 *   +-------------------+
 *   | TargetDescription  |  (abstract base, virtual methods)
 *   +-------------------+
 *          ^
 *          |
 *   +------+------+
 *   |             |
 *  X86Target   ARM64Target
 *
 * The regalloc uses TargetDescription* and never touches arch-specific
 * code directly. Adding a new architecture = writing a new subclass.
 */

#ifndef VORTEX_TARGET_H
#define VORTEX_TARGET_H

#include <stdint.h>
#include <stdbool.h>

#include "lower/regalloc.h"  /* for vtx_reg_class_t (VTX_REG_CLASS_GPR/XMM/COUNT) */

/* ========================================================================== */
/* Calling convention description                                              */
/* ========================================================================== */

typedef struct {
    /* Argument registers (in order: first arg → arg_regs[0], etc.)
     * For x86-64 System V: RDI, RSI, RDX, RCX, R8, R9
     * For ARM64: X0-X7
     * For RISC-V: a0-a7 (x10-x17)
     * NULL-terminated. */
    const uint8_t *arg_regs;
    uint32_t       arg_reg_count;

    /* Return value register.
     * x86-64: RAX (0), ARM64: X0 (0), RISC-V: a0 (10) */
    uint8_t        return_reg;

    /* Callee-saved registers (must be preserved across calls).
     * Bitmask: bit N set = register N is callee-saved. */
    uint64_t       callee_saved_mask;

    /* Caller-saved registers (clobbered by calls).
     * Bitmask: bit N set = register N is caller-saved. */
    uint64_t       caller_saved_mask;

    /* Reserved registers (RSP, RBP, thread pointer, etc.)
     * The regalloc must never assign these to vregs. */
    uint64_t       reserved_mask;

    /* Stack alignment requirement (in bytes).
     * x86-64: 16, ARM64: 16, RISC-V: 16 */
    uint32_t       stack_alignment;

    /* Return address is stored on stack (x86) vs. in link register (ARM/RISC-V). */
    bool           return_address_on_stack;
} vtx_calling_conv_t;

/* ========================================================================== */
/* Register description                                                         */
/* ========================================================================== */

typedef struct {
    uint8_t  id;          /* register number (0-31) */
    uint8_t  class;       /* vtx_reg_class_t */
    bool     is_callee_saved;
    bool     is_reserved;
    const char *name;     /* "rax", "x0", "a0", etc. for debugging */
} vtx_reg_desc_t;

/* ========================================================================== */
/* Abstract TargetDescription                                                  */
/* ========================================================================== */

typedef struct vtx_target_description vtx_target_description_t;

struct vtx_target_description {
    /* ---- Virtual function table (manual vtable for C compat) ---- */

    /* Return the target name ("x86-64", "arm64", "riscv64"). */
    const char *(*name)(const vtx_target_description_t *self);

    /* Return the pointer size in bytes (8 for 64-bit targets). */
    uint32_t (*pointer_size)(const vtx_target_description_t *self);

    /* Return the number of physical registers in the given class. */
    uint32_t (*reg_count)(const vtx_target_description_t *self,
                            vtx_reg_class_t cls);

    /* Return the register descriptor for a given (class, index).
     * Returns NULL if the index is out of bounds. */
    const vtx_reg_desc_t *(*reg_at)(const vtx_target_description_t *self,
                                      vtx_reg_class_t cls, uint32_t index);

    /* Look up a register by ID. Returns the register class and index.
     * Returns false if the register ID is invalid. */
    bool (*reg_lookup)(const vtx_target_description_t *self,
                         uint8_t reg_id,
                         vtx_reg_class_t *cls_out, uint32_t *index_out);

    /* Return the calling convention descriptor. */
    const vtx_calling_conv_t *(*calling_conv)(const vtx_target_description_t *self);

    /* Return the bitmask of allocatable registers in the given class.
     * (All registers minus reserved.) */
    uint64_t (*allocatable_mask)(const vtx_target_description_t *self,
                                   vtx_reg_class_t cls);

    /* Return the bitmask of registers clobbered by a CALL instruction. */
    uint64_t (*call_clobber_mask)(const vtx_target_description_t *self);

    /* ---- End vtable ---- */

    /* Concrete target data (set by subclass constructor). */
    vtx_reg_class_t  primary_class;   /* GPR for x86/riscv, GPR for arm64 */
    bool              has_sib_byte;   /* x86: true (RSP needs SIB), ARM/RISC-V: false */
    bool              has_complex_addring;  /* x86: true, ARM: false (load/store only) */
};

/* ========================================================================== */
/* Concrete targets                                                            */
/* ========================================================================== */

/* x86-64 (System V ABI). The default target. */
const vtx_target_description_t *vtx_target_x86_64(void);

/* ARM64 (AArch64, AAPCS64 ABI). */
const vtx_target_description_t *vtx_target_arm64(void);

/* RISC-V64 (RV64G, standard ABI). */
const vtx_target_description_t *vtx_target_riscv64(void);

/* ========================================================================== */
/* Convenience accessors (inline, query the vtable)                             */
/* ========================================================================== */

static inline const char *vtx_target_name(const vtx_target_description_t *t) {
    return t->name(t);
}

static inline uint32_t vtx_target_pointer_size(const vtx_target_description_t *t) {
    return t->pointer_size(t);
}

static inline uint32_t vtx_target_reg_count(const vtx_target_description_t *t,
                                              vtx_reg_class_t cls) {
    return t->reg_count(t, cls);
}

static inline const vtx_reg_desc_t *vtx_target_reg_at(
        const vtx_target_description_t *t, vtx_reg_class_t cls, uint32_t idx) {
    return t->reg_at(t, cls, idx);
}

static inline const vtx_calling_conv_t *vtx_target_calling_conv(
        const vtx_target_description_t *t) {
    return t->calling_conv(t);
}

static inline uint64_t vtx_target_allocatable_mask(
        const vtx_target_description_t *t, vtx_reg_class_t cls) {
    return t->allocatable_mask(t, cls);
}

static inline uint64_t vtx_target_call_clobber_mask(
        const vtx_target_description_t *t) {
    return t->call_clobber_mask(t);
}

#endif /* VORTEX_TARGET_H */
