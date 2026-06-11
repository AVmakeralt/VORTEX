#ifndef VORTEX_SELECTOR_H
#define VORTEX_SELECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "interp/profiler.h"
#include "profile/data.h"

/**
 * VORTEX Trace Selector
 *
 * Identifies hot loops from profiling data and selects them as candidates
 * for trace recording. A loop is considered hot when its backward branch
 * count exceeds the T2 compilation threshold.
 *
 * Selection algorithm:
 *   1. Iterate over all profiled methods.
 *   2. For each method, examine loop back-edge profiles.
 *   3. A loop is hot if backedge_count > VORTEX_T2_THRESHOLD.
 *   4. Compute heat = backedge_count for each hot loop.
 *   5. Sort hot loops by heat in descending order.
 *   6. Return the sorted list as trace recording candidates.
 *
 * The selector uses two profile sources:
 *   - vtx_profiler_t: the interpreter's per-method profiling data
 *     (used for method-level heat and backward branch counts)
 *   - vtx_profile_global_t: the detailed global profile with per-loop
 *     back-edge counts (used for fine-grained loop identification)
 */

/* ========================================================================== */
/* Hot loop descriptor                                                         */
/* ========================================================================== */

/**
 * Describes a hot loop identified by the trace selector.
 * The loop header PC serves as the trace anchor — the starting point
 * for trace recording.
 */
typedef struct {
    const vtx_method_desc_t *method;       /* the method containing the loop */
    uint32_t                 loop_header_pc; /* bytecode PC of the loop header */
    uint64_t                 heat;         /* backedge count (heat metric) */
    uint32_t                 method_id;    /* method ID from global profile */
} vtx_hot_loop_t;

/* ========================================================================== */
/* Hot loop list                                                               */
/* ========================================================================== */

#define VTX_HOT_LOOP_INITIAL_CAPACITY 16

/**
 * A list of hot loops, sorted by heat in descending order.
 */
typedef struct {
    vtx_hot_loop_t *loops;    /* array of hot loop descriptors */
    uint32_t        count;    /* number of hot loops */
    uint32_t        capacity; /* allocated capacity */
} vtx_hot_loop_list_t;

/* ========================================================================== */
/* Trace selector                                                              */
/* ========================================================================== */

/**
 * The trace selector scans profile data to identify hot loops.
 */
typedef struct {
    vtx_hot_loop_list_t hot_loops; /* current list of hot loops */
} vtx_trace_selector_t;

/**
 * Initialize the trace selector.
 * Returns 0 on success, -1 on failure.
 */
int vtx_trace_selector_init(vtx_trace_selector_t *selector);

/**
 * Destroy the trace selector and free memory.
 */
void vtx_trace_selector_destroy(vtx_trace_selector_t *selector);

/**
 * Select hot loops from profiling data.
 *
 * Scans both the interpreter profiler and the global profile data for
 * loops with back-edge counts exceeding VORTEX_T2_THRESHOLD.
 * The result is sorted by heat (backedge_count) in descending order.
 *
 * Returns the list of hot loop descriptors. The list is owned by the
 * selector and remains valid until the next call to select() or
 * until the selector is destroyed.
 */
const vtx_hot_loop_list_t *vtx_trace_selector_select(
    vtx_trace_selector_t *selector,
    const vtx_profiler_t *profiler,
    const vtx_profile_global_t *profile_data);

/**
 * Get the number of hot loops found by the last select() call.
 */
uint32_t vtx_trace_selector_hot_count(const vtx_trace_selector_t *selector);

/**
 * Get a hot loop by index (0 = hottest).
 * Returns NULL if index is out of bounds.
 */
const vtx_hot_loop_t *vtx_trace_selector_get_hot(
    const vtx_trace_selector_t *selector, uint32_t index);

#endif /* VORTEX_SELECTOR_H */
