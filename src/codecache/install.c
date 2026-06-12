/**
 * VORTEX Method Installation
 *
 * Copies compiled native code into the code cache and updates the
 * method's code pointer atomically. The key invariant is:
 *
 *   After vtx_install_method returns, any thread calling the method
 *   will execute the newly compiled code.
 *
 * Atomicity is ensured by using __atomic_store_n with __ATOMIC_RELEASE
 * when updating the method's code pointer.
 */

#include "codecache/install.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Method registry                                                             */
/* ========================================================================== */

int vtx_method_registry_init(vtx_method_registry_t *registry, vtx_arena_t *arena)
{
    if (!registry) return -1;
    registry->method_count = 0;
    registry->capacity = VTX_METHOD_REGISTRY_INITIAL_CAPACITY;
    registry->methods = (vtx_compiled_method_t **)vtx_arena_alloc(
        arena, registry->capacity * sizeof(vtx_compiled_method_t *));
    if (!registry->methods) {
        registry->capacity = 0;
        return -1;
    }
    memset(registry->methods, 0, registry->capacity * sizeof(vtx_compiled_method_t *));
    return 0;
}

void vtx_method_registry_destroy(vtx_method_registry_t *registry)
{
    if (!registry) return;
    /* Free all compiled method metadata */
    for (uint32_t i = 0; i < registry->method_count; i++) {
        vtx_compiled_method_t *m = registry->methods[i];
        if (m) {
            if (m->side_table) {
                vtx_side_table_destroy(m->side_table);
            }
            free(m); /* Allocated with malloc below */
        }
    }
    /* The methods array is arena-allocated, no free needed */
    registry->methods = NULL;
    registry->method_count = 0;
    registry->capacity = 0;
}

int vtx_method_registry_add(vtx_method_registry_t *registry,
                             vtx_compiled_method_t *method)
{
    if (!registry || !method) return -1;

    /* Grow array if needed */
    if (method->method_id >= registry->capacity) {
        uint32_t new_cap = registry->capacity;
        while (new_cap <= method->method_id) new_cap *= 2;
        vtx_compiled_method_t **new_arr = (vtx_compiled_method_t **)malloc(
            new_cap * sizeof(vtx_compiled_method_t *));
        if (!new_arr) return -1;
        memset(new_arr, 0, new_cap * sizeof(vtx_compiled_method_t *));
        if (registry->methods) {
            memcpy(new_arr, registry->methods,
                   registry->capacity * sizeof(vtx_compiled_method_t *));
        }
        registry->methods = new_arr;
        registry->capacity = new_cap;
    }

    registry->methods[method->method_id] = method;
    if (method->method_id >= registry->method_count) {
        registry->method_count = method->method_id + 1;
    }
    return 0;
}

vtx_compiled_method_t *vtx_method_registry_get(vtx_method_registry_t *registry,
                                                uint32_t method_id)
{
    if (!registry || method_id >= registry->capacity) return NULL;
    return registry->methods[method_id];
}

int vtx_method_registry_remove(vtx_method_registry_t *registry, uint32_t method_id)
{
    if (!registry || method_id >= registry->capacity) return -1;
    registry->methods[method_id] = NULL;
    return 0;
}

/* ========================================================================== */
/* Installation                                                                */
/* ========================================================================== */

bool vtx_install_method(vtx_code_cache_t *cache,
                         vtx_method_registry_t *registry,
                         const vtx_method_desc_t *method,
                         uint32_t method_id,
                         const uint8_t *code,
                         uint32_t code_size,
                         vtx_side_table_t *side_table,
                         const uint32_t *dep_types,
                         uint32_t dep_type_count,
                         const uint32_t *dep_shapes,
                         uint32_t dep_shape_count,
                         vtx_arena_t *arena)
{
    if (!cache || !method || !code || code_size == 0) return false;

    /* Allocate space in the code cache */
    void *code_mem = vtx_code_cache_alloc(cache, code_size);
    if (!code_mem) return false;

    /* Copy the compiled code into the cache */
    memcpy(code_mem, code, code_size);

    /* Make the code executable */
    if (vtx_code_cache_make_exec(cache, code_mem, code_size) != 0) {
        /* Failed to make executable — try finalizing the whole segment */
        vtx_code_cache_finalize(cache);
    }

    /* Create the compiled method metadata */
    vtx_compiled_method_t *cm = (vtx_compiled_method_t *)malloc(sizeof(vtx_compiled_method_t));
    if (!cm) return false;
    memset(cm, 0, sizeof(*cm));

    cm->method_id = method_id;
    cm->method_desc = method;
    cm->code_start = (uint8_t *)code_mem;
    cm->code_size = code_size;
    cm->side_table = side_table;
    cm->is_installed = true;
    cm->is_valid = true;
    cm->last_used_timestamp = 0;
    cm->call_count = 0;
    cm->next = NULL;

    /* Copy dependency sets */
    if (dep_type_count > 0 && dep_types) {
        cm->dep_type_ids = (uint32_t *)malloc(dep_type_count * sizeof(uint32_t));
        if (cm->dep_type_ids) {
            memcpy(cm->dep_type_ids, dep_types, dep_type_count * sizeof(uint32_t));
            cm->dep_type_count = dep_type_count;
        }
    }
    if (dep_shape_count > 0 && dep_shapes) {
        cm->dep_shape_ids = (uint32_t *)malloc(dep_shape_count * sizeof(uint32_t));
        if (cm->dep_shape_ids) {
            memcpy(cm->dep_shape_ids, dep_shapes, dep_shape_count * sizeof(uint32_t));
            cm->dep_shape_count = dep_shape_count;
        }
    }

    /* Register the method */
    if (vtx_method_registry_add(registry, cm) != 0) {
        free(cm);
        return false;
    }

    /* Update the method's code pointer with release store.
     * This ensures that all writes to the code and metadata are
     * visible to other threads before they see the new code pointer. */
    __atomic_store_n(&method->compiled_code, code_mem, __ATOMIC_RELEASE);

    (void)arena;
    return true;
}

int vtx_uninstall_method(vtx_code_cache_t *cache,
                          vtx_method_registry_t *registry,
                          uint32_t method_id)
{
    if (!cache || !registry) return -1;

    vtx_compiled_method_t *cm = vtx_method_registry_get(registry, method_id);
    if (!cm) return -1;

    /* Mark as not installed */
    cm->is_installed = false;
    cm->is_valid = false;

    /* Set the method's code pointer to NULL with release store */
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
    }
    if (cm->dep_shape_ids) {
        free(cm->dep_shape_ids);
        cm->dep_shape_ids = NULL;
    }

    /* Remove from registry */
    vtx_method_registry_remove(registry, method_id);

    /* Free the compiled method struct */
    free(cm);

    return 0;
}

void *vtx_method_entry_point(const vtx_compiled_method_t *method)
{
    if (!method || !method->is_installed || !method->is_valid) return NULL;
    return (void *)method->code_start;
}
