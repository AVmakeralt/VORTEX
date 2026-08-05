/* ========================================================================== */
/* x86-64 Target Description                                                  */
/* ========================================================================== */

#include "lower/target.h"
#include "lower/regalloc.h"  /* for VTX_CALLER_SAVED_MASK, VTX_CALLEE_SAVED_MASK, VTX_REG_RESERVED_MASK */

/* ---- Register descriptors ---- */

static const vtx_reg_desc_t x86_gpr_descs[] = {
    { 0,  VTX_REG_CLASS_GPR, false, false, "rax" },
    { 1,  VTX_REG_CLASS_GPR, false, false, "rcx" },
    { 2,  VTX_REG_CLASS_GPR, false, false, "rdx" },
    { 3,  VTX_REG_CLASS_GPR, true,  false, "rbx" },
    { 4,  VTX_REG_CLASS_GPR, false, true,  "rsp" },
    { 5,  VTX_REG_CLASS_GPR, false, true,  "rbp" },
    { 6,  VTX_REG_CLASS_GPR, false, false, "rsi" },
    { 7,  VTX_REG_CLASS_GPR, false, false, "rdi" },
    { 8,  VTX_REG_CLASS_GPR, false, false, "r8"  },
    { 9,  VTX_REG_CLASS_GPR, false, false, "r9"  },
    { 10, VTX_REG_CLASS_GPR, false, false, "r10" },
    { 11, VTX_REG_CLASS_GPR, false, false, "r11" },
    { 12, VTX_REG_CLASS_GPR, true,  false, "r12" },
    { 13, VTX_REG_CLASS_GPR, true,  false, "r13" },
    { 14, VTX_REG_CLASS_GPR, true,  false, "r14" },
    { 15, VTX_REG_CLASS_GPR, true,  false, "r15" },
};
static const uint32_t x86_gpr_count = sizeof(x86_gpr_descs) / sizeof(x86_gpr_descs[0]);

static const vtx_reg_desc_t x86_xmm_descs[] = {
    { 0,  VTX_REG_CLASS_XMM, false, false, "xmm0"  },
    { 1,  VTX_REG_CLASS_XMM, false, false, "xmm1"  },
    { 2,  VTX_REG_CLASS_XMM, false, false, "xmm2"  },
    { 3,  VTX_REG_CLASS_XMM, false, false, "xmm3"  },
    { 4,  VTX_REG_CLASS_XMM, false, false, "xmm4"  },
    { 5,  VTX_REG_CLASS_XMM, false, false, "xmm5"  },
    { 6,  VTX_REG_CLASS_XMM, false, false, "xmm6"  },
    { 7,  VTX_REG_CLASS_XMM, false, false, "xmm7"  },
    { 8,  VTX_REG_CLASS_XMM, false, false, "xmm8"  },
    { 9,  VTX_REG_CLASS_XMM, false, false, "xmm9"  },
    { 10, VTX_REG_CLASS_XMM, false, false, "xmm10" },
    { 11, VTX_REG_CLASS_XMM, false, false, "xmm11" },
    { 12, VTX_REG_CLASS_XMM, false, false, "xmm12" },
    { 13, VTX_REG_CLASS_XMM, false, false, "xmm13" },
    { 14, VTX_REG_CLASS_XMM, false, false, "xmm14" },
    { 15, VTX_REG_CLASS_XMM, false, false, "xmm15" },
};
static const uint32_t x86_xmm_count = sizeof(x86_xmm_descs) / sizeof(x86_xmm_descs[0]);

/* ---- Calling convention (System V AMD64) ---- */

static const uint8_t x86_arg_regs[] = { 7, 6, 2, 1, 8, 9 };  /* RDI, RSI, RDX, RCX, R8, R9 */
static const vtx_calling_conv_t x86_cc = {
    .arg_regs                = x86_arg_regs,
    .arg_reg_count           = 6,
    .return_reg              = 0,  /* RAX */
    .callee_saved_mask       = VTX_CALLEE_SAVED_MASK,
    .caller_saved_mask       = VTX_CALLER_SAVED_MASK,
    .reserved_mask           = VTX_REG_RESERVED_MASK,
    .stack_alignment         = 16,
    .return_address_on_stack = true,
};

/* ---- Virtual function implementations ---- */

static const char *x86_name(const vtx_target_description_t *self) {
    (void)self;
    return "x86-64";
}

static uint32_t x86_pointer_size(const vtx_target_description_t *self) {
    (void)self;
    return 8;
}

