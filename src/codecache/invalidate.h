#ifndef VORTEX_CODECACHE_INVALIDATE_H
#define VORTEX_CODECACHE_INVALIDATE_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "runtime/type_system.h"
#include "runtime/arena.h"

/**
 * VORTEX Dependency-Set Invalidation
 *
 * When a class is loaded or redefined, all compiled methods whose
 * guards depend on that class must be invalidated. This module
 * maintains an inverted index from TypeID/ShapeID to sets of methods,
 * enabling efficient lookup of all methods affected by a type change.
 *
 * Invalidation steps:
 *   1. Look up the TypeID in the inverted index
 *   2. For each method in the set:
 *      a. Mark it as not-compiled (code pointer → NULL)
 *      b. Free its side table and deopt info
 *      c. Remove it from the inverted index
 *   3. The code in segments is freed only when segments are empty
 *
 * The inverted index is updated when methods are installed (their
 * dependency sets are recorded) and when methods are uninstalled
 * or invalidated (their entries are removed).
 */

/* ========================================================================== */
/* Inverted index entry                                                        */
/* ========================================================================== */

/**
 * A set of method IDs that depend on a particular TypeID or ShapeID.
 */
#define VTX_DEP_SET_INITIAL_CAPACITY 8

typedef struct {
    uint32_t *method_ids;     /* array of method IDs */
    uint32_t  count;          /* number of methods */
    uint32_t  capacity;       /* allocated capacity */
} vtx_dep_set_t;

/* ========================================================================== */
/* Hash table capacity (shared by both index types)                             */
/* ========================================================================== */

#define VTX_INVERTED_INDEX_CAPACITY 256

/* ========================================================================== */
/* Guard-level dependency tracking (Proposal #4)                                 */
/* ========================================================================== */

/**
 * A guard-level dependency: maps (TypeID, guard_id) instead of just TypeID.
 * This enables fine-grained invalidation that only patches affected guards
 * rather than invalidating entire methods.
 */
typedef struct {
    uint32_t     type_id;       /* TypeID that this guard depends on */
    uint32_t     guard_id;      /* guard_id within the method */
    uint32_t     method_id;     /* method that contains this guard */
    uint32_t     guard_branch_offset; /* offset from code_start to guard's JCC rel32 */
    uint8_t     *code_start;    /* base address of compiled code */
} vtx_guard_dep_t;

/**
 * Guard-level inverted index: TypeID → array of guard dependencies.
 * This is the fine-grained counterpart of vtx_inverted_index_t.
 * Instead of mapping TypeID → {method_ids}, it maps
 * TypeID → {guard_id, method_id, code_start, branch_offset}.
 */
#define VTX_GUARD_DEP_INITIAL_CAPACITY 16

typedef struct vtx_guard_dep_entry vtx_guard_dep_entry_t;

struct vtx_guard_dep_entry {
    uint32_t              type_id;        /* TypeID key */
    vtx_guard_dep_t      *deps;           /* array of guard dependencies */
    uint32_t              dep_count;       /* number of dependencies */
    uint32_t              dep_capacity;    /* allocated capacity */
    vtx_guard_dep_entry_t *next;          /* hash chain */
};

typedef struct {
    vtx_guard_dep_entry_t *buckets[VTX_INVERTED_INDEX_CAPACITY];
    uint32_t               entry_count;
    vtx_arena_t           *arena;
} vtx_guard_dep_index_t;

/* ========================================================================== */
/* Inverted index                                                              */
/* ========================================================================== */

/**
 * Inverted index: TypeID → set of methods.
 * Implemented as a hash table for O(1) average lookup.
 */
typedef struct vtx_index_entry vtx_index_entry_t;

struct vtx_index_entry {
    uint32_t            key;          /* TypeID or ShapeID */
    vtx_dep_set_t       dep_set;     /* set of method IDs */
    vtx_index_entry_t  *next;        /* hash chain */
};

typedef struct {
    vtx_index_entry_t  *buckets[VTX_INVERTED_INDEX_CAPACITY];
    uint32_t            entry_count;
    vtx_arena_t        *arena;       /* arena for allocations */
} vtx_inverted_index_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the inverted index.
 */
int vtx_inverted_index_init(vtx_inverted_index_t *index, vtx_arena_t *arena);

/**
 * Destroy the inverted index.
 */
void vtx_inverted_index_destroy(vtx_inverted_index_t *index);

/* ========================================================================== */
/* Index management                                                            */
/* ========================================================================== */

