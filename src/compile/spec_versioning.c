/**
 * VORTEX Speculative Method Versioning by Argument Type (Level 2B)
 * and Speculative Loop Versioning (Level 2C)
 *
 * Implementation of type-specialized method versions and loop versioning.
 * See compile/spec_versioning.h for the design rationale and API documentation.
 */

#define _POSIX_C_SOURCE 199309L
#include "compile/spec_versioning.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Internal: FNV-1a hash constants                                             */
/* ========================================================================== */

/**
 * FNV-1a offset basis and prime for 64-bit hashing.
 * Same constants used in deoptless.c and phase_react.c for consistency.
 */
#define FNV_OFFSET_BASIS 14695981039346656037ULL
#define FNV_PRIME        1099511628211ULL

/* ========================================================================== */
/* Internal: FNV-1a hash of a single uint32_t                                  */
/* ========================================================================== */

/**
 * Hash a uint32_t value byte-by-byte using FNV-1a.
 * This provides better distribution than hashing the raw 32-bit value
 * because it diffuses the entropy across 4 hash rounds.
 */
static uint64_t fnv1a_hash_u32(uint64_t h, uint32_t value)
{
    h ^= (uint64_t)(value & 0xFF);
    h *= FNV_PRIME;
    h ^= (uint64_t)((value >> 8) & 0xFF);
    h *= FNV_PRIME;
    h ^= (uint64_t)((value >> 16) & 0xFF);
    h *= FNV_PRIME;
    h ^= (uint64_t)((value >> 24) & 0xFF);
    h *= FNV_PRIME;
    return h;
}

/* ========================================================================== */
/* Internal: grow the registries array                                         */
/* ========================================================================== */

/**
 * Ensure the registries array can accommodate method_id.
 * Grows the array via realloc, zeroing new entries.
 *
 * Caller must hold mgr->mutex.
 *
 * @return  0 on success, -1 on failure
 */
static int ensure_registry_capacity(vtx_spec_version_manager_t *mgr,
                                     uint32_t method_id)
{
    if (method_id < mgr->registry_capacity) return 0;

    uint32_t new_cap = mgr->registry_capacity * 2;
    if (new_cap <= method_id) {
        new_cap = method_id + 1;
    }

    vtx_spec_version_registry_t *new_reg = realloc(
        mgr->registries,
        new_cap * sizeof(vtx_spec_version_registry_t));
    if (!new_reg) return -1;

    /* Zero out new entries */
    memset(new_reg + mgr->registry_capacity, 0,
           (new_cap - mgr->registry_capacity) * sizeof(vtx_spec_version_registry_t));

    mgr->registries = new_reg;
    mgr->registry_capacity = new_cap;
    return 0;
}

/* ========================================================================== */
/* Internal: find the least stable version in a registry                       */
/* ========================================================================== */

/**
 * Find the version with the lowest stability in the registry.
 * Used for eviction when the registry is at capacity.
 *
 * If multiple versions have the same stability, the one with the
 * fewest executions is preferred for eviction (least invested).
 *
 * Caller must hold mgr->mutex.
 *
 * @return  Pointer to the least stable version, or NULL if no versions exist
 */
static vtx_spec_version_t *find_least_stable(vtx_spec_version_registry_t *registry)
{
    if (!registry || !registry->versions) return NULL;

    vtx_spec_version_t *worst = registry->versions;
    for (vtx_spec_version_t *v = registry->versions; v != NULL; v = v->next) {
        if (v->stability < worst->stability ||
            (v->stability == worst->stability &&
             v->execution_count < worst->execution_count)) {
            worst = v;
        }
    }
    return worst;
}

/* ========================================================================== */
/* Internal: evict a version from a registry                                   */
/* ========================================================================== */

/**
 * Evict a specific version from the registry's linked list.
 * Marks the version as inactive and frees it.
 *
 * Caller must hold mgr->mutex.
 */
