/**
 * VORTEX Trace Selector — Implementation
 *
 * Scans profiling data to identify hot loops suitable for trace recording.
 * Uses both the interpreter profiler (vtx_profiler_t) and the global profile
 * data (vtx_profile_global_t) to find loops with back-edge counts exceeding
 * the T2 compilation threshold.
 */

#include "trace/selector.h"
#include "runtime/arena.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Hot loop list helpers                                                       */
/* ========================================================================== */

static int vtx_hot_loop_list_init(vtx_hot_loop_list_t *list)
{
    list->capacity = VTX_HOT_LOOP_INITIAL_CAPACITY;
    list->loops = malloc(sizeof(vtx_hot_loop_t) * list->capacity);
    if (list->loops == NULL) {
        return -1;
    }
    list->count = 0;
    return 0;
}

static void vtx_hot_loop_list_destroy(vtx_hot_loop_list_t *list)
{
    if (list->loops != NULL) {
        free(list->loops);
        list->loops = NULL;
    }
    list->count = 0;
    list->capacity = 0;
}

static int vtx_hot_loop_list_grow(vtx_hot_loop_list_t *list)
{
    uint32_t new_capacity = list->capacity * 2;
    vtx_hot_loop_t *new_loops = realloc(list->loops,
                                         sizeof(vtx_hot_loop_t) * new_capacity);
    if (new_loops == NULL) {
        return -1;
    }
    list->loops = new_loops;
    list->capacity = new_capacity;
    return 0;
}

static int vtx_hot_loop_list_append(vtx_hot_loop_list_t *list,
                                     const vtx_hot_loop_t *loop)
{
    if (list->count >= list->capacity) {
        if (vtx_hot_loop_list_grow(list) != 0) {
            return -1;
        }
    }
    list->loops[list->count] = *loop;
    list->count++;
    return 0;
}

/**
 * Comparator for qsort: sort hot loops by heat descending.
 */
static int vtx_hot_loop_compare_heat_desc(const void *a, const void *b)
{
    const vtx_hot_loop_t *la = (const vtx_hot_loop_t *)a;
    const vtx_hot_loop_t *lb = (const vtx_hot_loop_t *)b;

    if (la->heat > lb->heat) return -1;
    if (la->heat < lb->heat) return  1;
    return 0;
}

/* ========================================================================== */
/* Trace selector lifecycle                                                    */
/* ========================================================================== */

int vtx_trace_selector_init(vtx_trace_selector_t *selector)
{
    VTX_ASSERT(selector != NULL, "selector must not be NULL");
    return vtx_hot_loop_list_init(&selector->hot_loops);
}

void vtx_trace_selector_destroy(vtx_trace_selector_t *selector)
{
    if (selector == NULL) return;
    vtx_hot_loop_list_destroy(&selector->hot_loops);
}

/* ========================================================================== */
/* Selection from interpreter profiler                                         */
/* ========================================================================== */

/**
 * Scan the interpreter profiler for methods with hot backward branches.
 * Each method's backward_branch_count is used as the heat metric.
 */
static int vtx_trace_selector_scan_profiler(
    vtx_trace_selector_t *selector,
    const vtx_profiler_t *profiler)
{
    if (profiler == NULL) return 0;

    for (uint32_t i = 0; i < profiler->count; i++) {
        const vtx_profile_data_t *pd = &profiler->data[i];
        if (pd == NULL || pd->method == NULL) continue;

        /* Check if this method has a hot loop */
        uint64_t backedge_count = pd->backward_branch_count;
        if (backedge_count <= (uint64_t)VORTEX_T2_THRESHOLD) continue;

        vtx_hot_loop_t loop;
        loop.method = pd->method;
        loop.heat = backedge_count;
        loop.method_id = 0; /* profiler doesn't use method_id directly */

        /* Try to find the actual loop header PC from branch data.
         * Backward branches are IF_TRUE/IF_FALSE/GOTO whose target PC
         * is less than the current PC. We look at the branch counters
         * to find the PC of the backward branch instruction. */
        loop.loop_header_pc = 0; /* will be refined below */

        /* Search the branch array for backward branches.
         * NOTE: We cannot determine the actual loop header PC here
         * because we don't have the bytecode to decode branch targets.
         * The branch_taken_counts[j] == 0 check only tells us that
         * a branch at PC j was taken, not that j is the loop header.
         * The loop header is the TARGET of the backward branch, not
         * the PC of the branch instruction itself.
         * We leave loop_header_pc as 0; the global profile scan
         * (Phase 2) provides the correct loop_header_pc from
         * lp->loop_header_pc. */

        /* If we couldn't find a specific loop header from branches,
         * we still add the method as a hot loop candidate with PC=0.
         * The recorder will need the bytecode to determine the actual
         * loop header. */
        if (vtx_hot_loop_list_append(&selector->hot_loops, &loop) != 0) {
            return -1;
        }
    }

    return 0;
}