/**
 * Add a dependency: method `method_id` depends on `typeid`.
 */
int vtx_inverted_index_add(vtx_inverted_index_t *index,
                            uint32_t typeid_, uint32_t method_id);

/**
 * Add a shape dependency: method `method_id` depends on `shapeid`.
 */
int vtx_inverted_index_add_shape(vtx_inverted_index_t *index,
                                  uint32_t shapeid, uint32_t method_id);

/**
 * Remove a method from all dependency sets.
 * Call this when a method is uninstalled or invalidated.
 */
int vtx_inverted_index_remove_method(vtx_inverted_index_t *index,
                                      uint32_t method_id);

/**
 * Look up the set of methods that depend on `typeid`.
 * Returns the dep set, or NULL if no methods depend on this type.
 */
const vtx_dep_set_t *vtx_inverted_index_lookup(vtx_inverted_index_t *index,
                                                 uint32_t typeid_);

/* ========================================================================== */
/* Invalidation                                                                */
/* ========================================================================== */

/**
 * Invalidate all methods that depend on the given TypeID.
 *
 * This is called when a class is loaded or redefined. All compiled
 * methods whose guards reference this TypeID are found via the
 * inverted index, marked as not-compiled, and their metadata is freed.
 *
 * @param typeid_    The TypeID that triggered invalidation
 * @param cache      Code cache
 * @param registry   Method registry
 * @param index      Inverted index
 * @return           Number of methods invalidated, or -1 on failure
 */
int vtx_invalidate_dependencies(uint32_t typeid_,
                                 vtx_code_cache_t *cache,
                                 vtx_method_registry_t *registry,
                                 vtx_inverted_index_t *index);

/**
 * Invalidate all methods that depend on the given ShapeID.
 *
 * Called when an object's shape changes (e.g., property added/removed).
 *
 * @param shapeid    The ShapeID that triggered invalidation
 * @param cache      Code cache
 * @param registry   Method registry
 * @param index      Inverted index
 * @return           Number of methods invalidated, or -1 on failure
 */
int vtx_invalidate_shape(vtx_shapeid_t shapeid,
                          vtx_code_cache_t *cache,
                          vtx_method_registry_t *registry,
                          vtx_inverted_index_t *index);

/**
 * Register a method's dependencies in the inverted index.
 * Called during method installation.
 *
 * @param index      Inverted index
 * @param method     The compiled method
 * @return           0 on success, -1 on failure
 */
int vtx_invalidate_register(vtx_inverted_index_t *index,
                             const vtx_compiled_method_t *method);

/* ========================================================================== */
/* Guard-level dependency index lifecycle (Proposal #4)                          */
/* ========================================================================== */

/* Guard-level dependency index lifecycle */
int vtx_guard_dep_index_init(vtx_guard_dep_index_t *index, vtx_arena_t *arena);
void vtx_guard_dep_index_destroy(vtx_guard_dep_index_t *index);

/* Add a guard-level dependency */
int vtx_guard_dep_index_add(vtx_guard_dep_index_t *index,
                              uint32_t type_id,
                              uint32_t guard_id,
                              uint32_t method_id,
                              uint32_t guard_branch_offset,
                              uint8_t *code_start);

/* Look up guard dependencies by TypeID */
const vtx_guard_dep_t *vtx_guard_dep_index_lookup(vtx_guard_dep_index_t *index,
                                                     uint32_t type_id,
                                                     uint32_t *out_count);

/* ========================================================================== */
/* Fine-grained invalidation (Proposal #4)                                       */
/* ========================================================================== */

/**
 * Fine-grained invalidation: only patch guards that depend on the
 * changed TypeID, leaving other guards in the method active.
 *
 * For each guard that depends on typeid_:
 *   1. Patch the guard's JCC in the compiled code to jump to a deopt stub
 *   2. The method's other guards remain active
 *   3. No recompilation is needed unless all guards are invalidated
 *
 * This avoids the "nuclear option" of invalidating entire methods.
 *
 * @param typeid_   The TypeID that triggered invalidation
 * @param index     Guard-level dependency index
 * @param cache     Code cache (for deopt stub allocation)
 * @return          Number of guards patched, or -1 on failure
 */
int vtx_invalidate_guard_fine_grained(uint32_t typeid_,
                                        vtx_guard_dep_index_t *index,
                                        vtx_code_cache_t *cache);

#endif /* VORTEX_CODECACHE_INVALIDATE_H */
