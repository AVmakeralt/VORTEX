#ifndef VORTEX_SOTA_LOOP_SPEC_H
#define VORTEX_SOTA_LOOP_SPEC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "profile/data.h"
#include "runtime/arena.h"

/**
 * VORTEX SOTA — Speculative Loop Transformations
 *
 * Profiles loop trip counts and access patterns. When a loop has:
 *   - Predictable trip count (coefficient of variation < VTX_LOOP_CV_THRESHOLD)
 *   - No aliasing (proven by type-based alias analysis + guard)
 *   - No dependencies (loop-carried dependencies don't prevent vectorization)
 *
 * Then speculatively emit SIMD code (SSE2/AVX2) for the loop body.
 * Guards are emitted for:
 *   - Alignment (vector load/store alignment requirements)
 *   - Aliasing (memory doesn't overlap between iterations)
 *   - Trip count (exact trip count matches profiled pattern)
 *
 * On guard failure → deopt to scalar loop.
 *
 * CPU feature flags:
 *   - VTX_CPU_SSE2  (0x01) — baseline for x86-64, always available
 *   - VTX_CPU_AVX2  (0x02) — 256-bit vectors
 *   - VTX_CPU_AVX512 (0x04) — 512-bit vectors (future)
 */

/* ========================================================================== */
/* CPU feature flags                                                           */
/* ========================================================================== */

#define VTX_CPU_SSE2   0x01u
#define VTX_CPU_AVX2   0x02u
#define VTX_CPU_AVX512 0x04u

/* ========================================================================== */
/* Loop analysis result                                                        */
/* ========================================================================== */

typedef enum {
    VTX_LOOP_CANT_VECTORIZE     = 0,  /* cannot vectorize */
    VTX_LOOP_MAYBE_VECTORIZE    = 1,  /* might be vectorizable, needs more analysis */
    VTX_LOOP_CAN_VECTORIZE_SSE2 = 2,  /* can vectorize with SSE2 (128-bit) */
    VTX_LOOP_CAN_VECTORIZE_AVX2 = 3   /* can vectorize with AVX2 (256-bit) */
} vtx_loop_vectorize_t;

typedef struct {
    vtx_loop_vectorize_t vectorizability; /* can this loop be vectorized? */

    /* Trip count statistics */
    double   mean_trip_count;    /* average trip count */
    double   cv_trip_count;      /* coefficient of variation (stddev/mean) */
    uint64_t profiled_iterations;/* number of times this loop was profiled */

    /* Dependency analysis */
    bool     has_loop_carried_dep;  /* true if loop has loop-carried dependencies */
    bool     has_aliased_access;    /* true if memory accesses may alias */
    bool     has_unknown_call;      /* true if loop contains calls to unknown functions */

    /* Access pattern */
    bool     is_stride1;         /* true if array accesses are stride-1 (sequential) */
    uint32_t stride;             /* access stride (1 = sequential, 2 = every other, etc.) */
    uint32_t element_size;       /* size of accessed elements in bytes */

    /* Vectorization parameters */
    uint32_t vector_width;       /* number of elements per vector (4 for SSE2 float, etc.) */
    uint32_t peel_count;         /* iterations to peel for alignment */
} vtx_loop_spec_result_t;

/* ========================================================================== */
/* Loop spec state                                                             */
/* ========================================================================== */

typedef struct {
    /* Statistics */
    uint32_t loops_checked;
    uint32_t loops_vectorized;
    uint32_t vectorization_attempts_failed;
} vtx_sota_loop_spec_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the loop speculation module.
 */
int vtx_sota_loop_spec_init(vtx_sota_loop_spec_t *spec);

/**
 * Destroy the loop speculation module.
 */
void vtx_sota_loop_spec_destroy(vtx_sota_loop_spec_t *spec);

/* ========================================================================== */
/* Analysis                                                                    */
/* ========================================================================== */

/**
 * Check if a loop can be speculatively vectorized.
 *
 * Analyzes the loop body for:
 *   - Predictable trip count (from profile data)
 *   - Stride-1 access pattern
 *   - No loop-carried dependencies
 *   - No aliasing between array accesses
 *   - No calls to unknown functions
 *
 * @param spec      Loop spec module
 * @param profile   Global profile data (for trip count statistics)
 * @param graph     The SoN graph
 * @param loop_node The LoopBegin node ID
 * @return          Analysis result with vectorizability and parameters
 */
vtx_loop_spec_result_t vtx_sota_loop_spec_check(
    vtx_sota_loop_spec_t *spec,
    const vtx_profile_global_t *profile,
    const vtx_graph_t *graph,
    vtx_nodeid_t loop_node);