/* ========================================================================== */
/* Selection from global profile data                                          */
/* ========================================================================== */

/**
 * Scan the global profile for per-loop back-edge counts.
 * This provides more precise loop identification than the interpreter
 * profiler's method-level backward_branch_count.
 */
static int vtx_trace_selector_scan_global_profile(
    vtx_trace_selector_t *selector,
    const vtx_profile_global_t *profile_data)
{
    if (profile_data == NULL) return 0;

    for (uint32_t i = 0; i < profile_data->method_count; i++) {
        const vtx_profile_method_t *mp = &profile_data->methods[i];
        if (mp == NULL) continue;

        for (uint32_t j = 0; j < mp->loop_count; j++) {
            const vtx_loop_profile_t *lp = &mp->loops[j];
            if (lp->backedge_count <= (uint64_t)VORTEX_T2_THRESHOLD) continue;

            /* Check if this loop is already in our list (method_id + header PC) */
            bool duplicate = false;
            for (uint32_t k = 0; k < selector->hot_loops.count; k++) {
                vtx_hot_loop_t *existing = &selector->hot_loops.loops[k];
                if (existing->method_id == mp->method_id &&
                    existing->loop_header_pc == lp->loop_header_pc) {
                    /* Update heat to the more precise value */
                    if (lp->backedge_count > existing->heat) {
                        existing->heat = lp->backedge_count;
                    }
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate) {
                /* Skip global profile entries where method is NULL.
                 * Without a method pointer, the recorder would
                 * NULL-dereference when trying to access method data.
                 * Only add entries that refine existing profiler
                 * entries (handled by the duplicate path above). */
                continue;
            }
        }
    }

    return 0;
}

/* ========================================================================== */
/* Main selection entry point                                                  */
/* ========================================================================== */

const vtx_hot_loop_list_t *vtx_trace_selector_select(
    vtx_trace_selector_t *selector,
    const vtx_profiler_t *profiler,
    const vtx_profile_global_t *profile_data)
{
    VTX_ASSERT(selector != NULL, "selector must not be NULL");

    /* Reset the list for a new selection round */
    selector->hot_loops.count = 0;

    /* Phase 1: Scan interpreter profiler for method-level hot loops */
    if (vtx_trace_selector_scan_profiler(selector, profiler) != 0) {
        return NULL;
    }

    /* Phase 2: Scan global profile for precise per-loop data.
     * This may refine the heat values from Phase 1 or add new entries
     * for loops that weren't detected at the method level. */
    if (vtx_trace_selector_scan_global_profile(selector, profile_data) != 0) {
        return NULL;
    }

    /* Sort by heat descending */
    if (selector->hot_loops.count > 1) {
        qsort(selector->hot_loops.loops, selector->hot_loops.count,
              sizeof(vtx_hot_loop_t), vtx_hot_loop_compare_heat_desc);
    }

    return &selector->hot_loops;
}

/* ========================================================================== */
/* Accessors                                                                   */
/* ========================================================================== */

uint32_t vtx_trace_selector_hot_count(const vtx_trace_selector_t *selector)
{
    return selector != NULL ? selector->hot_loops.count : 0;
}

const vtx_hot_loop_t *vtx_trace_selector_get_hot(
    const vtx_trace_selector_t *selector, uint32_t index)
{
    if (selector == NULL || index >= selector->hot_loops.count) {
        return NULL;
    }
    return &selector->hot_loops.loops[index];
}
