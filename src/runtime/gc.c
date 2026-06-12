#include "runtime/gc.h"

#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>

/* ========================================================================== */
/* Internal helpers                                                            */
/* ========================================================================== */

/**
 * Check if a pointer falls within a memory range [start, start+size).
 */
static inline bool ptr_in_range(const void *ptr, const uint8_t *start, size_t size)
{
    return (const uint8_t *)ptr >= start && (const uint8_t *)ptr < (start + size);
}

/**
 * Align a pointer up to 8-byte alignment.
 */
static inline size_t align_up_8(size_t value)
{
    return (value + 7) & ~(size_t)7;
}

/**
 * Initialize a semi-space by allocating VTX_GC_YOUNG_SIZE bytes via mmap.
 * Returns 0 on success, -1 on failure.
 */
static int semi_space_init(vtx_semi_space_t *space, size_t size)
{
    space->start = (uint8_t *)mmap(NULL, size, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (space->start == MAP_FAILED) {
        space->start = NULL;
        space->current = NULL;
        space->size = 0;
        return -1;
    }
    space->current = space->start;
    space->size = size;
    return 0;
}

/**
 * Destroy a semi-space by munmap-ing its memory.
 */
static void semi_space_destroy(vtx_semi_space_t *space)
{
    if (space->start != NULL) {
        munmap(space->start, space->size);
        space->start = NULL;
        space->current = NULL;
        space->size = 0;
    }
}

/**
 * Reset a semi-space for reuse (e.g., to-space after a collection).
 */
static void semi_space_reset(vtx_semi_space_t *space)
{
    space->current = space->start;
}

/**
 * Allocate from a semi-space using bump-pointer allocation.
 * Returns NULL if there's not enough space.
 */
static void *semi_space_alloc(vtx_semi_space_t *space, size_t size)
{
    size = align_up_8(size);
    if (space->current + size > space->start + space->size) {
        return NULL; /* out of space */
    }
    void *ptr = space->current;
    space->current += size;
    return ptr;
}

/* ========================================================================== */
/* Old generation helpers                                                      */
/* ========================================================================== */

/**
 * Initialize the old generation.
 */
static int old_gen_init(vtx_old_gen_t *old, size_t size)
{
    old->start = (uint8_t *)mmap(NULL, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (old->start == MAP_FAILED) {
        old->start = NULL;
        old->size = 0;
        old->used = 0;
        old->free_list = NULL;
        return -1;
    }
    old->size = size;
    old->used = 0;

    /* The entire old gen starts as one big free block */
    old->free_list = (vtx_free_node_t *)old->start;
    old->free_list->size = size;
    old->free_list->next = NULL;
    old->free_list->gc_mark = 0xFF; /* RT-4: Mark as free block */

    return 0;
}

/**
 * Destroy the old generation.
 */
static void old_gen_destroy(vtx_old_gen_t *old)
{
    if (old->start != NULL) {
        munmap(old->start, old->size);
        old->start = NULL;
        old->size = 0;
        old->used = 0;
        old->free_list = NULL;
    }
}

/**
 * Allocate from the old generation using first-fit free list.
 * Returns NULL if no suitable block is found.
 */
static void *old_gen_alloc(vtx_old_gen_t *old, size_t size)
{
    size = align_up_8(size);

    /* Minimum allocation size must fit the free list header */
    if (size < sizeof(vtx_free_node_t)) {
        size = sizeof(vtx_free_node_t);
    }

    vtx_free_node_t *prev = NULL;
    vtx_free_node_t *curr = old->free_list;

    while (curr != NULL) {
        if (curr->size >= size) {
            /* Found a suitable block */
            if (curr->size >= size + sizeof(vtx_free_node_t)) {
                /* Split the block */
                vtx_free_node_t *remainder = (vtx_free_node_t *)((uint8_t *)curr + size);
                remainder->size = curr->size - size;
                remainder->next = curr->next;

                if (prev != NULL) {
                    prev->next = remainder;
                } else {
                    old->free_list = remainder;
                }
            } else {
                /* Use the entire block (may be slightly larger than requested) */
                size = curr->size;
                if (prev != NULL) {
                    prev->next = curr->next;
                } else {
                    old->free_list = curr->next;
                }
            }

            old->used += size;
            return (void *)curr;
        }
        prev = curr;
        curr = curr->next;
    }

    /* No suitable block found */
    return NULL;
}

/**
 * Add a block back to the old generation free list.
 * Simplest approach: prepend to free list. In a production system,
 * we would coalesce adjacent blocks.
 */
static void old_gen_free(vtx_old_gen_t *old, void *ptr, size_t size)
{
    size = align_up_8(size);
    if (size < sizeof(vtx_free_node_t)) {
        size = sizeof(vtx_free_node_t);
    }

    vtx_free_node_t *node = (vtx_free_node_t *)ptr;
    node->size = size;
    node->next = old->free_list;
    node->gc_mark = 0xFF; /* RT-4: Mark as free block so sweep can identify it */
    old->free_list = node;
    old->used -= size;

    /* Coalesce with adjacent free blocks */
    /* Look for the adjacent block (next in memory) in the free list */
    vtx_free_node_t *prev_adj = NULL;
    vtx_free_node_t *curr_adj = old->free_list;
    while (curr_adj != NULL) {
        if ((uint8_t *)curr_adj == (uint8_t *)node + node->size) {
            /* Coalesce: merge curr_adj into node */
            node->size += curr_adj->size;
            /* Remove curr_adj from free list */
            vtx_free_node_t *p = NULL;
            vtx_free_node_t *c = old->free_list;
            while (c != NULL) {
                if (c == curr_adj) {
                    if (p != NULL) {
                        p->next = c->next;
                    } else {
                        old->free_list = c->next;
                    }
                    break;
                }
                p = c;
                c = c->next;
            }
            break;
        }
        prev_adj = curr_adj;
        curr_adj = curr_adj->next;
    }
    (void)prev_adj; /* suppress unused warning */
}

/* ========================================================================== */
/* GC init/destroy                                                             */
/* ========================================================================== */

int vtx_gc_init(vtx_gc_t *gc, vtx_type_system_t *ts, vtx_gc_mode_t mode)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    VTX_ASSERT(ts != NULL, "type system must not be NULL");

    memset(gc, 0, sizeof(vtx_gc_t));

    /* Initialize young generation semi-spaces */
    if (semi_space_init(&gc->young_from, VTX_GC_YOUNG_SIZE) != 0) {
        return -1;
    }
    if (semi_space_init(&gc->young_to, VTX_GC_YOUNG_SIZE) != 0) {
        semi_space_destroy(&gc->young_from);
        return -1;
    }

    /* Initialize old generation */
    if (old_gen_init(&gc->old_gen, VTX_GC_OLD_SIZE) != 0) {
        semi_space_destroy(&gc->young_from);
        semi_space_destroy(&gc->young_to);
        return -1;
    }

    /* Initialize root stack */
    gc->root_capacity = VTX_GC_ROOT_STACK_SIZE;
    gc->root_stack = (vtx_root_entry_t *)malloc(
        gc->root_capacity * sizeof(vtx_root_entry_t));
    if (gc->root_stack == NULL) {
        semi_space_destroy(&gc->young_from);
        semi_space_destroy(&gc->young_to);
        old_gen_destroy(&gc->old_gen);
        return -1;
    }
    gc->root_count = 0;

    /* Initialize remembered set */
    gc->remembered_capacity = VTX_GC_REMEMBERED_SET_INIT;
    gc->remembered_set = (vtx_remembered_entry_t *)malloc(
        gc->remembered_capacity * sizeof(vtx_remembered_entry_t));
    if (gc->remembered_set == NULL) {
        semi_space_destroy(&gc->young_from);
        semi_space_destroy(&gc->young_to);
        old_gen_destroy(&gc->old_gen);
        free(gc->root_stack);
        return -1;
    }
    gc->remembered_count = 0;

    /* Initialize pinned objects list */
    gc->pinned_capacity = 64;
    gc->pinned_objects = (vtx_heap_object_t **)malloc(
        gc->pinned_capacity * sizeof(vtx_heap_object_t *));
    if (gc->pinned_objects == NULL) {
        semi_space_destroy(&gc->young_from);
        semi_space_destroy(&gc->young_to);
        old_gen_destroy(&gc->old_gen);
        free(gc->root_stack);
        free(gc->remembered_set);
        return -1;
    }
    gc->pinned_count = 0;

    gc->type_system = ts;
    gc->collection_requested = false;
    gc->collections_done = 0;
    gc->mode = mode;

    /* Initialize card table covering the entire heap.
     * The card table covers from the minimum heap address (old gen start)
     * through the end of old gen. Young-gen objects don't need card marking
     * (they're collected every GC cycle), but old-gen objects do.
     * We compute heap_base as the minimum of old gen and young gen starts,
     * and heap_size to cover all heap regions. */
    {
        /* Find the minimum heap address to use as heap_base */
        uint8_t *min_addr = gc->old_gen.start;
        if (gc->young_from.start < min_addr) min_addr = gc->young_from.start;
        if (gc->young_to.start < min_addr) min_addr = gc->young_to.start;

        /* Find the maximum end address */
        uint8_t *max_end = gc->old_gen.start + gc->old_gen.size;
        if (gc->young_from.start + gc->young_from.size > max_end)
            max_end = gc->young_from.start + gc->young_from.size;
        if (gc->young_to.start + gc->young_to.size > max_end)
            max_end = gc->young_to.start + gc->young_to.size;

        gc->heap_base = min_addr;
        gc->heap_size = (size_t)(max_end - min_addr);
        gc->card_table_size = (gc->heap_size + VTX_CARD_SIZE - 1) / VTX_CARD_SIZE;
        gc->card_table = (uint8_t *)calloc(gc->card_table_size, 1);
        if (gc->card_table == NULL) {
            /* Card table allocation failed — not fatal, we fall back
             * to remembered-set-only barrier */
            gc->card_table_size = 0;
            gc->heap_base = NULL;
            gc->heap_size = 0;
        }
    }

    vtx_gc_set_mode(gc, mode);

    return 0;
}

void vtx_gc_set_mode(vtx_gc_t *gc, vtx_gc_mode_t mode)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    gc->mode = mode;

    switch (mode) {
    case VTX_GC_GENERATIONAL:
        gc->fn_write_barrier = gc->card_table != NULL
            ? vtx_gc_write_barrier_card
            : vtx_gc_write_barrier;
        gc->fn_safepoint = vtx_gc_safepoint;
        gc->fn_root_push = vtx_gc_root_push;
        gc->fn_root_pop = vtx_gc_root_pop;
        break;
    case VTX_GC_NONE:
        gc->fn_write_barrier = NULL;
        gc->fn_safepoint = NULL;
        gc->fn_root_push = NULL;
        gc->fn_root_pop = NULL;
        break;
    case VTX_GC_MANUAL:
        gc->fn_write_barrier = NULL;
        gc->fn_safepoint = NULL;
        gc->fn_root_push = vtx_gc_root_push;
        gc->fn_root_pop = vtx_gc_root_pop;
        gc->manual_free_list = NULL;
        break;
    case VTX_GC_ARENA:
        gc->fn_write_barrier = NULL;
        gc->fn_safepoint = NULL;
        gc->fn_root_push = vtx_gc_root_push;
        gc->fn_root_pop = vtx_gc_root_pop;
        gc->arena_save_point = gc->young_from.current;
        break;
    }
}

