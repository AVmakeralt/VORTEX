#include "interp/profiler.h"

#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Profiler init / destroy                                                     */
/* ========================================================================== */

#define VTX_PROFILER_INITIAL_CAPACITY 64

int vtx_profiler_init(vtx_profiler_t *profiler)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");

    profiler->capacity = VTX_PROFILER_INITIAL_CAPACITY;
    profiler->data = (vtx_profile_data_t *)calloc(
        profiler->capacity, sizeof(vtx_profile_data_t));
    if (profiler->data == NULL) {
        profiler->count = 0;
        return -1;
    }
    profiler->count = 0;

    /* Initialize LRU cache to empty */
    memset(profiler->lru, 0, sizeof(profiler->lru));

    return 0;
}

void vtx_profiler_destroy(vtx_profiler_t *profiler)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");

    for (uint32_t i = 0; i < profiler->count; i++) {
        vtx_profile_data_t *pd = &profiler->data[i];
        free(pd->call_site_types);
        pd->call_site_types = NULL;
        free(pd->branch_taken_counts);
        pd->branch_taken_counts = NULL;
        free(pd->branch_total_counts);
        pd->branch_total_counts = NULL;
        free(pd->field_shape_ids);
        pd->field_shape_ids = NULL;
    }

    free(profiler->data);
    profiler->data = NULL;
    profiler->count = 0;
    profiler->capacity = 0;
}

/* ========================================================================== */
/* Helper: grow the profiler array                                             */
/* ========================================================================== */

static int profiler_ensure_capacity(vtx_profiler_t *profiler, uint32_t needed)
{
    if (needed <= profiler->capacity) {
        return 0;
    }

    uint32_t new_capacity = profiler->capacity;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }

    vtx_profile_data_t *new_data = (vtx_profile_data_t *)realloc(
        profiler->data, new_capacity * sizeof(vtx_profile_data_t));
    if (new_data == NULL) {
        return -1;
    }

    /* Zero-initialize new slots */
    memset(new_data + profiler->capacity, 0,
           (new_capacity - profiler->capacity) * sizeof(vtx_profile_data_t));

    profiler->data = new_data;
    profiler->capacity = new_capacity;
    return 0;
}

/* ========================================================================== */
/* Helper: ensure branch and field arrays are allocated for a method           */
/* ========================================================================== */

static void ensure_profile_arrays(vtx_profile_data_t *pd)
{
    if (pd->method == NULL || pd->method->bytecode == NULL) {
        return;
    }

    uint32_t code_length = (uint32_t)pd->method->bytecode->length;

    /* Allocate branch frequency arrays if needed */
    if (pd->branch_taken_counts == NULL && code_length > 0) {
        pd->branch_taken_counts = (uint32_t *)calloc(code_length, sizeof(uint32_t));
        pd->branch_total_counts = (uint32_t *)calloc(code_length, sizeof(uint32_t));
        pd->branch_array_size = code_length;
    }

    /* Allocate field shape array if needed */
    if (pd->field_shape_ids == NULL && code_length > 0) {
        pd->field_shape_ids = (uint32_t *)calloc(code_length, sizeof(uint32_t));
        pd->field_array_size = code_length;
    }
}

/* ========================================================================== */
/* LRU cache helpers                                                           */
/* ========================================================================== */

/**
 * Look up a method in the LRU cache. Returns the profile data pointer
 * if found, or NULL. On hit, moves the entry to MRU position (slot 0).
 */
