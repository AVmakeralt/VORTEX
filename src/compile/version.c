/**
 * VORTEX Code Version Management
 *
 * Manages multiple compiled versions per method with reference counting
 * and lifecycle transitions.
 */

#define _POSIX_C_SOURCE 199309L
#include "compile/version.h"
#include "compile/priority.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========================================================================== */
/* Version state name                                                          */
/* ========================================================================== */

const char *vtx_version_state_name(vtx_version_state_t s)
{
    switch (s) {
    case VTX_VERSION_COMPILING:   return "Compiling";
    case VTX_VERSION_ACTIVE:      return "Active";
    case VTX_VERSION_DEPRECATED:  return "Deprecated";
    case VTX_VERSION_INVALIDATED: return "Invalidated";
    case VTX_VERSION_FREED:       return "Freed";
    }
    return "Unknown";
}

/* ========================================================================== */
/* Internal: get current time in nanoseconds                                  */
/* ========================================================================== */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ========================================================================== */
/* Internal: find or create method versions entry                             */
/* ========================================================================== */

static vtx_method_versions_t *get_method_versions(
    vtx_version_manager_t *manager, uint32_t method_id, bool create)
{
    /* Grow the array if needed */
    if (method_id >= manager->capacity) {
        if (!create) return NULL;

        uint32_t new_cap = method_id + 1;
        if (new_cap < manager->capacity * 2) {
            new_cap = manager->capacity * 2;
        }

        vtx_method_versions_t **new_methods = realloc(manager->methods,
            new_cap * sizeof(vtx_method_versions_t *));
        if (!new_methods) return NULL;

        /* Zero out new entries */
        memset(new_methods + manager->capacity, 0,
               (new_cap - manager->capacity) * sizeof(vtx_method_versions_t *));

        manager->methods = new_methods;
        manager->capacity = new_cap;
    }

    if (!manager->methods[method_id] && create) {
        vtx_method_versions_t *mv = calloc(1, sizeof(vtx_method_versions_t));
        if (!mv) return NULL;

        mv->method_id = method_id;
        mv->newest = NULL;
        mv->oldest = NULL;
        mv->version_count = 0;

        if (pthread_mutex_init(&mv->method_mutex, NULL) != 0) {
            free(mv);
            return NULL;
        }

        manager->methods[method_id] = mv;
        manager->method_count++;
    }

    return manager->methods[method_id];
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_version_manager_init(vtx_version_manager_t *manager, vtx_arena_t *arena)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");

    memset(manager, 0, sizeof(*manager));

    manager->capacity = VTX_VERSION_INITIAL_CAPACITY;
    manager->methods = calloc(manager->capacity, sizeof(vtx_method_versions_t *));
    if (!manager->methods) return -1;

    manager->method_count = 0;
    manager->arena = arena;

    return 0;
}

void vtx_version_manager_destroy(vtx_version_manager_t *manager)
{
    if (!manager) return;

    for (uint32_t i = 0; i < manager->capacity; i++) {
        vtx_method_versions_t *mv = manager->methods[i];
        if (!mv) continue;

        /* Free all versions in the chain */
        vtx_code_version_t *v = mv->oldest;
        while (v) {
            vtx_code_version_t *next = v->next_version;
            /* Don't free compiled method — it's owned by the code cache */
            free(v);
            v = next;
        }

        pthread_mutex_destroy(&mv->method_mutex);
        free(mv);
    }

    free(manager->methods);
    manager->methods = NULL;
    manager->capacity = 0;
    manager->method_count = 0;
}

/* ========================================================================== */
/* Version lifecycle                                                           */
/* ========================================================================== */

vtx_code_version_t *vtx_version_create_compiling(vtx_version_manager_t *manager,
                                                   uint32_t method_id,
                                                   vtx_compile_tier_t tier)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");

    vtx_method_versions_t *mv = get_method_versions(manager, method_id, true);
    if (!mv) return NULL;

    pthread_mutex_lock(&mv->method_mutex);

    /* Allocate the new version */
    vtx_code_version_t *version = calloc(1, sizeof(vtx_code_version_t));
    if (!version) {
        pthread_mutex_unlock(&mv->method_mutex);
        return NULL;
    }

    version->method_id         = method_id;
    version->version_id        = ++manager->total_versions_created;
    version->tier              = tier;
    version->state             = VTX_VERSION_COMPILING;
    version->compiled          = NULL;
    version->refcount          = 0;
    version->compile_start_ns  = now_ns();
    version->compile_end_ns    = 0;
    version->activate_ns       = 0;
    version->deprecate_ns      = 0;
    version->is_deoptless      = false;
    version->deoptless_guard_id = 0;

    /* Add to the version chain as the newest version */
    version->next_version = NULL;
    version->prev_version = mv->newest;

    if (mv->newest) {
        mv->newest->next_version = version;
    } else {
        mv->oldest = version; /* first version */
    }
    mv->newest = version;
    mv->version_count++;

    pthread_mutex_unlock(&mv->method_mutex);

    return version;
}

