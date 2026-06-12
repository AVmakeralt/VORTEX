/**
 * VORTEX Phase-Reactive Version Reactivation
 *
 * Implementation of phase-reactive version parking and reactivation.
 * See phase_react.h for the design rationale and API documentation.
 */

#define _POSIX_C_SOURCE 199309L
#include "compile/phase_react.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========================================================================== */
/* Internal: FNV-1a hash constants                                             */
/* ========================================================================== */

/**
 * FNV-1a offset basis and prime for 64-bit hashing.
 * Same constants used in deoptless.c for consistency.
 */
#define FNV_OFFSET_BASIS 14695981039346656037ULL
#define FNV_PRIME        1099511628211ULL

/* ========================================================================== */
/* Internal: get current time in nanoseconds                                   */
/* ========================================================================== */

/**
 * Returns the current monotonic time in nanoseconds.
 * Used for LRU eviction (park_time_ns) and timestamping.
 */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ========================================================================== */
/* Internal: registry helpers                                                  */
/* ========================================================================== */

/**
 * Find an entry in the registry by phase hash.
 *
 * Linear scan over the entries array. For the small sizes we deal with
 * (max 16 entries), linear scan is faster than a hash table due to
 * cache locality and zero overhead.
 *
 * @param registry   Per-method registry
 * @param phase_hash Phase hash to search for
 * @return           Index of the matching entry, or -1 if not found
 */
