/**
 * VORTEX LRU Eviction
 *
 * Evicts least-recently-used methods from the code cache when it
 * exceeds the maximum size. Uses amortized timestamps to minimize
 * overhead on hot call paths.
 */

#include "codecache/evict.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* ========================================================================== */
/* LRU touch (update timestamp)                                                */
/* ========================================================================== */

void vtx_evict_touch(vtx_compiled_method_t *method, uint64_t ts)
{
    if (!method || !method->is_installed) return;

    method->call_count++;

    /* Only update the timestamp every VTX_LRU_UPDATE_INTERVAL calls
     * to amortize the cost of the atomic write. */
    if (method->call_count % VTX_LRU_UPDATE_INTERVAL == 0) {
        method->last_used_timestamp = ts;
    }
}

/* ========================================================================== */
/* Find LRU method                                                             */
/* ========================================================================== */

vtx_compiled_method_t *vtx_evict_find_lru(vtx_method_registry_t *registry)
{
    if (!registry || registry->method_count == 0) return NULL;

    vtx_compiled_method_t *lru = NULL;
    uint64_t oldest_ts = UINT64_MAX;

    for (uint32_t i = 0; i < registry->method_count; i++) {
        vtx_compiled_method_t *m = registry->methods[i];
        if (!m || !m->is_installed || !m->is_valid) continue;

        /* Prefer evicting methods with the oldest timestamp.
         * If timestamps are equal, prefer the one with fewer calls. */
        if (m->last_used_timestamp < oldest_ts) {
            oldest_ts = m->last_used_timestamp;
            lru = m;
        } else if (m->last_used_timestamp == oldest_ts && lru &&
                   m->call_count < lru->call_count) {
            lru = m;
        }
    }

    return lru;
}

/* ========================================================================== */
/* Evict a single method                                                       */
/* ========================================================================== */

int vtx_evict_method(vtx_code_cache_t *cache,
                      vtx_method_registry_t *registry,
                      vtx_compiled_method_t *method)
{
    if (!cache || !registry || !method) return -1;

    /* Mark the method as not compiled */
    method->is_installed = false;
    method->is_valid = false;

    /* Set the method's code pointer to NULL with release store */
    if (method->method_desc) {
        __atomic_store_n(&method->method_desc->bytecode, NULL, __ATOMIC_RELEASE);
    }

    /* Free the code in the cache segment */
    vtx_code_cache_free(cache, method->code_start, method->code_size);

    /* Free metadata */
    if (method->side_table) {
        vtx_side_table_destroy(method->side_table);
        method->side_table = NULL;
    }
    if (method->dep_type_ids) {
        free(method->dep_type_ids);
        method->dep_type_ids = NULL;
        method->dep_type_count = 0;
    }
    if (method->dep_shape_ids) {
        free(method->dep_shape_ids);
        method->dep_shape_ids = NULL;
        method->dep_shape_count = 0;
    }

    /* Remove from registry */
    vtx_method_registry_remove(registry, method->method_id);

    /* Free the compiled method struct */
    free(method);

    return 0;
}

/* ========================================================================== */
/* LRU eviction loop                                                           */
/* ========================================================================== */

int vtx_evict_lru(vtx_code_cache_t *cache,
                   vtx_method_registry_t *registry,
                   uint64_t current_ts)
{
    if (!cache || !registry) return -1;

    int evicted = 0;

    /* Keep evicting until we're under the max size */
    while (vtx_code_cache_is_full(cache)) {
        vtx_compiled_method_t *lru = vtx_evict_find_lru(registry);
        if (!lru) break; /* No more methods to evict */

        if (vtx_evict_method(cache, registry, lru) != 0) {
            break; /* Eviction failed */
        }
        evicted++;
    }

    (void)current_ts;
    return evicted;
}
