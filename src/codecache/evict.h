#ifndef VORTEX_CODECACHE_EVICT_H
#define VORTEX_CODECACHE_EVICT_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "codecache/cache.h"
#include "codecache/install.h"

/**
 * VORTEX Clock (Second Chance) Eviction
 *
 * When the code cache exceeds VORTEX_CACHE_MAX_SIZE, evict methods
 * using the clock (second chance) algorithm. This is O(1) amortized
 * per eviction, replacing the previous O(n) full-scan LRU approach.
 *
 * Each method has a use_bit that is set on call (touch) and cleared
 * by the clock hand during eviction scanning. Methods with a cleared
 * use_bit are eviction candidates; those with a set use_bit get a
 * "second chance" (bit cleared, hand advances).
 *
 * Evicted methods will be recompiled if they become hot again.
 */

/* ========================================================================== */
/* Clock eviction                                                              */
/* ========================================================================== */

/**
 * Evict methods using the clock (second chance) algorithm until the
 * cache is under the max size. O(1) amortized per eviction.
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
 * Update the use bit for a method (clock eviction touch).
 * Called when a method is invoked. Sets the use_bit so the clock
 * algorithm gives the method a "second chance" during eviction.
 * The timestamp is also updated every VTX_LRU_UPDATE_INTERVAL calls
 * for diagnostics.
 *
 * @param method  The compiled method
 * @param ts      Current global timestamp
 */
void vtx_evict_touch(vtx_compiled_method_t *method, uint64_t ts);

/**
 * Find the next eviction candidate using the clock (second chance)
 * algorithm. O(1) amortized per call.
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
