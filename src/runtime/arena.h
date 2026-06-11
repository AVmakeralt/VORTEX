#ifndef VORTEX_ARENA_H
#define VORTEX_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include "vortex_config.h"

/**
 * VORTEX Arena Allocator
 *
 * Region-based allocator used throughout the compiler pipeline.
 * Each compilation gets its own arena; when compilation finishes,
 * the entire arena is freed. No individual free — only wholesale reset.
 *
 * Pages are allocated via mmap and tracked in a singly-linked list.
 * Allocations are 16-byte aligned within each page.
 */

/* Forward declaration for page structure */
typedef struct vtx_arena_page vtx_arena_page_t;

/* A single page in the arena's page list */
struct vtx_arena_page {
    vtx_arena_page_t *next;   /* linked list of pages */
    size_t            size;   /* total usable size of this page (excluding header) */
    size_t            used;   /* bytes consumed so far in this page */
    /* The usable memory starts immediately after this header, aligned to 16 bytes */
};

/* Arena handle */
typedef struct {
    vtx_arena_page_t *first_page;   /* first (oldest) page — kept on reset */
    vtx_arena_page_t *current_page; /* page we are currently allocating from */
    size_t            total_allocated; /* total bytes allocated across all pages */
} vtx_arena_t;

/**
 * Initialize an arena with its initial 64KB page.
 * Returns 0 on success, -1 on failure (mmap error).
 */
int vtx_arena_init(vtx_arena_t *arena);

/**
 * Allocate `size` bytes from the arena with 16-byte alignment.
 * If the current page doesn't have enough space, a new 64KB page
 * (or larger if size requires it) is allocated via mmap.
 * Returns a valid pointer on success, NULL on failure.
 */
void *vtx_arena_alloc(vtx_arena_t *arena, size_t size);

/**
 * Reset the arena: free all pages except the first, and reset
 * the allocation position within the first page to the beginning.
 */
void vtx_arena_reset(vtx_arena_t *arena);

/**
 * Destroy the arena: munmap all pages and zero out the struct.
 */
void vtx_arena_destroy(vtx_arena_t *arena);

/**
 * Return total number of bytes currently allocated from this arena.
 */
size_t vtx_arena_total_allocated(const vtx_arena_t *arena);

#endif /* VORTEX_ARENA_H */
