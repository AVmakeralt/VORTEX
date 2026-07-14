/* ============================================================================ *
 * AI-MODIFIED CODE
 *
 * This file was originally written by a human developer. It has been
 * substantially modified by an AI assistant (GLM/Z.ai) for bug fixes,
 * performance improvements, and feature additions.
 *
 * Original human-written structure is preserved; AI changes are marked
 * with bug fix IDs (B1-B28) or perf notes (Perf 1-10) in comments.
 *
 * If reviewing, please verify AI changes against the original logic.
 * ============================================================================ */

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
 *
 * B21 fix: We now properly record method_id (from the method descriptor)
 * and loop_header_pc (decoded from the bytecode at each hot branch PC).
 * This lets Phase 2 (global profile scan) match its per-loop entries
 * against the per-method entries from Phase 1 to refine heat values.
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

        /* B21 fix: record the actual method_id so Phase 2 can match.
         * The convention used by main_new.c when syncing profiler data
         * into the global profile is `method_id = method->vtable_index`,
         * so we use the same here. */
        const vtx_method_desc_t *method = pd->method;
        uint32_t method_id = method->vtable_index;

        /* B21 fix: scan the branch arrays and the bytecode to identify
         * the actual loop header PC for each hot backward branch.
         *
         * For every PC `j` with branch_taken_counts[j] > 0:
         *   1. Read the opcode at PC j from the bytecode.
         *   2. If it's GOTO/IF_TRUE/IF_FALSE, read the 2-byte target PC.
         *   3. If target < j, this is a backward branch — the target is
         *      the loop header. Add a hot_loop entry for it.
         *
         * Without this, Phase 1 sets loop_header_pc=0 for every method,
         * so Phase 2's duplicate check (existing->loop_header_pc ==
         * lp->loop_header_pc) never matches and Phase 2 effectively
         * becomes dead code — global profile per-loop data is discarded. */
        const vtx_bytecode_t *bc = method->bytecode;
        bool found_loop_header = false;

        if (bc != NULL && bc->code != NULL &&
            pd->branch_taken_counts != NULL &&
            pd->branch_total_counts != NULL) {
            uint32_t bsize = pd->branch_array_size;
            if (bsize > bc->length) bsize = (uint32_t)bc->length;

            size_t pc = 0;
            while (pc < bsize) {
                vtx_opcode_t op = vtx_bytecode_opcode_at(bc, pc);
                size_t insn_len = vtx_bytecode_insn_length(bc, pc);

                if (op == VT_OP_GOTO || op == VT_OP_IF_TRUE ||
                    op == VT_OP_IF_FALSE) {
                    uint32_t taken = pd->branch_taken_counts[pc];
                    if (taken > 0) {
                        uint16_t target = vtx_bytecode_read_operand(bc, pc);
                        if (target < pc) {
                            /* Backward branch — target is the loop header.
                             * Use the branch's taken count as the heat. */
                            vtx_hot_loop_t loop;
                            loop.method = method;
                            loop.method_id = method_id;
                            loop.loop_header_pc = target;
                            loop.heat = taken;
                            if (vtx_hot_loop_list_append(&selector->hot_loops,
                                                          &loop) != 0) {
                                return -1;
                            }
                            found_loop_header = true;
                        }
                    }
                }

                if (insn_len == 0) break; /* safety: avoid infinite loop */
                pc += insn_len;
            }
        }

        /* If we didn't find a specific loop header (e.g., the bytecode
         * wasn't available or no backward branch was hot enough), still
         * add the method as a hot loop candidate with the method-level
         * backedge_count as the heat. PC=0 here means "any loop in this
         * method" — Phase 2 may still refine it. */
        if (!found_loop_header) {
            vtx_hot_loop_t loop;
            loop.method = method;
            loop.method_id = method_id;
            loop.loop_header_pc = 0;
            loop.heat = backedge_count;
            if (vtx_hot_loop_list_append(&selector->hot_loops, &loop) != 0) {
                return -1;
            }
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
