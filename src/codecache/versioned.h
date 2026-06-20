#ifndef VORTEX_CODECACHE_VERSIONED_H
#define VORTEX_CODECACHE_VERSIONED_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "codecache/cache.h"

/**
 * VORTEX Versioned Code Cache
 * ===========================
 *
 * Audit priority #6 (Hardening): "Code cache robustness — needs patching
 * of already-installed code, N+1 versioning (never uninstall old version
 * while it's on a thread's stack), fragmentation management (compaction)."
 *
 * This module wraps vtx_code_cache_t with version tracking and safe
 * reclamation. The key invariant:
 *
 *   When a method is recompiled (e.g. after a deopt), the OLD version
 *   is NOT immediately freed. Instead, it's marked "retired" and kept
 *   alive until no thread's stack references it. This prevents the
 *   catastrophic bug where thread A is executing old version's code
 *   while thread B frees that code.
 *
 * N+1 versioning: at any time, there may be N retired versions + 1
 * active version per method. Retired versions are reclaimed when their
 * "on-stack reference count" drops to zero.
 *
 * Patching: vtx_codecache_versioned_patch() atomically patches an
 * already-installed code range (e.g. to update an inline cache).
 * This is used for monomorphic IC → direct call promotion.
 *
 * Compaction: vtx_codecache_versioned_compact() walks the cache and
 * frees segments that are entirely retired-and-unreferenced. This
 * manages fragmentation over long runtimes.
 */

/* Maximum number of methods tracked. Methods are indexed by a uint32_t
 * method_id (typically the method registry index). */
#define VTX_VERSIONED_CACHE_MAX_METHODS 4096

/* Maximum retired versions kept alive per method. If exceeded, the oldest
 * retired version is force-freed (with a warning, since this could be
 * unsafe if a thread is still on its stack). */
#define VTX_VERSIONED_CACHE_MAX_RETIRED 8

typedef struct vtx_code_version vtx_code_version_t;

struct vtx_code_version {
    uint32_t              method_id;       /* which method this is a version of */
    uint32_t              version_number;  /* monotonically increasing per method */
    void                 *code_ptr;        /* pointer into the code cache */
    uint32_t              code_size;       /* size of the code in bytes */
    bool                  is_active;       /* true if this is the current installed version */
    bool                  is_retired;      /* true if retired (waiting for reclamation) */
    int32_t               on_stack_count;  /* number of threads currently executing this version */
    uint64_t              retire_time_ns;  /* when this version was retired */
    vtx_code_version_t   *next;            /* linked list of versions for the same method */
};

typedef struct {
    vtx_code_cache_t            *cache;             /* underlying code cache (owned by caller) */
    vtx_code_version_t          *versions[VTX_VERSIONED_CACHE_MAX_METHODS];  /* per-method version list */
    uint32_t                     next_version_number[VTX_VERSIONED_CACHE_MAX_METHODS];
    uint32_t                     total_active;
    uint32_t                     total_retired;
    uint64_t                     total_compactions;
    uint64_t                     total_force_frees;  /* retired versions force-freed due to overflow */
} vtx_versioned_cache_t;

/* Initialize the versioned cache wrapper. The underlying cache is owned
 * by the caller and must outlive this wrapper. */
int vtx_versioned_cache_init(vtx_versioned_cache_t *vc, vtx_code_cache_t *cache);

/* Destroy the versioned cache wrapper. Does NOT destroy the underlying
 * cache (caller owns it). Frees all version tracking metadata. */
void vtx_versioned_cache_destroy(vtx_versioned_cache_t *vc);

/* Install a new version of a method. The old active version (if any) is
 * retired but NOT freed — it stays alive until on_stack_count drops to 0.
 *
 * Returns the new version's number, or 0 on failure. */
uint32_t vtx_versioned_cache_install(vtx_versioned_cache_t *vc,
                                      uint32_t method_id,
                                      void *code_ptr, uint32_t code_size);

/* Get the active version of a method, or NULL if no version is installed. */
vtx_code_version_t *vtx_versioned_cache_get_active(
    const vtx_versioned_cache_t *vc, uint32_t method_id);

/* Called when a thread ENTERS a method's code (i.e. the method's frame
 * is pushed onto the thread's stack). Increments on_stack_count so the
 * version won't be freed while the thread is executing it. */
void vtx_versioned_cache_on_enter(vtx_versioned_cache_t *vc, uint32_t method_id);

/* Called when a thread EXITS a method's code (frame popped). Decrements
 * on_stack_count. If a retired version's count drops to 0, it's freed. */
void vtx_versioned_cache_on_exit(vtx_versioned_cache_t *vc, uint32_t method_id);

/* Atomically patch an already-installed code range. Used for inline cache
 * updates (monomorphic → direct call promotion).
 *
 * The patch is: write `new_bytes` (length `len`) at `patch_offset` within
 * the active version's code. The code is temporarily made writable,
 * patched, then made executable again.
 *
 * Returns 0 on success, -1 on failure. */
int vtx_versioned_cache_patch(vtx_versioned_cache_t *vc, uint32_t method_id,
                               uint32_t patch_offset, const uint8_t *new_bytes,
                               uint32_t len);

/* Reclaim all retired versions whose on_stack_count is 0. Called
 * periodically (e.g. at safepoints) to free memory. Returns the number
 * of versions reclaimed. */
uint32_t vtx_versioned_cache_reclaim(vtx_versioned_cache_t *vc);

/* Compact the cache: free code-cache segments that contain only
 * retired-and-reclaimed versions. Returns the number of bytes freed. */
uint64_t vtx_versioned_cache_compact(vtx_versioned_cache_t *vc);

/* Get statistics as a printable string. */
void vtx_versioned_cache_stats_str(const vtx_versioned_cache_t *vc,
                                    char *buf, size_t bufsize);

#endif /* VORTEX_CODECACHE_VERSIONED_H */