static uint32_t x86_reg_count(const vtx_target_description_t *self,
                                vtx_reg_class_t cls) {
    (void)self;
    switch (cls) {
    case VTX_REG_CLASS_GPR: return x86_gpr_count;
    case VTX_REG_CLASS_XMM: return x86_xmm_count;
    default: return 0;
    }
}

static const vtx_reg_desc_t *x86_reg_at(const vtx_target_description_t *self,
                                          vtx_reg_class_t cls, uint32_t idx) {
    (void)self;
    switch (cls) {
    case VTX_REG_CLASS_GPR:
        return (idx < x86_gpr_count) ? &x86_gpr_descs[idx] : NULL;
    case VTX_REG_CLASS_XMM:
        return (idx < x86_xmm_count) ? &x86_xmm_descs[idx] : NULL;
    default:
        return NULL;
    }
}

static bool x86_reg_lookup(const vtx_target_description_t *self,
                            uint8_t reg_id, vtx_reg_class_t *cls_out,
                            uint32_t *idx_out) {
    (void)self;
    /* Search GPRs */
    for (uint32_t i = 0; i < x86_gpr_count; i++) {
        if (x86_gpr_descs[i].id == reg_id) {
            *cls_out = VTX_REG_CLASS_GPR;
            *idx_out = i;
            return true;
        }
    }
    /* Search XMMs */
    for (uint32_t i = 0; i < x86_xmm_count; i++) {
        if (x86_xmm_descs[i].id == reg_id) {
            *cls_out = VTX_REG_CLASS_XMM;
            *idx_out = i;
            return true;
        }
    }
    return false;
}

static const vtx_calling_conv_t *x86_calling_conv(const vtx_target_description_t *self) {
    (void)self;
    return &x86_cc;
}

static uint64_t x86_allocatable_mask(const vtx_target_description_t *self,
                                       vtx_reg_class_t cls) {
    (void)self;
    if (cls == VTX_REG_CLASS_GPR) {
        /* All GPRs except reserved (RSP, RBP, and R12/R13 for spill scratch) */
        return VTX_CALLER_SAVED_MASK | VTX_CALLEE_SAVED_MASK & ~VTX_REG_RESERVED_MASK;
    }
    if (cls == VTX_REG_CLASS_XMM) {
        return VTX_XMM_ALL_MASK;  /* all 16 XMM regs */
    }
    return 0;
}

static uint64_t x86_call_clobber_mask(const vtx_target_description_t *self) {
    (void)self;
    return VTX_CALLER_SAVED_MASK;
}

/* ---- Target instance ---- */

static const vtx_target_description_t x86_target = {
    .name                   = x86_name,
    .pointer_size           = x86_pointer_size,
    .reg_count              = x86_reg_count,
    .reg_at                 = x86_reg_at,
    .reg_lookup             = x86_reg_lookup,
    .calling_conv           = x86_calling_conv,
    .allocatable_mask       = x86_allocatable_mask,
    .call_clobber_mask      = x86_call_clobber_mask,
    .primary_class          = VTX_REG_CLASS_GPR,
    .has_sib_byte           = true,    /* x86 RSP addressing needs SIB */
    .has_complex_addring   = true,    /* x86 has [base+index*scale+disp] */
};

const vtx_target_description_t *vtx_target_x86_64(void) {
    return &x86_target;
}

/* ========================================================================== */
/* ARM64 Target Description (stub — register layout only, no codegen)       */
/* ========================================================================== */

