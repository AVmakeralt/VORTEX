#ifndef VORTEX_CODECACHE_EVICT_H
#define VORTEX_CODECACHE_EVICT_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "codecache/cache.h"
#include "codecache/install.h"

/**
 * VORTEX LRU Eviction
 *
 * When the code cache exceeds VORTEX_CACHE_MAX_SIZE, evict the
 * least-recently-used methods. LRU tracking uses an amortized
 * timestamp: updated every VTX_LRU_UPDATE_INTERVAL calls to avoid
 * write contention on hot call paths.
 *
 * Eviction policy:
 *   1. Find the method with the oldest last_used_timestamp
 *   2. Mark it as not-compiled (code pointer → NULL)
 *   3. Free its side table and deopt info
 *   4. Code in segments is freed only when the segment is completely empty
 *
 * Evicted methods will be recompiled if they become hot again.
 */

/* ========================================================================== */
/* LRU eviction                                                                */
/* ========================================================================== */

/**
 * Evict methods using LRU policy until the cache is under the max size.
 *
 * For each method, the last_used_timestamp is updated every
 * VTX_LRU_UPDATE_INTERVAL calls (amortized cost). This function
 * finds the oldest methods and evicts them.
 *
 * @param cache       Code cache
 * @param registry    Method registry
 * @param current_ts  Current global timestamp (monotonically increasing)
 * @return            Number of methods evicted, or -1 on failure
 */
int vtx_evict_lru(vtx_code_cache_t *cache,
                   vtx_method_registry_t *registry,
                   uint64_t current_ts);

/**
 * Update the LRU timestamp for a method.
 * Called when a method is invoked. The timestamp is only updated
 * every VTX_LRU_UPDATE_INTERVAL calls to amortize the cost.
 *
 * @param method  The compiled method
 * @param ts      Current global timestamp
 */
void vtx_evict_touch(vtx_compiled_method_t *method, uint64_t ts);

/**
 * Find the least recently used method in the registry.
 * Returns the method, or NULL if the registry is empty.
 */
vtx_compiled_method_t *vtx_evict_find_lru(vtx_method_registry_t *registry);

/**
 * Evict a specific method from the cache.
 *
 * @param cache     Code cache
 * @param registry  Method registry
 * @param method    The method to evict
 * @return          0 on success, -1 on failure
 */
int vtx_evict_method(vtx_code_cache_t *cache,
                      vtx_method_registry_t *registry,
                      vtx_compiled_method_t *method);

#endif /* VORTEX_CODECACHE_EVICT_H */