vtx_gc_mode_t vtx_gc_get_mode(const vtx_gc_t *gc)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    return gc->mode;
}

void vtx_gc_manual_free(vtx_gc_t *gc, void *ptr, size_t size)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    VTX_ASSERT(gc->mode == VTX_GC_MANUAL, "manual_free only valid in manual mode");
    if (gc->mode != VTX_GC_MANUAL) return;

    size = align_up_8(size);
    if (size < sizeof(vtx_free_node_t)) {
        size = sizeof(vtx_free_node_t);
    }
    vtx_free_node_t *node = (vtx_free_node_t *)ptr;
    node->size = size;
    node->next = gc->manual_free_list;
    node->gc_mark = 0xFF; /* RT-4: Mark as free block */
    gc->manual_free_list = node;
}

void vtx_gc_arena_enter(vtx_gc_t *gc)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    VTX_ASSERT(gc->mode == VTX_GC_ARENA, "arena_enter only valid in arena mode");
    gc->arena_save_point = gc->young_from.current;
}

void vtx_gc_arena_leave(vtx_gc_t *gc)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    VTX_ASSERT(gc->mode == VTX_GC_ARENA, "arena_leave only valid in arena mode");
    gc->young_from.current = gc->arena_save_point;
}

void vtx_gc_destroy(vtx_gc_t *gc)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");

    semi_space_destroy(&gc->young_from);
    semi_space_destroy(&gc->young_to);
    old_gen_destroy(&gc->old_gen);

    free(gc->root_stack);
    gc->root_stack = NULL;

    free(gc->remembered_set);
    gc->remembered_set = NULL;

    free(gc->pinned_objects);
    gc->pinned_objects = NULL;

    free(gc->card_table);
    gc->card_table = NULL;
    gc->card_table_size = 0;
    gc->heap_base = NULL;
    gc->heap_size = 0;

    gc->root_count = 0;
    gc->remembered_count = 0;
    gc->pinned_count = 0;
}

