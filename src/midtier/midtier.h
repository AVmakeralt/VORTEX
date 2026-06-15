#ifndef VORTEX_MIDTIER_H
#define VORTEX_MIDTIER_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/arena.h"
#include "interp/type_feedback.h"

/**
 * VORTEX Mid-Tier (T1.5) — Type-Specialized Compilation
 *
 * Between the baseline T1 and the optimizing T2, the mid-tier provides
 * type-specialized compilation with minimal analysis. This reduces warmup
 * time by 3-5x compared to waiting for T2 compilation.
 *
 * Key differences from T1 (baseline):
 *   - Uses type feedback to specialize operations (e.g., "this is always int")
 *   - Eliminates type checks for monomorphic call sites
 *   - Inline caches are pre-populated from type feedback
 *
 * Key differences from T2 (optimizing):
 *   - No PEA / escape analysis / scalar replacement
 *   - No inlining of non-trivial methods
 *   - No LICM / bounds check elimination
 *   - Compilation is ~10x faster than T2
 */

typedef struct {
    /* Configuration */
    bool use_type_feedback;     /* use type feedback for specialization? */
    uint32_t inline_max_size;   /* max callee bytecode size for inlining (0 = no inlining) */
    bool specialize_arithmetic; /* specialize integer/float ops based on type feedback? */

    /* Statistics — stored in global struct so they persist across compilations */
    uint32_t methods_compiled;
    uint32_t sites_specialized;
    uint32_t guards_inserted;
    uint64_t compilation_time_ns;  /* total compilation time in nanoseconds (uint64 to avoid wrap) */
} vtx_midtier_config_t;

/* Result of mid-tier compilation */
typedef struct {
    uint8_t  *code;               /* generated machine code */
    uint32_t  code_size;          /* size of generated code */
    uint32_t  guards_inserted;    /* number of guards inserted */
    uint32_t  sites_specialized;  /* number of specialized sites */
} vtx_midtier_result_t;

/* Initialize mid-tier configuration with defaults */
vtx_midtier_config_t vtx_midtier_config_default(void);

/* Run mid-tier compilation on a method */
vtx_midtier_result_t *vtx_midtier_compile(
    const vtx_method_desc_t *method,
    const vtx_type_feedback_t *type_feedback,
    vtx_type_system_t *ts,
    vtx_arena_t *arena);

#endif /* VORTEX_MIDTIER_H */
