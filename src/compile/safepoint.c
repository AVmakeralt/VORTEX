/**
 * VORTEX Safe Point Checks
 *
 * Application threads check for pending installations and invalidations
 * at safe points. The fast path is a single atomic load.
 */

#define _POSIX_C_SOURCE 199309L
#include "compile/safepoint.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_safepoint_init(vtx_safepoint_manager_t *manager,
                        vtx_method_registry_t *registry,
                        vtx_code_cache_t *code_cache)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");

    memset(manager, 0, sizeof(*manager));

    manager->state = VTX_SP_CLEAR;
    manager->registry = registry;
    manager->code_cache = code_cache;
    manager->install_head = NULL;
    manager->install_tail = NULL;
    manager->invalidate_head = NULL;
    manager->invalidate_tail = NULL;

    if (pthread_mutex_init(&manager->install_mutex, NULL) != 0) {
        return -1;
    }

    if (pthread_mutex_init(&manager->invalidate_mutex, NULL) != 0) {
        pthread_mutex_destroy(&manager->install_mutex);
        return -1;
    }

    return 0;
}

void vtx_safepoint_destroy(vtx_safepoint_manager_t *manager)
{
    if (!manager) return;

    /* Free pending installation requests */
    pthread_mutex_lock(&manager->install_mutex);
    vtx_sp_install_request_t *ireq = manager->install_head;
    while (ireq) {
        vtx_sp_install_request_t *next = ireq->next;
        free(ireq);
        ireq = next;
    }
    manager->install_head = NULL;
    manager->install_tail = NULL;
    pthread_mutex_unlock(&manager->install_mutex);

    /* Free pending invalidation requests */
    pthread_mutex_lock(&manager->invalidate_mutex);
    vtx_sp_invalidate_request_t *vreq = manager->invalidate_head;
    while (vreq) {
        vtx_sp_invalidate_request_t *next = vreq->next;
        free(vreq);
        vreq = next;
    }
    manager->invalidate_head = NULL;
    manager->invalidate_tail = NULL;
    pthread_mutex_unlock(&manager->invalidate_mutex);

    pthread_mutex_destroy(&manager->install_mutex);
    pthread_mutex_destroy(&manager->invalidate_mutex);
}

/* ========================================================================== */
/* Internal: process pending installations                                     */
/* ========================================================================== */

static uint32_t process_installations(vtx_safepoint_manager_t *manager)
{
    uint32_t installed = 0;

    pthread_mutex_lock(&manager->install_mutex);

    /* Drain the installation queue */
    vtx_sp_install_request_t *req = manager->install_head;
    manager->install_head = NULL;
    manager->install_tail = NULL;

    pthread_mutex_unlock(&manager->install_mutex);

    while (req) {
        /* Install the compiled method */
        if (manager->registry) {
            vtx_compiled_method_t *existing =
                vtx_method_registry_get(manager->registry, req->method_id);

            if (existing) {
                /* Update the existing entry's code pointer atomically.
                 * The compiled_method's code_start is already set by
                 * the compilation thread. We just need to mark it as
                 * installed so future calls use it. */
                existing->is_installed = true;
                existing->code_start   = req->compiled_method->code_start;
                existing->code_size    = req->compiled_method->code_size;
                existing->is_valid     = true;

                installed++;
            } else {
                /* New method — add to registry */
                vtx_method_registry_add(manager->registry,
                                         req->compiled_method);
                req->compiled_method->is_installed = true;
                installed++;
            }
        }

        vtx_sp_install_request_t *next = req->next;
        free(req);
        req = next;
    }

    return installed;
}

/* ========================================================================== */
/* Internal: process pending invalidations                                     */
/* ========================================================================== */

static uint32_t process_invalidations(vtx_safepoint_manager_t *manager)
{
    uint32_t invalidated = 0;

    pthread_mutex_lock(&manager->invalidate_mutex);

    /* Drain the invalidation queue */
    vtx_sp_invalidate_request_t *req = manager->invalidate_head;
    manager->invalidate_head = NULL;
    manager->invalidate_tail = NULL;

    pthread_mutex_unlock(&manager->invalidate_mutex);

    while (req) {
        if (manager->registry) {
            vtx_compiled_method_t *method =
                vtx_method_registry_get(manager->registry, req->method_id);

            if (method && method->is_installed) {
                /* Mark the method as invalid — future calls will go
                 * through the interpreter. The code in the cache is
                 * not freed immediately; it will be cleaned up by the
                 * eviction policy. */
                method->is_valid     = false;
                method->is_installed = false;
                invalidated++;
            }
        }

        vtx_sp_invalidate_request_t *next = req->next;
        free(req);
        req = next;
    }

    return invalidated;
}

