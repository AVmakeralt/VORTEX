#ifndef VORTEX_CODECACHE_TYPES_H
#define VORTEX_CODECACHE_TYPES_H

#include <stdint.h>
#include "vortex_config.h"
#include "runtime/type_system.h"
#include "baseline/frame_layout.h"
#include "baseline/guards.h"
#include "baseline/deopt_stubs.h"
#include "deopt/side_table.h"
#include "deopt/types.h"

/* Forward declaration — vtx_poly_ic_t is defined in baseline/codegen.h */
struct vtx_poly_ic;
typedef struct vtx_poly_ic vtx_poly_ic_t;

/**
 * VORTEX Code Cache — Shared Compiled Code Type Definitions
 *
 * This header contains the canonical definition of vtx_compiled_code_t,
 * which is used by both the baseline JIT code generator and the deopt/OSR
 * subsystems.
 */

/* ========================================================================== */
/* Bytecode-to-native PC mapping entry                                         */
/* ========================================================================== */

typedef struct {
    uint32_t bytecode_pc;    /* bytecode PC */
    uint32_t native_offset;  /* corresponding native code offset */
    uint32_t stack_depth;    /* expression stack depth at this bytecode boundary */
} vtx_bc_pc_map_entry_t;

/* ========================================================================== */
/* OSR entry register convention (OSR-33)                                      */
/* ========================================================================== */

/**
 * Identifies which physical registers the JIT entry point expects the
 * top of the operand stack in. The OSR-up trampoline must populate the
 * same registers before jumping — otherwise the JIT reads garbage.
 *
 * VTX_OSR_CONV_DEFAULT is the only convention currently emitted by the
 * baseline codegen (T1). It matches the expression-stack register
 * assignment documented in baseline/frame_layout.h:
 *   TOS   → RAX
 *   TOS-1 → RCX
 *   TOS-2 → RDX
 *   TOS-3 → RBX
 *
 * The field exists so a future codegen change can declare a different
 * convention and vtx_osr_up will refuse to OSR into mismatched code
 * instead of silently loading values into the wrong registers.
 */
typedef enum {
    VTX_OSR_CONV_DEFAULT = 0,  /* RAX/RCX/RDX/RBX — T1 baseline convention */
} vtx_osr_entry_conv_t;

/* ========================================================================== */
/* Compiled code result                                                        */
/* ========================================================================== */

/**
 * The result of compilation: contains the generated native code
 * and all associated metadata.
 *
 * Fields from baseline/codegen.h:
 *   code, code_size, frame_layout, guards, deopt_stubs, side_table,
 *   deopt_info, bc_pc_map, bc_pc_map_count, native_to_bc_pc,
 *   native_to_bc_pc_count, method
 *
 * Fields from deopt/osr.h:
 *   entry_point, method_id, stack_slots, local_slots
 */
typedef struct {
    uint8_t              *code;           /* executable native code (malloc'd) */
    uint32_t              code_size;      /* size of native code in bytes */

    vtx_jit_frame_layout_t frame_layout;  /* frame layout for this method */
    vtx_guard_array_t     guards;         /* emitted guards */
    vtx_deopt_stub_array_t deopt_stubs;   /* generated deopt stubs */
    vtx_side_table_t      *side_table;    /* deopt side table */
    vtx_deopt_info_t      *deopt_info;    /* deopt info for the interpreter */

    /* Bytecode PC → native offset mapping for debugging and deopt */
    vtx_bc_pc_map_entry_t *bc_pc_map;     /* sorted by bytecode_pc */
    uint32_t               bc_pc_map_count;

    /* Native offset → bytecode PC mapping (for deopt) */
    uint32_t              *native_to_bc_pc; /* indexed by native offset / 8 */
    uint32_t               native_to_bc_pc_count;

    /* Method identity */
    const vtx_method_desc_t *method;      /* the compiled method */

    /* Polymorphic inline caches allocated during compilation.
     * These must be freed when the compiled code is destroyed. */
    vtx_poly_ic_t **poly_ics;             /* array of IC pointers */
    uint32_t         poly_ic_count;       /* number of ICs */

    /* Entry point and frame sizing (used by OSR) */
    void             *entry_point;    /* native code entry address */
    uint32_t          method_id;      /* method this code was compiled for */
    uint32_t          stack_slots;    /* number of stack slots in the JIT frame */
    uint32_t          local_slots;    /* number of local slots in the JIT frame */

    /* OSR-16: if true, the compiled code contains inlined callees and
     * the JIT frame shape does NOT match a single interpreter frame.
     * vtx_osr_up refuses OSR into such code (falls through to the failure
     * path) because the trampoline only knows how to copy one interp
     * frame into the JIT frame. Set by the codegen when inlining occurs;
     * memset-zero leaves it false for non-inlined T1 code. */
    bool              has_inlined_frames;

    /* OSR-33: declares which register convention the JIT entry point
     * expects for the top of the operand stack. vtx_osr_up verifies
     * the convention matches before jumping. Defaults to
     * VTX_OSR_CONV_DEFAULT (RAX/RCX/RDX/RBX) which is the only
     * convention currently emitted. */
    vtx_osr_entry_conv_t entry_register_convention;

    /* OSR-30: profile-data pointer written into the JIT frame header
     * at [RBP+8] by the OSR-up trampoline. The JIT epilogue does not
     * restore it, but the GC and stack walker read it from [RBP+8] to
     * locate GC roots and profile state. NULL is acceptable (matches
     * the pre-fix behavior) but the interpreter should populate this
     * from interp->profiler when known. */
    void             *profile_data;
} vtx_compiled_code_t;

#endif /* VORTEX_CODECACHE_TYPES_H */