static void evict_version(vtx_spec_version_registry_t *registry,
                           vtx_spec_version_t *version)
{
    if (!registry || !version) return;

    /* Unlink from the linked list */
    vtx_spec_version_t **prev_ptr = &registry->versions;
    while (*prev_ptr && *prev_ptr != version) {
        prev_ptr = &(*prev_ptr)->next;
    }
    if (*prev_ptr == version) {
        *prev_ptr = version->next;
        registry->version_count--;
    }

    /* If we're evicting the hot version, clear the pointer */
    if (registry->hot_version == version) {
        registry->hot_version = NULL;
    }
    /* If we're evicting the default version, clear the pointer */
    if (registry->default_version == version) {
        registry->default_version = NULL;
    }

    /* Note: we don't free version->code_version — it's owned by
     * the version manager (compile/version.h). */
    free(version);
}

/* ========================================================================== */
/* Internal: update arg profile for a single argument position                 */
/* ========================================================================== */

/**
 * Update the argument type profile for a specific argument position.
 *
 * Maintains a top-4 list of types with their frequencies.
 * If the observed type is already in the top-4, its frequency is updated.
 * If not, and there's room, it's inserted. If the top-4 is full,
 * the lowest-frequency entry is replaced if the new type has higher
 * frequency (which it won't on first observation, so this effectively
 * means new types can't displace existing ones unless they accumulate
 * enough observations to exceed the lowest entry).
 *
 * The frequency is computed as: count_for_type / total_observations.
 */
static void update_arg_profile(
    vtx_spec_version_registry_t *registry,
    uint32_t arg_index,
    uint32_t type_id)
{
    if (arg_index >= VTX_SPEC_VERSION_MAX_ARGS) return;

    uint64_t total = registry->arg_profiles[arg_index].total_observations + 1;
    registry->arg_profiles[arg_index].total_observations = total;

    uint32_t *top_ids = registry->arg_profiles[arg_index].top_type_ids;
    double   *top_freqs = registry->arg_profiles[arg_index].top_frequencies;

    /* Check if type_id is already in the top-4 */
    int found_idx = -1;
    for (int i = 0; i < 4; i++) {
        if (top_ids[i] == type_id) {
            found_idx = i;
            break;
        }
    }

    if (found_idx >= 0) {
        /* Update existing entry's frequency */
        top_freqs[found_idx] = (double)(top_freqs[found_idx] * (total - 1) + 1) / (double)total;
    } else {
        /* Type not in top-4 — find a slot */
        int slot = -1;
        /* First, try to find an empty slot (type_id == 0) */
        for (int i = 0; i < 4; i++) {
            if (top_ids[i] == 0) {
                slot = i;
                break;
            }
        }
        if (slot >= 0) {
            top_ids[slot] = type_id;
            top_freqs[slot] = 1.0 / (double)total;
        } else {
            /* All slots occupied — replace the entry with the lowest frequency
             * only if we have enough observations to justify the replacement.
             * For now, we don't replace (conservative: keep existing data). */
        }
    }

    /* Re-sort top-4 by frequency (descending) using insertion sort.
     * Only 4 elements, so this is fast. */
    for (int i = 1; i < 4; i++) {
        if (top_freqs[i] > top_freqs[i - 1]) {
            /* Swap */
            uint32_t tmp_id = top_ids[i];
            double tmp_freq = top_freqs[i];
            top_ids[i] = top_ids[i - 1];
            top_freqs[i] = top_freqs[i - 1];
            top_ids[i - 1] = tmp_id;
            top_freqs[i - 1] = tmp_freq;
        }
    }

    /* Recompute all frequencies as exact counts / total.
     * Since we track frequencies as running averages, they drift.
     * Periodically renormalize to maintain accuracy.
     * For simplicity, we renormalize every call (4 divisions is cheap). */
    if (total > 0) {
        /* The frequencies above are approximate. We don't track exact
         * counts per type, so we use the frequency as-is but rescale
         * so they sum to <= 1.0. The "other" bucket gets the remainder. */
        double sum = 0.0;
        for (int i = 0; i < 4; i++) {
            sum += top_freqs[i];
        }
        /* Clamp: if sum exceeds 1.0, rescale */
        if (sum > 1.0) {
            for (int i = 0; i < 4; i++) {
                top_freqs[i] /= sum;
            }
        }
    }
}

/* ========================================================================== */
/* Internal: find or create a loop registry                                    */
/* ========================================================================== */

