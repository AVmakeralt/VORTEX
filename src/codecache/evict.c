/**
 * VORTEX Clock (Second Chance) Eviction
 *
 * Evicts methods from the code cache using the clock (second chance)
 * algorithm, which is O(1) amortized per eviction instead of the
 * previous O(n) full-scan LRU approach.
 *
 * Each method has a use_bit that is set on call (touch) and cleared
 * by the clock hand during eviction scanning. Methods with a cleared
 * use_bit are eviction candidates; those with a set use_bit get a
 * "second chance" (bit cleared, hand advances).
 *
 * The methods array capacity is always a power of 2, enabling
 * (pos & capacity_mask) instead of (pos % capacity).
 */

#include "codecache/evict.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* ========================================================================== */
/* Touch — set use bit for clock eviction                                      */
/* ========================================================================== */

void vtx_evict_touch(vtx_compiled_method_t *method, uint64_t ts)
{
    if (!method || !method->is_installed) return;

    method->call_count++;
    method->clock_state.use_bit = true;

    /* Only update the timestamp every VTX_LRU_UPDATE_INTERVAL calls
     * to amortize the cost of the atomic write. Kept for diagnostics. */
    if (method->call_count % VTX_LRU_UPDATE_INTERVAL == 0) {
        method->last_used_timestamp = ts;
    }
}

/* ========================================================================== */
/* Clock (Second Chance) eviction — O(1) amortized per eviction                */
/* ========================================================================== */

vtx_compiled_method_t *vtx_evict_find_lru(vtx_method_registry_t *registry)
{
    if (!registry || registry->method_count == 0) return NULL;

    /* Clock (Second Chance) algorithm:
     * Walk the method array starting from clock_hand.
     * For each method:
     *   - If use_bit is set: clear it and advance (give "second chance")
     *   - If use_bit is clear: this is our victim
     * This is O(1) amortized per eviction instead of O(n).
     *
     * We use (pos & capacity_mask) instead of (pos % capacity)
     * because capacity is always a power of 2. */

    uint32_t capacity = registry->capacity;
    uint32_t mask = registry->capacity_mask;
    uint32_t start = registry->clock_hand & mask;
    uint32_t pos = start;

    do {
        vtx_compiled_method_t *m = registry->methods[pos];
        if (m && m->is_installed && m->is_valid) {
            if (m->clock_state.use_bit) {
                /* Recently used — give second chance */
                m->clock_state.use_bit = false;
            } else {
                /* Not recently used — this is our victim */
                registry->clock_hand = (pos + 1) & mask;
                return m;
            }
        }
        pos = (pos + 1) & mask;
    } while (pos != start);

    /* All valid methods had use bits set — evict the first valid one */
    pos = start;
    do {
        vtx_compiled_method_t *m = registry->methods[pos];
        if (m && m->is_installed && m->is_valid) {
            registry->clock_hand = (pos + 1) & mask;
            return m;
        }
        pos = (pos + 1) & mask;
    } while (pos != start);

    return NULL; /* no evictable methods */
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
        __atomic_store_n(&method->method_desc->compiled_code, NULL, __ATOMIC_RELEASE);
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
