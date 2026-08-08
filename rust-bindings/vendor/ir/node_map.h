/* ========================================================================== */
/* NodeMap — Reusable per-node auxiliary data                                  */
/* ========================================================================== */
/*
 * §2.1: A reusable NodeMap<T> backed by a single arena-allocated array
 * indexed by NodeID. Every IR pass (constant_prop, bounds_check, licm)
 * currently re-allocates node_count * sizeof(T) per invocation. This
 * header provides a C-compatible API that:
 *   - Allocates once per pass (arena-allocated)
 *   - Is zeroed on allocation
 *   - Supports O(1) lookup by NodeID
 *   - Is type-agnostic (void* + element size)
 *
 * V8 uses NodeId-scoped auxiliary data (NodeAux in graph.h).
 * HotSpot uses Node::expect_type and Phase::_node_types.
 * Cranelift uses SecondaryMap<K, V> in entities.rs.
 */

#ifndef VORTEX_IR_NODE_MAP_H
#define VORTEX_IR_NODE_MAP_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ir/node.h"
#include "runtime/arena.h"

/* A NodeMap is a zeroed array of node_count elements, each of elem_size
 * bytes, indexed by NodeID. It's arena-allocated for fast allocation
 * and bulk deallocation (the arena is reset at the end of the pass). */
typedef struct {
    void    *data;         /* arena-allocated array */
    uint32_t elem_size;    /* size of each element in bytes */
    uint32_t count;        /* number of elements (= node_table.count at alloc) */
} vtx_node_map_t;

/* Initialize a NodeMap with the given element size.
 * Allocates node_count * elem_size bytes from the arena, zeroed.
 * Returns 0 on success, -1 on allocation failure. */
static inline int vtx_node_map_init(vtx_node_map_t *map,
                                      vtx_node_table_t *nt,
                                      size_t elem_size,
                                      vtx_arena_t *arena)
{
    if (!map || !nt) return -1;
    map->count = nt->count;
    map->elem_size = (uint32_t)elem_size;
    if (map->count == 0 || elem_size == 0) {
        map->data = NULL;
        return 0;
    }
    map->data = vtx_arena_alloc(arena, (size_t)map->count * elem_size);
    if (!map->data) return -1;
    memset(map->data, 0, (size_t)map->count * elem_size);
    return 0;
}

/* Get a pointer to the element for the given NodeID.
 * Returns NULL if the NodeID is out of bounds. */
static inline void *vtx_node_map_get(vtx_node_map_t *map, vtx_nodeid_t id)
{
    if (!map || !map->data) return NULL;
    if (id >= map->count) return NULL;
    return (char *)map->data + (size_t)id * map->elem_size;
}

/* Get a const pointer to the element for the given NodeID. */
static inline const void *vtx_node_map_get_const(const vtx_node_map_t *map,
                                                    vtx_nodeid_t id)
{
    if (!map || !map->data) return NULL;
    if (id >= map->count) return NULL;
    return (const char *)map->data + (size_t)id * map->elem_size;
}

/* Check if the NodeMap is valid (non-NULL data). */
static inline bool vtx_node_map_valid(const vtx_node_map_t *map)
{
    return map != NULL && map->data != NULL;
}

/* Grow the NodeMap if the node table has grown since initialization.
 * Re-allocates from the arena and copies the old data.
 * Returns 0 on success, -1 on failure. */
static inline int vtx_node_map_grow(vtx_node_map_t *map,
                                      vtx_node_table_t *nt,
                                      vtx_arena_t *arena)
{
    if (!map || !nt) return -1;
    if (nt->count <= map->count) return 0;  /* no growth needed */

    uint32_t new_count = nt->count;
    void *new_data = vtx_arena_alloc(arena, (size_t)new_count * map->elem_size);
    if (!new_data) return -1;

    /* Copy old data + zero the new portion */
    if (map->data && map->count > 0) {
        memcpy(new_data, map->data, (size_t)map->count * map->elem_size);
    }
    memset((char *)new_data + (size_t)map->count * map->elem_size, 0,
           (size_t)(new_count - map->count) * map->elem_size);

    map->data = new_data;
    map->count = new_count;
    return 0;
}

#endif /* VORTEX_IR_NODE_MAP_H */
