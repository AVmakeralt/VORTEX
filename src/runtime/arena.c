#include "runtime/arena.h"

#include <sys/mman.h>
#include <string.h>
#include <stdint.h>

/* Alignment for all arena allocations */
#define VTX_ARENA_ALIGN 16

/* Compute the aligned size (round up to next multiple of VTX_ARENA_ALIGN) */
static inline size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

/*
 * The page header is vtx_arena_page_t, but we need to ensure the usable
 * memory after the header is 16-byte aligned. We compute the offset from
 * the start of the mmap'd region to the first usable byte.
 */
static inline size_t page_header_size(void)
{
    return align_up(sizeof(vtx_arena_page_t), VTX_ARENA_ALIGN);
}

/*
 * Allocate a new page of at least `min_usable` bytes of usable space.
 * If min_usable <= VTX_ARENA_PAGE_SIZE, we allocate VTX_ARENA_PAGE_SIZE
 * total (header + usable). Otherwise we allocate enough to satisfy the request.
 */
static vtx_arena_page_t *arena_new_page(size_t min_usable)
{
    size_t header_sz = page_header_size();
    size_t total = header_sz + min_usable;
    /* Round total up to a full page of VTX_ARENA_PAGE_SIZE for small requests */
    if (total < VTX_ARENA_PAGE_SIZE) {
        total = VTX_ARENA_PAGE_SIZE;
    } else {
        total = align_up(total, VTX_ARENA_PAGE_SIZE);
    }

    void *raw = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        return NULL;
    }

    vtx_arena_page_t *page = (vtx_arena_page_t *)raw;
    page->next = NULL;
    page->size = total - header_sz; /* usable size */
    page->used = 0;

    return page;
}

/* Get a pointer to the usable memory region of a page */
static inline void *page_data(vtx_arena_page_t *page)
{
    return (char *)page + page_header_size();
}

int vtx_arena_init(vtx_arena_t *arena)
{
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    arena->first_page = arena_new_page(VTX_ARENA_PAGE_SIZE);
    if (arena->first_page == NULL) {
        arena->current_page = NULL;
        arena->total_allocated = 0;
        return -1;
    }

    arena->current_page = arena->first_page;
    arena->total_allocated = 0;
    return 0;
}

void *vtx_arena_alloc(vtx_arena_t *arena, size_t size)
{
    VTX_ASSERT(arena != NULL, "arena must not be NULL");
    if (size == 0) {
        size = VTX_ARENA_ALIGN; /* minimum allocation */
    }

    /* Align the allocation size */
    size_t aligned_size = align_up(size, VTX_ARENA_ALIGN);

    vtx_arena_page_t *page = arena->current_page;

    /* Try to allocate from the current page */
    if (page != NULL && (page->used + aligned_size) <= page->size) {
        void *ptr = (char *)page_data(page) + page->used;
        page->used += aligned_size;
        arena->total_allocated += aligned_size;
        return ptr;
    }

    /* Current page is full (or doesn't exist). Allocate a new page.
     * If the request is larger than the default page size, allocate
     * a page big enough for it. */
    size_t min_usable = aligned_size;
    vtx_arena_page_t *new_page = arena_new_page(min_usable);
    if (new_page == NULL) {
        return NULL;
    }

    /* Link the new page into the list after the current page */
    if (page != NULL) {
        new_page->next = page->next;
        page->next = new_page;
    } else {
        /* No pages at all (shouldn't happen after init, but be safe) */
        arena->first_page = new_page;
    }
    arena->current_page = new_page;

    /* Allocate from the new page */
    void *ptr = (char *)page_data(new_page) + new_page->used;
    new_page->used += aligned_size;
    arena->total_allocated += aligned_size;

    VTX_ASSERT(((uintptr_t)ptr & (VTX_ARENA_ALIGN - 1)) == 0,
               "arena allocation must be 16-byte aligned");

    return ptr;
}

void vtx_arena_reset(vtx_arena_t *arena)
{
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    if (arena->first_page == NULL) {
        return;
    }

    /* Free all pages except the first */
    vtx_arena_page_t *page = arena->first_page->next;
    while (page != NULL) {
        vtx_arena_page_t *next = page->next;
        size_t header_sz = page_header_size();
        munmap(page, header_sz + page->size);
        page = next;
    }

    /* Reset the first page */
    arena->first_page->next = NULL;
    arena->first_page->used = 0;
    arena->current_page = arena->first_page;
    arena->total_allocated = 0;
}

void vtx_arena_destroy(vtx_arena_t *arena)
{
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    vtx_arena_page_t *page = arena->first_page;
    while (page != NULL) {
        vtx_arena_page_t *next = page->next;
        size_t header_sz = page_header_size();
        munmap(page, header_sz + page->size);
        page = next;
    }

    arena->first_page = NULL;
    arena->current_page = NULL;
    arena->total_allocated = 0;
}

size_t vtx_arena_total_allocated(const vtx_arena_t *arena)
{
    return arena->total_allocated;
}
