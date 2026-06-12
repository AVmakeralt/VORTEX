#ifndef VORTEX_PROFILER_H
#define VORTEX_PROFILER_H

#include "vortex_config.h"
#include <stdint.h>
#include <stdbool.h>
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"

/**
 * VORTEX Profiling Counters
 *
 * Per-method profile data collected by the interpreter (T0).
 * Used to make compilation tier decisions and feed the optimizer.
 *
 * All counters are saturating — they never overflow.
 */

/* ========================================================================== */
/* Compilation tier enum                                                       */
/* ========================================================================== */

typedef enum {
    VT_TIER_T0 = 0,   /* Interpreter (no compiled code) */
    VT_TIER_T1 = 1,   /* Baseline JIT */
    VT_TIER_T2 = 2,   /* Optimizing JIT */
    VT_TIER_T3 = 3    /* Speculative JIT */
} vtx_compile_tier_t;

/* Aliases for consistent VTX_ prefixed naming */
#define VTX_TIER_T0 VT_TIER_T0
#define VTX_TIER_T1 VT_TIER_T1
#define VTX_TIER_T2 VT_TIER_T2
#define VTX_TIER_T3 VT_TIER_T3

/* ========================================================================== */
/* Per-method profile data                                                     */
/* ========================================================================== */

/**
 * Per-call-site IC data: up to VTX_POLY_LIMIT type observations.
 * This is separate from the runtime inline cache (vtx_inline_cache_t)
 * in that it stores the raw observations for profiling purposes,
 * while the IC is used for dispatch optimization.
 */
typedef struct {
    vtx_ic_entry_t entries[VTX_POLY_LIMIT]; /* observed typeid → method pairs */
    uint32_t       count;                    /* number of entries filled */
} vtx_call_site_profile_t;

/**
 * Per-method profiling data.
 */
typedef struct {
    /* Method identity */
    const vtx_method_desc_t *method;       /* the method being profiled */

    /* Invocation counters (saturating) */
    uint64_t invocation_count;             /* number of times this method was called */
    uint64_t backward_branch_count;        /* number of backward branches executed */

    /* Call site type profiles: array indexed by call site bytecode PC.
     * Each call site has its own array of IC entries. */
    vtx_call_site_profile_t *call_site_types; /* allocated per call site */
    uint32_t                 call_site_count;  /* number of call sites */

    /* Branch frequency counters: indexed by branch bytecode PC.
     * These arrays are sized to the method's bytecode length. */
    uint32_t *branch_taken_counts;  /* count of times branch was taken, per PC */
    uint32_t *branch_total_counts;  /* total count of times branch was reached, per PC */
    uint32_t  branch_array_size;    /* size of the branch count arrays */

    /* Field shape observations: indexed by field access bytecode PC.
     * Stores the shape ID observed at each field access site. */
    uint32_t *field_shape_ids;      /* shape IDs per field site PC */
    uint32_t  field_array_size;     /* size of the field shape array */

    /* Compilation state */
    vtx_compile_tier_t compiled_tier;  /* current compilation tier */
    uint32_t           deopt_count;    /* number of deoptimizations from this method */
} vtx_profile_data_t;

/* ========================================================================== */
/* Profiler                                                                    */
/* ========================================================================== */

/**
 * The profiler holds an array of profile data, one per method.
 * Methods are identified by their vtx_method_desc_t pointer.
 */
/* LRU cache size for method lookups — avoids O(N) linear search on hot path */
#define VTX_PROFILER_LRU_SIZE 8

typedef struct {
    vtx_profile_data_t *data;       /* array of profile data entries */
    uint32_t            count;      /* number of methods being profiled */
    uint32_t            capacity;   /* capacity of the data array */

    /* LRU cache: most recently used method→profile_data mappings.
     * Checked before the linear scan on every lookup.
     * On a hit, the entry is moved to slot 0 (MRU position). */
    struct {
        const vtx_method_desc_t *method;
        vtx_profile_data_t      *pd;
    } lru[VTX_PROFILER_LRU_SIZE];
} vtx_profiler_t;

/**
 * Initialize the profiler.
 * Returns 0 on success, -1 on failure.
 */
int vtx_profiler_init(vtx_profiler_t *profiler);