/**
 * Find an existing loop registry by (method_id, loop_header_pc).
 * Returns the index in the loop_registries array, or -1 if not found.
 *
 * Caller must hold mgr->mutex.
 */
static int find_loop_registry_index(const vtx_spec_version_manager_t *mgr,
                                      uint32_t method_id,
                                      uint32_t loop_header_pc)
{
    for (uint32_t i = 0; i < mgr->loop_registry_count; i++) {
        if (mgr->loop_registries[i].method_id == method_id &&
            mgr->loop_registries[i].loop_header_pc == loop_header_pc) {
            return (int)i;
        }
    }
    return -1;
}

/* ========================================================================== */
/* Internal: grow the loop registries array                                    */
/* ========================================================================== */

/**
 * Grow the loop registries array if at capacity.
 *
 * Caller must hold mgr->mutex.
 *
 * @return  0 on success, -1 on failure
 */
static int ensure_loop_registry_capacity(vtx_spec_version_manager_t *mgr)
{
    if (mgr->loop_registry_count < mgr->loop_registry_capacity) return 0;

    uint32_t new_cap = mgr->loop_registry_capacity == 0
        ? VTX_LOOP_VERSION_INITIAL_CAPACITY
        : mgr->loop_registry_capacity * 2;

    vtx_loop_version_registry_t *new_arr = realloc(
        mgr->loop_registries,
        new_cap * sizeof(vtx_loop_version_registry_t));
    if (!new_arr) return -1;

    /* Zero out new entries */
    memset(new_arr + mgr->loop_registry_capacity, 0,
           (new_cap - mgr->loop_registry_capacity) * sizeof(vtx_loop_version_registry_t));

    mgr->loop_registries = new_arr;
    mgr->loop_registry_capacity = new_cap;
    return 0;
}

/* ========================================================================== */
/* Internal: evict the least-used loop version                                 */
/* ========================================================================== */

/**
 * Evict the loop version with the lowest execution_count.
 *
 * Caller must hold mgr->mutex.
 */
static void evict_least_used_loop_version(vtx_loop_version_registry_t *registry)
{
    if (!registry || registry->version_count == 0) return;

    uint32_t worst_idx = 0;
    for (uint32_t i = 1; i < registry->version_count; i++) {
        if (registry->versions[i].execution_count <
            registry->versions[worst_idx].execution_count) {
            worst_idx = i;
        }
    }

    /* Shift remaining entries down */
    /* Note: we don't free code_version — owned by the version manager */
    for (uint32_t i = worst_idx; i < registry->version_count - 1; i++) {
        registry->versions[i] = registry->versions[i + 1];
    }
    registry->version_count--;

    /* Adjust active_version index if needed */
    if (registry->active_version == worst_idx) {
        registry->active_version = UINT32_MAX; /* no active version */
    } else if (registry->active_version > worst_idx &&
               registry->active_version != UINT32_MAX) {
        registry->active_version--;
    }
}

/* ========================================================================== */
/* Type signature helpers                                                      */
/* ========================================================================== */

uint64_t vtx_spec_type_sig_hash(const vtx_spec_type_sig_t *sig)
{
    if (!sig) return 0;

    uint64_t h = FNV_OFFSET_BASIS;

    /* Hash arg_count first so signatures with different lengths
     * always produce different hashes. */
    h = fnv1a_hash_u32(h, sig->arg_count);

    /* Hash each arg_type */
    for (uint32_t i = 0; i < sig->arg_count && i < VTX_SPEC_VERSION_MAX_ARGS; i++) {
        h = fnv1a_hash_u32(h, sig->arg_types[i]);
    }

    return h;
}

bool vtx_spec_type_sig_equal(const vtx_spec_type_sig_t *a,
                              const vtx_spec_type_sig_t *b)
{
    if (!a || !b) return false;

    /* Fast rejection via hash */
    if (a->signature_hash != b->signature_hash) return false;

    /* Full comparison */
    if (a->arg_count != b->arg_count) return false;

    for (uint32_t i = 0; i < a->arg_count && i < VTX_SPEC_VERSION_MAX_ARGS; i++) {
        if (a->arg_types[i] != b->arg_types[i]) return false;
    }

    return true;
}

