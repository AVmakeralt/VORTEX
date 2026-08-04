#ifndef VORTEX_CODECACHE_CACHE_H
#define VORTEX_CODECACHE_CACHE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"

/**
 * VORTEX Segmented Code Cache
 *
 * The code cache is divided into segments of VTX_CACHE_SEGMENT_SIZE (1MB).
 * Each segment is an mmap'd region of executable memory. Segments are
 * allocated on demand and freed when empty.
 *
 * Allocation flow:
 *   1. Request N bytes from vtx_code_cache_alloc()
 *   2. If current segment has space, allocate from it
 *   3. If not, allocate a new segment
 *   4. After filling, mprotect to PROT_EXEC|PROT_READ
 *
 * Thread safety: The code cache is NOT thread-safe. The caller must
 * synchronize access (e.g., via the compilation thread pool's lock).
 */

/* ========================================================================== */
/* Code segment                                                                */
/* ========================================================================== */

typedef struct vtx_code_segment vtx_code_segment_t;

struct vtx_code_segment {
    uint8_t             *memory;       /* mmap'd executable memory */
    uint32_t             size;         /* total size (VTX_CACHE_SEGMENT_SIZE) */
    uint32_t             used;         /* bytes used so far */
    uint32_t             method_count; /* number of methods in this segment */
    bool                 writable;     /* true if PROT_WRITE is set */
    vtx_code_segment_t  *next;         /* linked list of segments */

    /* Free list: tracks freed regions within this segment so they can
     * be reused for new allocations. Without this, freed memory within
     * a non-empty segment is permanently wasted (leaked).
     * Each free entry records (offset, size) within the segment. */
    struct {
        uint32_t offset;
        uint32_t size;
    }                   *free_list;
    uint32_t             free_count;
    uint32_t             free_capacity;
};

/* ========================================================================== */
/* Code cache                                                                  */
/* ========================================================================== */

typedef struct vtx_code_cache {
    vtx_code_segment_t  *segments;         /* linked list of segments */
    vtx_code_segment_t  *current_segment;  /* segment we're currently allocating from */
    uint32_t             segment_count;    /* total number of segments */
    uint64_t             total_size;       /* total bytes used across all segments */
    uint64_t             max_size;         /* maximum cache size (VORTEX_CACHE_MAX_SIZE) */
} vtx_code_cache_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the code cache.
 * The max_size defaults to VORTEX_CACHE_MAX_SIZE.
 * Returns 0 on success, -1 on failure.
 */
int vtx_code_cache_init(vtx_code_cache_t *cache, uint64_t max_size);

/**
 * Destroy the code cache and unmap all segments.
 */
void vtx_code_cache_destroy(vtx_code_cache_t *cache);

/* ========================================================================== */
/* Allocation                                                                  */
/* ========================================================================== */

/**
 * Allocate `size` bytes of executable memory from the code cache.
 * The returned pointer is aligned to 16 bytes.
 *
 * After writing code to the allocated memory, call
 * vtx_code_cache_finalize() to make the memory executable.
 *
 * Returns a pointer to the allocated memory, or NULL on failure.
 */
void *vtx_code_cache_alloc(vtx_code_cache_t *cache, uint32_t size);

/**
 * Finalize the current segment: make it executable (PROT_EXEC|PROT_READ).
 * Must be called after writing code to the allocated memory.
 *
 * Returns 0 on success, -1 on failure.
 */
int vtx_code_cache_finalize(vtx_code_cache_t *cache);

/**
 * Make a specific range executable.
 * @param cache  Code cache
 * @param ptr    Pointer within the cache memory
 * @param size   Size of the range
 * @return       0 on success, -1 on failure
 */
int vtx_code_cache_make_exec(vtx_code_cache_t *cache, void *ptr, uint32_t size);

/**
 * Make a specific range writable (for patching).
 * @param cache  Code cache
 * @param ptr    Pointer within the cache memory
 * @param size   Size of the range
 * @return       0 on success, -1 on failure
 */
int vtx_code_cache_make_writable(vtx_code_cache_t *cache, void *ptr, uint32_t size);

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

/**
 * Get the total number of bytes used in the cache.
 */
uint64_t vtx_code_cache_total_used(const vtx_code_cache_t *cache);

/**
 * Get the total number of segments.
 */
uint32_t vtx_code_cache_segment_count(const vtx_code_cache_t *cache);

/**
 * Check if the cache has exceeded its maximum size.
 */
bool vtx_code_cache_is_full(const vtx_code_cache_t *cache);

/**
 * Free a method's code from its segment.
 * If the segment becomes empty, it is freed (unmapped).
 *
 * @param cache     Code cache
 * @param code_ptr  Pointer to the method's code in the cache
 * @param code_size Size of the method's code
 * @return          0 on success, -1 on failure
 */
int vtx_code_cache_free(vtx_code_cache_t *cache, void *code_ptr, uint32_t code_size);

#endif /* VORTEX_CODECACHE_CACHE_H */
