#ifndef VORTEX_GC_H
#define VORTEX_GC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/type_system.h"

/**
 * VORTEX Generational Garbage Collector
 *
 * Young generation: semi-space copying collector (two semi-spaces,
 * each VTX_GC_YOUNG_SIZE bytes). Allocation is bump-pointer.
 * Objects that survive VTX_GC_PROMOTION_AGE young-gen collections
 * are promoted to the old generation.
 *
 * Old generation: mark-sweep collector (VTX_GC_OLD_SIZE bytes).
 * Uses a free list for allocation. Mark-sweep runs when old gen
 * is full or when explicitly requested.
 *
 * Write barrier: generational write barrier. If a store writes a
 * young-gen pointer into an old-gen object's field, the old-gen
 * object is added to the remembered set.
 *
 * Pinning: objects can be pinned to prevent moving during young-gen
 * collection. This is used when JIT code holds raw pointers.
 */

/* GC configuration constants */
#define VTX_GC_YOUNG_SIZE      (4 * 1024 * 1024)  /* 4MB per semi-space */
#define VTX_GC_OLD_SIZE        (64 * 1024 * 1024)  /* 64MB old generation */
#define VTX_GC_PROMOTION_AGE   3                    /* collections before promotion */
#define VTX_GC_ROOT_STACK_SIZE 4096                 /* max root stack entries */
#define VTX_GC_REMEMBERED_SET_INIT 256              /* initial remembered set capacity */

/* Card table for write barrier.
 * The heap is divided into "cards" of VTX_CARD_SIZE bytes each.
 * Each card has a 1-byte entry in the card table.
 * A dirty card (value = VTX_CARD_DIRTY) means that some old-gen
 * object in that card region has had a young-gen pointer stored into it.
 * During young-gen collection, only objects in dirty cards need to
 * be scanned for young-gen pointers.
 *
 * Card marking reduces the write barrier from ~10 branches (range checks
 * + remembered set insert) to ~3 instructions on the fast path:
 *   1. Check if value is a young-gen pointer (1 branch)
 *   2. Compute card index from object address (1 subtraction + shift)
 *   3. Mark the card as dirty (1 byte store)
 */
#define VTX_CARD_SIZE      512     /* 512 bytes per card */
#define VTX_CARD_SHIFT     9       /* log2(512) */
#define VTX_CARD_CLEAN     0x00    /* card is clean */
#define VTX_CARD_DIRTY     0x01    /* card is dirty (has young-gen ref) */

/* ========================================================================== */
/* GC mode enumeration                                                         */
/* ========================================================================== */

typedef enum {
    VTX_GC_GENERATIONAL = 0,  /* current behavior — full GC */
    VTX_GC_NONE,              /* no collection, no barriers, no safepoints */
    VTX_GC_MANUAL,            /* explicit alloc/free, no barriers */
    VTX_GC_ARENA,             /* bump-pointer only, free-all-at-once */
} vtx_gc_mode_t;

/* ========================================================================== */
/* GC structures                                                               */
/* ========================================================================== */

/* A semi-space for the young generation */
typedef struct {
    uint8_t    *start;       /* start of the semi-space memory */
    uint8_t    *current;     /* current bump pointer */
    size_t      size;        /* total size of this semi-space */
} vtx_semi_space_t;

/* Free list node for old generation.
 * RT-4 fix: Added gc_mark field set to 0xFF so the sweep phase can
 * distinguish free blocks from live objects. Live objects have
 * gc_mark == 0 or 1 (0=unmarked, 1=marked). Free blocks have
 * gc_mark == 0xFF, which is never a valid mark state for a live object. */
typedef struct vtx_free_node vtx_free_node_t;
struct vtx_free_node {
    size_t           size;     /* size of this free block (including this header) */
    vtx_free_node_t *next;    /* next free block */
    uint8_t          gc_mark; /* RT-4: always 0xFF for free blocks */
};

/* Old generation (mark-sweep) */
typedef struct {
    uint8_t          *start;       /* start of old gen memory */
    size_t            size;        /* total size of old gen */
    size_t            used;        /* bytes currently in use */
    vtx_free_node_t  *free_list;  /* free list head */
} vtx_old_gen_t;

