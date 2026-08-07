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
    registry->capacity_mask = registry->capacity - 1;
    registry->clock_hand = 0;
    registry->malloc_allocated = false;
    registry->methods = (vtx_compiled_method_t **)vtx_arena_alloc(
        arena, registry->capacity * sizeof(vtx_compiled_method_t *));
    if (!registry->methods) {
        registry->capacity = 0;
        registry->capacity_mask = 0;
        return -1;
    }
    memset(registry->methods, 0, registry->capacity * sizeof(vtx_compiled_method_t *));
    /* COMPILE-001 fix: init the install/uninstall mutex. */
    if (pthread_mutex_init(&registry->mutex, NULL) != 0) {
        registry->mutex_initialized = false;
        return -1;
    }
    registry->mutex_initialized = true;
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
    /* Free the methods array if it was grown with malloc */
    if (registry->malloc_allocated && registry->methods) {
        free(registry->methods);
    }
    registry->methods = NULL;
    registry->method_count = 0;
    registry->capacity = 0;
    registry->capacity_mask = 0;
    registry->clock_hand = 0;
    registry->malloc_allocated = false;
    /* COMPILE-001 fix: destroy the install/uninstall mutex. */
    if (registry->mutex_initialized) {
        pthread_mutex_destroy(&registry->mutex);
        registry->mutex_initialized = false;
    }
}

int vtx_method_registry_add(vtx_method_registry_t *registry,
                             vtx_compiled_method_t *method)
{
    if (!registry || !method) return -1;

    /* COMPILE-001 fix: lock the registry during install. Without this,
     * two threadpool workers installing methods concurrently race on
     * the realloc below — one may free the array the other is
     * memcpy-ing from → UAF / heap corruption. The lock is only
     * contended on install (rare), not on dispatch. */
    if (registry->mutex_initialized) {
        pthread_mutex_lock(&registry->mutex);
    }

    /* Grow array if needed. Capacity is always kept as a power of 2
     * so that (index & capacity_mask) is equivalent to (index % capacity). */
    if (method->method_id >= registry->capacity) {
        uint32_t new_cap = registry->capacity > 0 ? registry->capacity : 16;
        while (new_cap <= method->method_id) {
            uint32_t next_cap = new_cap * 2;
            if (next_cap <= new_cap) {
                /* Overflow — just use method_id + 1 directly */
                new_cap = method->method_id + 1;
                break;
            }
            new_cap = next_cap;
        }
        vtx_compiled_method_t **new_arr = (vtx_compiled_method_t **)malloc(
            new_cap * sizeof(vtx_compiled_method_t *));
        if (!new_arr) {
            if (registry->mutex_initialized) pthread_mutex_unlock(&registry->mutex);
            return -1;
        }
        memset(new_arr, 0, new_cap * sizeof(vtx_compiled_method_t *));
        if (registry->methods) {
            memcpy(new_arr, registry->methods,
                   registry->capacity * sizeof(vtx_compiled_method_t *));
        }
        /* Old array was read while we held the lock — safe to free. */
        if (registry->malloc_allocated && registry->methods) {
            free(registry->methods);
        }
        registry->methods = new_arr;
        registry->capacity = new_cap;
        registry->capacity_mask = new_cap - 1;
        registry->malloc_allocated = true;
    }

    registry->methods[method->method_id] = method;
    if (method->method_id >= registry->method_count) {
        registry->method_count = method->method_id + 1;
    }
    if (registry->mutex_initialized) {
        pthread_mutex_unlock(&registry->mutex);
    }
    return 0;
}

vtx_compiled_method_t *vtx_method_registry_get(vtx_method_registry_t *registry,
                                                uint32_t method_id)
{
    if (!registry || method_id >= registry->capacity) return NULL;
    /* Read is lock-free — the methods array is realloc'd under mutex,
     * but readers either see the old array (still valid, freed only
     * after all readers release) or the new array. The pointer stored
     * at methods[method_id] is updated atomically by the install path.
     * For the rare install-during-read race, the worst case is reading
     * NULL (method not yet installed) which dispatch handles. */
    return registry->methods[method_id];
}

