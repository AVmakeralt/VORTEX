#ifndef VORTEX_COMPILE_VERSION_H
#define VORTEX_COMPILE_VERSION_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "vortex_config.h"
#include "codecache/install.h"
#include "runtime/arena.h"

/**
 * VORTEX Code Version Management
 *
 * Manages multiple compiled versions of a single method. This is essential
 * for concurrent compilation and tiered optimization:
 *
 *   - While T2 compiles in the background, T1 code continues to run
 *   - When T2 finishes, T1 is deprecated (not removed — running threads
 *     may still be executing it)
 *   - T1 is freed only after all threads have exited the old version
 *   - Deoptless continuations create alternative versions with failed
 *     speculations removed
 *   - Class loads may invalidate specific versions
 *
 * Version lifecycle:
 *
 *   Compiling → Active → Deprecated → Freed
 *                    ↓
 *               Invalidated
 *
 * Transitions:
 *   Compiling → Active:    compilation finished, code installed
 *   Active → Deprecated:   a newer version is installed
 *   Deprecated → Freed:    no threads are executing this version
 *   Active → Invalidated:  a dependency was broken (class load, etc.)
 *   Compiling → Invalidated: compilation was cancelled
 *
 * Thread safety:
 *   - Version state transitions are protected by a per-method mutex
 *   - Reference counting tracks how many threads are executing each version
 *   - A version is freed only when refcount drops to zero after deprecation
 */

/* ========================================================================== */
/* Code version state                                                          */
/* ========================================================================== */

typedef enum {
    VTX_VERSION_COMPILING  = 0,  /* compilation in progress */
    VTX_VERSION_ACTIVE     = 1,  /* installed and callable */
    VTX_VERSION_DEPRECATED = 2,  /* replaced by newer version, may still be running */
    VTX_VERSION_INVALIDATED = 3, /* no longer valid (dependency broken) */
    VTX_VERSION_FREED      = 4   /* memory freed */
} vtx_version_state_t;

/* Human-readable name for version state */
const char *vtx_version_state_name(vtx_version_state_t s);

/* ========================================================================== */
/* Code version                                                                */
/* ========================================================================== */

/**
 * A single compiled version of a method. Multiple versions can exist
 * simultaneously (e.g., T1 active while T2 compiles, or deoptless
 * continuation alongside the main version).
 */
typedef struct vtx_code_version vtx_code_version_t;

struct vtx_code_version {
    uint32_t             method_id;       /* method this is a version of */
    uint32_t             version_id;      /* unique version identifier */
    vtx_compile_tier_t   tier;            /* compilation tier */
    vtx_version_state_t  state;           /* current lifecycle state */

    /* The compiled code */
    vtx_compiled_method_t *compiled;      /* compiled method metadata (may be NULL during Compiling) */

    /* Reference counting: number of threads currently executing this version */
    volatile int32_t     refcount;        /* threads executing this version */

    /* Timestamps */
    uint64_t             compile_start_ns; /* when compilation started */
    uint64_t             compile_end_ns;   /* when compilation finished */
    uint64_t             activate_ns;      /* when version became Active */
    uint64_t             deprecate_ns;     /* when version was deprecated */

    /* Link to the next version (linked list of versions for a method) */
    vtx_code_version_t  *next_version;    /* newer version of the same method */
    vtx_code_version_t  *prev_version;    /* older version */

    /* Whether this version is a deoptless continuation */
    bool                 is_deoptless;
    uint32_t             deoptless_guard_id; /* the guard that was removed */
};

/* ========================================================================== */
/* Version manager                                                             */
/* ========================================================================== */

/**
 * Per-method version chain: linked list of versions from oldest to newest.
 */
typedef struct {
    uint32_t            method_id;       /* method ID */
    vtx_code_version_t *newest;          /* newest (most recently compiled) version */
    vtx_code_version_t *oldest;          /* oldest still-alive version */
    uint32_t            version_count;   /* number of versions in the chain */
    pthread_mutex_t     method_mutex;    /* protects the version chain */
} vtx_method_versions_t;

/**
 * Global version manager: tracks all method version chains.
 */
#define VTX_VERSION_INITIAL_CAPACITY 256