int vtx_version_install(vtx_version_manager_t *manager,
                         uint32_t method_id,
                         vtx_code_version_t *version,
                         vtx_compiled_method_t *compiled)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");
    VTX_ASSERT(version != NULL, "version must not be NULL");
    /* compiled may be NULL for test/version-lifecycle-only installs */

    vtx_method_versions_t *mv = get_method_versions(manager, method_id, false);
    if (!mv) return -1;

    pthread_mutex_lock(&mv->method_mutex);

    /* Verify the version is in Compiling state */
    if (version->state != VTX_VERSION_COMPILING) {
        pthread_mutex_unlock(&mv->method_mutex);
        return -1;
    }

    /* Transition: Compiling → Active */
    version->state          = VTX_VERSION_ACTIVE;
    version->compiled       = compiled;
    version->compile_end_ns = now_ns();
    version->activate_ns    = version->compile_end_ns;

    /* Deprecate the previous active version (if any) */
    vtx_code_version_t *prev = version->prev_version;
    while (prev) {
        if (prev->state == VTX_VERSION_ACTIVE) {
            prev->state = VTX_VERSION_DEPRECATED;
            prev->deprecate_ns = now_ns();
            manager->total_versions_deprecated++;
            break; /* only one active version at a time */
        }
        prev = prev->prev_version;
    }

    pthread_mutex_unlock(&mv->method_mutex);
    return 0;
}

int vtx_version_deprecate(vtx_version_manager_t *manager,
                           vtx_code_version_t *version)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");
    VTX_ASSERT(version != NULL, "version must not be NULL");

    vtx_method_versions_t *mv = get_method_versions(manager,
        version->method_id, false);
    if (!mv) return -1;

    pthread_mutex_lock(&mv->method_mutex);

    if (version->state != VTX_VERSION_ACTIVE) {
        pthread_mutex_unlock(&mv->method_mutex);
        return -1;
    }

    version->state = VTX_VERSION_DEPRECATED;
    version->deprecate_ns = now_ns();
    manager->total_versions_deprecated++;

    /* The version will be freed when:
     *   - vtx_version_exit() is called and refcount drops to 0, or
     *   - vtx_version_free() is called explicitly by the caller.
     * We do NOT auto-free here so that callers can observe the
     * DEPRECATED state before the version transitions to FREED. */

    pthread_mutex_unlock(&mv->method_mutex);
    return 0;
}

uint32_t vtx_version_invalidate(vtx_version_manager_t *manager,
                                 uint32_t method_id)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");

    vtx_method_versions_t *mv = get_method_versions(manager, method_id, false);
    if (!mv) return 0;

    pthread_mutex_lock(&mv->method_mutex);

    uint32_t invalidated = 0;
    vtx_code_version_t *v = mv->oldest;

    while (v) {
        if (v->state == VTX_VERSION_COMPILING ||
            v->state == VTX_VERSION_ACTIVE ||
            v->state == VTX_VERSION_DEPRECATED) {
            v->state = VTX_VERSION_INVALIDATED;
            invalidated++;
            manager->total_versions_invalidated++;

            /* Mark the compiled method as invalid */
            if (v->compiled) {
                v->compiled->is_valid = false;
                v->compiled->is_installed = false;
            }
        }
        v = v->next_version;
    }

    pthread_mutex_unlock(&mv->method_mutex);
    return invalidated;
}

int vtx_version_free(vtx_version_manager_t *manager,
                      vtx_code_version_t *version)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");
    VTX_ASSERT(version != NULL, "version must not be NULL");

    /* Only free deprecated or invalidated versions with zero refcount */
    if (version->state != VTX_VERSION_DEPRECATED &&
        version->state != VTX_VERSION_INVALIDATED) {
        return -1;
    }

    if (__atomic_load_n(&version->refcount, __ATOMIC_ACQUIRE) != 0) {
        return -1; /* still has threads executing */
    }

    vtx_method_versions_t *mv = get_method_versions(manager,
        version->method_id, false);
    if (!mv) return -1;

    /* Remove from the linked list */
    if (version->prev_version) {
        version->prev_version->next_version = version->next_version;
    } else {
        mv->oldest = version->next_version;
    }

    if (version->next_version) {
        version->next_version->prev_version = version->prev_version;
    } else {
        mv->newest = version->prev_version;
    }

    mv->version_count--;

    /* Mark as freed and deallocate */
    version->state = VTX_VERSION_FREED;
    manager->total_versions_freed++;

    /* Note: we don't free version->compiled here — the code cache
     * owns the compiled method and will free it during eviction. */
    free(version);

    return 0;
}