/**
 * Destroy the profiler and release all memory.
 */
void vtx_profiler_destroy(vtx_profiler_t *profiler);

/**
 * Get or create profile data for the given method.
 * If the method is not yet being profiled, a new entry is created.
 * Returns a pointer to the profile data, or NULL on failure.
 */
vtx_profile_data_t *vtx_profiler_get_method_data(vtx_profiler_t *profiler,
                                                   const vtx_method_desc_t *method);

/**
 * Record a method invocation. Increments the invocation counter
 * (saturating at UINT64_MAX).
 */
void vtx_profiler_record_invocation(vtx_profiler_t *profiler,
                                     const vtx_method_desc_t *method);

/**
 * Record a backward branch execution. Increments the backward_branch_count
 * (saturating at UINT64_MAX).
 */
void vtx_profiler_record_backward_branch(vtx_profiler_t *profiler,
                                          const vtx_method_desc_t *method);

/**
 * Record a branch outcome at the given PC.
 * Increments the taken and total counters (saturating at UINT32_MAX).
 */
void vtx_profiler_record_branch(vtx_profiler_t *profiler,
                                 const vtx_method_desc_t *method,
                                 uint32_t pc,
                                 bool taken);

/**
 * Record the receiver type at a call site.
 * The call site is identified by its bytecode PC (call_pc).
 */
void vtx_profiler_record_call_type(vtx_profiler_t *profiler,
                                    const vtx_method_desc_t *method,
                                    uint32_t call_pc,
                                    vtx_typeid_t typeid_);

/**
 * Record the shape ID at a field access site.
 * The field access is identified by its bytecode PC (field_pc).
 */
void vtx_profiler_record_field_shape(vtx_profiler_t *profiler,
                                      const vtx_method_desc_t *method,
                                      uint32_t field_pc,
                                      vtx_shapeid_t shapeid);

/**
 * Compute the "heat" of a method: a weighted score combining
 * invocation count and backward branch density.
 *
 * heat = invocation_count + backward_branch_count * 2
 *
 * This means a method with many loop iterations is hotter than one
 * that's called often but does little work per call.
 */
uint64_t vtx_profiler_method_heat(vtx_profiler_t *profiler,
                                   const vtx_method_desc_t *method);

/**
 * Check if a method should be compiled at T1 (baseline JIT).
 * Returns true if heat > VORTEX_T1_THRESHOLD.
 */
bool vtx_profiler_should_compile_t1(vtx_profiler_t *profiler,
                                     const vtx_method_desc_t *method);

/**
 * Check if a method should be compiled at T2 (optimizing JIT).
 * Returns true if heat > VORTEX_T2_THRESHOLD.
 */
bool vtx_profiler_should_compile_t2(vtx_profiler_t *profiler,
                                     const vtx_method_desc_t *method);

/**
 * Get the call site profile data for a given method and PC.
 * Returns the call site profile, or NULL if not found.
 */
const vtx_call_site_profile_t *vtx_profiler_get_call_site_profile(
    const vtx_profiler_t *profiler,
    const vtx_method_desc_t *method,
    uint32_t call_pc);

/**
 * Get the branch probability (taken / total) at a given PC.
 * Returns 0.5 if no data is available.
 */
double vtx_profiler_get_branch_probability(const vtx_profiler_t *profiler,
                                            const vtx_method_desc_t *method,
                                            uint32_t pc);

/**
 * Get the field shape ID observed at a given PC.
 * Returns VTX_SHAPE_INVALID if no data is available.
 */
vtx_shapeid_t vtx_profiler_get_field_shape(const vtx_profiler_t *profiler,
                                             const vtx_method_desc_t *method,
                                             uint32_t field_pc);

/**
 * Record a deoptimization from the given method.
 * Increments deopt_count (saturating at UINT32_MAX).
 */
void vtx_profiler_record_deopt(vtx_profiler_t *profiler,
                                const vtx_method_desc_t *method);

/**
 * Set the compilation tier for a method.
 */
void vtx_profiler_set_compiled_tier(vtx_profiler_t *profiler,
                                     const vtx_method_desc_t *method,
                                     vtx_compile_tier_t tier);

#endif /* VORTEX_PROFILER_H */
