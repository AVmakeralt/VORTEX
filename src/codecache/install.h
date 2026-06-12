#ifndef VORTEX_CODECACHE_INSTALL_H
#define VORTEX_CODECACHE_INSTALL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "codecache/cache.h"
#include "deopt/side_table.h"
#include "lower/reloc.h"
#include "runtime/type_system.h"
#include "runtime/arena.h"

/**
 * VORTEX Method Installation
 *
 * Copies compiled native code into a code cache segment and updates
 * the method's code pointer atomically. After installation, the
 * method is immediately callable from any thread.
 *
 * Installation steps:
 *   1. Allocate space in the code cache
 *   2. Copy the compiled code into the segment
 *   3. Apply external relocations (fix up call targets using final address)
 *   4. Make the code segment executable
 *   5. Store metadata (side table, deopt info)
 *   6. Update the method's code pointer with release store
 */

/* ========================================================================== */
/* Clock eviction state                                                        */
/* ========================================================================== */

/**
 * Per-method state for the clock (Second Chance) eviction algorithm.
 * The use_bit is set when the method is called (touch) and cleared
 * by the clock hand during eviction scanning. Methods with a cleared
 * use_bit are eviction candidates.
 */
typedef struct {
    uint32_t clock_hand;       /* current position of the clock hand */
    bool     use_bit;          /* "recently used" bit (set on call, cleared by clock) */
} vtx_clock_state_t;

/* ========================================================================== */
/* Compiled method metadata                                                    */
/* ========================================================================== */

/**
 * Metadata for a compiled method, stored separately from the code.
 * This includes everything needed for deoptimization and invalidation.
 */
typedef struct vtx_compiled_method vtx_compiled_method_t;

struct vtx_compiled_method {
    /* Method identity */
    uint32_t                 method_id;         /* unique method identifier */
    const vtx_method_desc_t *method_desc;       /* method descriptor */

    /* Code location */
    uint8_t                 *code_start;        /* pointer to code in cache */
    uint32_t                 code_size;         /* size of compiled code */

    /* Deoptimization metadata */
    vtx_side_table_t        *side_table;        /* deopt side table */

    /* Relocation table (for re-applying external call relocations
     * if the code is moved or re-installed) */
    vtx_reloc_table_t       *reloc_table;

    /* Dependency set: TypeIDs and ShapeIDs this method's guards depend on */
    uint32_t                *dep_type_ids;      /* array of TypeIDs */
    uint32_t                 dep_type_count;
    uint32_t                *dep_shape_ids;     /* array of ShapeIDs */
    uint32_t                 dep_shape_count;

    /* LRU timestamp for eviction (kept for diagnostics) */
    uint64_t                 last_used_timestamp;
    uint32_t                 call_count;        /* call counter for LRU */

    /* Clock eviction state */
    vtx_clock_state_t        clock_state;       /* use-bit and clock-hand position */

    /* State */
    bool                     is_installed;      /* true if code is installed */
    bool                     is_valid;          /* false if invalidated */

    /* Linked list for method registry */
    vtx_compiled_method_t   *next;
};

/* ========================================================================== */
/* Method registry                                                             */
/* ========================================================================== */

#define VTX_METHOD_REGISTRY_INITIAL_CAPACITY 256  /* must be power of 2 */

typedef struct vtx_method_registry {
    vtx_compiled_method_t **methods;        /* array indexed by method_id */
    uint32_t                method_count;   /* number of registered methods */
    uint32_t                capacity;       /* allocated capacity (always power of 2) */
    uint32_t                capacity_mask;  /* capacity - 1, for fast modulo via bitwise AND */
    uint32_t                clock_hand;     /* current position of the clock hand */
} vtx_method_registry_t;

/**
 * Initialize the method registry.
 */
int vtx_method_registry_init(vtx_method_registry_t *registry, vtx_arena_t *arena);

/**
 * Destroy the method registry.
 */
void vtx_method_registry_destroy(vtx_method_registry_t *registry);

/**
 * Register a compiled method in the registry.
 */
int vtx_method_registry_add(vtx_method_registry_t *registry,
                             vtx_compiled_method_t *method);

/**
 * Look up a compiled method by its method_id.
 */
vtx_compiled_method_t *vtx_method_registry_get(vtx_method_registry_t *registry,
                                                uint32_t method_id);

/**
 * Remove a compiled method from the registry.
 */
int vtx_method_registry_remove(vtx_method_registry_t *registry, uint32_t method_id);

/* ========================================================================== */
/* Installation                                                                */
/* ========================================================================== */

/**
 * Install a compiled method into the code cache.
 *
 * Steps:
 *   1. Allocate space in the cache for the compiled code
 *   2. Copy the code into the cache
 *   3. Make the code executable
 *   4. Register the method in the registry
 *   5. Update the method's code pointer atomically
 *
 * @param cache       Code cache
 * @param registry    Method registry
 * @param method      Method descriptor
 * @param code        Compiled code bytes
 * @param code_size   Size of compiled code
 * @param side_table  Deoptimization side table (ownership transferred)
 * @param reloc_table Relocation table (for external call fixups at install time)
 * @param dep_types   Array of TypeIDs this method depends on
 * @param dep_type_count Number of TypeIDs
 * @param dep_shapes  Array of ShapeIDs this method depends on
 * @param dep_shape_count Number of ShapeIDs
 * @param arena       Arena for allocations
 * @return            true on success, false on failure
 */
bool vtx_install_method(vtx_code_cache_t *cache,
                         vtx_method_registry_t *registry,
                         const vtx_method_desc_t *method,
                         uint32_t method_id,
                         const uint8_t *code,
                         uint32_t code_size,
                         vtx_side_table_t *side_table,
                         vtx_reloc_table_t *reloc_table,
                         const uint32_t *dep_types,
                         uint32_t dep_type_count,
                         const uint32_t *dep_shapes,
                         uint32_t dep_shape_count,
                         vtx_arena_t *arena);

/**
 * Uninstall a method: mark it as not compiled and free its metadata.
 * The code in the cache is freed separately (when the segment is empty).
 *
 * @param cache       Code cache
 * @param registry    Method registry
 * @param method_id   Method ID to uninstall
 * @return            0 on success, -1 on failure
 */
int vtx_uninstall_method(vtx_code_cache_t *cache,
                          vtx_method_registry_t *registry,
                          uint32_t method_id);

/**
 * Get the entry point for a compiled method.
 * Returns NULL if the method is not compiled or not installed.
 */
void *vtx_method_entry_point(const vtx_compiled_method_t *method);

#endif /* VORTEX_CODECACHE_INSTALL_H */