/* ========================================================================== */
/* Allocation                                                                  */
/* ========================================================================== */

vtx_heap_object_t *vtx_gc_alloc(vtx_gc_t *gc, size_t size, vtx_typeid_t type_id)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    VTX_ASSERT(size >= VTX_HEAP_OBJECT_HEADER_SIZE, "allocation too small for header");

    size = align_up_8(size);

    /* Try young gen first */
    void *ptr = semi_space_alloc(&gc->young_from, size);
    if (ptr == NULL) {
        /* Young gen is full — request a collection */
        gc->collection_requested = true;
        vtx_gc_safepoint(gc);

        /* Try again after collection */
        ptr = semi_space_alloc(&gc->young_from, size);
        if (ptr == NULL) {
            /* Still no space — allocation failure */
            return NULL;
        }
    }

    vtx_heap_object_t *obj = (vtx_heap_object_t *)ptr;

    /* Initialize the header */
    const vtx_type_desc_t *td = vtx_type_get(gc->type_system, type_id);
    uint32_t shape_id = td != NULL ? td->shape_id : VTX_SHAPE_INVALID;
    uint32_t field_count = td != NULL ?
        (uint32_t)((size - VTX_HEAP_OBJECT_HEADER_SIZE) / sizeof(vtx_value_t)) : 0;

    vtx_heap_object_init(obj, type_id, shape_id, field_count, (uint32_t)size);

    return obj;
}

void vtx_gc_safepoint(vtx_gc_t *gc)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");

    if (gc->collection_requested) {
        gc->collection_requested = false;
        vtx_gc_collect_young(gc);
    }
}

/* ========================================================================== */
/* Root management                                                             */
/* ========================================================================== */

void vtx_gc_root_push(vtx_gc_t *gc, vtx_value_t value)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");

    if (gc->root_count >= gc->root_capacity) {
        /* Grow the root stack */
        uint32_t new_cap = gc->root_capacity * 2;
        vtx_root_entry_t *new_stack = (vtx_root_entry_t *)realloc(
            gc->root_stack, new_cap * sizeof(vtx_root_entry_t));
        if (new_stack == NULL) {
            VTX_ASSERT(false, "root stack overflow — out of memory");
            return;
        }
        gc->root_stack = new_stack;
        gc->root_capacity = new_cap;
    }

    gc->root_stack[gc->root_count].value = value;
    gc->root_count++;
}

