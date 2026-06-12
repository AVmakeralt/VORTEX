#ifndef VORTEX_DEOPT_TYPES_H
#define VORTEX_DEOPT_TYPES_H

#include <stdint.h>
#include "runtime/type_system.h"

/**
 * VORTEX Deopt — Shared Deopt Type Definitions
 *
 * This header contains type definitions shared between the baseline JIT
 * and the deopt/OSR subsystems.
 */

/* ========================================================================== */
/* Deopt info for a compiled method                                            */
/* ========================================================================== */

/**
 * Per-method deoptimization information stored in the JIT frame.
 * Maps native PC offsets to bytecode PCs and stack states for deopt.
 */
typedef struct {
    const vtx_method_desc_t *method;        /* the compiled method */
    uint32_t                *native_offsets; /* sorted array of native PC offsets */
    uint32_t                *pc_map;         /* parallel array: bytecode_pc for each native_offset */
    uint32_t                 pc_map_count;   /* number of entries in pc_map/native_offsets */
    uint32_t                *stack_depth_map;/* stack depth at each native_offset */
} vtx_deopt_info_t;

#endif /* VORTEX_DEOPT_TYPES_H */