static vtx_profile_data_t *lru_lookup(vtx_profiler_t *profiler,
                                       const vtx_method_desc_t *method)
{
    for (int i = 0; i < VTX_PROFILER_LRU_SIZE; i++) {
        if (profiler->lru[i].method == method && profiler->lru[i].pd != NULL) {
            /* Hit — move to MRU (slot 0) by shifting entries down */
            if (i > 0) {
                const vtx_method_desc_t *tmp_method = profiler->lru[i].method;
                vtx_profile_data_t      *tmp_pd     = profiler->lru[i].pd;
                memmove(&profiler->lru[1], &profiler->lru[0],
                        i * sizeof(profiler->lru[0]));
                profiler->lru[0].method = tmp_method;
                profiler->lru[0].pd     = tmp_pd;
            }
            return profiler->lru[0].pd;
        }
    }
    return NULL;
}

/**
 * Insert a method→pd mapping into the LRU cache at MRU position.
 * Evicts the least recently used entry (last slot).
 */
static void lru_insert(vtx_profiler_t *profiler,
                        const vtx_method_desc_t *method,
                        vtx_profile_data_t *pd)
{
    /* Shift everything down by one, evicting the last entry */
    memmove(&profiler->lru[1], &profiler->lru[0],
            (VTX_PROFILER_LRU_SIZE - 1) * sizeof(profiler->lru[0]));
    profiler->lru[0].method = method;
    profiler->lru[0].pd = pd;
}

/* ========================================================================== */
/* Get or create method profile data                                           */
/* ========================================================================== */

vtx_profile_data_t *vtx_profiler_get_method_data(vtx_profiler_t *profiler,
                                                   const vtx_method_desc_t *method)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    /* Fast path: check LRU cache first (O(1) for repeated calls) */
    vtx_profile_data_t *cached = lru_lookup(profiler, method);
    if (cached != NULL) {
        return cached;
    }

    /* Slow path: linear search for existing entry */
    for (uint32_t i = 0; i < profiler->count; i++) {
        if (profiler->data[i].method == method) {
            vtx_profile_data_t *pd = &profiler->data[i];
            lru_insert(profiler, method, pd);
            return pd;
        }
    }

    /* Not found — create a new entry */
    if (profiler_ensure_capacity(profiler, profiler->count + 1) != 0) {
        return NULL;
    }

    vtx_profile_data_t *pd = &profiler->data[profiler->count];
    memset(pd, 0, sizeof(vtx_profile_data_t));
    pd->method = method;
    pd->compiled_tier = VT_TIER_T0;
    pd->deopt_count = 0;
    pd->invocation_count = 0;
    pd->backward_branch_count = 0;
    pd->call_site_types = NULL;
    pd->call_site_count = 0;

    /* Allocate per-PC arrays based on bytecode length */
    ensure_profile_arrays(pd);

    profiler->count++;

    /* Insert into LRU cache */
    lru_insert(profiler, method, pd);

    return pd;
}

/* ========================================================================== */
/* Recording functions                                                         */
/* ========================================================================== */

void vtx_profiler_record_invocation(vtx_profiler_t *profiler,
                                     const vtx_method_desc_t *method)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    vtx_profile_data_t *pd = vtx_profiler_get_method_data(profiler, method);
    if (pd == NULL) {
        return;
    }

    /* Saturating increment */
    if (pd->invocation_count < UINT64_MAX) {
        pd->invocation_count++;
    }
}

void vtx_profiler_record_backward_branch(vtx_profiler_t *profiler,
                                          const vtx_method_desc_t *method)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    vtx_profile_data_t *pd = vtx_profiler_get_method_data(profiler, method);
    if (pd == NULL) {
        return;
    }

    /* Saturating increment */
    if (pd->backward_branch_count < UINT64_MAX) {
        pd->backward_branch_count++;
    }
}

void vtx_profiler_record_branch(vtx_profiler_t *profiler,
                                 const vtx_method_desc_t *method,
                                 uint32_t pc,
                                 bool taken)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    vtx_profile_data_t *pd = vtx_profiler_get_method_data(profiler, method);
    if (pd == NULL) {
        return;
    }

    /* Ensure arrays are allocated */
    ensure_profile_arrays(pd);

    /* Bounds check on PC */
    if (pd->branch_total_counts == NULL || pc >= pd->branch_array_size) {
        return;
    }

    /* Saturating increments */
    if (pd->branch_total_counts[pc] < UINT32_MAX) {
        pd->branch_total_counts[pc]++;
    }
    if (taken && pd->branch_taken_counts[pc] < UINT32_MAX) {
        pd->branch_taken_counts[pc]++;
    }
}