vtx_value_t vtx_gc_root_pop(vtx_gc_t *gc)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    VTX_ASSERT(gc->root_count > 0, "root stack underflow");

    gc->root_count--;
    return gc->root_stack[gc->root_count].value;
}

/* ========================================================================== */
/* Write barrier                                                               */
/* ========================================================================== */

void vtx_gc_write_barrier(vtx_gc_t *gc, vtx_heap_object_t *obj,
                          uint32_t field_offset, vtx_value_t value)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    VTX_ASSERT(obj != NULL, "object must not be NULL");
    (void)field_offset; /* parameter kept for API compatibility; may be used for card marking */

    /* Only care about old→young references */
    if (!vtx_gc_in_old(gc, obj)) {
        return; /* obj is in young gen, no barrier needed */
    }

    if (!vtx_is_heap_ptr(value)) {
        return; /* value is not a heap pointer, no barrier needed */
    }

    void *val_ptr = vtx_heap_ptr(value);
    if (!vtx_gc_in_young(gc, val_ptr)) {
        return; /* value is not in young gen, no barrier needed */
    }

    /* Add obj to remembered set */
    if (obj->gc_remembered) {
        return; /* already in remembered set */
    }

    /* Grow remembered set if needed */
    if (gc->remembered_count >= gc->remembered_capacity) {
        uint32_t new_cap = gc->remembered_capacity * 2;
        vtx_remembered_entry_t *new_set = (vtx_remembered_entry_t *)realloc(
            gc->remembered_set, new_cap * sizeof(vtx_remembered_entry_t));
        if (new_set == NULL) {
            VTX_ASSERT(false, "remembered set overflow — out of memory");
            return;
        }
        gc->remembered_set = new_set;
        gc->remembered_capacity = new_cap;
    }

    gc->remembered_set[gc->remembered_count].obj = obj;
    gc->remembered_count++;
    obj->gc_remembered = 1;
}

void vtx_gc_write_barrier_card(vtx_gc_t *gc, vtx_heap_object_t *obj,
                               uint32_t field_offset, vtx_value_t value)
{
    /* Card marking write barrier: ~3 instructions for the common case.
     *
     * Algorithm:
     * 1. If value is not a young-gen pointer → return (no barrier needed)
     * 2. Compute card index from object address: (obj - heap_base) >> CARD_SHIFT
     * 3. Mark the card as dirty: card_table[index] = CARD_DIRTY
     *
     * This replaces the old 10-branch barrier with:
     *   - 1 branch (is value young-gen?)
     *   - 1 subtraction + shift (compute card index)
     *   - 1 byte store (mark card dirty)
     *
     * Total: ~3 instructions on the fast path (value is young-gen pointer)
     *        ~1 instruction on the super-fast path (value is not young-gen)
     */
    (void)field_offset;

    /* Fast check: is the value a heap pointer at all? */
    if (!vtx_is_heap_ptr(value)) return;

    void *value_ptr = vtx_heap_ptr(value);

    /* Check if value is in young generation */
    if (!vtx_gc_in_young(gc, value_ptr)) return;

    /* Object must be in old gen for the barrier to matter */
    if (!vtx_gc_in_old(gc, obj)) return;

    /* Mark the card as dirty */
    if (gc->card_table != NULL && gc->heap_base != NULL) {
        size_t card_index = ((size_t)((uint8_t *)obj - gc->heap_base)) >> VTX_CARD_SHIFT;
        if (card_index < gc->card_table_size) {
            gc->card_table[card_index] = VTX_CARD_DIRTY;
        }
    }

    /* Also maintain the remembered set as a fallback for correctness.
     * This ensures the old remembered-set scanning path still works
     * if needed, and provides a complete list of old→young references
     * for the remembered-set rebuild after collection. */
    if (obj->gc_remembered) {
        return; /* already in remembered set */
    }

    if (gc->remembered_count >= gc->remembered_capacity) {
        uint32_t new_cap = gc->remembered_capacity * 2;
        vtx_remembered_entry_t *new_set = (vtx_remembered_entry_t *)realloc(
            gc->remembered_set, new_cap * sizeof(vtx_remembered_entry_t));
        if (new_set == NULL) {
            VTX_ASSERT(false, "remembered set overflow — out of memory");
            return;
        }
        gc->remembered_set = new_set;
        gc->remembered_capacity = new_cap;
    }

    gc->remembered_set[gc->remembered_count].obj = obj;
    gc->remembered_count++;
    obj->gc_remembered = 1;
}

/* ========================================================================== */
/* Young-gen collection (copying collector)                                    */
/* ========================================================================== */

/**
 * Forward (copy) an object from from-space to to-space.
 * Returns the new location of the object in to-space.
 * If the object has already been forwarded, returns the forwarding address.
 */
/* Sentinel value for size field indicating the object has been forwarded.
 * No valid object can have this size because it's astronomically large.
 * The forwarding address is stored in the first sizeof(uintptr_t) bytes
 * of the object (overwriting type_id + gc_* fields), which is safe because
 * the object's data has already been copied to the new location. */
#define VTX_GC_FORWARDING_SENTINEL ((uint32_t)0xDEADF00D)

