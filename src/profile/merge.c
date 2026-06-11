#include "profile/merge.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ========================================================================== */
/* Internal helpers                                                           */
/* ========================================================================== */

static inline uint64_t saturating_add64(uint64_t a, uint64_t b)
{
    uint64_t sum = a + b;
    return (sum < a) ? UINT64_MAX : sum;
}

/* ========================================================================== */
/* Callsite merge                                                             */
/* ========================================================================== */

void vtx_profile_merge_callsite(vtx_callsite_profile_t *target,
                                 const vtx_callsite_profile_t *source)
{
    if (!target || !source) return;

    /* If either is megamorphic, result is megamorphic */
    if (source->megamorphic) {
        target->megamorphic = true;
    }

    if (target->megamorphic) return;

    /* Union the type observations from source into target */
    for (uint32_t i = 0; i < source->count; i++) {
        vtx_typeid_t src_type = source->types[i];

        /* Check if already in target */
        bool found = false;
        for (uint32_t j = 0; j < target->count; j++) {
            if (target->types[j] == src_type) {
                found = true;
                break;
            }
        }

        if (!found) {
            if (target->count < VTX_POLY_LIMIT) {
                target->types[target->count] = src_type;
                target->count++;
            } else {
                /* Exceeded polymorphic limit */
                target->megamorphic = true;
                break;
            }
        }
    }
}

/* ========================================================================== */
/* Branch merge                                                               */
/* ========================================================================== */

void vtx_profile_merge_branch(vtx_branch_profile_t *target,
                               const vtx_branch_profile_t *source)
{
    if (!target || !source) return;

    target->taken     = saturating_add64(target->taken, source->taken);
    target->not_taken = saturating_add64(target->not_taken, source->not_taken);
}

/* ========================================================================== */
/* Field access merge                                                         */
/* ========================================================================== */

void vtx_profile_merge_field(vtx_field_profile_t *target,
                              const vtx_field_profile_t *source)
{
    if (!target || !source) return;

    /* If either is megamorphic, result is megamorphic */
    if (source->megamorphic) {
        target->megamorphic = true;
    }

    if (target->megamorphic) return;

    /* Union the shape observations from source into target */
    for (uint32_t i = 0; i < source->count; i++) {
        vtx_shapeid_t src_shape = source->shapes[i];

        bool found = false;
        for (uint32_t j = 0; j < target->count; j++) {
            if (target->shapes[j] == src_shape) {
                found = true;
                break;
            }
        }

        if (!found) {
            if (target->count < VTX_POLY_LIMIT) {
                target->shapes[target->count] = src_shape;
                target->count++;
            } else {
                target->megamorphic = true;
                break;
            }
        }
    }
}

/* ========================================================================== */
/* Loop merge                                                                 */
/* ========================================================================== */

void vtx_profile_merge_loop(vtx_loop_profile_t *target,
                             const vtx_loop_profile_t *source)
{
    if (!target || !source) return;

    target->backedge_count = saturating_add64(target->backedge_count,
                                               source->backedge_count);
}

/* ========================================================================== */
/* Method merge                                                               */
/* ========================================================================== */