void vtx_profiler_record_call_type(vtx_profiler_t *profiler,
                                    const vtx_method_desc_t *method,
                                    uint32_t call_pc,
                                    vtx_typeid_t typeid_)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    vtx_profile_data_t *pd = vtx_profiler_get_method_data(profiler, method);
    if (pd == NULL) {
        return;
    }

    /* Ensure call_site_types array is large enough.
     * We use the call_pc as an index. If the array is too small,
     * grow it. */
    uint32_t needed_count = call_pc + 1;
    if (pd->call_site_types == NULL || needed_count > pd->call_site_count) {
        uint32_t new_count = pd->call_site_count > 0 ? pd->call_site_count : 16;
        while (new_count < needed_count) {
            new_count *= 2;
        }

        vtx_call_site_profile_t *new_arr = (vtx_call_site_profile_t *)realloc(
            pd->call_site_types, new_count * sizeof(vtx_call_site_profile_t));
        if (new_arr == NULL) {
            return;
        }

        /* Zero-initialize new slots */
        memset(new_arr + pd->call_site_count, 0,
               (new_count - pd->call_site_count) * sizeof(vtx_call_site_profile_t));

        pd->call_site_types = new_arr;
        pd->call_site_count = new_count;
    }

    /* Record the type at this call site */
    vtx_call_site_profile_t *site = &pd->call_site_types[call_pc];

    /* Check if this typeid is already recorded */
    for (uint32_t i = 0; i < site->count; i++) {
        if (site->entries[i].typeid_ == typeid_) {
            return; /* already recorded */
        }
    }

    /* Add the new type if there's room */
    if (site->count < VTX_POLY_LIMIT) {
        site->entries[site->count].typeid_ = typeid_;
        site->entries[site->count].method = NULL; /* filled in by lookup */
        site->count++;
    }
    /* If VTX_POLY_LIMIT reached, we stop recording (megamorphic,
     * the optimizer will fall back to vtable) */
}

void vtx_profiler_record_field_shape(vtx_profiler_t *profiler,
                                      const vtx_method_desc_t *method,
                                      uint32_t field_pc,
                                      vtx_shapeid_t shapeid)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    vtx_profile_data_t *pd = vtx_profiler_get_method_data(profiler, method);
    if (pd == NULL) {
        return;
    }

    /* Ensure arrays are allocated */
    ensure_profile_arrays(pd);

    /* Record the shape at this PC */
    if (pd->field_shape_ids != NULL && field_pc < pd->field_array_size) {
        pd->field_shape_ids[field_pc] = shapeid;
    }
}

/* ========================================================================== */
/* Heat calculation and tier decisions                                         */
/* ========================================================================== */

uint64_t vtx_profiler_method_heat(vtx_profiler_t *profiler,
                                   const vtx_method_desc_t *method)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    vtx_profile_data_t *pd = vtx_profiler_get_method_data(profiler, method);
    if (pd == NULL) {
        return 0;
    }

    uint64_t invocations = pd->invocation_count;
    uint64_t backward_branches = pd->backward_branch_count;

    /* heat = invocations + backward_branches * 2
     * This doubles the weight of backward branches, penalizing hot loops.
     * A method with many loop iterations is hotter than one that's called
     * often but does little work per call. */
    uint64_t heat = invocations + backward_branches * 2;

    /* Saturating: check for overflow */
    if (heat < invocations) {
        heat = UINT64_MAX;
    }

    return heat;
}