static vtx_heap_object_t *forward_object(vtx_gc_t *gc, vtx_heap_object_t *obj)
{
    VTX_ASSERT(obj != NULL, "object must not be NULL");

    /* RT-1 fix: Check if this object has already been forwarded.
     * OLD BUG: Used `first_word & 1` as the forwarding pointer tag, but on
     * little-endian x86-64, any object with an odd type_id (e.g.
     * VTX_TYPE_OBJECT=1) sets bit 0 of the first uintptr_t, causing the GC
     * to misidentify LIVE objects as forwarding pointers → wild pointers,
     * memory corruption, segfaults.
     *
     * NEW approach: Use a sentinel value in the size field
     * (VTX_GC_FORWARDING_SENTINEL = 0xDEADF00D). No valid object can have
     * this size. The forwarding address is stored in the first 8 bytes of
     * the object (overwriting type_id + gc_* byte fields). This is safe
     * because the object has already been copied to its new location. */
    if (obj->size == VTX_GC_FORWARDING_SENTINEL) {
        /* This is a forwarding pointer — address is in first 8 bytes */
        uintptr_t fwd_addr = *(uintptr_t *)obj;
        return (vtx_heap_object_t *)fwd_addr;
    }

    /* Check if the object is pinned — don't move it */
    if (obj->gc_pinned) {
        return obj;
    }

    /* Check if the object should be promoted to old gen */
    if (obj->gc_age >= VTX_GC_PROMOTION_AGE) {
        /* Promote to old gen */
        void *new_ptr = old_gen_alloc(&gc->old_gen, obj->size);
        if (new_ptr != NULL) {
            memcpy(new_ptr, obj, obj->size);
            vtx_heap_object_t *new_obj = (vtx_heap_object_t *)new_ptr;

            /* Leave a forwarding pointer in the old location.
             * RT-1 fix: Store the new address in the first 8 bytes
             * and set size to the sentinel value. */
            *(uintptr_t *)obj = (uintptr_t)new_obj;
            obj->size = VTX_GC_FORWARDING_SENTINEL;

            return new_obj;
        }
        /* If old gen is full, fall through to copy to to-space */
    }

    /* Copy to to-space */
    void *new_ptr = semi_space_alloc(&gc->young_to, obj->size);
    if (new_ptr == NULL) {
        /* to-space is full — this is a fatal error in a simple implementation */
        VTX_ASSERT(false, "young gen to-space overflow during collection");
        return obj;
    }

    memcpy(new_ptr, obj, obj->size);
    vtx_heap_object_t *new_obj = (vtx_heap_object_t *)new_ptr;
    new_obj->gc_age = obj->gc_age + 1;

    /* Leave a forwarding pointer in the old location.
     * RT-1 fix: Store the new address in the first 8 bytes
     * and set size to the sentinel value. */
    *(uintptr_t *)obj = (uintptr_t)new_obj;
    obj->size = VTX_GC_FORWARDING_SENTINEL;

    return new_obj;
}

/**
 * Trace a value: if it's a heap pointer in young gen, forward it
 * and return the updated value.
 */
static vtx_value_t trace_value(vtx_gc_t *gc, vtx_value_t value)
{
    if (!vtx_is_heap_ptr(value)) {
        return value;
    }

    vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(value);

    /* Only trace objects in the from-space */
    if (!vtx_gc_in_young(gc, obj)) {
        return value;
    }

    vtx_heap_object_t *new_obj = forward_object(gc, obj);
    return vtx_make_heap_ptr(new_obj);
}

/**
 * Scan an object's fields and trace any heap pointers.
 */
static void scan_object(vtx_gc_t *gc, vtx_heap_object_t *obj)
{
    VTX_ASSERT(obj != NULL, "object must not be NULL");

    for (uint32_t i = 0; i < obj->field_count; i++) {
        vtx_value_t field = obj->fields[i];
        if (vtx_is_heap_ptr(field)) {
            vtx_value_t new_field = trace_value(gc, field);
            obj->fields[i] = new_field;

            /* If the field was updated and this is an old-gen object,
             * we need the write barrier */
            if (vtx_gc_in_old(gc, obj) && vtx_is_heap_ptr(new_field)) {
                void *new_ptr = vtx_heap_ptr(new_field);
                if (vtx_gc_in_young(gc, new_ptr)) {
                    vtx_gc_write_barrier(gc, obj, i, new_field);
                }
            }
        }
    }
}

/**
 * Process the to-space scan pointer: scan all copied objects.
 */
static void scan_to_space(vtx_gc_t *gc)
{
    /* We scan from the beginning of to-space up to the current pointer.
     * New objects may be added during scanning, so we keep scanning
     * until we catch up to the allocation pointer. */
    uint8_t *scan_ptr = gc->young_to.start;

    while (scan_ptr < gc->young_to.current) {
        vtx_heap_object_t *obj = (vtx_heap_object_t *)scan_ptr;
        if (obj->size == 0) {
            break; /* shouldn't happen, but safety check */
        }
        scan_object(gc, obj);
        scan_ptr += align_up_8(obj->size);
    }
}

/**
 * Scan dirty cards in the card table for old→young references.
 * For each dirty card, scan all objects in that card's region.
 * This replaces scanning the entire remembered set with a targeted
 * scan of only the cards that have been marked dirty by write barriers.
 *
 * After scanning, all dirty cards are cleared.
 */