static const vtx_reg_desc_t arm64_gpr_descs[] = {
    { 0,  VTX_REG_CLASS_GPR, false, false, "x0"  },
    { 1,  VTX_REG_CLASS_GPR, false, false, "x1"  },
    { 2,  VTX_REG_CLASS_GPR, false, false, "x2"  },
    { 3,  VTX_REG_CLASS_GPR, false, false, "x3"  },
    { 4,  VTX_REG_CLASS_GPR, false, false, "x4"  },
    { 5,  VTX_REG_CLASS_GPR, false, false, "x5"  },
    { 6,  VTX_REG_CLASS_GPR, false, false, "x6"  },
    { 7,  VTX_REG_CLASS_GPR, false, false, "x7"  },
    { 8,  VTX_REG_CLASS_GPR, true,  false, "x8"  },  /* indirect result reg */
    { 9,  VTX_REG_CLASS_GPR, true,  false, "x9"  },
    { 10, VTX_REG_CLASS_GPR, true,  false, "x10" },
    { 11, VTX_REG_CLASS_GPR, true,  false, "x11" },
    { 12, VTX_REG_CLASS_GPR, true,  false, "x12" },
    { 13, VTX_REG_CLASS_GPR, true,  false, "x13" },
    { 14, VTX_REG_CLASS_GPR, true,  false, "x14" },
    { 15, VTX_REG_CLASS_GPR, true,  false, "x15" },
    { 16, VTX_REG_CLASS_GPR, false, false, "x16" },  /* IP0 (intra-proc call) */
    { 17, VTX_REG_CLASS_GPR, false, false, "x17" },  /* IP1 */
    { 18, VTX_REG_CLASS_GPR, true,  false, "x18" },  /* platform reg */
    /* x19-x28: callee-saved */
    { 19, VTX_REG_CLASS_GPR, true,  false, "x19" },
    { 20, VTX_REG_CLASS_GPR, true,  false, "x20" },
    { 21, VTX_REG_CLASS_GPR, true,  false, "x21" },
    { 22, VTX_REG_CLASS_GPR, true,  false, "x22" },
    { 23, VTX_REG_CLASS_GPR, true,  false, "x23" },
    { 24, VTX_REG_CLASS_GPR, true,  false, "x24" },
    { 25, VTX_REG_CLASS_GPR, true,  false, "x25" },
    { 26, VTX_REG_CLASS_GPR, true,  false, "x26" },
    { 27, VTX_REG_CLASS_GPR, true,  false, "x27" },
    { 28, VTX_REG_CLASS_GPR, true,  false, "x28" },
    { 29, VTX_REG_CLASS_GPR, false, true,  "fp"  },  /* frame pointer */
    { 30, VTX_REG_CLASS_GPR, false, false, "lr"  },  /* link register */
    { 31, VTX_REG_CLASS_GPR, false, true,  "sp"  },  /* stack pointer (zr/sp) */
};
static const uint32_t arm64_gpr_count = sizeof(arm64_gpr_descs) / sizeof(arm64_gpr_descs[0]);

/* ARM64 calling convention (AAPCS64):
 *   Args: x0-x7 (8 regs), return: x0, callee-saved: x19-x28, fp
 *   Return address in lr (x30), NOT on stack. */