static int find_entry_by_hash(const vtx_phase_version_registry_t *registry,
                               vtx_phase_hash_t phase_hash)
{
    for (uint32_t i = 0; i < VTX_PHASE_REACT_MAX_PARKED; i++) {
        if (registry->entries[i].is_valid &&
            registry->entries[i].phase_hash == phase_hash) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Find the first free slot in the registry.
 *
 * A free slot is one where is_valid is false. If no free slot exists,
 * returns -1 (the caller must evict before inserting).
 *
 * @param registry  Per-method registry
 * @return          Index of the first free slot, or -1 if full
 */
static int find_free_slot(const vtx_phase_version_registry_t *registry)
{
    for (uint32_t i = 0; i < VTX_PHASE_REACT_MAX_PARKED; i++) {
        if (!registry->entries[i].is_valid) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Find the index of the oldest (least recently parked) valid entry.
 *
 * Used for LRU eviction: the entry with the smallest park_time_ns
 * is the one that has been parked the longest and is the best
 * candidate for eviction.
 *
 * @param registry  Per-method registry
 * @return          Index of the oldest entry, or -1 if no valid entries
 */
static int find_oldest_entry(const vtx_phase_version_registry_t *registry)
{
    int oldest_idx = -1;
    uint64_t oldest_time = UINT64_MAX;

    for (uint32_t i = 0; i < VTX_PHASE_REACT_MAX_PARKED; i++) {
        if (registry->entries[i].is_valid &&
            registry->entries[i].park_time_ns < oldest_time) {
            oldest_time = registry->entries[i].park_time_ns;
            oldest_idx = (int)i;
        }
    }
    return oldest_idx;
}

/**
 * Get the code size of a parked version.
 *
 * Looks up the compiled method's code size from the version's
 * compiled metadata. Returns 0 if the version or its compiled
 * metadata is NULL (should not happen for valid parked versions,
 * but we handle it gracefully).
 *
 * @param version  The code version
 * @return         Code size in bytes, or 0 if unknown
 */
static size_t version_code_size(const vtx_code_version_t *version)
{
    if (!version) return 0;
    if (!version->compiled) return 0;
    return version->compiled->code_size;
}

/**
 * Invalidate an entry: mark it as invalid and update the budget.
 *
 * Does NOT free the code version itself — that is the responsibility
 * of the version manager. This only updates the registry entry and
 * the code budget tracking.
 *
 * @param registry  Per-method registry
 * @param idx       Index of the entry to invalidate
 * @param mgr       Phase-reactive manager (for budget update)
 */
static void invalidate_entry(vtx_phase_version_registry_t *registry,
                              int idx,
                              vtx_phase_react_manager_t *mgr)
{
    vtx_phase_version_entry_t *entry = &registry->entries[idx];

    if (!entry->is_valid) return;

    /* Update the code budget: subtract the parked version's code size */
    size_t code_sz = version_code_size(entry->parked_version);
    if (code_sz <= mgr->parked_code_used) {
        mgr->parked_code_used -= code_sz;
    } else {
        /* Safety: should not happen, but prevent underflow */
        mgr->parked_code_used = 0;
    }

    mgr->total_parked_versions--;
    mgr->total_evicted_versions++;
    registry->total_evictions++;
    registry->entry_count--;

    /* Clear the entry */
    entry->is_valid = false;
    entry->phase_hash = VTX_PHASE_HASH_NONE;
    entry->parked_version = NULL;
    entry->park_time_ns = 0;
}

/* ========================================================================== */
/* Internal: grow the registries array                                         */
/* ========================================================================== */

/**
 * Ensure the registries array can accommodate the given method_id.
 * Grows the array via realloc if necessary, zeroing new entries.
 *
 * Caller must hold manager_mutex.
 *
 * @param mgr        Phase-reactive manager
 * @param method_id  Method ID that must be accommodated
 * @return           0 on success, -1 on failure
 */
static int ensure_registry_capacity(vtx_phase_react_manager_t *mgr,
                                     uint32_t method_id)
{
    if (method_id < mgr->registry_capacity) return 0;

    /* Calculate new capacity: at least method_id + 1, but double
     * the current capacity for amortized O(1) growth. */
    uint32_t new_cap = method_id + 1;
    if (new_cap < mgr->registry_capacity * 2) {
        new_cap = mgr->registry_capacity * 2;
    }

    vtx_phase_version_registry_t *new_reg = (vtx_phase_version_registry_t *)realloc(
        mgr->registries, (size_t)new_cap * sizeof(vtx_phase_version_registry_t));
    if (!new_reg) return -1;

    /* Zero out new entries */
    memset(new_reg + mgr->registry_capacity, 0,
           (size_t)(new_cap - mgr->registry_capacity) * sizeof(vtx_phase_version_registry_t));

    mgr->registries = new_reg;
    mgr->registry_capacity = new_cap;
    return 0;
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_phase_react_manager_init(vtx_phase_react_manager_t *mgr, size_t code_budget)
{
    if (!mgr) return -1;

    memset(mgr, 0, sizeof(*mgr));

    mgr->registry_capacity = VTX_PHASE_REACT_INITIAL_CAPACITY;
    mgr->registries = (vtx_phase_version_registry_t *)calloc(
        mgr->registry_capacity, sizeof(vtx_phase_version_registry_t));
    if (!mgr->registries) return -1;

    mgr->registry_count = 0;
    mgr->parked_code_budget = code_budget;
    mgr->parked_code_used = 0;
    mgr->total_parked_versions = 0;
    mgr->total_reactivated_versions = 0;
    mgr->total_evicted_versions = 0;

    if (pthread_mutex_init(&mgr->manager_mutex, NULL) != 0) {
        free(mgr->registries);
        mgr->registries = NULL;
        return -1;
    }

    return 0;
}

void vtx_phase_react_manager_destroy(vtx_phase_react_manager_t *mgr)
{
    if (!mgr) return;

    /* All parked versions are owned by the version manager, not by us.
     * We only free our own registry structures. The version manager
     * will handle freeing the actual code versions. */
    free(mgr->registries);
    mgr->registries = NULL;

    pthread_mutex_destroy(&mgr->manager_mutex);

    mgr->registry_count = 0;
    mgr->registry_capacity = 0;
    mgr->total_parked_versions = 0;
    mgr->parked_code_used = 0;
}

/* ========================================================================== */
/* Registry management                                                         */
/* ========================================================================== */

vtx_phase_version_registry_t *vtx_phase_react_get_registry(
    vtx_phase_react_manager_t *mgr, uint32_t method_id)
{
    if (!mgr) return NULL;

    pthread_mutex_lock(&mgr->manager_mutex);

    /* Grow the array if needed */
    if (ensure_registry_capacity(mgr, method_id) != 0) {
        pthread_mutex_unlock(&mgr->manager_mutex);
        return NULL;
    }

    /* Update the high-water mark */
    if (method_id + 1 > mgr->registry_count) {
        mgr->registry_count = method_id + 1;
    }

    vtx_phase_version_registry_t *reg = &mgr->registries[method_id];

    /* Initialize the registry if this is the first access.
     * We detect this by checking if method_id is still 0 (the zero
     * value from calloc). A registry that was previously used and
     * then cleared would have method_id == 0 but entries would
     * all be zeroed, which is the same state. So we also check
     * current_phase_hash and entry_count for a more reliable
     * detection. Since method_id is set below on first access,
     * subsequent calls will find it already set. */
    if (reg->method_id != method_id) {
        reg->method_id = method_id;
        reg->entry_count = 0;
        reg->current_phase_hash = VTX_PHASE_HASH_NONE;
        reg->total_reactivations = 0;
        reg->total_recompilations = 0;
        reg->total_evictions = 0;
        /* entries array is already zeroed from calloc/realloc */
    }

    pthread_mutex_unlock(&mgr->manager_mutex);
    return reg;
}

/* ========================================================================== */
/* Parking                                                                     */
/* ========================================================================== */

int vtx_phase_react_park(vtx_phase_react_manager_t *mgr,
                          uint32_t method_id,
                          vtx_code_version_t *version,
                          vtx_phase_hash_t phase_hash)
{
    if (!mgr || !version || phase_hash == VTX_PHASE_HASH_NONE) {
        return -1;
    }

    pthread_mutex_lock(&mgr->manager_mutex);

    /* Ensure the registry exists */
    if (ensure_registry_capacity(mgr, method_id) != 0) {
        pthread_mutex_unlock(&mgr->manager_mutex);
        return -1;
    }

    if (method_id + 1 > mgr->registry_count) {
        mgr->registry_count = method_id + 1;
    }

    vtx_phase_version_registry_t *reg = &mgr->registries[method_id];

    /* Initialize the registry if this is the first access for this method */
    if (reg->method_id != method_id) {
        reg->method_id = method_id;
        reg->entry_count = 0;
        reg->current_phase_hash = VTX_PHASE_HASH_NONE;
        reg->total_reactivations = 0;
        reg->total_recompilations = 0;
        reg->total_evictions = 0;
    }

    /* Check if we already have an entry for this phase hash.
     * If so, update it in place (the old parked version for this
     * phase is replaced by the new one). */
    int existing_idx = find_entry_by_hash(reg, phase_hash);
    if (existing_idx >= 0) {
        vtx_phase_version_entry_t *entry = &reg->entries[existing_idx];

        /* Remove the old parked version from the code budget */
        size_t old_code_sz = version_code_size(entry->parked_version);
        if (old_code_sz <= mgr->parked_code_used) {
            mgr->parked_code_used -= old_code_sz;
        } else {
            mgr->parked_code_used = 0;
        }

        /* Update the entry with the new version */
        entry->parked_version = version;
        entry->park_time_ns = now_ns();

        /* Add the new version's code size to the budget */
        size_t new_code_sz = version_code_size(version);
        mgr->parked_code_used += new_code_sz;

        /* Transition the version state to Parked.
         * The Parked state indicates that the version has valid compiled
         * code but is not currently callable. It can be reactivated
         * in O(1) when the same phase recurs. */
        version->state = VTX_VERSION_PARKED;

        pthread_mutex_unlock(&mgr->manager_mutex);
        return 0;
    }

    /* No existing entry for this phase. Find a free slot. */
    int slot = find_free_slot(reg);

    /* If no free slot, evict the oldest entry to make room */
    if (slot < 0) {
        int oldest = find_oldest_entry(reg);
        if (oldest < 0) {
            /* Should not happen: no valid entries but no free slots either */
            pthread_mutex_unlock(&mgr->manager_mutex);
            return -1;
        }

        /* Evict the oldest entry */
        invalidate_entry(reg, oldest, mgr);

        /* The evicted entry's slot is now free */
        slot = oldest;
    }

    /* Fill the slot */
    vtx_phase_version_entry_t *entry = &reg->entries[slot];
    entry->phase_hash = phase_hash;
    entry->parked_version = version;
    entry->method_id = method_id;
    entry->park_time_ns = now_ns();
    entry->reactivate_count = 0;
    entry->is_valid = true;

    reg->entry_count++;
    reg->current_phase_hash = phase_hash;

    /* Update the code budget */
    size_t code_sz = version_code_size(version);
    mgr->parked_code_used += code_sz;
    mgr->total_parked_versions++;

    /* Transition the version state to Parked.
     * The Parked state indicates that the version has valid compiled
     * code but is not currently callable. It can be reactivated
     * in O(1) when the same phase recurs. */
    version->state = VTX_VERSION_PARKED;

    /* If we're now over budget, evict globally */
    if (mgr->parked_code_used > mgr->parked_code_budget) {
        vtx_phase_react_evict_for_budget(mgr);
    }

    pthread_mutex_unlock(&mgr->manager_mutex);
    return 0;
}

/* ========================================================================== */
/* Reactivation                                                               */
/* ========================================================================== */

vtx_code_version_t *vtx_phase_react_try_reactivate(
    vtx_phase_react_manager_t *mgr,
    uint32_t method_id,
    vtx_phase_hash_t phase_hash)
{
    if (!mgr || phase_hash == VTX_PHASE_HASH_NONE) return NULL;

    pthread_mutex_lock(&mgr->manager_mutex);

    /* Bounds check: method_id must be within the registries array */
    if (method_id >= mgr->registry_count) {
        pthread_mutex_unlock(&mgr->manager_mutex);
        return NULL;
    }

    vtx_phase_version_registry_t *reg = &mgr->registries[method_id];

    /* Verify the registry is initialized for this method */
    if (reg->method_id != method_id) {
        pthread_mutex_unlock(&mgr->manager_mutex);
        return NULL;
    }

    /* Look up the phase hash in the registry */
    int idx = find_entry_by_hash(reg, phase_hash);
    if (idx < 0) {
        /* No parked version for this phase. The caller must recompile. */
        reg->total_recompilations++;
        pthread_mutex_unlock(&mgr->manager_mutex);
        return NULL;
    }

    vtx_phase_version_entry_t *entry = &reg->entries[idx];
    vtx_code_version_t *version = entry->parked_version;

    if (!version || !entry->is_valid) {
        /* Entry was invalidated (version freed due to memory pressure) */
        reg->total_recompilations++;
        pthread_mutex_unlock(&mgr->manager_mutex);
        return NULL;
    }

    /* Reactivate: transition the version from Parked → Active.
     *
     * The version was in VTX_VERSION_PARKED state. We transition it
     * back to Active so it can be called. The caller is responsible
     * for deprecating the current active version (if any) via the
     * version manager's standard deprecation flow, and for
     * installing this reactivated version as the method's active code.
     */
    version->state = VTX_VERSION_ACTIVE;
    version->activate_ns = now_ns();

    /* Update the entry: remove it from the registry since it's no
     * longer parked. The slot becomes available for future parking. */
    size_t code_sz = version_code_size(version);
    if (code_sz <= mgr->parked_code_used) {
        mgr->parked_code_used -= code_sz;
    } else {
        mgr->parked_code_used = 0;
    }

    mgr->total_parked_versions--;
    mgr->total_reactivated_versions++;

    /* Update reactivation count before clearing */
    entry->reactivate_count++;

    /* Update per-method statistics */
    reg->total_reactivations++;
    reg->current_phase_hash = phase_hash;
    reg->entry_count--;

    /* Clear the entry — the version is no longer parked */
    entry->is_valid = false;
    entry->phase_hash = VTX_PHASE_HASH_NONE;
    entry->parked_version = NULL;
    entry->park_time_ns = 0;

    pthread_mutex_unlock(&mgr->manager_mutex);
    return version;
}

/* ========================================================================== */
/* Eviction                                                                    */
/* ========================================================================== */

int vtx_phase_react_evict_oldest(vtx_phase_react_manager_t *mgr,
                                  uint32_t method_id)
{
    if (!mgr) return -1;

    /* Note: caller must hold manager_mutex, or this must be called
     * from a context where it's already held. We don't lock here
     * to avoid recursive locking (this is called from park() which
     * already holds the lock). */

    if (method_id >= mgr->registry_count) return -1;

    vtx_phase_version_registry_t *reg = &mgr->registries[method_id];
    if (reg->method_id != method_id) return -1;

    int oldest_idx = find_oldest_entry(reg);
    if (oldest_idx < 0) return -1; /* no valid entries to evict */

    invalidate_entry(reg, oldest_idx, mgr);
    return 0;
}

uint32_t vtx_phase_react_evict_for_budget(vtx_phase_react_manager_t *mgr)
{
    if (!mgr) return 0;

    /* Note: caller must hold manager_mutex. This function is called
     * from vtx_phase_react_park() which already holds the lock. */

    uint32_t evicted = 0;

    /* Keep evicting until we're under budget or there's nothing left.
     *
     * We make multiple passes because eviction from one registry may
     * not free enough space — we need to evict the globally oldest
     * parked version across all registries.
     *
     * Optimization opportunity: for large numbers of methods, a
     * priority queue (min-heap on park_time_ns) would be faster.
     * But for typical workloads (< 1000 methods, < 16 entries each),
     * a linear scan is acceptable. */
    while (mgr->parked_code_used > mgr->parked_code_budget &&
           mgr->total_parked_versions > 0) {

        /* Find the globally oldest parked version */
        uint64_t oldest_time = UINT64_MAX;
        uint32_t oldest_method = UINT32_MAX;
        int oldest_entry_idx = -1;

        for (uint32_t m = 0; m < mgr->registry_count; m++) {
            vtx_phase_version_registry_t *reg = &mgr->registries[m];
            if (reg->method_id != m) continue; /* not initialized */

            for (uint32_t i = 0; i < VTX_PHASE_REACT_MAX_PARKED; i++) {
                if (reg->entries[i].is_valid &&
                    reg->entries[i].park_time_ns < oldest_time) {
                    oldest_time = reg->entries[i].park_time_ns;
                    oldest_method = m;
                    oldest_entry_idx = (int)i;
                }
            }
        }

        if (oldest_entry_idx < 0) break; /* no valid entries found */

        /* Evict the oldest entry */
        invalidate_entry(&mgr->registries[oldest_method], oldest_entry_idx, mgr);
        evicted++;
    }

    return evicted;
}

bool vtx_phase_react_needs_eviction(const vtx_phase_react_manager_t *mgr)
{
    if (!mgr) return false;
    return mgr->parked_code_used > mgr->parked_code_budget;
}

/* ========================================================================== */
/* Phase hash computation                                                      */
/* ========================================================================== */

vtx_phase_hash_t vtx_phase_react_compute_hash(
    const vtx_type_feedback_t *type_feedback,
    uint32_t method_id)
{
    if (!type_feedback) return VTX_PHASE_HASH_NONE;

    /* FNV-1a hash over the type feedback data for the given method.
     *
     * The phase hash captures the type distribution that affects
     * speculation decisions. We hash:
     *   1. The dominant type at each call site (from type_freq)
     *   2. The shape at each field access site
     *
     * This differs from vtx_profile_compute_hash() in deoptless.c:
     *   - deoptless hashes ALL type frequency entries (full distribution)
     *   - phase_react hashes only the TOP (dominant) type at each site
     *     plus the shape at field sites
     *   - deoptless captures the full profile history
     *   - phase_react captures the current phase's type behavior
     *
     * The result: two calls with the same phase hash have the same
     * dominant types and shapes, so a version compiled for one phase
     * should work for the other.
     */
    vtx_phase_hash_t h = FNV_OFFSET_BASIS;

    /* Hash method_id to distinguish different methods */
    h ^= (uint64_t)(method_id & 0xFF);
    h *= FNV_PRIME;
    h ^= (uint64_t)((method_id >> 8) & 0xFF);
    h *= FNV_PRIME;
    h ^= (uint64_t)((method_id >> 16) & 0xFF);
    h *= FNV_PRIME;
    h ^= (uint64_t)((method_id >> 24) & 0xFF);
    h *= FNV_PRIME;

    /* Hash call site type information.
     * For each call site, we hash the dominant (most frequent) type.
     * This captures the current phase's type distribution. */
    if (method_id < type_feedback->call_site_count) {
        const vtx_tf_call_site_t *site = &type_feedback->call_sites[method_id];

        /* Hash the dominant type from the frequency table.
         * The dominant type is the one with the highest count. */
        if (site->type_freq.entry_count > 0) {
            uint32_t max_count = 0;
            vtx_typeid_t dominant_type = 0;

            for (uint32_t i = 0; i < site->type_freq.entry_count &&
                                  i < VTX_TYPE_FREQ_MAX_SLOTS; i++) {
                if (site->type_freq.entries[i].count > max_count) {
                    max_count = site->type_freq.entries[i].count;
                    dominant_type = site->type_freq.entries[i].type_id;
                }
            }

            h ^= (uint64_t)(dominant_type & 0xFF);
            h *= FNV_PRIME;
            h ^= (uint64_t)((dominant_type >> 8) & 0xFF);
            h *= FNV_PRIME;
            h ^= (uint64_t)((dominant_type >> 16) & 0xFF);
            h *= FNV_PRIME;
        }

        /* Hash the stable signature if available.
         * Hyper-stable sites have a verified type signature that
         * uniquely identifies the phase. */
        if (site->stable_signature.slot_count > 0) {
            for (uint32_t i = 0; i < site->stable_signature.slot_count &&
                                  i < VTX_TYPE_SIGNATURE_MAX_SLOTS; i++) {
                uint32_t tid = site->stable_signature.types[i];
                h ^= (uint64_t)(tid & 0xFF);
                h *= FNV_PRIME;
                h ^= (uint64_t)((tid >> 8) & 0xFF);
                h *= FNV_PRIME;
            }
        }

        /* Hash the number of distinct types observed.
         * This distinguishes monomorphic (1 type) from polymorphic
         * (2-4 types) from megamorphic (>4 types) phases. */
        uint32_t type_count = site->type_freq.entry_count;
        h ^= (uint64_t)(type_count & 0xFF);
        h *= FNV_PRIME;
    }

    /* Hash field access site shape information.
     * Shape stability is a strong indicator of the execution phase:
     * different phases often have different object shapes. */
    for (uint32_t f = 0; f < type_feedback->field_site_count; f++) {
        const vtx_tf_field_site_t *fsite = &type_feedback->field_sites[f];

        if (fsite->is_shape_stable && fsite->last_shapeid != 0) {
            uint32_t sid = (uint32_t)fsite->last_shapeid;
            h ^= (uint64_t)(sid & 0xFF);
            h *= FNV_PRIME;
            h ^= (uint64_t)((sid >> 8) & 0xFF);
            h *= FNV_PRIME;
        }
    }

    /* Mix in the method_id again at the end to avoid collisions
     * between methods with similar type distributions */
    h ^= (uint64_t)(method_id & 0xFF);
    h *= FNV_PRIME;

    return h;
}

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

void vtx_phase_react_get_stats(const vtx_phase_react_manager_t *mgr,
                                uint32_t *total_parked,
                                uint32_t *total_reactivated,
                                uint32_t *total_evicted)
{
    if (!mgr) {
        if (total_parked)     *total_parked = 0;
        if (total_reactivated) *total_reactivated = 0;
        if (total_evicted)    *total_evicted = 0;
        return;
    }

    if (total_parked)      *total_parked = mgr->total_parked_versions;
    if (total_reactivated) *total_reactivated = mgr->total_reactivated_versions;
    if (total_evicted)     *total_evicted = mgr->total_evicted_versions;
}

void vtx_phase_react_get_method_stats(const vtx_phase_version_registry_t *registry,
                                       uint32_t *reactivations,
                                       uint32_t *recompilations,
                                       uint32_t *evictions)
{
    if (!registry) {
        if (reactivations)   *reactivations = 0;
        if (recompilations)  *recompilations = 0;
        if (evictions)       *evictions = 0;
        return;
    }

    if (reactivations)   *reactivations = registry->total_reactivations;
    if (recompilations)  *recompilations = registry->total_recompilations;
    if (evictions)       *evictions = registry->total_evictions;
}