static void scan_dirty_cards(vtx_gc_t *gc)
{
    if (gc->card_table == NULL || gc->heap_base == NULL) {
        return; /* no card table available */
    }

    /* Iterate over all cards in the card table */
    for (size_t i = 0; i < gc->card_table_size; i++) {
        if (gc->card_table[i] != VTX_CARD_DIRTY) {
            continue;
        }

        /* This card is dirty — compute the address range it covers */
        uint8_t *card_start = gc->heap_base + (i << VTX_CARD_SHIFT);
        uint8_t *card_end   = card_start + VTX_CARD_SIZE;

        /* Only scan within old gen (young-gen objects are traced directly) */
        uint8_t *old_end = gc->old_gen.start + gc->old_gen.used;
        if (card_start < gc->old_gen.start) {
            card_start = gc->old_gen.start;
        }
        if (card_end > old_end) {
            card_end = old_end;
        }

        /* Scan objects within this card's address range */
        uint8_t *ptr = card_start;
        while (ptr < card_end) {
            /* Check if this is a free block */
            vtx_heap_object_t *obj = (vtx_heap_object_t *)ptr;
            if (obj->gc_mark == 0xFF) {
                /* Free block — skip it */
                vtx_free_node_t *free_node = (vtx_free_node_t *)ptr;
                size_t free_size = align_up_8(free_node->size);
                if (free_size < sizeof(vtx_free_node_t)) {
                    free_size = sizeof(vtx_free_node_t);
                }
                ptr += free_size;
                continue;
            }

            if (obj->size == 0 || obj->size >= VTX_GC_FORWARDING_SENTINEL) {
                /* Uninitialized/corrupted block — skip minimum unit */
                ptr += sizeof(vtx_free_node_t);
                continue;
            }

            /* Scan this object's fields for young-gen pointers */
            scan_object(gc, obj);

            ptr += align_up_8(obj->size);
        }

        /* Clear the card after scanning */
        gc->card_table[i] = VTX_CARD_CLEAN;
    }
}

void vtx_gc_collect_young(vtx_gc_t *gc)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");

    /* Reset to-space */
    semi_space_reset(&gc->young_to);

    /* Phase 1: Trace roots */
    for (uint32_t i = 0; i < gc->root_count; i++) {
        gc->root_stack[i].value = trace_value(gc, gc->root_stack[i].value);
    }

    /* Phase 2: Trace old→young references.
     * If card table is available, scan dirty cards (more efficient — only
     * scans cards that were actually written to). Otherwise, fall back
     * to scanning the entire remembered set. */
    if (gc->card_table != NULL) {
        scan_dirty_cards(gc);
    } else {
        /* Fallback: scan remembered set */
        for (uint32_t i = 0; i < gc->remembered_count; i++) {
            vtx_heap_object_t *old_obj = gc->remembered_set[i].obj;
            scan_object(gc, old_obj);
        }
    }

    /* Phase 3: Scan to-space (processes objects copied in phases 1 & 2) */
    scan_to_space(gc);

    /* Phase 4: Handle pinned objects — they stayed in from-space.
     * RT-3 fix: After scanning pinned objects, we must continue scanning
     * to-space because pinned-object scanning may have forwarded additional
     * young-gen objects that weren't reached in Phase 3. These newly-forwarded
     * objects need their fields traced too (the transitive closure must be
     * complete). We keep alternating between scanning pinned objects and
     * scanning to-space until no new objects are forwarded. */
    bool pinned_changed = true;
    while (pinned_changed) {
        pinned_changed = false;
        for (uint32_t i = 0; i < gc->pinned_count; i++) {
            vtx_heap_object_t *pinned = gc->pinned_objects[i];
            /* Pinned objects stay in from-space, but their fields may need updating */
            uint8_t old_field_count = pinned->field_count; /* save before scan may change refs */
            (void)old_field_count;
            scan_object(gc, pinned);
        }
        /* Scan to-space again — pinned-object scanning may have forwarded
         * new objects that need their fields traced */
        uint8_t *scan_before = gc->young_to.current;
        scan_to_space(gc);
        if (gc->young_to.current != scan_before) {
            pinned_changed = true; /* New objects were forwarded, re-scan pinned */
        }
    }

    /* Phase 5: Swap semi-spaces.
     * RT-2 fix: Pinned objects remain in the OLD from-space (young_from
     * before swap). After swap, young_from becomes the NEW from-space
     * (containing the live objects), and young_to becomes the OLD from-space
     * (to be used as to-space next collection). The pinned objects are in
     * the OLD from-space, which after swap is young_to. We must NOT reset
     * young_to on the next collection until all pinned objects have been
     * copied out or unpinned.
     *
     * For now, the simplest correct fix is to copy pinned objects into
     * the new from-space (young_to before swap = young_from after swap)
     * before swapping, so they survive the swap. */
    for (uint32_t i = 0; i < gc->pinned_count; i++) {
        vtx_heap_object_t *pinned = gc->pinned_objects[i];
        /* Pinned objects are still in from-space. Copy them to to-space
         * so they survive the swap. We can't move them (they're pinned),
         * so we copy them and leave a forwarding pointer. But wait —
         * other references to the pinned object (from root stack, etc.)
         * still point to the OLD location. We need to update those too.
         *
         * The simplest approach: since pinned objects are rare, just copy
         * them to to-space and update all references. The forwarding pointer
         * left in the old location will redirect any stale references. */
        void *new_ptr = semi_space_alloc(&gc->young_to, pinned->size);
        if (new_ptr != NULL) {
            memcpy(new_ptr, pinned, pinned->size);
            vtx_heap_object_t *new_obj = (vtx_heap_object_t *)new_ptr;
            new_obj->gc_pinned = 0; /* No longer needs to be pinned in new location */
            /* Leave forwarding pointer in old location */
            *(uintptr_t *)pinned = (uintptr_t)new_obj;
            pinned->size = VTX_GC_FORWARDING_SENTINEL;
        }
    }

    vtx_semi_space_t tmp = gc->young_from;
    gc->young_from = gc->young_to;
    gc->young_to = tmp;

    /* RT-6 fix: Rebuild the remembered set instead of clearing it.
     * OLD BUG: The remembered set was unconditionally cleared, hoping write
     * barriers would rebuild it. But write barriers only fire on NEW writes.
     * Old-gen objects that still reference into the new young generation
     * would not have their entries recreated, causing the next collection
     * to miss those references and prematurely collect live objects.
     *
     * NEW approach: After the swap, scan all old-gen objects and rebuild
     * the remembered set with any old→young references found. */
    for (uint32_t i = 0; i < gc->remembered_count; i++) {
        vtx_heap_object_t *old_obj = gc->remembered_set[i].obj;
        if (old_obj != NULL) {
            old_obj->gc_remembered = 0;
        }
    }
    gc->remembered_count = 0;

    /* Rebuild remembered set by scanning old-gen objects for young references */
    if (gc->old_gen.start != NULL && gc->old_gen.size > 0) {
        uint8_t *ptr = gc->old_gen.start;
        uint8_t *end = gc->old_gen.start + gc->old_gen.used;
        while (ptr < end) {
            vtx_heap_object_t *obj = (vtx_heap_object_t *)ptr;
            if (obj->size == 0 || obj->size == VTX_GC_FORWARDING_SENTINEL) {
                ptr += sizeof(vtx_free_node_t);
                continue;
            }
            if (obj->size >= VTX_GC_FORWARDING_SENTINEL) {
                /* Skip corrupted/invalid entries */
                break;
            }
            size_t obj_size = align_up_8(obj->size);
            /* Check if this old-gen object has any references into young gen */
            for (uint32_t f = 0; f < obj->field_count; f++) {
                vtx_value_t field = obj->fields[f];
                if (vtx_is_heap_ptr(field)) {
                    void *field_ptr = vtx_heap_ptr(field);
                    if (vtx_gc_in_young(gc, field_ptr)) {
                        /* Add to remembered set */
                        if (gc->remembered_count < gc->remembered_capacity) {
                            gc->remembered_set[gc->remembered_count].obj = obj;
                            gc->remembered_count++;
                            obj->gc_remembered = 1;
                        }
                        break; /* Only add each object once */
                    }
                }
            }
            ptr += obj_size;
        }
    }

    gc->collections_done++;
}