typedef struct {
    vtx_method_versions_t **methods;     /* array indexed by method_id */
    uint32_t                method_count; /* number of registered methods */
    uint32_t                capacity;     /* allocated capacity */

    /* Statistics */
    uint32_t                total_versions_created;
    uint32_t                total_versions_deprecated;
    uint32_t                total_versions_invalidated;
    uint32_t                total_versions_freed;

    /* Arena for version allocations */
    vtx_arena_t            *arena;
} vtx_version_manager_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the version manager.
 *
 * @param manager Version manager to initialize
 * @param arena   Arena for allocating version structures
 * @return        0 on success, -1 on failure
 */
int vtx_version_manager_init(vtx_version_manager_t *manager, vtx_arena_t *arena);

/**
 * Destroy the version manager and free all resources.
 */
void vtx_version_manager_destroy(vtx_version_manager_t *manager);

/* ========================================================================== */
/* Version lifecycle                                                           */
/* ========================================================================== */

/**
 * Create a new code version for a method (in Compiling state).
 *
 * @param manager   Version manager
 * @param method_id Method ID
 * @param tier      Compilation tier
 * @return          New code version, or NULL on failure
 */
vtx_code_version_t *vtx_version_create_compiling(vtx_version_manager_t *manager,
                                                   uint32_t method_id,
                                                   vtx_compile_tier_t tier);

/**
 * Install a compiled version: transition from Compiling → Active.
 *
 * @param manager   Version manager
 * @param method_id Method ID
 * @param version   The version to install
 * @param compiled  The compiled method metadata
 * @return          0 on success, -1 on failure
 */
int vtx_version_install(vtx_version_manager_t *manager,
                         uint32_t method_id,
                         vtx_code_version_t *version,
                         vtx_compiled_method_t *compiled);

/**
 * Deprecate a version: transition from Active → Deprecated.
 * This happens when a newer version is installed.
 *
 * The deprecated version is not freed immediately — it remains in
 * memory until all threads have exited it (refcount drops to zero).
 *
 * @param manager Version manager
 * @param version The version to deprecate
 * @return        0 on success, -1 on failure
 */
int vtx_version_deprecate(vtx_version_manager_t *manager,
                           vtx_code_version_t *version);

/**
 * Invalidate all versions of a method.
 * This happens when a class load breaks a dependency.
 *
 * All active and compiling versions are transitioned to Invalidated.
 * Deprecated versions are also invalidated if they haven't been freed yet.
 *
 * @param manager   Version manager
 * @param method_id Method ID to invalidate
 * @return          Number of versions invalidated
 */
uint32_t vtx_version_invalidate(vtx_version_manager_t *manager,
                                 uint32_t method_id);

/**
 * Free a deprecated version whose refcount has dropped to zero.
 * Transition: Deprecated → Freed.
 *
 * @param manager Version manager
 * @param version The version to free
 * @return        0 on success, -1 on failure
 */
int vtx_version_free(vtx_version_manager_t *manager,
                      vtx_code_version_t *version);

/* ========================================================================== */
/* Reference counting                                                          */
/* ========================================================================== */

/**
 * Increment the reference count of a version (a thread is entering it).
 */
void vtx_version_enter(vtx_code_version_t *version);

/**
 * Decrement the reference count of a version (a thread is leaving it).
 * If the count drops to zero and the version is deprecated, it is freed.
 *
 * @param manager Version manager
 * @param version The version being exited
 * @return        true if the version was freed, false otherwise
 */
bool vtx_version_exit(vtx_version_manager_t *manager,
                       vtx_code_version_t *version);

/* ========================================================================== */
/* Queries                                                                     */
/* ========================================================================== */

/**
 * Get the active version for a method.
 * Returns NULL if no active version exists.
 */
vtx_code_version_t *vtx_version_get_active(vtx_version_manager_t *manager,
                                             uint32_t method_id);

/**
 * Get the newest version for a method (regardless of state).
 * Returns NULL if no versions exist.
 */
vtx_code_version_t *vtx_version_get_newest(vtx_version_manager_t *manager,
                                              uint32_t method_id);

/**
 * Get the number of versions for a method.
 */
uint32_t vtx_version_count(vtx_version_manager_t *manager,
                             uint32_t method_id);

/**
 * Check if a specific version is still valid for execution.
 */
bool vtx_version_is_executable(const vtx_code_version_t *version);

#endif /* VORTEX_COMPILE_VERSION_H */
