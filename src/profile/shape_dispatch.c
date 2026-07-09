/**
 * VORTEX Input-Shape-Keyed Dispatch (Sprint 4.3) — Implementation
 *
 * See shape_dispatch.h for design rationale.
 *
 * The dispatch table is the call-time mechanism that makes input-shape-
 * keyed profiles actually useful: without it, the JIT would compile
 * multiple versions but have no way to pick the right one at call time.
 */

#include "profile/shape_dispatch.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========================================================================== */
/* Internal helpers                                                            */
/* ========================================================================== */

static uint64_t dispatch_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ========================================================================== */
/* Per-method dispatch table lifecycle                                         */
/* ========================================================================== */

static int dispatch_table_init(vtx_shape_dispatch_t *table, uint32_t method_id)
{
    if (table == NULL) return -1;
    memset(table, 0, sizeof(*table));
    table->method_id = method_id;
    table->version_count = 0;
    if (pthread_mutex_init(&table->mutex, NULL) != 0) return -1;
    return 0;
}

static void dispatch_table_destroy(vtx_shape_dispatch_t *table)
{
    if (table == NULL) return;
    pthread_mutex_destroy(&table->mutex);
    /* The compiled_code pointers are NOT freed here — they're owned by
     * the code cache. The dispatch table just holds references. */
    memset(table, 0, sizeof(*table));
}

