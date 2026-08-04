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
} vtx_compiled_code_t;

#endif /* VORTEX_CODECACHE_TYPES_H */
