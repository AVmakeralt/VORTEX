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

static uint32_t index_hash(uint64_t key)
{
    /* FNV-1a hash for 64-bit key */
    uint32_t h = 2166136261u;
    h ^= (uint32_t)(key & 0xFF);
    h *= 16777619u;
    h ^= (uint32_t)((key >> 8) & 0xFF);
    h *= 16777619u;
    h ^= (uint32_t)((key >> 16) & 0xFF);
    h *= 16777619u;
    h ^= (uint32_t)((key >> 24) & 0xFF);
    h *= 16777619u;
    h ^= (uint32_t)((key >> 32) & 0xFF);
    h *= 16777619u;
    h ^= (uint32_t)((key >> 40) & 0xFF);
    h *= 16777619u;
    h ^= (uint32_t)((key >> 48) & 0xFF);
    h *= 16777619u;
    h ^= (uint32_t)((key >> 56) & 0xFF);
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

static vtx_index_entry_t *find_entry(vtx_inverted_index_t *index, uint64_t key)
{
    uint32_t bucket = index_hash(key);
    vtx_index_entry_t *entry = index->buckets[bucket];
    while (entry) {
        if (entry->key == key) return entry;
        entry = entry->next;
    }
    return NULL;
}

static vtx_index_entry_t *create_entry(vtx_inverted_index_t *index, uint64_t key)
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
                            uint64_t typeid_, uint32_t method_id)
{
    if (!index) return -1;

    vtx_index_entry_t *entry = find_entry(index, typeid_);
    if (!entry) {
        entry = create_entry(index, typeid_);
        if (!entry) return -1;
    }

    return add_to_dep_set(&entry->dep_set, method_id);
}

#define VTX_SHAPE_KEY_OFFSET 0x40000000u

int vtx_inverted_index_add_shape(vtx_inverted_index_t *index,
                                  uint32_t shapeid, uint32_t method_id)
{
    /* Shape IDs use a different key space: offset by VTX_SHAPE_KEY_OFFSET
     * to avoid collision with TypeIDs. Use 64-bit arithmetic to prevent
     * overflow when shapeid >= 0xC0000000u. */
    return vtx_inverted_index_add(index, (uint64_t)shapeid + VTX_SHAPE_KEY_OFFSET, method_id);
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
                                                 uint64_t typeid_)
{
    if (!index) return NULL;
    vtx_index_entry_t *entry = find_entry(index, typeid_);
    if (!entry) return NULL;
    return &entry->dep_set;
}

/* ========================================================================== */
/* Invalidation                                                                */
/* ========================================================================== */

