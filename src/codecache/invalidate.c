/**
 * VORTEX Dependency-Set Invalidation
 *
 * Maintains an inverted index from TypeID/ShapeID to sets of compiled
 * methods. When a type changes, all dependent methods are found and
 * invalidated efficiently.
 */

#include "codecache/invalidate.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Hash function                                                               */
/* ========================================================================== */

static uint32_t index_hash(uint32_t key)
{
    /* FNV-1a hash */
    uint32_t h = 2166136261u;
    h ^= key & 0xFF;
    h *= 16777619u;
    h ^= (key >> 8) & 0xFF;
    h *= 16777619u;
    h ^= (key >> 16) & 0xFF;
    h *= 16777619u;
    h ^= (key >> 24) & 0xFF;
    h *= 16777619u;
    return h % VTX_INVERTED_INDEX_CAPACITY;
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_inverted_index_init(vtx_inverted_index_t *index, vtx_arena_t *arena)
{
    if (!index) return -1;
    memset(index->buckets, 0, sizeof(index->buckets));
    index->entry_count = 0;
    index->arena = arena;
    return 0;
}

void vtx_inverted_index_destroy(vtx_inverted_index_t *index)
{
    if (!index) return;

    /* Free all entries */
    for (uint32_t b = 0; b < VTX_INVERTED_INDEX_CAPACITY; b++) {
        vtx_index_entry_t *entry = index->buckets[b];
        while (entry) {
            vtx_index_entry_t *next = entry->next;
            if (entry->dep_set.method_ids) {
                free(entry->dep_set.method_ids);
            }
            free(entry);
            entry = next;
        }
        index->buckets[b] = NULL;
    }
    index->entry_count = 0;
}

/* ========================================================================== */
/* Find or create an entry                                                     */
/* ========================================================================== */

static vtx_index_entry_t *find_entry(vtx_inverted_index_t *index, uint32_t key)
{
    uint32_t bucket = index_hash(key);
    vtx_index_entry_t *entry = index->buckets[bucket];
    while (entry) {
        if (entry->key == key) return entry;
        entry = entry->next;
    }
    return NULL;
}

static vtx_index_entry_t *create_entry(vtx_inverted_index_t *index, uint32_t key)
{
    uint32_t bucket = index_hash(key);

    vtx_index_entry_t *entry = (vtx_index_entry_t *)malloc(sizeof(vtx_index_entry_t));
    if (!entry) return NULL;
    memset(entry, 0, sizeof(*entry));

    entry->key = key;
    entry->dep_set.method_ids = (uint32_t *)malloc(
        VTX_DEP_SET_INITIAL_CAPACITY * sizeof(uint32_t));
    if (!entry->dep_set.method_ids) {
        free(entry);
        return NULL;
    }
    entry->dep_set.count = 0;
    entry->dep_set.capacity = VTX_DEP_SET_INITIAL_CAPACITY;

    /* Insert at head of bucket chain */
    entry->next = index->buckets[bucket];
    index->buckets[bucket] = entry;
    index->entry_count++;

    return entry;
}

/* ========================================================================== */
/* Add dependency                                                              */
/* ========================================================================== */

static int add_to_dep_set(vtx_dep_set_t *set, uint32_t method_id)
{
    /* Check if already present */
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->method_ids[i] == method_id) return 0;
    }

    /* Grow if needed */
    if (set->count >= set->capacity) {
        uint32_t new_cap = set->capacity * 2;
        uint32_t *new_ids = (uint32_t *)realloc(set->method_ids,
            new_cap * sizeof(uint32_t));
        if (!new_ids) return -1;
        set->method_ids = new_ids;
        set->capacity = new_cap;
    }

    set->method_ids[set->count++] = method_id;
    return 0;
}

int vtx_inverted_index_add(vtx_inverted_index_t *index,
                            uint32_t typeid_, uint32_t method_id)
{
    if (!index) return -1;

    vtx_index_entry_t *entry = find_entry(index, typeid_);
    if (!entry) {
        entry = create_entry(index, typeid_);
        if (!entry) return -1;
    }

    return add_to_dep_set(&entry->dep_set, method_id);
}

int vtx_inverted_index_add_shape(vtx_inverted_index_t *index,
                                  uint32_t shapeid, uint32_t method_id)
{
    /* Shape IDs use a different key space: offset by 0x80000000 */
    return vtx_inverted_index_add(index, shapeid | 0x80000000u, method_id);
}

/* ========================================================================== */
/* Remove method from all sets                                                 */
/* ========================================================================== */