/**
 * Transform a loop by emitting speculative SIMD code.
 *
 * The transformation:
 *   1. Computes the vector width based on CPU features and element size
 *   2. Emits a pre-loop for alignment (peel iterations)
 *   3. Emits the vectorized loop body with SIMD operations
 *   4. Emits a post-loop for remaining iterations
 *   5. Adds guards for: alignment, aliasing, trip count
 *   6. On guard failure → deopt to the original scalar loop
 *
 * @param graph        The SoN graph (modified in place)
 * @param loop_node    The LoopBegin node ID
 * @param cpu_features CPU feature flags (VTX_CPU_SSE2, VTX_CPU_AVX2, etc.)
 * @param spec_result  Result from vtx_sota_loop_spec_check
 * @param arena        Arena for temporary allocations
 * @return             true if transformation was applied, false if not
 */
bool vtx_sota_loop_spec_transform(vtx_graph_t *graph,
                                    vtx_nodeid_t loop_node,
                                    uint32_t cpu_features,
                                    const vtx_loop_spec_result_t *spec_result,
                                    vtx_arena_t *arena);

/* ========================================================================== */
/* Loop unrolling                                                              */
/* ========================================================================== */

/**
 * Result of a loop unrolling analysis.
 */
typedef struct {
    bool     can_unroll;         /* true if the loop can be unrolled */
    uint32_t unroll_factor;      /* number of copies to unroll (2, 4, 8) */
    uint64_t constant_trip_count;/* trip count if known (0 = unknown) */
    bool     requires_guard;     /* true if a trip count guard is needed */
} vtx_loop_unroll_result_t;

/**
 * Check if a loop can be speculatively unrolled.
 *
 * A loop can be unrolled when:
 *   - The trip count is small and constant (from profiling)
 *   - The CV of trip counts is below VTX_LOOP_CV_THRESHOLD
 *   - The loop body is small enough that unrolling won't exceed
 *     VTX_MAX_NATIVE_SIZE
 *
 * Unrolling is beneficial when the loop overhead (branch, induction
 * variable update) is significant relative to the loop body.
 *
 * @param spec      Loop spec module
 * @param profile   Global profile data
 * @param graph     The SoN graph
 * @param loop_node The LoopBegin node ID
 * @return          Unroll analysis result
 */
vtx_loop_unroll_result_t vtx_sota_loop_unroll_check(
    vtx_sota_loop_spec_t *spec,
    const vtx_profile_global_t *profile,
    const vtx_graph_t *graph,
    vtx_nodeid_t loop_node);

/**
 * Transform a loop by unrolling it with a trip count guard.
 *
 * The transformation:
 *   1. Emits a DeoptGuard that the trip count equals the expected value
 *   2. Copies the loop body N times (where N = unroll_factor)
 *   3. Removes the loop back-edge (since the trip count is known)
 *   4. On guard failure → deopt to the original loop
 *
 * @param graph        The SoN graph (modified in place)
 * @param loop_node    The LoopBegin node ID
 * @param unroll_result Result from vtx_sota_loop_unroll_check
 * @param arena        Arena for temporary allocations
 * @return             true if transformation was applied
 */
bool vtx_sota_loop_unroll_transform(vtx_graph_t *graph,
                                      vtx_nodeid_t loop_node,
                                      const vtx_loop_unroll_result_t *unroll_result,
                                      vtx_arena_t *arena);

/* ========================================================================== */
/* Helpers                                                                     */
/* ========================================================================== */

/**
 * Compute the coefficient of variation from a set of trip count observations.
 * CV = stddev / mean. A low CV (< VTX_LOOP_CV_THRESHOLD) indicates
 * a predictable trip count.
 *
 * @param trip_counts Array of observed trip counts
 * @param count       Number of observations
 * @return            Coefficient of variation, or INFINITY if mean is 0
 */
double vtx_loop_cv(const uint64_t *trip_counts, uint32_t count);

/**
 * Compute the vector width for a given element size and CPU features.
 * Returns the number of elements that fit in one vector register.
 *
 * SSE2: 128 bits → 4 floats, 2 doubles, 4 int32, 2 int64
 * AVX2: 256 bits → 8 floats, 4 doubles, 8 int32, 4 int64
 */
uint32_t vtx_loop_vector_width(uint32_t element_size, uint32_t cpu_features);

/**
 * Detect the CPU features available at runtime.
 * Uses CPUID on x86-64.
 */
uint32_t vtx_cpu_detect_features(void);

/**
 * Detect CPU features at runtime (alias for vtx_cpu_detect_features).
 * Returns a bitmask with VTX_CPU_SSE2, VTX_CPU_AVX2, VTX_CPU_AVX512 set
 * according to what the CPU supports.
 *
 * SSE2 is always available on x86-64 (baseline).
 * AVX2 requires CPUID leaf 7, EBX bit 5.
 */
uint32_t vtx_sota_loop_detect_cpu_features(void);

#endif /* VORTEX_SOTA_LOOP_SPEC_H */