int vtx_invalidate_dependencies(uint64_t typeid_,
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
            __atomic_store_n(&cm->method_desc->compiled_code, NULL, __ATOMIC_RELEASE);
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
    /* Shape IDs are stored with VTX_SHAPE_KEY_OFFSET to avoid TypeID collision.
     * Use 64-bit arithmetic to prevent overflow. */
    return vtx_invalidate_dependencies((uint64_t)shapeid + VTX_SHAPE_KEY_OFFSET,
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

/* ========================================================================== */
/* Guard-level dependency index (Proposal #4)                                    */
/* ========================================================================== */

int vtx_guard_dep_index_init(vtx_guard_dep_index_t *index, vtx_arena_t *arena)
{
    if (!index) return -1;
    memset(index->buckets, 0, sizeof(index->buckets));
    index->entry_count = 0;
    index->arena = arena;
    return 0;
}

void vtx_guard_dep_index_destroy(vtx_guard_dep_index_t *index)
{
    if (!index) return;

    for (uint32_t b = 0; b < VTX_INVERTED_INDEX_CAPACITY; b++) {
        vtx_guard_dep_entry_t *entry = index->buckets[b];
        while (entry) {
            vtx_guard_dep_entry_t *next = entry->next;
            if (entry->deps) free(entry->deps);
            free(entry);
            entry = next;
        }
        index->buckets[b] = NULL;
    }
    index->entry_count = 0;
}

static vtx_guard_dep_entry_t *find_guard_dep_entry(vtx_guard_dep_index_t *index,
                                                      uint32_t type_id)
{
    uint32_t bucket = index_hash(type_id);  /* reuse the same hash function */
    vtx_guard_dep_entry_t *entry = index->buckets[bucket];
    while (entry) {
        if (entry->type_id == type_id) return entry;
        entry = entry->next;
    }
    return NULL;
}

static vtx_guard_dep_entry_t *create_guard_dep_entry(vtx_guard_dep_index_t *index,
                                                        uint32_t type_id)
{
    uint32_t bucket = index_hash(type_id);

    vtx_guard_dep_entry_t *entry = (vtx_guard_dep_entry_t *)malloc(
        sizeof(vtx_guard_dep_entry_t));
    if (!entry) return NULL;
    memset(entry, 0, sizeof(*entry));

    entry->type_id = type_id;
    entry->deps = (vtx_guard_dep_t *)malloc(
        VTX_GUARD_DEP_INITIAL_CAPACITY * sizeof(vtx_guard_dep_t));
    if (!entry->deps) {
        free(entry);
        return NULL;
    }
    entry->dep_count = 0;
    entry->dep_capacity = VTX_GUARD_DEP_INITIAL_CAPACITY;

    /* Insert at head of bucket chain */
    entry->next = index->buckets[bucket];
    index->buckets[bucket] = entry;
    index->entry_count++;

    return entry;
}

int vtx_guard_dep_index_add(vtx_guard_dep_index_t *index,
                              uint32_t type_id,
                              uint32_t guard_id,
                              uint32_t method_id,
                              uint32_t guard_branch_offset,
                              uint8_t *code_start)
{
    if (!index) return -1;

    vtx_guard_dep_entry_t *entry = find_guard_dep_entry(index, type_id);
    if (!entry) {
        entry = create_guard_dep_entry(index, type_id);
        if (!entry) return -1;
    }

    /* Check for duplicate */
    for (uint32_t i = 0; i < entry->dep_count; i++) {
        if (entry->deps[i].guard_id == guard_id &&
            entry->deps[i].method_id == method_id) {
            return 0; /* already recorded */
        }
    }

    /* Grow array if needed */
    if (entry->dep_count >= entry->dep_capacity) {
        uint32_t new_cap = entry->dep_capacity * 2;
        vtx_guard_dep_t *new_deps = (vtx_guard_dep_t *)realloc(
            entry->deps, new_cap * sizeof(vtx_guard_dep_t));
        if (!new_deps) return -1;
        entry->deps = new_deps;
        entry->dep_capacity = new_cap;
    }

    /* Add the guard dependency */
    vtx_guard_dep_t *dep = &entry->deps[entry->dep_count++];
    dep->type_id = type_id;
    dep->guard_id = guard_id;
    dep->method_id = method_id;
    dep->guard_branch_offset = guard_branch_offset;
    dep->code_start = code_start;

    return 0;
}

const vtx_guard_dep_t *vtx_guard_dep_index_lookup(vtx_guard_dep_index_t *index,
                                                     uint32_t type_id,
                                                     uint32_t *out_count)
{
    if (!index || !out_count) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    vtx_guard_dep_entry_t *entry = find_guard_dep_entry(index, type_id);
    if (!entry) {
        *out_count = 0;
        return NULL;
    }

    *out_count = entry->dep_count;
    return entry->deps;
}

/* ========================================================================== */
/* Fine-grained invalidation (Proposal #4)                                       */
/* ========================================================================== */

int vtx_invalidate_guard_fine_grained(uint32_t typeid_,
                                        vtx_guard_dep_index_t *index,
                                        vtx_code_cache_t *cache)
{
    if (!index || !cache) return -1;

    uint32_t dep_count = 0;
    const vtx_guard_dep_t *deps = vtx_guard_dep_index_lookup(index, typeid_, &dep_count);
    if (!deps || dep_count == 0) return 0;

    int patched = 0;

    for (uint32_t i = 0; i < dep_count; i++) {
        const vtx_guard_dep_t *dep = &deps[i];

        if (!dep->code_start || dep->guard_branch_offset == 0) continue;

        /* Bug #7 fix: Actually patch the guard's JCC instruction in the
         * compiled code to unconditionally jump (deopt).
         *
         * On x86-64, a near JCC has format: 0F 8x [4-byte rel32] (6 bytes)
         * We patch it to: E9 [4-byte rel32] 90 (JMP + NOP, also 6 bytes)
         *
         * The JMP displacement needs +1 adjustment relative to the JCC
         * displacement because JMP is 5 bytes (displacement relative to
         * JMP end) while JCC is 6 bytes (displacement relative to JCC end).
         * So: new_rel32 = old_rel32 + 1
         *
         * This makes the guard always jump to the deopt path, effectively
         * invalidating it without requiring whole-method deoptimization. */

        uint8_t *jcc_addr = dep->code_start + dep->guard_branch_offset;

        /* Verify this looks like a near JCC (0F 8x) before patching */
        if (jcc_addr[0] != 0x0F || (jcc_addr[1] & 0xF0) != 0x80) {
            /* Not a near JCC — skip this guard to avoid corrupting code */
            continue;
        }

        /* Read the existing 4-byte rel32 displacement (little-endian) */
        int32_t old_rel32;
        memcpy(&old_rel32, jcc_addr + 2, sizeof(int32_t));

        /* Adjust displacement: JMP is 5 bytes, JCC is 6 bytes.
         * The displacement is relative to the end of the instruction,
         * so we add 1 to compensate for the shorter instruction. */
        int32_t new_rel32 = old_rel32 + 1;

        /* Write the patched instruction: E9 [rel32] 90 */
        jcc_addr[0] = 0xE9;  /* near JMP rel32 */
        memcpy(jcc_addr + 1, &new_rel32, sizeof(int32_t));
        jcc_addr[5] = 0x90;  /* NOP padding */

        patched++;
    }

    return patched;
}