void vtx_profile_merge_method(vtx_profile_global_t *target,
                               const vtx_profile_method_t *source_method)
{
    if (!target || !source_method) return;

    /* Find or create the target method */
    vtx_profile_method_t *tm = vtx_profile_get_method(target,
                                                       source_method->method_id);
    if (!tm) {
        tm = vtx_profile_add_method(target, source_method->method_id);
        if (!tm) return;
    }

    /* Sum invocation counts */
    tm->invocation_count = saturating_add64(tm->invocation_count,
                                             source_method->invocation_count);

    /* Merge call sites */
    for (uint32_t i = 0; i < source_method->call_site_count; i++) {
        const vtx_callsite_profile_t *src_cs = &source_method->call_sites[i];

        /* Find or create the target callsite at the same index */
        /* We need to ensure target has enough callsite entries */
        if (i >= tm->call_site_capacity) {
            uint32_t new_cap = tm->call_site_capacity;
            if (new_cap == 0) new_cap = 8;
            while (new_cap <= i) new_cap *= 2;

            vtx_callsite_profile_t *new_arr = realloc(
                tm->call_sites,
                (size_t)new_cap * sizeof(vtx_callsite_profile_t));
            if (!new_arr) continue;
            memset(new_arr + tm->call_site_capacity, 0,
                   (size_t)(new_cap - tm->call_site_capacity) *
                   sizeof(vtx_callsite_profile_t));
            tm->call_sites = new_arr;
            tm->call_site_capacity = new_cap;
        }
        if (i >= tm->call_site_count) {
            tm->call_site_count = i + 1;
        }

        vtx_profile_merge_callsite(&tm->call_sites[i], src_cs);
    }

    /* Merge branches */
    for (uint32_t i = 0; i < source_method->branch_count; i++) {
        const vtx_branch_profile_t *src_br = &source_method->branches[i];

        /* Find matching branch by bytecode_pc */
        vtx_branch_profile_t *tgt_br = NULL;
        for (uint32_t j = 0; j < tm->branch_count; j++) {
            if (tm->branches[j].bytecode_pc == src_br->bytecode_pc) {
                tgt_br = &tm->branches[j];
                break;
            }
        }

        if (tgt_br) {
            vtx_profile_merge_branch(tgt_br, src_br);
        } else {
            /* Add new branch entry */
            if (tm->branch_count < tm->branch_capacity) {
                tgt_br = &tm->branches[tm->branch_count++];
            } else {
                /* Grow */
                uint32_t new_cap = tm->branch_capacity * 2;
                if (new_cap == 0) new_cap = 8;
                vtx_branch_profile_t *new_arr = realloc(
                    tm->branches,
                    (size_t)new_cap * sizeof(vtx_branch_profile_t));
                if (!new_arr) continue;
                memset(new_arr + tm->branch_capacity, 0,
                       (size_t)(new_cap - tm->branch_capacity) *
                       sizeof(vtx_branch_profile_t));
                tm->branches = new_arr;
                tm->branch_capacity = new_cap;
                tgt_br = &tm->branches[tm->branch_count++];
            }
            *tgt_br = *src_br;
        }
    }

    /* Merge field accesses */
    for (uint32_t i = 0; i < source_method->field_access_count; i++) {
        const vtx_field_profile_t *src_fa = &source_method->field_accesses[i];

        /* Find matching field access by field_offset */
        vtx_field_profile_t *tgt_fa = NULL;
        for (uint32_t j = 0; j < tm->field_access_count; j++) {
            if (tm->field_accesses[j].field_offset == src_fa->field_offset) {
                tgt_fa = &tm->field_accesses[j];
                break;
            }
        }

        if (tgt_fa) {
            vtx_profile_merge_field(tgt_fa, src_fa);
        } else {
            /* Add new field access entry */
            if (tm->field_access_count < tm->field_access_capacity) {
                tgt_fa = &tm->field_accesses[tm->field_access_count++];
            } else {
                uint32_t new_cap = tm->field_access_capacity * 2;
                if (new_cap == 0) new_cap = 8;
                vtx_field_profile_t *new_arr = realloc(
                    tm->field_accesses,
                    (size_t)new_cap * sizeof(vtx_field_profile_t));
                if (!new_arr) continue;
                memset(new_arr + tm->field_access_capacity, 0,
                       (size_t)(new_cap - tm->field_access_capacity) *
                       sizeof(vtx_field_profile_t));
                tm->field_accesses = new_arr;
                tm->field_access_capacity = new_cap;
                tgt_fa = &tm->field_accesses[tm->field_access_count++];
            }
            *tgt_fa = *src_fa;
        }
    }

    /* Merge loops */
    for (uint32_t i = 0; i < source_method->loop_count; i++) {
        const vtx_loop_profile_t *src_lp = &source_method->loops[i];

        /* Find matching loop by loop_header_pc */
        vtx_loop_profile_t *tgt_lp = NULL;
        for (uint32_t j = 0; j < tm->loop_count; j++) {
            if (tm->loops[j].loop_header_pc == src_lp->loop_header_pc) {
                tgt_lp = &tm->loops[j];
                break;
            }
        }

        if (tgt_lp) {
            vtx_profile_merge_loop(tgt_lp, src_lp);
        } else {
            /* Add new loop entry */
            if (tm->loop_count < tm->loop_capacity) {
                tgt_lp = &tm->loops[tm->loop_count++];
            } else {
                uint32_t new_cap = tm->loop_capacity * 2;
                if (new_cap == 0) new_cap = 8;
                vtx_loop_profile_t *new_arr = realloc(
                    tm->loops,
                    (size_t)new_cap * sizeof(vtx_loop_profile_t));
                if (!new_arr) continue;
                memset(new_arr + tm->loop_capacity, 0,
                       (size_t)(new_cap - tm->loop_capacity) *
                       sizeof(vtx_loop_profile_t));
                tm->loops = new_arr;
                tm->loop_capacity = new_cap;
                tgt_lp = &tm->loops[tm->loop_count++];
            }
            *tgt_lp = *src_lp;
        }
    }
}

/* ========================================================================== */
/* Global merge                                                               */
/* ========================================================================== */

void vtx_profile_merge_into(vtx_profile_global_t *target,
                             const vtx_profile_global_t *source)
{
    if (!target || !source) return;

    /* Merge all methods from source into target */
    for (uint32_t i = 0; i < source->method_count; i++) {
        vtx_profile_merge_method(target, &source->methods[i]);
    }

    /* Merge call edges */
    for (uint32_t i = 0; i < source->call_edge_count; i++) {
        const vtx_call_edge_t *src_edge = &source->call_edges[i];

        /* Find matching edge in target */
        bool found = false;
        for (uint32_t j = 0; j < target->call_edge_count; j++) {
            vtx_call_edge_t *tgt_edge = &target->call_edges[j];
            if (tgt_edge->caller_method_id == src_edge->caller_method_id &&
                tgt_edge->callee_method_id == src_edge->callee_method_id) {
                tgt_edge->frequency = saturating_add64(tgt_edge->frequency,
                                                        src_edge->frequency);
                found = true;
                break;
            }
        }

        if (!found) {
            /* Add new edge */
            if (target->call_edge_count < target->call_edge_capacity) {
                vtx_call_edge_t *e = &target->call_edges[target->call_edge_count++];
                e->caller_method_id = src_edge->caller_method_id;
                e->callee_method_id = src_edge->callee_method_id;
                e->frequency = src_edge->frequency;
            } else {
                /* Grow */
                uint32_t new_cap = target->call_edge_capacity * 2;
                vtx_call_edge_t *new_arr = realloc(
                    target->call_edges,
                    (size_t)new_cap * sizeof(vtx_call_edge_t));
                if (!new_arr) continue;
                target->call_edges = new_arr;
                target->call_edge_capacity = new_cap;
                vtx_call_edge_t *e = &target->call_edges[target->call_edge_count++];
                e->caller_method_id = src_edge->caller_method_id;
                e->callee_method_id = src_edge->callee_method_id;
                e->frequency = src_edge->frequency;
            }
        }
    }

    /* Note: phase transitions are NOT merged — they are recomputed
     * by vtx_phase_detect() from the merged call graph. */
}