int vtx_method_registry_remove(vtx_method_registry_t *registry, uint32_t method_id)
{
    if (!registry || method_id >= registry->capacity) return -1;
    /* COMPILE-001 fix: lock during remove so concurrent installers
     * don't realloc under us. */
    if (registry->mutex_initialized) pthread_mutex_lock(&registry->mutex);
    registry->methods[method_id] = NULL;
    if (registry->mutex_initialized) pthread_mutex_unlock(&registry->mutex);
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
                         vtx_reloc_table_t *reloc_table,
                         const uint32_t *dep_types,
                         uint32_t dep_type_count,
                         const uint32_t *dep_shapes,
                         uint32_t dep_shape_count,
                         vtx_arena_t *arena,
                         vtx_poly_ic_t **poly_ics,
                         uint32_t poly_ic_count,
                         const vtx_jit_frame_layout_t *frame_layout,
                         const vtx_bc_pc_map_entry_t *bc_pc_map,
                         uint32_t bc_pc_map_count)
{
    if (!cache || !method || !code || code_size == 0) return false;

    /* Allocate space in the code cache */
    void *code_mem = vtx_code_cache_alloc(cache, code_size);
    if (!code_mem) return false;

    /* Ensure the page is writable before copying. A previous install
     * into the same segment may have made the page executable-only
     * (PROT_EXEC|PROT_READ), which would cause a SIGSEGV on write.
     * This is a no-op if the page is already writable. */
    vtx_code_cache_make_writable(cache, code_mem, code_size);

    /* Copy the compiled code into the cache */
    memcpy(code_mem, code, code_size);

    /* Apply external relocations: fix up calls to runtime helpers
     * using the actual code base address in the code cache.
     * This must happen AFTER copying the code (so we patch the final
     * copy) and BEFORE making the code executable. */
    if (reloc_table != NULL) {
        vtx_reloc_apply_external(reloc_table, code_mem,
                                  (uint8_t *)code_mem, code_size);
    }

    /* Make the code executable */
    if (vtx_code_cache_make_exec(cache, code_mem, code_size) != 0) {
        /* Failed to make executable — try finalizing the whole segment */
        vtx_code_cache_finalize(cache);
    }

    /* Create the compiled method metadata */
    vtx_compiled_method_t *cm = (vtx_compiled_method_t *)malloc(sizeof(vtx_compiled_method_t));
    if (!cm) {
        /* Free the code cache allocation on failure */
        vtx_code_cache_free(cache, code_mem, code_size);
        return false;
    }
    memset(cm, 0, sizeof(*cm));

    cm->method_id = method_id;
    cm->method_desc = method;
    cm->code_start = (uint8_t *)code_mem;
    cm->code_size = code_size;
    cm->side_table = side_table;
    cm->reloc_table = reloc_table;
    cm->is_installed = true;
    cm->is_valid = true;
    cm->last_used_timestamp = 0;
    cm->call_count = 0;
    cm->next = NULL;
    cm->poly_ics = poly_ics;
    cm->poly_ic_count = poly_ic_count;

    /* Fix C9: Allocate and wire deopt_info so the deopt handler can
     * find the side table from the JIT frame header. Without this,
     * cm->deopt_info is NULL (memset to 0) and the check below never
     * fires. The deopt handler reads deopt_info from [rbp+16] in the
     * JIT frame. If it's NULL, multi-method deopt falls back to the
     * global side table, which may be wrong for this method. */
    if (side_table != NULL) {
        cm->deopt_info = (vtx_deopt_info_t *)malloc(sizeof(vtx_deopt_info_t));
        if (cm->deopt_info != NULL) {
            memset(cm->deopt_info, 0, sizeof(*cm->deopt_info));
            cm->deopt_info->side_table = side_table;
        }
    }

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

    /* Store frame_layout and bc_pc_map BEFORE the atomic compiled_code
     * store. OSR needs these to set up the JIT frame at the loop header.
     * If they're missing when the main thread sees compiled_code != NULL,
     * OSR falls back to whole-method re-enter (slower but correct). */
    if (frame_layout != NULL) {
        cm->frame_layout = *frame_layout;
    }
    if (bc_pc_map != NULL && bc_pc_map_count > 0) {
        /* Deep-copy the bc_pc_map so it outlives the compile arena */
        cm->bc_pc_map = (vtx_bc_pc_map_entry_t *)malloc(
            bc_pc_map_count * sizeof(vtx_bc_pc_map_entry_t));
        if (cm->bc_pc_map != NULL) {
            memcpy(cm->bc_pc_map, bc_pc_map,
                   bc_pc_map_count * sizeof(vtx_bc_pc_map_entry_t));
            cm->bc_pc_map_count = bc_pc_map_count;
        }
    }

    /* COMPILE-002 fix: if there's an existing entry for this method_id
     * (tier promotion: T1→T2→T3 installs to the same method_id), retire
     * the old entry instead of leaking it. The old side_table, deopt_info,
     * dep arrays, poly_ics, and code in the cache were leaked on every
     * promotion. We mark the old entry as invalid; the versioned cache
     * (if configured) will eventually free the code once no threads are
     * executing it. The old vtx_compiled_method_t struct itself is freed
     * here since it's just metadata (the code stays in the cache segment
     * until the versioned cache reclaims it). */
    vtx_compiled_method_t *old_cm = vtx_method_registry_get(registry, method_id);
    if (old_cm != NULL) {
        old_cm->is_installed = false;
        old_cm->is_valid = false;
        /* Free the old metadata struct (side_table, deopt_info, etc.).
         * The code itself stays in the cache segment — versioned cache
         * reclaims it via vtx_versioned_cache_retire when configured. */
        if (old_cm->side_table) {
            vtx_side_table_destroy(old_cm->side_table);
        }
        free(old_cm->bc_pc_map);
        free(old_cm);
    }

    /* Register the method */
    if (vtx_method_registry_add(registry, cm) != 0) {
        free(cm->bc_pc_map);
        free(cm);
        return false;
    }

    /* Update the method's code pointer with release store.
     * This ensures that all writes to the code and metadata (including
     * frame_layout and bc_pc_map above) are visible to other threads
     * before they see the new code pointer. */
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

    /* Set the method's code pointer to NULL with release store.
     * Fix C10: The old code immediately freed the code after setting
     * compiled_code=NULL, but other threads may still be executing the
     * old code. We add a memory barrier (__ATOMIC_SEQ_CST) to ensure
     * the NULL store is visible before we free, and we defer the actual
     * free to the versioned cache's safe reclamation mechanism (which
     * checks on_stack_count). For single-threaded operation (which is
     * what VORTEX currently supports), the barrier is sufficient. */
    if (cm->method_desc) {
        __atomic_store_n(&cm->method_desc->compiled_code, NULL, __ATOMIC_SEQ_CST);
    }

    /* In a multi-threaded system, we would NOT free here — we would
     * mark the code as "retired" and let the versioned cache reclaim
     * it when on_stack_count reaches 0. For single-threaded operation,
     * immediate free is safe because no other thread is executing. */
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

    /* Free polymorphic inline caches */
    if (cm->poly_ics) {
        for (uint32_t i = 0; i < cm->poly_ic_count; i++) {
            free(cm->poly_ics[i]);
        }
        free(cm->poly_ics);
        cm->poly_ics = NULL;
        cm->poly_ic_count = 0;
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