/* ========================================================================== */
/* Reference counting                                                          */
/* ========================================================================== */

void vtx_version_enter(vtx_code_version_t *version)
{
    VTX_ASSERT(version != NULL, "version must not be NULL");
    __atomic_fetch_add(&version->refcount, 1, __ATOMIC_ACQUIRE);
}

bool vtx_version_exit(vtx_version_manager_t *manager,
                       vtx_code_version_t *version)
{
    VTX_ASSERT(version != NULL, "version must not be NULL");

    int32_t old = __atomic_fetch_sub(&version->refcount, 1, __ATOMIC_RELEASE);
    if (old == 1) {
        /* Refcount dropped to zero */
        if (version->state == VTX_VERSION_DEPRECATED ||
            version->state == VTX_VERSION_INVALIDATED) {
            /* Safe to free — must hold method_mutex to modify the version list */
            vtx_method_versions_t *mv = get_method_versions(manager,
                version->method_id, false);
            if (mv) {
                pthread_mutex_lock(&mv->method_mutex);
                /* Re-check state under lock: another thread may have
                 * changed it between the lockless check and here. */
                if (version->state == VTX_VERSION_DEPRECATED ||
                    version->state == VTX_VERSION_INVALIDATED) {
                    vtx_version_free(manager, version);
                }
                pthread_mutex_unlock(&mv->method_mutex);
            }
            return true;
        }
    }
    return false;
}

/* ========================================================================== */
/* Queries                                                                     */
/* ========================================================================== */

vtx_code_version_t *vtx_version_get_active(vtx_version_manager_t *manager,
                                             uint32_t method_id)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");

    vtx_method_versions_t *mv = get_method_versions(manager, method_id, false);
    if (!mv) return NULL;

    pthread_mutex_lock(&mv->method_mutex);

    /* Search from newest to oldest for an Active version */
    vtx_code_version_t *v = mv->newest;
    while (v) {
        if (v->state == VTX_VERSION_ACTIVE) {
            pthread_mutex_unlock(&mv->method_mutex);
            return v;
        }
        v = v->prev_version;
    }

    pthread_mutex_unlock(&mv->method_mutex);
    return NULL;
}

vtx_code_version_t *vtx_version_get_newest(vtx_version_manager_t *manager,
                                              uint32_t method_id)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");

    vtx_method_versions_t *mv = get_method_versions(manager, method_id, false);
    if (!mv) return NULL;

    return mv->newest;
}

uint32_t vtx_version_count(vtx_version_manager_t *manager,
                             uint32_t method_id)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");

    vtx_method_versions_t *mv = get_method_versions(manager, method_id, false);
    if (!mv) return 0;

    return mv->version_count;
}

bool vtx_version_is_executable(const vtx_code_version_t *version)
{
    if (!version) return false;
    return version->state == VTX_VERSION_ACTIVE;
}

/* ========================================================================== */
/* Tier-based version lookup (Proposal #5)                                       */
/* ========================================================================== */

vtx_code_version_t *vtx_version_find_tier(vtx_version_manager_t *manager,
                                             uint32_t method_id,
                                             vtx_compile_tier_t max_tier)
{
    if (!manager) return NULL;
    if (method_id >= manager->capacity) return NULL;

    vtx_method_versions_t *mv = manager->methods[method_id];
    if (!mv) return NULL;

    /* Walk the version chain from newest to oldest, looking for
     * an active version at or below max_tier. */
    vtx_code_version_t *best = NULL;

    for (vtx_code_version_t *v = mv->newest; v != NULL; v = v->prev_version) {
        if (v->state != VTX_VERSION_ACTIVE) continue;
        if ((int)v->tier > (int)max_tier) continue;
        if (v->compiled == NULL || !v->compiled->is_installed) continue;

        /* Prefer the highest tier that's still within our budget */
        if (best == NULL || (int)v->tier > (int)best->tier) {
            best = v;
        }
    }

    return best;
}