/* Remembered set entry */
typedef struct {
    vtx_heap_object_t *obj;    /* old-gen object that references young-gen */
} vtx_remembered_entry_t;

/* Root stack entry */
typedef struct {
    vtx_value_t value;
} vtx_root_entry_t;

/* The garbage collector */
typedef struct vtx_gc_t {
    /* Young generation: two semi-spaces */
    vtx_semi_space_t  young_from;     /* current allocation space */
    vtx_semi_space_t  young_to;       /* copy destination during collection */

    /* Old generation: mark-sweep */
    vtx_old_gen_t     old_gen;

    /* Root stack */
    vtx_root_entry_t *root_stack;
    uint32_t          root_count;
    uint32_t          root_capacity;

    /* Remembered set (old→young references) */
    vtx_remembered_entry_t *remembered_set;
    uint32_t               remembered_count;
    uint32_t               remembered_capacity;

    /* Type system reference (for object size/field scanning) */
    vtx_type_system_t *type_system;

    /* Collection state */
    bool              collection_requested;
    uint32_t          collections_done; /* total young-gen collections */

    /* Pinned objects list (for young-gen collection) */
    vtx_heap_object_t **pinned_objects;
    uint32_t            pinned_count;
    uint32_t            pinned_capacity;

    /* GC mode and mode-dependent function pointers */
    vtx_gc_mode_t     mode;           /* current GC mode */

    /* Function pointers for mode-dependent operations */
    void (*fn_write_barrier)(struct vtx_gc_t *gc, vtx_heap_object_t *obj,
                             uint32_t field_offset, vtx_value_t value);
    void (*fn_safepoint)(struct vtx_gc_t *gc);
    void (*fn_root_push)(struct vtx_gc_t *gc, vtx_value_t value);
    vtx_value_t (*fn_root_pop)(struct vtx_gc_t *gc);

    /* Manual mode: free list for explicit free */
    vtx_free_node_t  *manual_free_list;

    /* Arena mode: entry/leave stack */
    uint8_t          *arena_save_point;  /* saved bump pointer for arena_enter/leave */

    /* Card table for write barrier (generational mode) */
    uint8_t          *card_table;       /* card table (1 byte per card) */
    size_t            card_table_size;   /* number of card entries */
    uint8_t          *heap_base;        /* base address of the heap (for card index computation) */
    size_t            heap_size;         /* total heap size covered by the card table */
} vtx_gc_t;

/* ========================================================================== */
/* GC lifecycle                                                                */
/* ========================================================================== */

/**
 * Initialize the garbage collector. Requires a type_system for
 * object size/field layout information.
 * The mode parameter selects the GC strategy.
 * Returns 0 on success, -1 on failure.
 */
int vtx_gc_init(vtx_gc_t *gc, vtx_type_system_t *ts, vtx_gc_mode_t mode);

/**
 * Destroy the garbage collector and release all memory.
 */
void vtx_gc_destroy(vtx_gc_t *gc);

/* ========================================================================== */
/* Allocation                                                                  */
/* ========================================================================== */

/**
 * Allocate a heap object of the given size with the given type_id.
 * Allocates in the young generation using bump-pointer allocation.
 * Returns a pointer to the initialized heap object, or NULL on failure.
 * The object header is initialized; fields are set to VTX_VALUE_UNDEFINED.
 */
vtx_heap_object_t *vtx_gc_alloc(vtx_gc_t *gc, size_t size, vtx_typeid_t type_id);

/**
 * Check if collection has been requested and if so, perform a young-gen
 * collection. This should be called at safe points (backward branches,
 * method calls).
 */
void vtx_gc_safepoint(vtx_gc_t *gc);

/* ========================================================================== */
/* Root management                                                             */
/* ========================================================================== */

/**
 * Push a value onto the GC root stack. The value (if it contains a
 * heap pointer) will be traced during collection.
 */
void vtx_gc_root_push(vtx_gc_t *gc, vtx_value_t value);

/**
 * Pop a value from the GC root stack.
 */
vtx_value_t vtx_gc_root_pop(vtx_gc_t *gc);

/* ========================================================================== */
/* Write barrier                                                               */
/* ========================================================================== */

/**
 * Generational write barrier. Call this when storing a value into a
 * field of an object. If the object is in old gen and the value is
 * a young-gen pointer, the object is added to the remembered set.
 */