static void remove_from_dep_set(vtx_dep_set_t *set, uint32_t method_id)
{
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->method_ids[i] == method_id) {
            /* Shift remaining entries down */
            for (uint32_t j = i; j < set->count - 1; j++) {
                set->method_ids[j] = set->method_ids[j + 1];
            }
            set->count--;
            return;
        }
    }
}

int vtx_inverted_index_remove_method(vtx_inverted_index_t *index,
                                      uint32_t method_id)
{
    if (!index) return -1;

    /* Walk all buckets and remove the method from any dep set it's in */
    for (uint32_t b = 0; b < VTX_INVERTED_INDEX_CAPACITY; b++) {
        vtx_index_entry_t *entry = index->buckets[b];
        while (entry) {
            remove_from_dep_set(&entry->dep_set, method_id);
            entry = entry->next;
        }
    }
    return 0;
}

/* ========================================================================== */
/* Lookup                                                                      */
/* ========================================================================== */

const vtx_dep_set_t *vtx_inverted_index_lookup(vtx_inverted_index_t *index,
                                                 uint32_t typeid_)
{
    if (!index) return NULL;
    vtx_index_entry_t *entry = find_entry(index, typeid_);
    if (!entry) return NULL;
    return &entry->dep_set;
}

/* ========================================================================== */
/* Invalidation                                                                */
/* ========================================================================== */

int vtx_invalidate_dependencies(uint32_t typeid_,
                                 vtx_code_cache_t *cache,
                                 vtx_method_registry_t *registry,
                                 vtx_inverted_index_t *index)
{
    if (!cache || !registry || !index) return -1;

    const vtx_dep_set_t *deps = vtx_inverted_index_lookup(index, typeid_);
    if (!deps || deps->count == 0) return 0;

    /* Make a copy of the method IDs to iterate over, since
     * invalidation will modify the dep set */
    uint32_t count = deps->count;
    uint32_t *method_ids = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!method_ids) return -1;
    memcpy(method_ids, deps->method_ids, count * sizeof(uint32_t));

    int invalidated = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t mid = method_ids[i];
        vtx_compiled_method_t *cm = vtx_method_registry_get(registry, mid);
        if (!cm || !cm->is_installed) continue;

        /* Mark as not compiled */
        cm->is_installed = false;
        cm->is_valid = false;

        /* Set the method's code pointer to NULL atomically */
        if (cm->method_desc) {
            __atomic_store_n(&cm->method_desc->bytecode, NULL, __ATOMIC_RELEASE);
        }

        /* Free the code in the cache */
        vtx_code_cache_free(cache, cm->code_start, cm->code_size);

        /* Free metadata */
        if (cm->side_table) {
            vtx_side_table_destroy(cm->side_table);
            cm->side_table = NULL;
        }
        if (cm->dep_type_ids) {
            free(cm->dep_type_ids);
            cm->dep_type_ids = NULL;
            cm->dep_type_count = 0;
        }
        if (cm->dep_shape_ids) {
            free(cm->dep_shape_ids);
            cm->dep_shape_ids = NULL;
            cm->dep_shape_count = 0;
        }

        /* Remove from registry */
        vtx_method_registry_remove(registry, mid);

        /* Free the compiled method struct */
        free(cm);

        invalidated++;
    }

    /* Clear the dep set for this typeid */
    vtx_index_entry_t *entry = find_entry(index, typeid_);
    if (entry) {
        entry->dep_set.count = 0;
    }

    free(method_ids);
    return invalidated;
}

int vtx_invalidate_shape(vtx_shapeid_t shapeid,
                          vtx_code_cache_t *cache,
                          vtx_method_registry_t *registry,
                          vtx_inverted_index_t *index)
{
    /* Shape IDs are stored with the high bit set */
    return vtx_invalidate_dependencies(shapeid | 0x80000000u,
                                        cache, registry, index);
}

/* ========================================================================== */
/* Register method dependencies                                                */
/* ========================================================================== */

int vtx_invalidate_register(vtx_inverted_index_t *index,
                             const vtx_compiled_method_t *method)
{
    if (!index || !method) return -1;

    /* Register TypeID dependencies */
    for (uint32_t i = 0; i < method->dep_type_count; i++) {
        if (vtx_inverted_index_add(index, method->dep_type_ids[i],
                                    method->method_id) != 0) {
            return -1;
        }
    }

    /* Register ShapeID dependencies */
    for (uint32_t i = 0; i < method->dep_shape_count; i++) {
        if (vtx_inverted_index_add_shape(index, method->dep_shape_ids[i],
                                          method->method_id) != 0) {
            return -1;
        }
    }

    return 0;
}