/* ========================================================================== */
/* Safe point check                                                            */
/* ========================================================================== */

int vtx_safepoint_check(vtx_safepoint_manager_t *manager, void *interp)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");

    /* Fast path: check if anything is pending */
    vtx_safepoint_state_t state =
        __atomic_load_n(&manager->state, __ATOMIC_ACQUIRE);

    if (state == VTX_SP_CLEAR) {
        __atomic_fetch_add(&manager->total_checks, 1, __ATOMIC_RELAXED);
        return 0;
    }

    /* Slow path: process pending work */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int result = 0;

    /* Process installations */
    if (state & VTX_SP_INSTALL_PENDING) {
        uint32_t installed = process_installations(manager);
        manager->total_installations += installed;
        if (installed > 0) {
            result = 1;
        }
    }

    /* Process invalidations */
    if (state & VTX_SP_INVALIDATE_PENDING) {
        uint32_t invalidated = process_invalidations(manager);
        manager->total_invalidations += invalidated;
        if (invalidated > 0) {
            result = -1; /* signal that deopt may be needed */
        }
    }

    /* Clear the global flag */
    __atomic_store_n(&manager->state, VTX_SP_CLEAR, __ATOMIC_RELEASE);

    clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t elapsed_ns =
        (uint64_t)(end.tv_sec - start.tv_sec) * 1000000000ULL +
        (uint64_t)(end.tv_nsec - start.tv_nsec);
    __atomic_fetch_add(&manager->total_time_ns, elapsed_ns, __ATOMIC_RELAXED);

    __atomic_fetch_add(&manager->total_checks, 1, __ATOMIC_RELAXED);

    (void)interp; /* used in full implementation for deopt triggering */

    return result;
}

/* ========================================================================== */
/* Installation requests                                                       */
/* ========================================================================== */

int vtx_safepoint_request_install(vtx_safepoint_manager_t *manager,
                                   uint32_t method_id,
                                   vtx_compiled_method_t *compiled_method)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");
    VTX_ASSERT(compiled_method != NULL, "compiled_method must not be NULL");

    /* Allocate request */
    vtx_sp_install_request_t *req = malloc(sizeof(vtx_sp_install_request_t));
    if (!req) return -1;

    req->method_id = method_id;
    req->compiled_method = compiled_method;
    req->next = NULL;

    /* Add to the installation queue */
    pthread_mutex_lock(&manager->install_mutex);
    if (manager->install_tail) {
        manager->install_tail->next = req;
    } else {
        manager->install_head = req;
    }
    manager->install_tail = req;
    pthread_mutex_unlock(&manager->install_mutex);

    /* Set the global flag to trigger safe point processing */
    vtx_safepoint_state_t old_state =
        __atomic_load_n(&manager->state, __ATOMIC_RELAXED);
    vtx_safepoint_state_t new_state;

    do {
        new_state = (vtx_safepoint_state_t)(
            (unsigned)old_state | VTX_SP_INSTALL_PENDING);
    } while (!__atomic_compare_exchange_n(&manager->state, &old_state,
                                           new_state, false,
                                           __ATOMIC_RELEASE,
                                           __ATOMIC_RELAXED));

    return 0;
}

/* ========================================================================== */
/* Invalidation requests                                                       */
/* ========================================================================== */

int vtx_safepoint_request_invalidate(vtx_safepoint_manager_t *manager,
                                      uint32_t method_id)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");

    /* Allocate request */
    vtx_sp_invalidate_request_t *req = malloc(sizeof(vtx_sp_invalidate_request_t));
    if (!req) return -1;

    req->method_id = method_id;
    req->next = NULL;

    /* Add to the invalidation queue */
    pthread_mutex_lock(&manager->invalidate_mutex);
    if (manager->invalidate_tail) {
        manager->invalidate_tail->next = req;
    } else {
        manager->invalidate_head = req;
    }
    manager->invalidate_tail = req;
    pthread_mutex_unlock(&manager->invalidate_mutex);

    /* Set the global flag to trigger safe point processing */
    vtx_safepoint_state_t old_state =
        __atomic_load_n(&manager->state, __ATOMIC_RELAXED);
    vtx_safepoint_state_t new_state;

    do {
        new_state = (vtx_safepoint_state_t)(
            (unsigned)old_state | VTX_SP_INVALIDATE_PENDING);
    } while (!__atomic_compare_exchange_n(&manager->state, &old_state,
                                           new_state, false,
                                           __ATOMIC_RELEASE,
                                           __ATOMIC_RELAXED));

    return 0;
}