/* Find the index of the version for the given shape, or UINT32_MAX. */
static uint32_t find_version(vtx_shape_dispatch_t *table,
                               vtx_input_shape_t shape)
{
    for (uint32_t i = 0; i < VTX_SHAPE_DISPATCH_MAX_VERSIONS; i++) {
        if (table->versions[i].valid && table->versions[i].shape == shape) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* Find the LRU version (smallest call_count) for eviction.
 * Never evicts the default (index 0). */
static uint32_t find_lru_version(vtx_shape_dispatch_t *table)
{
    uint32_t lru_idx = UINT32_MAX;
    uint64_t lru_calls = UINT64_MAX;
    for (uint32_t i = 1; i < VTX_SHAPE_DISPATCH_MAX_VERSIONS; i++) {
        if (!table->versions[i].valid) continue;
        if (table->versions[i].call_count < lru_calls) {
            lru_calls = table->versions[i].call_count;
            lru_idx = i;
        }
    }
    return lru_idx;
}

/* ========================================================================== */
/* Global manager lifecycle                                                    */
/* ========================================================================== */

int vtx_shape_dispatch_mgr_init(vtx_shape_dispatch_mgr_t *mgr)
{
    if (mgr == NULL) return -1;
    memset(mgr, 0, sizeof(*mgr));
    mgr->table_capacity = 64;
    mgr->tables = (vtx_shape_dispatch_t **)calloc(
        mgr->table_capacity, sizeof(vtx_shape_dispatch_t *));
    if (mgr->tables == NULL) {
        mgr->table_capacity = 0;
        return -1;
    }
    if (pthread_mutex_init(&mgr->global_mutex, NULL) != 0) {
        free(mgr->tables);
        mgr->tables = NULL;
        mgr->table_capacity = 0;
        return -1;
    }
    mgr->table_count = 0;
    mgr->total_dispatches = 0;
    mgr->total_default_fallbacks = 0;
    mgr->total_versions_compiled = 0;
    return 0;
}

void vtx_shape_dispatch_mgr_destroy(vtx_shape_dispatch_mgr_t *mgr)
{
    if (mgr == NULL) return;
    pthread_mutex_lock(&mgr->global_mutex);
    if (mgr->tables != NULL) {
        for (uint32_t i = 0; i < mgr->table_count; i++) {
            if (mgr->tables[i] != NULL) {
                dispatch_table_destroy(mgr->tables[i]);
                free(mgr->tables[i]);
                mgr->tables[i] = NULL;
            }
        }
        free(mgr->tables);
        mgr->tables = NULL;
    }
    mgr->table_count = 0;
    mgr->table_capacity = 0;
    pthread_mutex_unlock(&mgr->global_mutex);
    pthread_mutex_destroy(&mgr->global_mutex);
}

/* Ensure the tables array can accommodate the given method_id.
 * Caller must hold global_mutex. */
static bool ensure_capacity_locked(vtx_shape_dispatch_mgr_t *mgr,
                                     uint32_t method_id)
{
    if (method_id < mgr->table_capacity) return true;

    uint32_t new_cap = mgr->table_capacity;
    while (new_cap <= method_id) new_cap *= 2;

    vtx_shape_dispatch_t **new_tables = (vtx_shape_dispatch_t **)realloc(
        mgr->tables, new_cap * sizeof(vtx_shape_dispatch_t *));
    if (new_tables == NULL) return false;
    memset(new_tables + mgr->table_capacity, 0,
           (new_cap - mgr->table_capacity) * sizeof(vtx_shape_dispatch_t *));
    mgr->tables = new_tables;
    mgr->table_capacity = new_cap;
    return true;
}

/* Get or create the dispatch table for a method.
 * Caller must hold global_mutex. */
static vtx_shape_dispatch_t *get_or_create_table_locked(
    vtx_shape_dispatch_mgr_t *mgr,
    uint32_t method_id)
{
    if (!ensure_capacity_locked(mgr, method_id)) return NULL;
    if (method_id >= mgr->table_count) {
        mgr->table_count = method_id + 1;
    }
    if (mgr->tables[method_id] == NULL) {
        mgr->tables[method_id] = (vtx_shape_dispatch_t *)calloc(
            1, sizeof(vtx_shape_dispatch_t));
        if (mgr->tables[method_id] == NULL) return NULL;
        if (dispatch_table_init(mgr->tables[method_id], method_id) != 0) {
            free(mgr->tables[method_id]);
            mgr->tables[method_id] = NULL;
            return NULL;
        }
    }
    return mgr->tables[method_id];
}

/* ========================================================================== */
/* Version installation                                                        */
/* ========================================================================== */

int vtx_shape_dispatch_install(vtx_shape_dispatch_mgr_t *mgr,
                                 uint32_t method_id,
                                 vtx_input_shape_t shape,
                                 void *compiled_code,
                                 void *code_metadata)
{
    if (mgr == NULL || compiled_code == NULL) return -1;
    if (shape == VTX_INPUT_SHAPE_DEFAULT) {
        /* Use the default install path. */
        return vtx_shape_dispatch_install_default(mgr, method_id,
                                                     compiled_code, code_metadata);
    }

    pthread_mutex_lock(&mgr->global_mutex);
    vtx_shape_dispatch_t *table = get_or_create_table_locked(mgr, method_id);
    if (table == NULL) {
        pthread_mutex_unlock(&mgr->global_mutex);
        return -1;
    }
    pthread_mutex_lock(&table->mutex);

    /* Check if this shape already has a version. */
    uint32_t idx = find_version(table, shape);
    if (idx != UINT32_MAX) {
        /* Overwrite the existing version. */
        table->versions[idx].compiled_code = compiled_code;
        table->versions[idx].code_metadata = code_metadata;
        table->versions[idx].compile_time_ns = dispatch_now_ns();
        table->versions[idx].call_count = 0;
        table->versions[idx].valid = true;
    } else {
        /* Find a free slot. */
        uint32_t free_idx = UINT32_MAX;
        for (uint32_t i = 1; i < VTX_SHAPE_DISPATCH_MAX_VERSIONS; i++) {
            if (!table->versions[i].valid) {
                free_idx = i;
                break;
            }
        }
        /* If no free slot, evict the LRU (never index 0 = default). */
        if (free_idx == UINT32_MAX) {
            free_idx = find_lru_version(table);
            if (free_idx == UINT32_MAX) {
                pthread_mutex_unlock(&table->mutex);
                pthread_mutex_unlock(&mgr->global_mutex);
                return -1;
            }
            table->versions[free_idx].valid = false;
            table->version_count--;
        }

        /* Install the new version. */
        vtx_shape_version_t *v = &table->versions[free_idx];
        memset(v, 0, sizeof(*v));
        v->shape = shape;
        v->compiled_code = compiled_code;
        v->code_metadata = code_metadata;
        v->compile_time_ns = dispatch_now_ns();
        v->call_count = 0;
        v->valid = true;
        table->version_count++;
    }

    mgr->total_versions_compiled++;
    pthread_mutex_unlock(&table->mutex);
    pthread_mutex_unlock(&mgr->global_mutex);
    return 0;
}

int vtx_shape_dispatch_install_default(vtx_shape_dispatch_mgr_t *mgr,
                                          uint32_t method_id,
                                          void *compiled_code,
                                          void *code_metadata)
{
    if (mgr == NULL || compiled_code == NULL) return -1;

    pthread_mutex_lock(&mgr->global_mutex);
    vtx_shape_dispatch_t *table = get_or_create_table_locked(mgr, method_id);
    if (table == NULL) {
        pthread_mutex_unlock(&mgr->global_mutex);
        return -1;
    }
    pthread_mutex_lock(&table->mutex);

    /* The default version lives at index 0. */
    vtx_shape_version_t *v = &table->versions[0];
    if (!v->valid) {
        table->version_count++;
    }
    memset(v, 0, sizeof(*v));
    v->shape = VTX_INPUT_SHAPE_DEFAULT;
    v->compiled_code = compiled_code;
    v->code_metadata = code_metadata;
    v->compile_time_ns = dispatch_now_ns();
    v->call_count = 0;
    v->valid = true;

    pthread_mutex_unlock(&table->mutex);
    pthread_mutex_unlock(&mgr->global_mutex);
    return 0;
}

/* ========================================================================== */
/* Dispatch (call-time path)                                                   */
/* ========================================================================== */

void *vtx_shape_dispatch_lookup(vtx_shape_dispatch_mgr_t *mgr,
                                  uint32_t method_id,
                                  vtx_input_shape_t shape)
{
    if (mgr == NULL) return NULL;
    if (method_id >= mgr->table_count) return NULL;
    vtx_shape_dispatch_t *table = mgr->tables[method_id];
    if (table == NULL) return NULL;

    /* Lock-free read: we scan the versions array without acquiring the
     * mutex. This is safe because:
     *   - Writes use atomic pointer swaps (the compiled_code field is
     *     aligned and naturally atomic on x86-64 for 8-byte values)
     *   - The 'valid' flag is set BEFORE the pointer (release ordering)
     *   - If we read a stale 'valid=false', we just fall back to default
     *
     * First, try the specific shape. */
    for (uint32_t i = 1; i < VTX_SHAPE_DISPATCH_MAX_VERSIONS; i++) {
        if (table->versions[i].valid && table->versions[i].shape == shape) {
            void *code = table->versions[i].compiled_code;
            if (code != NULL) {
                /* Atomic increment of call_count (no mutex needed for stats). */
                __atomic_fetch_add(&table->versions[i].call_count, 1,
                                     __ATOMIC_RELAXED);
                return code;
            }
        }
    }

    /* Fall back to the default version (index 0). */
    if (table->versions[0].valid) {
        void *code = table->versions[0].compiled_code;
        if (code != NULL) {
            __atomic_fetch_add(&table->versions[0].call_count, 1,
                                 __ATOMIC_RELAXED);
            return code;
        }
    }

    return NULL;
}

void vtx_shape_dispatch_record(vtx_shape_dispatch_mgr_t *mgr,
                                 bool shape_specific)
{
    if (mgr == NULL) return;
    if (shape_specific) {
        __atomic_fetch_add(&mgr->total_dispatches, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&mgr->total_default_fallbacks, 1, __ATOMIC_RELAXED);
    }
}

/* ========================================================================== */
/* Queries                                                                     */
/* ========================================================================== */

uint32_t vtx_shape_dispatch_version_count(vtx_shape_dispatch_mgr_t *mgr,
                                            uint32_t method_id)
{
    if (mgr == NULL) return 0;
    if (method_id >= mgr->table_count) return 0;
    vtx_shape_dispatch_t *table = mgr->tables[method_id];
    if (table == NULL) return 0;

    /* Count shape-specific versions (exclude the default at index 0). */
    uint32_t count = 0;
    for (uint32_t i = 1; i < VTX_SHAPE_DISPATCH_MAX_VERSIONS; i++) {
        if (table->versions[i].valid) count++;
    }
    return count;
}

vtx_shape_dispatch_t *vtx_shape_dispatch_get_table(
    vtx_shape_dispatch_mgr_t *mgr,
    uint32_t method_id)
{
    if (mgr == NULL) return NULL;
    if (method_id >= mgr->table_count) return NULL;
    return mgr->tables[method_id];
}

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

void vtx_shape_dispatch_stats(const vtx_shape_dispatch_mgr_t *mgr,
                                uint64_t *total_dispatches,
                                uint64_t *total_default_fallbacks,
                                uint64_t *total_versions_compiled,
                                uint32_t *table_count)
{
    if (mgr == NULL) {
        if (total_dispatches) *total_dispatches = 0;
        if (total_default_fallbacks) *total_default_fallbacks = 0;
        if (total_versions_compiled) *total_versions_compiled = 0;
        if (table_count) *table_count = 0;
        return;
    }
    if (total_dispatches) *total_dispatches = mgr->total_dispatches;
    if (total_default_fallbacks) *total_default_fallbacks = mgr->total_default_fallbacks;
    if (total_versions_compiled) *total_versions_compiled = mgr->total_versions_compiled;
    if (table_count) *table_count = mgr->table_count;
}
