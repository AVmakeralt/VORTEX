/* ============================================================================ *
 * AI-GENERATED CODE
 *
 * This file was written from scratch by an AI assistant (GLM/Z.ai).
 * It is part of the VORTEX JIT compiler project.
 *
 * Human-written code lives in: src/interp/ (dispatch loop), src/baseline/
 * (codegen), src/runtime/ (GC, type system, arena), src/main_new.c.
 *
 * If reviewing, please verify correctness independently.
 * ============================================================================ */

/**
 * VORTEX Phase-Aware Profile Partitioning (Sprint 2) — Implementation
 *
 * See phase_partition.h for design rationale.
 */

#include "profile/phase_partition.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========================================================================== */
/* Internal helpers                                                            */
/* ========================================================================== */

static uint64_t part_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Find the index of the entry for the given phase_id, or UINT32_MAX if
 * not found. */
static uint32_t find_entry(const vtx_phase_partition_t *part, uint32_t phase_id)
{
    for (uint32_t i = 0; i < part->entry_count; i++) {
        if (part->entries[i].valid && part->entries[i].phase_id == phase_id) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* Find or create the entry for the given phase_id. Returns the index,
 * or UINT32_MAX on failure. */
static uint32_t find_or_create_entry(vtx_phase_partition_t *part, uint32_t phase_id)
{
    uint32_t idx = find_entry(part, phase_id);
    if (idx != UINT32_MAX) return idx;

    /* Look for an invalid slot to reuse. */
    uint32_t reuse = UINT32_MAX;
    for (uint32_t i = 0; i < part->entry_count; i++) {
        if (!part->entries[i].valid) {
            reuse = i;
            break;
        }
    }

    /* Grow if no reuse slot. */
    if (reuse == UINT32_MAX) {
        if (part->entry_count >= part->entry_capacity) {
            uint32_t new_cap = part->entry_capacity * 2;
            if (new_cap == 0) new_cap = VTX_PHASE_PARTITION_INITIAL_CAPACITY;
            vtx_phase_profile_entry_t *new_entries = (vtx_phase_profile_entry_t *)realloc(
                part->entries, new_cap * sizeof(vtx_phase_profile_entry_t));
            if (new_entries == NULL) return UINT32_MAX;
            /* Zero out the new slots. */
            memset(new_entries + part->entry_capacity, 0,
                   (new_cap - part->entry_capacity) * sizeof(vtx_phase_profile_entry_t));
            part->entries = new_entries;
            part->entry_capacity = new_cap;
        }
        reuse = part->entry_count;
        part->entry_count++;
    }

    /* Initialize the new entry. */
    vtx_phase_profile_entry_t *e = &part->entries[reuse];
    memset(e, 0, sizeof(*e));
    e->phase_id = phase_id;
    e->valid = true;
    if (vtx_profile_global_init(&e->profile) != 0) {
        e->valid = false;
        return UINT32_MAX;
    }
    part->total_phase_creations++;
    return reuse;
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_phase_partition_init(vtx_phase_partition_t *part)
{
    if (part == NULL) return -1;
    memset(part, 0, sizeof(*part));

    part->entry_capacity = VTX_PHASE_PARTITION_INITIAL_CAPACITY;
    part->entries = (vtx_phase_profile_entry_t *)calloc(
        part->entry_capacity, sizeof(vtx_phase_profile_entry_t));
    if (part->entries == NULL) {
        part->entry_capacity = 0;
        return -1;
    }
    part->entry_count = 0;
    part->active_index = 0;
    part->total_transitions = 0;
    part->total_phase_creations = 0;

    /* Create the default phase (VTX_PHASE_NONE) and make it active. */
    uint32_t default_idx = find_or_create_entry(part, VTX_PHASE_NONE);
    if (default_idx == UINT32_MAX) {
        free(part->entries);
        part->entries = NULL;
        part->entry_capacity = 0;
        return -1;
    }
    part->active_index = default_idx;
    return 0;
}

void vtx_phase_partition_destroy(vtx_phase_partition_t *part)
{
    if (part == NULL) return;
    if (part->entries != NULL) {
        for (uint32_t i = 0; i < part->entry_count; i++) {
            if (part->entries[i].valid) {
                vtx_profile_global_destroy(&part->entries[i].profile);
                part->entries[i].valid = false;
            }
        }
        free(part->entries);
        part->entries = NULL;
    }
    part->entry_count = 0;
    part->entry_capacity = 0;
    part->active_index = 0;
}

/* ========================================================================== */
/* Active profile access                                                       */
/* ========================================================================== */

vtx_profile_global_t *vtx_phase_partition_get_active(vtx_phase_partition_t *part)
{
    if (part == NULL) return NULL;
    if (part->active_index >= part->entry_count) return NULL;
    if (!part->entries[part->active_index].valid) return NULL;
    return &part->entries[part->active_index].profile;
}

uint32_t vtx_phase_partition_active_phase(const vtx_phase_partition_t *part)
{
    if (part == NULL) return VTX_PHASE_NONE;
    if (part->active_index >= part->entry_count) return VTX_PHASE_NONE;
    if (!part->entries[part->active_index].valid) return VTX_PHASE_NONE;
    return part->entries[part->active_index].phase_id;
}

/* ========================================================================== */
/* Phase transitions                                                           */
/* ========================================================================== */

vtx_profile_global_t *vtx_phase_partition_transition(
    vtx_phase_partition_t *part,
    uint32_t new_phase_id,
    uint64_t now_ns_)
{
    if (part == NULL) return NULL;

    uint64_t now = (now_ns_ == 0) ? part_now_ns() : now_ns_;

    /* No-op if same phase. */
    uint32_t current_phase = vtx_phase_partition_active_phase(part);
    if (current_phase == new_phase_id) {
        return vtx_phase_partition_get_active(part);
    }

    /* Update outgoing phase stats. */
    if (part->active_index < part->entry_count &&
        part->entries[part->active_index].valid) {
        part->entries[part->active_index].transition_count++;
        part->entries[part->active_index].last_transition_ns = now;
    }

    /* Find or create the new phase's entry. */
    uint32_t new_idx = find_or_create_entry(part, new_phase_id);
    if (new_idx == UINT32_MAX) {
        /* Allocation failure — keep the current active phase. */
        return vtx_phase_partition_get_active(part);
    }

    /* Update incoming phase stats. */
    part->entries[new_idx].transition_count++;
    part->entries[new_idx].last_transition_ns = now;

    /* Make it active. */
    part->active_index = new_idx;
    part->total_transitions++;

    return &part->entries[new_idx].profile;
}

/* ========================================================================== */
/* Per-phase queries                                                           */
/* ========================================================================== */

vtx_profile_global_t *vtx_phase_partition_get_phase(
    vtx_phase_partition_t *part,
    uint32_t phase_id)
{
    if (part == NULL) return NULL;
    uint32_t idx = find_entry(part, phase_id);
    if (idx == UINT32_MAX) return NULL;
    return &part->entries[idx].profile;
}

vtx_profile_global_t *vtx_phase_partition_get_or_create(
    vtx_phase_partition_t *part,
    uint32_t phase_id)
{
    if (part == NULL) return NULL;
    uint32_t idx = find_or_create_entry(part, phase_id);
    if (idx == UINT32_MAX) return NULL;
    return &part->entries[idx].profile;
}

uint32_t vtx_phase_partition_phase_count(const vtx_phase_partition_t *part)
{
    if (part == NULL) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < part->entry_count; i++) {
        if (part->entries[i].valid) count++;
    }
    return count;
}

/* ========================================================================== */
/* Preemptive recompilation helper                                             */
/* ========================================================================== */

/* Comparison function for sorting methods by descending invocation count.
 * Used by qsort in hot_methods_for_phase. */
typedef struct {
    uint32_t method_id;
    uint64_t invocation_count;
} method_heat_t;

static int compare_method_heat_desc(const void *a, const void *b)
{
    const method_heat_t *ma = (const method_heat_t *)a;
    const method_heat_t *mb = (const method_heat_t *)b;
    if (ma->invocation_count > mb->invocation_count) return -1;
    if (ma->invocation_count < mb->invocation_count) return 1;
    return 0;
}

uint32_t vtx_phase_partition_hot_methods_for_phase(
    vtx_phase_partition_t *part,
    const vtx_phase_graph_t *graph,
    uint32_t phase_id,
    uint32_t *out_methods,
    uint32_t out_capacity)
{
    if (part == NULL || graph == NULL || out_methods == NULL || out_capacity == 0) {
        return 0;
    }

    /* Get the phase definition from the graph. */
    const vtx_phase_t *phase_def = vtx_phase_get_by_id(graph, phase_id);
    if (phase_def == NULL) return 0;

    /* Get this phase's profile. */
    vtx_profile_global_t *phase_profile = vtx_phase_partition_get_phase(part, phase_id);
    if (phase_profile == NULL) return 0;

    /* Build (method_id, invocation_count) pairs for all methods in the
     * phase that also have profile data. */
    uint32_t n = phase_def->method_count;
    if (n > out_capacity) n = out_capacity;

    /* Allocate a temp array (size bounded by phase's method count). */
    method_heat_t *heat = (method_heat_t *)malloc(
        phase_def->method_count * sizeof(method_heat_t));
    if (heat == NULL) return 0;

    uint32_t heat_count = 0;
    for (uint32_t i = 0; i < phase_def->method_count; i++) {
        uint32_t mid = phase_def->method_ids[i];
        const vtx_profile_method_t *m = vtx_profile_get_method(phase_profile, mid);
        if (m == NULL) continue;
        if (m->invocation_count == 0) continue;
        heat[heat_count].method_id = mid;
        heat[heat_count].invocation_count = m->invocation_count;
        heat_count++;
    }

    /* Sort by descending heat. */
    qsort(heat, heat_count, sizeof(method_heat_t), compare_method_heat_desc);

    /* Copy the top out_capacity method IDs to the output. */
    uint32_t to_copy = (heat_count < out_capacity) ? heat_count : out_capacity;
    for (uint32_t i = 0; i < to_copy; i++) {
        out_methods[i] = heat[i].method_id;
    }

    free(heat);
    return to_copy;
}

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

void vtx_phase_partition_stats(const vtx_phase_partition_t *part,
                                 uint64_t *total_transitions,
                                 uint64_t *total_creations,
                                 uint32_t *active_phase_id)
{
    if (part == NULL) {
        if (total_transitions) *total_transitions = 0;
        if (total_creations)   *total_creations = 0;
        if (active_phase_id)   *active_phase_id = VTX_PHASE_NONE;
        return;
    }
    if (total_transitions) *total_transitions = part->total_transitions;
    if (total_creations)   *total_creations   = part->total_phase_creations;
    if (active_phase_id)   *active_phase_id   = vtx_phase_partition_active_phase(part);
}