/* ========================================================================== */
/* Old-gen collection (mark-sweep)                                             */
/* ========================================================================== */

/**
 * Mark an object and recursively trace its fields.
 * RT-5 fix: Trace through ALL reachable objects, not just old-gen.
 * Young-gen objects can hold references to old-gen objects, and we must
 * follow those references during old-gen collection to avoid sweeping
 * live old-gen objects.
 */
static void mark_object(vtx_gc_t *gc, vtx_heap_object_t *obj)
{
    VTX_ASSERT(obj != NULL, "object must not be NULL");

    if (obj->gc_mark) {
        return; /* already marked */
    }

    obj->gc_mark = 1;

    /* Trace fields — follow ALL references, not just old-gen */
    for (uint32_t i = 0; i < obj->field_count; i++) {
        vtx_value_t field = obj->fields[i];
        if (vtx_is_heap_ptr(field)) {
            vtx_heap_object_t *field_obj = (vtx_heap_object_t *)vtx_heap_ptr(field);
            /* RT-5 fix: Only mark old-gen objects (we're doing old-gen
             * collection). But we DO recurse through young-gen objects
             * to find their old-gen references. */
            if (vtx_gc_in_old(gc, field_obj)) {
                mark_object(gc, field_obj);
            }
            /* Also recurse into young-gen objects to find transitive
             * old-gen references, but don't mark them (they're managed
             * by the young-gen collector). Use a separate visited set
             * to avoid infinite loops on cycles in young-gen. */
            else if (vtx_gc_in_young(gc, field_obj) && !field_obj->gc_mark) {
                /* Temporarily mark to avoid re-visiting */
                field_obj->gc_mark = 1;
                /* Recurse to find old-gen references through young-gen */
                for (uint32_t j = 0; j < field_obj->field_count; j++) {
                    vtx_value_t inner = field_obj->fields[j];
                    if (vtx_is_heap_ptr(inner)) {
                        vtx_heap_object_t *inner_obj = (vtx_heap_object_t *)vtx_heap_ptr(inner);
                        if (vtx_gc_in_old(gc, inner_obj)) {
                            mark_object(gc, inner_obj);
                        }
                    }
                }
                /* Note: we don't clear gc_mark on young-gen objects here
                 * because young-gen collection handles its own marking. */
            }
        }
    }
}

