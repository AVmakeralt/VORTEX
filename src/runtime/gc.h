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

/* ========================================================================== */
/* GC structures                                                               */
/* ========================================================================== */

/* A semi-space for the young generation */
typedef struct {
    uint8_t    *start;       /* start of the semi-space memory */
    uint8_t    *current;     /* current bump pointer */
    size_t      size;        /* total size of this semi-space */
} vtx_semi_space_t;

/* Free list node for old generation */
typedef struct vtx_free_node vtx_free_node_t;
struct vtx_free_node {
    size_t           size;     /* size of this free block (including this header) */
    vtx_free_node_t *next;    /* next free block */
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
typedef struct {
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
} vtx_gc_t;

/* ========================================================================== */
/* GC lifecycle                                                                */
/* ========================================================================== */

/**
 * Initialize the garbage collector. Requires a type_system for
 * object size/field layout information.
 * Returns 0 on success, -1 on failure.
 */
int vtx_gc_init(vtx_gc_t *gc, vtx_type_system_t *ts);

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