void vtx_spec_type_sig_init(vtx_spec_type_sig_t *sig,
                              uint32_t arg_count,
                              const uint32_t *arg_types)
{
    if (!sig) return;

    memset(sig, 0, sizeof(*sig));

    sig->arg_count = (arg_count <= VTX_SPEC_VERSION_MAX_ARGS)
        ? arg_count : VTX_SPEC_VERSION_MAX_ARGS;

    if (arg_types && sig->arg_count > 0) {
        memcpy(sig->arg_types, arg_types,
               sig->arg_count * sizeof(uint32_t));
    }

    sig->signature_hash = vtx_spec_type_sig_hash(sig);
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_spec_version_manager_init(vtx_spec_version_manager_t *mgr)
{
    VTX_ASSERT(mgr != NULL, "manager must not be NULL");

    memset(mgr, 0, sizeof(*mgr));

    mgr->registry_capacity = VTX_SPEC_VERSION_INITIAL_CAPACITY;
    mgr->registries = calloc(mgr->registry_capacity,
                              sizeof(vtx_spec_version_registry_t));
    if (!mgr->registries) return -1;

    mgr->loop_registry_capacity = VTX_LOOP_VERSION_INITIAL_CAPACITY;
    mgr->loop_registries = calloc(mgr->loop_registry_capacity,
                                   sizeof(vtx_loop_version_registry_t));
    if (!mgr->loop_registries) {
        free(mgr->registries);
        mgr->registries = NULL;
        return -1;
    }

    if (pthread_mutex_init(&mgr->mutex, NULL) != 0) {
        free(mgr->registries);
        free(mgr->loop_registries);
        mgr->registries = NULL;
        mgr->loop_registries = NULL;
        return -1;
    }

    mgr->registry_count = 0;
    mgr->loop_registry_count = 0;
    mgr->loop_versioning_stats.total_loops_versioned = 0;
    mgr->loop_versioning_stats.guards_hoisted = 0;
    mgr->loop_versioning_stats.estimated_checks_saved = 0;
    mgr->total_versions_created = 0;
    mgr->total_versions_deopted = 0;
    mgr->total_direct_dispatches = 0;

    return 0;
}

void vtx_spec_version_manager_destroy(vtx_spec_version_manager_t *mgr)
{
    if (!mgr) return;

    /* Free method version registries */
    for (uint32_t i = 0; i < mgr->registry_count; i++) {
        vtx_spec_version_registry_t *reg = &mgr->registries[i];
        if (!reg->versions) continue;

        /* Free all versions in the linked list */
        vtx_spec_version_t *v = reg->versions;
        while (v) {
            vtx_spec_version_t *next = v->next;
            /* Note: v->code_version is owned by the version manager
             * (compile/version.h), so we don't free it here. */
            free(v);
            v = next;
        }
    }
    free(mgr->registries);
    mgr->registries = NULL;

    /* Free loop version registries */
    for (uint32_t i = 0; i < mgr->loop_registry_count; i++) {
        vtx_loop_version_registry_t *lreg = &mgr->loop_registries[i];
        /* Note: each loop version's code_version is owned by the
         * version manager (compile/version.h), so we don't free it. */
        free(lreg->versions);
        lreg->versions = NULL;
    }
    free(mgr->loop_registries);
    mgr->loop_registries = NULL;

    pthread_mutex_destroy(&mgr->mutex);

    mgr->registry_count = 0;
    mgr->registry_capacity = 0;
    mgr->loop_registry_count = 0;
    mgr->loop_registry_capacity = 0;
}

/* ========================================================================== */
/* Method versioning (Level 2B)                                                */
/* ========================================================================== */

vtx_spec_version_registry_t *vtx_spec_version_get_registry(
    vtx_spec_version_manager_t *mgr, uint32_t method_id)
{
    if (!mgr) return NULL;

    pthread_mutex_lock(&mgr->mutex);

    /* Grow the array if needed */
    if (ensure_registry_capacity(mgr, method_id) != 0) {
        pthread_mutex_unlock(&mgr->mutex);
        return NULL;
    }

    /* Update registry_count if this is a new method */
    if (method_id >= mgr->registry_count) {
        mgr->registry_count = method_id + 1;
    }

    vtx_spec_version_registry_t *reg = &mgr->registries[method_id];

    /* Initialize the registry if this is the first access */
    if (reg->method_id == 0 && reg->versions == NULL) {
        memset(reg, 0, sizeof(*reg));
        reg->method_id = method_id;
    }

    pthread_mutex_unlock(&mgr->mutex);
    return reg;
}

vtx_spec_version_t *vtx_spec_version_find(
    const vtx_spec_version_registry_t *registry,
    const vtx_spec_type_sig_t *signature)
{
    if (!registry || !signature) return NULL;

    for (vtx_spec_version_t *v = registry->versions; v != NULL; v = v->next) {
        if (vtx_spec_type_sig_equal(&v->signature, signature)) {
            return v;
        }
    }
    return NULL;
}

vtx_spec_version_t *vtx_spec_version_create(
    vtx_spec_version_manager_t *mgr,
    uint32_t method_id,
    const vtx_spec_type_sig_t *signature)
{
    if (!mgr || !signature) return NULL;

    pthread_mutex_lock(&mgr->mutex);

    /* Ensure the registry exists */
    if (ensure_registry_capacity(mgr, method_id) != 0) {
        pthread_mutex_unlock(&mgr->mutex);
        return NULL;
    }

    if (method_id >= mgr->registry_count) {
        mgr->registry_count = method_id + 1;
    }

    vtx_spec_version_registry_t *reg = &mgr->registries[method_id];
    if (reg->method_id == 0 && reg->versions == NULL) {
        memset(reg, 0, sizeof(*reg));
        reg->method_id = method_id;
    }

    /* Check if a version with this signature already exists */
    vtx_spec_version_t *existing = vtx_spec_version_find(reg, signature);
    if (existing) {
        pthread_mutex_unlock(&mgr->mutex);
        return existing;
    }

    /* Evict least stable version if at capacity */
    if (reg->version_count >= VTX_SPEC_VERSION_MAX) {
        vtx_spec_version_t *victim = find_least_stable(reg);
        if (victim) {
            evict_version(reg, victim);
            mgr->total_versions_deopted++;
        }
    }

    /* Allocate new version */
    vtx_spec_version_t *v = calloc(1, sizeof(vtx_spec_version_t));
    if (!v) {
        pthread_mutex_unlock(&mgr->mutex);
        return NULL;
    }

    v->method_id = method_id;
    v->signature = *signature;  /* struct copy */
    v->code_version = NULL;
    v->execution_count = 0;
    v->deopt_count = 0;
    v->stability = 1.0;  /* start optimistic */
    v->is_active = false;
    v->is_compiling = true;
    v->guard_id = 0;     /* will be set during compilation */

    /* Prepend to linked list (newest first) */
    v->next = reg->versions;
    reg->versions = v;
    reg->version_count++;

    mgr->total_versions_created++;

    pthread_mutex_unlock(&mgr->mutex);
    return v;
}

bool vtx_spec_version_should_specialize(
    const vtx_spec_version_registry_t *registry,
    const vtx_spec_type_sig_t *signature)
{
    if (!registry || !signature) return false;

    /* Check minimum observation threshold */
    if (registry->total_dispatches < VTX_SPEC_VERSION_MIN_OBSERVATIONS) {
        return false;
    }

    /* Check if we're already at capacity and all versions are stable */
    if (registry->version_count >= VTX_SPEC_VERSION_MAX) {
        /* Only specialize if there's a version we can evict */
        bool has_unstable = false;
        for (vtx_spec_version_t *v = registry->versions; v != NULL; v = v->next) {
            if (v->stability < VTX_SPEC_VERSION_STABILITY_THRESHOLD) {
                has_unstable = true;
                break;
            }
        }
        if (!has_unstable) return false;
    }

    /* Check if the signature's argument types have sufficient frequency
     * in the arg profiles. At least one argument position must have
     * the corresponding type at or above the frequency threshold. */
    bool any_arg_qualifies = false;
    for (uint32_t i = 0; i < signature->arg_count && i < VTX_SPEC_VERSION_MAX_ARGS; i++) {
        uint32_t type_id = signature->arg_types[i];
        if (type_id == 0) continue;  /* skip unset arg positions */

        const uint32_t *top_ids = registry->arg_profiles[i].top_type_ids;
        const double   *top_freqs = registry->arg_profiles[i].top_frequencies;
        uint64_t total_obs = registry->arg_profiles[i].total_observations;

        /* Need enough observations at this arg position */
        if (total_obs < VTX_SPEC_VERSION_MIN_OBSERVATIONS / 4) continue;

        /* Check if this type has sufficient frequency */
        for (int j = 0; j < 4; j++) {
            if (top_ids[j] == type_id && top_freqs[j] >= VTX_SPEC_VERSION_FREQUENCY_THRESHOLD) {
                any_arg_qualifies = true;
                break;
            }
        }
        if (any_arg_qualifies) break;
    }

    return any_arg_qualifies;
}

void vtx_spec_version_record_dispatch(
    vtx_spec_version_registry_t *registry,
    const vtx_spec_type_sig_t *signature)
{
    if (!registry || !signature) return;

    registry->total_dispatches++;

    /* Update arg profiles for each argument position */
    for (uint32_t i = 0; i < signature->arg_count && i < VTX_SPEC_VERSION_MAX_ARGS; i++) {
        update_arg_profile(registry, i, signature->arg_types[i]);
    }

    /* Try to find a matching specialized version */
    vtx_spec_version_t *v = vtx_spec_version_find(registry, signature);
    if (v && v->is_active) {
        v->execution_count++;
        registry->direct_hits++;

        /* Update stability */
        uint64_t total = v->execution_count + v->deopt_count;
        if (total > 0) {
            v->stability = (double)v->execution_count / (double)total;
        }

        /* Update hot version pointer if this version is now the hottest */
        if (!registry->hot_version ||
            v->execution_count > registry->hot_version->execution_count) {
            registry->hot_version = v;
        }
    } else {
        registry->generic_fallbacks++;
    }
}

/* ========================================================================== */
/* Loop versioning (Level 2C)                                                  */
/* ========================================================================== */

vtx_loop_version_registry_t *vtx_loop_version_get_registry(
    vtx_spec_version_manager_t *mgr,
    uint32_t method_id,
    uint32_t loop_header_pc)
{
    if (!mgr) return NULL;

    pthread_mutex_lock(&mgr->mutex);

    /* Check for existing registry */
    int idx = find_loop_registry_index(mgr, method_id, loop_header_pc);
    if (idx >= 0) {
        vtx_loop_version_registry_t *reg = &mgr->loop_registries[idx];
        pthread_mutex_unlock(&mgr->mutex);
        return reg;
    }

    /* Create new registry */
    if (ensure_loop_registry_capacity(mgr) != 0) {
        pthread_mutex_unlock(&mgr->mutex);
        return NULL;
    }

    uint32_t new_idx = mgr->loop_registry_count;
    vtx_loop_version_registry_t *reg = &mgr->loop_registries[new_idx];

    memset(reg, 0, sizeof(*reg));
    reg->method_id = method_id;
    reg->loop_header_pc = loop_header_pc;
    reg->versions = NULL;
    reg->version_count = 0;
    reg->version_capacity = 0;
    reg->active_version = UINT32_MAX; /* no active version */

    mgr->loop_registry_count++;

    pthread_mutex_unlock(&mgr->mutex);
    return reg;
}

vtx_loop_version_t *vtx_loop_version_create(
    vtx_spec_version_manager_t *mgr,
    uint32_t method_id,
    uint32_t loop_header_pc,
    uint32_t item_type_id)
{
    if (!mgr) return NULL;

    pthread_mutex_lock(&mgr->mutex);

    /* Find or create the loop registry */
    int idx = find_loop_registry_index(mgr, method_id, loop_header_pc);
    if (idx < 0) {
        /* Create the registry */
        if (ensure_loop_registry_capacity(mgr) != 0) {
            pthread_mutex_unlock(&mgr->mutex);
            return NULL;
        }

        idx = (int)mgr->loop_registry_count;
        vtx_loop_version_registry_t *reg = &mgr->loop_registries[idx];
        memset(reg, 0, sizeof(*reg));
        reg->method_id = method_id;
        reg->loop_header_pc = loop_header_pc;
        reg->versions = NULL;
        reg->version_count = 0;
        reg->version_capacity = 0;
        reg->active_version = UINT32_MAX;
        mgr->loop_registry_count++;
    }

    vtx_loop_version_registry_t *reg = &mgr->loop_registries[idx];

    /* Check if a version for this item_type already exists */
    vtx_loop_version_t *existing = vtx_loop_version_find(reg, item_type_id);
    if (existing) {
        pthread_mutex_unlock(&mgr->mutex);
        return existing;
    }

    /* Evict least-used version if at capacity */
    if (reg->version_count >= VTX_LOOP_VERSION_MAX) {
        evict_least_used_loop_version(reg);
    }

    /* Grow the versions array if needed */
    if (reg->version_count >= reg->version_capacity) {
        uint32_t new_cap = reg->version_capacity == 0
            ? VTX_LOOP_VERSION_INITIAL_CAPACITY
            : reg->version_capacity * 2;
        vtx_loop_version_t *new_arr = realloc(
            reg->versions,
            new_cap * sizeof(vtx_loop_version_t));
        if (!new_arr) {
            pthread_mutex_unlock(&mgr->mutex);
            return NULL;
        }
        memset(new_arr + reg->version_capacity, 0,
               (new_cap - reg->version_capacity) * sizeof(vtx_loop_version_t));
        reg->versions = new_arr;
        reg->version_capacity = new_cap;
    }

    /* Create the new loop version */
    vtx_loop_version_t *lv = &reg->versions[reg->version_count];
    memset(lv, 0, sizeof(*lv));
    lv->method_id = method_id;
    lv->loop_header_pc = loop_header_pc;
    lv->item_type_id = item_type_id;
    lv->code_version = NULL;
    lv->guard_id = 0;
    lv->execution_count = 0;
    lv->is_active = false;

    reg->version_count++;

    /* Update global statistics */
    mgr->loop_versioning_stats.total_loops_versioned++;
    mgr->loop_versioning_stats.guards_hoisted++;

    pthread_mutex_unlock(&mgr->mutex);
    return lv;
}

vtx_loop_version_t *vtx_loop_version_find(
    const vtx_loop_version_registry_t *registry,
    uint32_t item_type_id)
{
    if (!registry || !registry->versions) return NULL;

    for (uint32_t i = 0; i < registry->version_count; i++) {
        if (registry->versions[i].item_type_id == item_type_id) {
            return &registry->versions[i];
        }
    }
    return NULL;
}

bool vtx_loop_should_version(
    const vtx_loop_version_registry_t *registry,
    uint32_t item_type_id,
    double frequency)
{
    if (!registry) return false;

    /* Frequency must exceed threshold */
    if (frequency < VTX_LOOP_VERSION_FREQUENCY_THRESHOLD) return false;

    /* Check if already at capacity */
    if (registry->version_count >= VTX_LOOP_VERSION_MAX) {
        /* Only version if there's a version we can evict */
        bool has_evictable = false;
        for (uint32_t i = 0; i < registry->version_count; i++) {
            if (!registry->versions[i].is_active ||
                registry->versions[i].execution_count < VTX_LOOP_VERSION_MIN_TRIP_COUNT) {
                has_evictable = true;
                break;
            }
        }
        if (!has_evictable) return false;
    }

    /* Check if a version for this type already exists */
    if (vtx_loop_version_find(registry, item_type_id)) {
        return false; /* already versioned */
    }

    return true;
}

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

void vtx_spec_version_get_stats(const vtx_spec_version_manager_t *mgr,
                                  uint32_t *versions_created,
                                  uint32_t *versions_deopted,
                                  uint32_t *direct_dispatches)
{
    if (!mgr) {
        if (versions_created)  *versions_created = 0;
        if (versions_deopted)  *versions_deopted = 0;
        if (direct_dispatches) *direct_dispatches = 0;
        return;
    }

    if (versions_created)  *versions_created = mgr->total_versions_created;
    if (versions_deopted)  *versions_deopted = mgr->total_versions_deopted;
    if (direct_dispatches) *direct_dispatches = mgr->total_direct_dispatches;
}