bool vtx_profiler_should_compile_t1(vtx_profiler_t *profiler,
                                     const vtx_method_desc_t *method)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    uint64_t heat = vtx_profiler_method_heat(profiler, method);
    return heat > (uint64_t)VORTEX_T1_THRESHOLD;
}

bool vtx_profiler_should_compile_t2(vtx_profiler_t *profiler,
                                     const vtx_method_desc_t *method)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    uint64_t heat = vtx_profiler_method_heat(profiler, method);
    return heat > (uint64_t)VORTEX_T2_THRESHOLD;
}

/* ========================================================================== */
/* Query functions                                                             */
/* ========================================================================== */

const vtx_call_site_profile_t *vtx_profiler_get_call_site_profile(
    const vtx_profiler_t *profiler,
    const vtx_method_desc_t *method,
    uint32_t call_pc)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    /* Find the method's profile data — check LRU cache first */
    const vtx_profile_data_t *pd = lru_lookup((vtx_profiler_t *)profiler, method);
    if (pd == NULL) {
        /* Slow path: linear search */
        for (uint32_t i = 0; i < profiler->count; i++) {
            if (profiler->data[i].method == method) {
                pd = &profiler->data[i];
                break;
            }
        }
    }
    if (pd == NULL) return NULL;

    if (pd->call_site_types != NULL && call_pc < pd->call_site_count) {
        return &pd->call_site_types[call_pc];
    }
    return NULL;
}

double vtx_profiler_get_branch_probability(const vtx_profiler_t *profiler,
                                            const vtx_method_desc_t *method,
                                            uint32_t pc)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    /* Find the method's profile data — check LRU cache first */
    const vtx_profile_data_t *pd = lru_lookup((vtx_profiler_t *)profiler, method);
    if (pd == NULL) {
        for (uint32_t i = 0; i < profiler->count; i++) {
            if (profiler->data[i].method == method) {
                pd = &profiler->data[i];
                break;
            }
        }
    }
    if (pd == NULL) return 0.5;

    if (pd->branch_total_counts != NULL && pc < pd->branch_array_size) {
        uint32_t total = pd->branch_total_counts[pc];
        uint32_t taken = pd->branch_taken_counts[pc];
        if (total == 0) {
            return 0.5; /* unknown */
        }
        return (double)taken / (double)total;
    }
    return 0.5;
}

vtx_shapeid_t vtx_profiler_get_field_shape(const vtx_profiler_t *profiler,
                                             const vtx_method_desc_t *method,
                                             uint32_t field_pc)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    /* Find the method's profile data — check LRU cache first */
    const vtx_profile_data_t *pd = lru_lookup((vtx_profiler_t *)profiler, method);
    if (pd == NULL) {
        for (uint32_t i = 0; i < profiler->count; i++) {
            if (profiler->data[i].method == method) {
                pd = &profiler->data[i];
                break;
            }
        }
    }
    if (pd == NULL) return VTX_SHAPE_INVALID;

    if (pd->field_shape_ids != NULL && field_pc < pd->field_array_size) {
        return (vtx_shapeid_t)pd->field_shape_ids[field_pc];
    }
    return VTX_SHAPE_INVALID;
}

void vtx_profiler_record_deopt(vtx_profiler_t *profiler,
                                const vtx_method_desc_t *method)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    vtx_profile_data_t *pd = vtx_profiler_get_method_data(profiler, method);
    if (pd == NULL) {
        return;
    }

    if (pd->deopt_count < UINT32_MAX) {
        pd->deopt_count++;
    }
}

void vtx_profiler_set_compiled_tier(vtx_profiler_t *profiler,
                                     const vtx_method_desc_t *method,
                                     vtx_compile_tier_t tier)
{
    VTX_ASSERT(profiler != NULL, "profiler must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    vtx_profile_data_t *pd = vtx_profiler_get_method_data(profiler, method);
    if (pd == NULL) {
        return;
    }

    pd->compiled_tier = tier;
}