void vtx_gc_write_barrier(vtx_gc_t *gc, vtx_heap_object_t *obj,
                          uint32_t field_offset, vtx_value_t value);

/**
 * Card marking write barrier. Replaces the old 10-branch barrier with
 * ~3 instructions on the fast path: check if value is young-gen, compute
 * card index, mark card dirty. Used in generational mode.
 */
void vtx_gc_write_barrier_card(vtx_gc_t *gc, vtx_heap_object_t *obj,
                               uint32_t field_offset, vtx_value_t value);

/* ========================================================================== */
/* Collection                                                                  */
/* ========================================================================== */

/**
 * Perform a young-generation collection (copying collector).
 * Copies live objects from young_from to young_to, then swaps.
 * Promotes survivors that have survived >= VTX_GC_PROMOTION_AGE collections.
 */
void vtx_gc_collect_young(vtx_gc_t *gc);

/**
 * Perform an old-generation collection (mark-sweep).
 * Marks all reachable objects from roots, then sweeps unreachable ones.
 */
void vtx_gc_collect_old(vtx_gc_t *gc);

/* ========================================================================== */
/* Pinning                                                                     */
/* ========================================================================== */

/**
 * Pin an object so it won't be moved during young-gen collection.
 * Used when JIT code holds raw pointers to the object.
 */
void vtx_gc_pin(vtx_gc_t *gc, vtx_heap_object_t *obj);

/**
 * Unpin an object.
 */
void vtx_gc_unpin(vtx_gc_t *gc, vtx_heap_object_t *obj);

/**
 * Check if an object is pinned.
 */
bool vtx_gc_is_pinned(vtx_gc_t *gc, vtx_heap_object_t *obj);

/* ========================================================================== */
/* GC mode management                                                         */
/* ========================================================================== */

/**
 * Set the GC mode at runtime. This reconfigures the function pointers
 * and may adjust internal state for the new mode.
 */
void vtx_gc_set_mode(vtx_gc_t *gc, vtx_gc_mode_t mode);

/**
 * Get the current GC mode.
 */
vtx_gc_mode_t vtx_gc_get_mode(const vtx_gc_t *gc);

/**
 * Manual mode: free a previously allocated object.
 * Adds the block to a free list for O(1) free.
 * Only valid when mode is VTX_GC_MANUAL.
 */
void vtx_gc_manual_free(vtx_gc_t *gc, void *ptr, size_t size);

/**
 * Arena mode: save current allocation point.
 * Only valid when mode is VTX_GC_ARENA.
 */
void vtx_gc_arena_enter(vtx_gc_t *gc);

/**
 * Arena mode: restore to saved allocation point (free everything since enter).
 * Only valid when mode is VTX_GC_ARENA.
 */
void vtx_gc_arena_leave(vtx_gc_t *gc);

/**
 * Check if a mode requires write barriers.
 */
static inline bool vtx_gc_mode_needs_barrier(vtx_gc_mode_t mode) {
    return mode == VTX_GC_GENERATIONAL;
}

/**
 * Check if a mode needs safepoints.
 */
static inline bool vtx_gc_mode_needs_safepoint(vtx_gc_mode_t mode) {
    return mode == VTX_GC_GENERATIONAL;
}

/**
 * Check if a mode needs root management.
 */
static inline bool vtx_gc_mode_needs_roots(vtx_gc_mode_t mode) {
    return mode == VTX_GC_GENERATIONAL;
}

/* ========================================================================== */
/* Internal helpers (exposed for testing)                                      */
/* ========================================================================== */

/**
 * Check if a pointer falls within the young generation from-space.
 */
bool vtx_gc_in_young(const vtx_gc_t *gc, const void *ptr);

/**
 * Check if a pointer falls within the old generation.
 */
bool vtx_gc_in_old(const vtx_gc_t *gc, const void *ptr);

/**
 * Get the total number of bytes allocated in young gen.
 */
size_t vtx_gc_young_used(const vtx_gc_t *gc);

/**
 * Get the total number of bytes used in old gen.
 */
size_t vtx_gc_old_used(const vtx_gc_t *gc);

#endif /* VORTEX_GC_H */
