/**
 * VORTEX Segmented Code Cache
 *
 * Manages executable memory for JIT-compiled code. Uses mmap'd segments
 * with explicit mprotect to control writability and executability.
 */

#include "codecache/cache.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ========================================================================== */
/* Segment management                                                          */
/* ========================================================================== */

/**
 * Allocate a new code segment via mmap.
 * Returns the new segment, or NULL on failure.
 */
static vtx_code_segment_t *segment_alloc(uint32_t size)
{
    /* Align size to page boundary */
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    uint32_t aligned_size = (size + (uint32_t)page_size - 1) & ~((uint32_t)page_size - 1);

    void *mem = mmap(NULL, aligned_size,
                     PROT_EXEC | PROT_WRITE | PROT_READ,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return NULL;

    vtx_code_segment_t *seg = (vtx_code_segment_t *)malloc(sizeof(vtx_code_segment_t));
    if (!seg) {
        munmap(mem, aligned_size);
        return NULL;
    }

    seg->memory = (uint8_t *)mem;
    seg->size = aligned_size;
    seg->used = 0;
    seg->method_count = 0;
    seg->writable = true;
    seg->next = NULL;
    return seg;
}

/**
 * Free a code segment (unmap its memory and free the descriptor).
 */
static void segment_free(vtx_code_segment_t *seg)
{
    if (!seg) return;
    if (seg->memory) {
        munmap(seg->memory, seg->size);
    }
    free(seg);
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_code_cache_init(vtx_code_cache_t *cache, uint64_t max_size)
{
    if (!cache) return -1;
    memset(cache, 0, sizeof(*cache));
    cache->max_size = max_size > 0 ? max_size : VORTEX_CACHE_MAX_SIZE;
    cache->segments = NULL;
    cache->current_segment = NULL;
    cache->segment_count = 0;
    cache->total_size = 0;
    return 0;
}

void vtx_code_cache_destroy(vtx_code_cache_t *cache)
{
    if (!cache) return;

    /* Free all segments */
    vtx_code_segment_t *seg = cache->segments;
    while (seg) {
        vtx_code_segment_t *next = seg->next;
        segment_free(seg);
        seg = next;
    }

    cache->segments = NULL;
    cache->current_segment = NULL;
    cache->segment_count = 0;
    cache->total_size = 0;
}

/* ========================================================================== */
/* Allocation                                                                  */
/* ========================================================================== */

void *vtx_code_cache_alloc(vtx_code_cache_t *cache, uint32_t size)
{
    if (!cache || size == 0) return NULL;

    /* Align size to 16 bytes */
    size = (size + 15u) & ~15u;

    /* Check if we'd exceed the maximum cache size */
    if (cache->total_size + size > cache->max_size) {
        return NULL; /* Cache full — caller should evict */
    }

    /* Try to allocate from the current segment */
    vtx_code_segment_t *seg = cache->current_segment;
    if (seg && seg->writable && (seg->used + size <= seg->size)) {
        void *ptr = seg->memory + seg->used;
        seg->used += size;
        seg->method_count++;
        cache->total_size += size;
        return ptr;
    }

    /* Need a new segment */
    uint32_t seg_size = VTX_CACHE_SEGMENT_SIZE;
    if (size > seg_size) {
        seg_size = size + 4096; /* Larger segment for large methods */
    }

    /* Finalize the old segment if it exists */
    if (seg && seg->writable) {
        vtx_code_cache_finalize(cache);
    }

    vtx_code_segment_t *new_seg = segment_alloc(seg_size);
    if (!new_seg) return NULL;

    /* Add to the segment list */
    new_seg->next = cache->segments;
    cache->segments = new_seg;
    cache->current_segment = new_seg;
    cache->segment_count++;

    /* Allocate from the new segment */
    void *ptr = new_seg->memory + new_seg->used;
    new_seg->used += size;
    new_seg->method_count++;
    cache->total_size += size;
    return ptr;
}

int vtx_code_cache_finalize(vtx_code_cache_t *cache)
{
    if (!cache || !cache->current_segment) return -1;

    vtx_code_segment_t *seg = cache->current_segment;
    if (!seg->writable) return 0; /* Already executable */

    if (mprotect(seg->memory, seg->size, PROT_EXEC | PROT_READ) != 0) {
        return -1;
    }
    seg->writable = false;
    return 0;
}

int vtx_code_cache_make_exec(vtx_code_cache_t *cache, void *ptr, uint32_t size)
{
    if (!cache || !ptr || size == 0) return -1;

    /* Align to page boundary */
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    uintptr_t start = (uintptr_t)ptr & ~((uintptr_t)page_size - 1);
    uintptr_t end = ((uintptr_t)ptr + size + (uintptr_t)page_size - 1) &
                    ~((uintptr_t)page_size - 1);

    if (mprotect((void *)start, end - start, PROT_EXEC | PROT_READ) != 0) {
        return -1;
    }
    return 0;
}

int vtx_code_cache_make_writable(vtx_code_cache_t *cache, void *ptr, uint32_t size)
{
    if (!cache || !ptr || size == 0) return -1;

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    uintptr_t start = (uintptr_t)ptr & ~((uintptr_t)page_size - 1);
    uintptr_t end = ((uintptr_t)ptr + size + (uintptr_t)page_size - 1) &
                    ~((uintptr_t)page_size - 1);

    if (mprotect((void *)start, end - start, PROT_EXEC | PROT_WRITE | PROT_READ) != 0) {
        return -1;
    }
    return 0;
}

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

uint64_t vtx_code_cache_total_used(const vtx_code_cache_t *cache)
{
    return cache ? cache->total_size : 0;
}

uint32_t vtx_code_cache_segment_count(const vtx_code_cache_t *cache)
{
    return cache ? cache->segment_count : 0;
}

bool vtx_code_cache_is_full(const vtx_code_cache_t *cache)
{
    if (!cache) return true;
    return cache->total_size >= cache->max_size;
}

/* ========================================================================== */
/* Free method code                                                            */
/* ========================================================================== */

int vtx_code_cache_free(vtx_code_cache_t *cache, void *code_ptr, uint32_t code_size)
{
    if (!cache || !code_ptr) return -1;

    /* Find the segment containing this pointer */
    vtx_code_segment_t **prev_ptr = &cache->segments;
    vtx_code_segment_t *seg = cache->segments;

    while (seg) {
        if ((uint8_t *)code_ptr >= seg->memory &&
            (uint8_t *)code_ptr < seg->memory + seg->size) {
            /* Found the segment */
            seg->method_count--;

            if (seg->method_count == 0) {
                /* Segment is empty — free it */
                *prev_ptr = seg->next;
                if (cache->current_segment == seg) {
                    cache->current_segment = NULL;
                }
                cache->total_size -= seg->used;
                cache->segment_count--;
                segment_free(seg);
            } else {
                /* Mark the space as freed (but don't compact) */
                cache->total_size -= code_size;
            }
            return 0;
        }
        prev_ptr = &seg->next;
        seg = seg->next;
    }

    return -1; /* Pointer not found in any segment */
}