static const uint8_t arm64_arg_regs[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static const vtx_calling_conv_t arm64_cc = {
    .arg_regs                = arm64_arg_regs,
    .arg_reg_count           = 8,
    .return_reg              = 0,  /* x0 */
    .callee_saved_mask       = (1u << 19) | (1u << 20) | (1u << 21) | (1u << 22) |
                               (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26) |
                               (1u << 27) | (1u << 28),  /* x19-x28 */
    .caller_saved_mask       = (1u << 0)  | (1u << 1)  | (1u << 2)  | (1u << 3) |
                               (1u << 4)  | (1u << 5)  | (1u << 6)  | (1u << 7) |
                               (1u << 8)  | (1u << 9)  | (1u << 10) | (1u << 11) |
                               (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15) |
                               (1u << 16) | (1u << 17) | (1u << 18) | (1u << 30),
    .reserved_mask           = (1u << 29) | (1u << 31),  /* fp, sp */
    .stack_alignment         = 16,
    .return_address_on_stack = false,  /* return address in lr (x30) */
};

/* ARM64 vtable */
static const char *arm64_name(const vtx_target_description_t *self) {
    (void)self; return "arm64";
}
static uint32_t arm64_pointer_size(const vtx_target_description_t *self) {
    (void)self; return 8;
}
static uint32_t arm64_reg_count(const vtx_target_description_t *self,
                                  vtx_reg_class_t cls) {
    (void)self;
    return (cls == VTX_REG_CLASS_GPR) ? arm64_gpr_count : 0;
}
static const vtx_reg_desc_t *arm64_reg_at(const vtx_target_description_t *self,
                                             vtx_reg_class_t cls, uint32_t idx) {
    (void)self;
    if (cls == VTX_REG_CLASS_GPR && idx < arm64_gpr_count)
        return &arm64_gpr_descs[idx];
    return NULL;
}
static bool arm64_reg_lookup(const vtx_target_description_t *self,
                               uint8_t reg_id, vtx_reg_class_t *cls_out,
                               uint32_t *idx_out) {
    (void)self;
    for (uint32_t i = 0; i < arm64_gpr_count; i++) {
        if (arm64_gpr_descs[i].id == reg_id) {
            *cls_out = VTX_REG_CLASS_GPR;
            *idx_out = i;
            return true;
        }
    }
    return false;
}
static const vtx_calling_conv_t *arm64_calling_conv(const vtx_target_description_t *self) {
    (void)self; return &arm64_cc;
}
static uint64_t arm64_allocatable_mask(const vtx_target_description_t *self,
                                          vtx_reg_class_t cls) {
    (void)self;
    if (cls != VTX_REG_CLASS_GPR) return 0;
    /* All 32 GPRs minus reserved (fp, sp) */
    return ~arm64_cc.reserved_mask;
}
static uint64_t arm64_call_clobber_mask(const vtx_target_description_t *self) {
    (void)self;
    return arm64_cc.caller_saved_mask;
}

static const vtx_target_description_t arm64_target = {
    .name                   = arm64_name,
    .pointer_size           = arm64_pointer_size,
    .reg_count              = arm64_reg_count,
    .reg_at                 = arm64_reg_at,
    .reg_lookup             = arm64_reg_lookup,
    .calling_conv           = arm64_calling_conv,
    .allocatable_mask       = arm64_allocatable_mask,
    .call_clobber_mask      = arm64_call_clobber_mask,
    .primary_class          = VTX_REG_CLASS_GPR,
    .has_sib_byte           = false,   /* ARM64 has no SIB */
    .has_complex_addring   = false,   /* load/store architecture */
};

const vtx_target_description_t *vtx_target_arm64(void) {
    return &arm64_target;
}

/* ========================================================================== */
/* RISC-V64 Target Description (stub — register layout only, no codegen)     */
/* ========================================================================== */

/* RISC-V RV64G has 32 integer registers (x0-x31).
 * x0 is always zero (hardwired).
 * ABI names: zero, ra, sp, gp, tp, t0-t2, s0-s1, a0-a7, s2-s11, t3-t6 */
static const vtx_reg_desc_t riscv64_gpr_descs[] = {
    { 0,  VTX_REG_CLASS_GPR, false, true,  "zero" },  /* always 0 */
    { 1,  VTX_REG_CLASS_GPR, false, false, "ra"   },  /* return address */
    { 2,  VTX_REG_CLASS_GPR, false, true,  "sp"   },  /* stack pointer */
    { 3,  VTX_REG_CLASS_GPR, false, true,  "gp"   },  /* global pointer */
    { 4,  VTX_REG_CLASS_GPR, false, true,  "tp"   },  /* thread pointer */
    { 5,  VTX_REG_CLASS_GPR, false, false, "t0"   },  /* temp */
    { 6,  VTX_REG_CLASS_GPR, false, false, "t1"   },
    { 7,  VTX_REG_CLASS_GPR, false, false, "t2"   },
    { 8,  VTX_REG_CLASS_GPR, true,  false, "s0"   },  /* callee-saved / fp */
    { 9,  VTX_REG_CLASS_GPR, true,  false, "s1"   },
    { 10, VTX_REG_CLASS_GPR, false, false, "a0"   },  /* arg0 / return */
    { 11, VTX_REG_CLASS_GPR, false, false, "a1"   },
    { 12, VTX_REG_CLASS_GPR, false, false, "a2"   },
    { 13, VTX_REG_CLASS_GPR, false, false, "a3"   },
    { 14, VTX_REG_CLASS_GPR, false, false, "a4"   },
    { 15, VTX_REG_CLASS_GPR, false, false, "a5"   },
    { 16, VTX_REG_CLASS_GPR, false, false, "a6"   },
    { 17, VTX_REG_CLASS_GPR, false, false, "a7"   },
    { 18, VTX_REG_CLASS_GPR, true,  false, "s2"   },
    { 19, VTX_REG_CLASS_GPR, true,  false, "s3"   },
    { 20, VTX_REG_CLASS_GPR, true,  false, "s4"   },
    { 21, VTX_REG_CLASS_GPR, true,  false, "s5"   },
    { 22, VTX_REG_CLASS_GPR, true,  false, "s6"   },
    { 23, VTX_REG_CLASS_GPR, true,  false, "s7"   },
    { 24, VTX_REG_CLASS_GPR, true,  false, "s8"   },
    { 25, VTX_REG_CLASS_GPR, true,  false, "s9"   },
    { 26, VTX_REG_CLASS_GPR, true,  false, "s10"  },
    { 27, VTX_REG_CLASS_GPR, true,  false, "s11"  },
    { 28, VTX_REG_CLASS_GPR, false, false, "t3"   },
    { 29, VTX_REG_CLASS_GPR, false, false, "t4"   },
    { 30, VTX_REG_CLASS_GPR, false, false, "t5"   },
    { 31, VTX_REG_CLASS_GPR, false, false, "t6"   },
};
static const uint32_t riscv64_gpr_count = sizeof(riscv64_gpr_descs) / sizeof(riscv64_gpr_descs[0]);

/* RISC-V calling convention:
 *   Args: a0-a7 (x10-x17, 8 regs), return: a0, callee-saved: s0-s11
 *   Return address in ra (x1), NOT on stack. */
static const uint8_t riscv64_arg_regs[] = { 10, 11, 12, 13, 14, 15, 16, 17 };
static const vtx_calling_conv_t riscv64_cc = {
    .arg_regs                = riscv64_arg_regs,
    .arg_reg_count           = 8,
    .return_reg              = 10,  /* a0 */
    .callee_saved_mask       = (1u << 8)  | (1u << 9)  | (1u << 18) | (1u << 19) |
                               (1u << 20) | (1u << 21) | (1u << 22) | (1u << 23) |
                               (1u << 24) | (1u << 25) | (1u << 26) | (1u << 27),
    .caller_saved_mask       = (1u << 1)  | (1u << 5)  | (1u << 6)  | (1u << 7)  |
                               (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13) |
                               (1u << 14) | (1u << 15) | (1u << 16) | (1u << 17) |
                               (1u << 28) | (1u << 29) | (1u << 30) | (1u << 31),
    .reserved_mask           = (1u << 0)  | (1u << 2)  | (1u << 3)  | (1u << 4),
    .stack_alignment         = 16,
    .return_address_on_stack = false,  /* return address in ra (x1) */
};

/* RISC-V vtable */
static const char *riscv64_name(const vtx_target_description_t *self) {
    (void)self; return "riscv64";
}
static uint32_t riscv64_pointer_size(const vtx_target_description_t *self) {
    (void)self; return 8;
}
static uint32_t riscv64_reg_count(const vtx_target_description_t *self,
                                    vtx_reg_class_t cls) {
    (void)self;
    return (cls == VTX_REG_CLASS_GPR) ? riscv64_gpr_count : 0;
}
static const vtx_reg_desc_t *riscv64_reg_at(const vtx_target_description_t *self,
                                               vtx_reg_class_t cls, uint32_t idx) {
    (void)self;
    if (cls == VTX_REG_CLASS_GPR && idx < riscv64_gpr_count)
        return &riscv64_gpr_descs[idx];
    return NULL;
}
static bool riscv64_reg_lookup(const vtx_target_description_t *self,
                                 uint8_t reg_id, vtx_reg_class_t *cls_out,
                                 uint32_t *idx_out) {
    (void)self;
    for (uint32_t i = 0; i < riscv64_gpr_count; i++) {
        if (riscv64_gpr_descs[i].id == reg_id) {
            *cls_out = VTX_REG_CLASS_GPR;
            *idx_out = i;
            return true;
        }
    }
    return false;
}
static const vtx_calling_conv_t *riscv64_calling_conv(const vtx_target_description_t *self) {
    (void)self; return &riscv64_cc;
}
static uint64_t riscv64_allocatable_mask(const vtx_target_description_t *self,
                                            vtx_reg_class_t cls) {
    (void)self;
    if (cls != VTX_REG_CLASS_GPR) return 0;
    /* All 32 regs minus reserved (zero, sp, gp, tp) */
    return ~riscv64_cc.reserved_mask;
}
static uint64_t riscv64_call_clobber_mask(const vtx_target_description_t *self) {
    (void)self;
    return riscv64_cc.caller_saved_mask;
}

static const vtx_target_description_t riscv64_target = {
    .name                   = riscv64_name,
    .pointer_size           = riscv64_pointer_size,
    .reg_count              = riscv64_reg_count,
    .reg_at                 = riscv64_reg_at,
    .reg_lookup             = riscv64_reg_lookup,
    .calling_conv           = riscv64_calling_conv,
    .allocatable_mask       = riscv64_allocatable_mask,
    .call_clobber_mask      = riscv64_call_clobber_mask,
    .primary_class          = VTX_REG_CLASS_GPR,
    .has_sib_byte           = false,
    .has_complex_addring   = false,
};

const vtx_target_description_t *vtx_target_riscv64(void) {
    return &riscv64_target;
}