/**
 * Mark phase: trace from all roots.
 * RT-5 fix: Also trace through young-gen objects that may reference old-gen.
 * OLD BUG: mark_phase only traced roots that are directly in old gen.
 * If Root → YoungObj → OldObj, the OldObj was never marked and got swept.
 */
static void mark_phase(vtx_gc_t *gc)
{
    /* Mark from root stack — trace ALL reachable objects, not just old-gen */
    for (uint32_t i = 0; i < gc->root_count; i++) {
        vtx_value_t value = gc->root_stack[i].value;
        if (vtx_is_heap_ptr(value)) {
            vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(value);
            mark_object(gc, obj);
        }
    }
}

/**
 * Sweep phase: walk through old gen memory, free unmarked objects,
 * clear marks on marked objects.
 * RT-4 fix: Use gc_mark == 0xFF sentinel to distinguish free blocks
 * from live objects. Free blocks are skipped (already on the free list).
 */
static void sweep_phase(vtx_gc_t *gc)
{
    uint8_t *ptr = gc->old_gen.start;
    uint8_t *end = gc->old_gen.start + gc->old_gen.size;

    while (ptr < end) {
        vtx_heap_object_t *obj = (vtx_heap_object_t *)ptr;

        /* RT-4 fix: Check if this is a free block (gc_mark == 0xFF) */
        if (obj->gc_mark == 0xFF) {
            /* This is a free block — skip it */
            vtx_free_node_t *free_node = (vtx_free_node_t *)ptr;
            size_t free_size = align_up_8(free_node->size);
            if (free_size < sizeof(vtx_free_node_t)) {
                free_size = sizeof(vtx_free_node_t);
            }
            ptr += free_size;
            continue;
        }

        if (obj->size == 0 || obj->size >= VTX_GC_FORWARDING_SENTINEL) {
            /* Uninitialized/corrupted block — skip minimum unit */
            ptr += sizeof(vtx_free_node_t);
            continue;
        }

        size_t obj_size = align_up_8(obj->size);

        if (obj->gc_mark) {
            /* Object is alive — clear the mark */
            obj->gc_mark = 0;
        } else {
            /* Object is dead — free it */
            old_gen_free(&gc->old_gen, ptr, obj_size);
        }

        ptr += obj_size;
    }
}

void vtx_gc_collect_old(vtx_gc_t *gc)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");

    /* Mark phase */
    mark_phase(gc);

    /* Sweep phase */
    sweep_phase(gc);
}

/* ========================================================================== */
/* Pinning                                                                     */
/* ========================================================================== */

void vtx_gc_pin(vtx_gc_t *gc, vtx_heap_object_t *obj)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    VTX_ASSERT(obj != NULL, "object must not be NULL");

    if (obj->gc_pinned) {
        return; /* already pinned */
    }

    obj->gc_pinned = 1;

    /* Add to pinned objects list */
    if (gc->pinned_count >= gc->pinned_capacity) {
        uint32_t new_cap = gc->pinned_capacity * 2;
        vtx_heap_object_t **new_list = (vtx_heap_object_t **)realloc(
            gc->pinned_objects, new_cap * sizeof(vtx_heap_object_t *));
        if (new_list == NULL) {
            VTX_ASSERT(false, "pinned objects list overflow — out of memory");
            return;
        }
        gc->pinned_objects = new_list;
        gc->pinned_capacity = new_cap;
    }

    gc->pinned_objects[gc->pinned_count] = obj;
    gc->pinned_count++;
}

void vtx_gc_unpin(vtx_gc_t *gc, vtx_heap_object_t *obj)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    VTX_ASSERT(obj != NULL, "object must not be NULL");

    if (!obj->gc_pinned) {
        return;
    }

    obj->gc_pinned = 0;

    /* Remove from pinned objects list */
    for (uint32_t i = 0; i < gc->pinned_count; i++) {
        if (gc->pinned_objects[i] == obj) {
            /* Swap with the last element for O(1) removal */
            gc->pinned_objects[i] = gc->pinned_objects[gc->pinned_count - 1];
            gc->pinned_count--;
            return;
        }
    }
}

bool vtx_gc_is_pinned(vtx_gc_t *gc, vtx_heap_object_t *obj)
{
    (void)gc;
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    VTX_ASSERT(obj != NULL, "object must not be NULL");
    return obj->gc_pinned != 0;
}

/* ========================================================================== */
/* Space queries                                                               */
/* ========================================================================== */

bool vtx_gc_in_young(const vtx_gc_t *gc, const void *ptr)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    /* Check both semi-spaces — an object could be in either during collection */
    return ptr_in_range(ptr, gc->young_from.start, gc->young_from.size) ||
           ptr_in_range(ptr, gc->young_to.start, gc->young_to.size);
}

bool vtx_gc_in_old(const vtx_gc_t *gc, const void *ptr)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    return ptr_in_range(ptr, gc->old_gen.start, gc->old_gen.size);
}

size_t vtx_gc_young_used(const vtx_gc_t *gc)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    return (size_t)(gc->young_from.current - gc->young_from.start);
}

size_t vtx_gc_old_used(const vtx_gc_t *gc)
{
    VTX_ASSERT(gc != NULL, "GC must not be NULL");
    return gc->old_gen.used;
}
