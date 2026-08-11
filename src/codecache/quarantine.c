/* ============================================================================ *
 * AI-MODIFIED CODE
 *
 * This file was originally written by a human developer. It has been
 * substantially modified by an AI assistant (GLM/Z.ai) for bug fixes,
 * performance improvements, and feature additions.
 *
 * Original human-written structure is preserved; AI changes are marked
 * with bug fix IDs (B1-B28) or perf notes (Perf 1-10) in comments.
 *
 * If reviewing, please verify AI changes against the original logic.
 * ============================================================================ */

/**
 * VORTEX Code-Cache Quarantine — Implementation
 *
 * See quarantine.h for design rationale.
 *
 * The quarantine holds retired compiled-method metadata until a
 * safepoint confirms no thread is in JIT code. This fixes:
 *   - OSR-20: vtx_install_method freeing old_cm immediately, UAF if
 *             another thread is in vtx_osr_up.
 *   - OSR-21: vtx_invalidate_dependencies NULLing cm->side_table /
 *             cm->deopt_info / cm->bc_pc_map while other threads may
 *             read them.
 */

#include "codecache/quarantine.h"
#include "codecache/install.h"     /* vtx_compiled_method_t */
#include "deopt/side_table.h"      /* vtx_side_table_destroy */

#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Global quarantine                                                          */
/* ========================================================================== */

static vtx_codecache_quarantine_t *the_quarantine = NULL;

vtx_codecache_quarantine_t *vtx_codecache_get_quarantine(void)
{
    return the_quarantine;
}

void vtx_codecache_set_quarantine(vtx_codecache_quarantine_t *q)
{
    the_quarantine = q;
}

/* ========================================================================== */
/* Lifecycle                                                                  */
/* ========================================================================== */

int vtx_codecache_quarantine_init(vtx_codecache_quarantine_t *q)
{
    if (q == NULL) return -1;
    q->head = NULL;
    q->entry_count = 0;
    if (pthread_mutex_init(&q->mutex, NULL) != 0) {
        q->mutex_initialized = false;
        return -1;
    }
    q->mutex_initialized = true;
    return 0;
}

void vtx_codecache_quarantine_destroy(vtx_codecache_quarantine_t *q)
{
    if (q == NULL) return;
    /* At shutdown, drain everything immediately. The caller is
     * expected to have stopped all mutator threads. */
    if (q->mutex_initialized) {
        pthread_mutex_lock(&q->mutex);
    }
    vtx_codecache_quarantine_entry_t *e = q->head;
    while (e != NULL) {
        vtx_codecache_quarantine_entry_t *next = e->next;
        if (e->ptr != NULL && e->dtor != NULL) {
            e->dtor(e->ptr);
        }
        free(e);
        e = next;
    }
    q->head = NULL;
    q->entry_count = 0;
    if (q->mutex_initialized) {
        pthread_mutex_unlock(&q->mutex);
        pthread_mutex_destroy(&q->mutex);
        q->mutex_initialized = false;
    }
}

/* ========================================================================== */
/* Retire / drain                                                             */
/* ========================================================================== */

void vtx_codecache_quarantine_retire(vtx_codecache_quarantine_t *q,
                                       void *ptr,
                                       vtx_quarantine_dtor_fn dtor,
                                       const char *tag)
{
    if (q == NULL || ptr == NULL || dtor == NULL) return;
    vtx_codecache_quarantine_entry_t *e =
        (vtx_codecache_quarantine_entry_t *)malloc(sizeof(*e));
    if (e == NULL) {
        /* OOM — fall back to immediate free. This is the pre-fix
         * behavior (still a UAF risk, but no worse than before).
         * In practice the quarantine is small (a few entries per
         * recompilation), so this should never happen. */
        dtor(ptr);
        return;
    }
    e->ptr = ptr;
    e->dtor = dtor;
    e->tag = tag;
    e->next = NULL;

    if (q->mutex_initialized) pthread_mutex_lock(&q->mutex);
    e->next = q->head;
    q->head = e;
    q->entry_count++;
    if (q->mutex_initialized) pthread_mutex_unlock(&q->mutex);
}

uint32_t vtx_codecache_quarantine_drain(vtx_codecache_quarantine_t *q)
{
    if (q == NULL) return 0;
    /* Splice the list out atomically, then free each entry without
     * holding the mutex (so the destructor can call back into the
     * code cache without recursive-locking). */
    if (q->mutex_initialized) pthread_mutex_lock(&q->mutex);
    vtx_codecache_quarantine_entry_t *e = q->head;
    q->head = NULL;
    uint32_t drained = q->entry_count;
    q->entry_count = 0;
    if (q->mutex_initialized) pthread_mutex_unlock(&q->mutex);

    while (e != NULL) {
        vtx_codecache_quarantine_entry_t *next = e->next;
        if (e->ptr != NULL && e->dtor != NULL) {
            e->dtor(e->ptr);
        }
        free(e);
        e = next;
    }
    return drained;
}

uint32_t vtx_codecache_quarantine_count(const vtx_codecache_quarantine_t *q)
{
    if (q == NULL) return 0;
    /* Read without the lock — entry_count is updated under the mutex
     * but we only need an approximate count for diagnostics. */
    return q->entry_count;
}

/* OSR-20/21: adapter that drains the global quarantine. Registered
 * with vtx_gc_set_quarantine_drain_callback so the GC's safepoint
 * can free retired code-cache metadata without a hard library
 * dependency from vortex_runtime → vortex_codecache. */
void vtx_codecache_quarantine_drain_global(void)
{
    vtx_codecache_quarantine_t *q = vtx_codecache_get_quarantine();
    if (q != NULL) {
        (void)vtx_codecache_quarantine_drain(q);
    }
}

/* ========================================================================== */
/* Common destructors                                                         */
/* ========================================================================== */

/**
 * Destroy a compiled_method_t: frees all its metadata and the cm
 * struct itself. The cm's code was already freed by the caller (or
 * is owned by the versioned cache), so we only free the metadata.
 *
 * For OSR-20, the old_cm passed to retire has already had its code
 * freed by vtx_code_cache_free (the install path calls it). Here we
 * free the rest: side_table, deopt_info, bc_pc_map, poly_ics,
 * dep_type_ids, dep_shape_ids, reloc_table, and finally the cm. */
void vtx_codecache_destroy_compiled_method(void *cm_ptr)
{
    if (cm_ptr == NULL) return;
    vtx_compiled_method_t *cm = (vtx_compiled_method_t *)cm_ptr;

    /* side_table */
    if (cm->side_table != NULL) {
        vtx_side_table_destroy(cm->side_table);
        cm->side_table = NULL;
    }
    /* deopt_info (allocated in vtx_install_method via malloc) */
    if (cm->deopt_info != NULL) {
        free(cm->deopt_info);
        cm->deopt_info = NULL;
    }
    /* bc_pc_map (allocated in vtx_install_method via malloc) */
    if (cm->bc_pc_map != NULL) {
        free(cm->bc_pc_map);
        cm->bc_pc_map = NULL;
    }
    /* dep arrays */
    if (cm->dep_type_ids != NULL) {
        free(cm->dep_type_ids);
        cm->dep_type_ids = NULL;
    }
    if (cm->dep_shape_ids != NULL) {
        free(cm->dep_shape_ids);
        cm->dep_shape_ids = NULL;
    }
    /* poly_ics: each IC pointer is owned by the cm, then the array */
    if (cm->poly_ics != NULL) {
        for (uint32_t i = 0; i < cm->poly_ic_count; i++) {
            free(cm->poly_ics[i]);
        }
        free(cm->poly_ics);
        cm->poly_ics = NULL;
        cm->poly_ic_count = 0;
    }
    /* reloc_table — declared as a pointer but currently never freed
     * in the existing code. Leave as-is (don't free) to match the
     * pre-fix behavior; a future patch can free it once ownership
     * is clear. */

    free(cm);
}

/* Pair used by the poly_ics retire path (OSR-21): captures
 * (poly_ics_array, poly_ic_count) so the destructor can free each IC. */
typedef struct {
    void   **poly_ics;
    uint32_t poly_ic_count;
} vtx_poly_ics_pair_t;

void vtx_codecache_destroy_poly_ics(void *pair_ptr)
{
    if (pair_ptr == NULL) return;
    vtx_poly_ics_pair_t *p = (vtx_poly_ics_pair_t *)pair_ptr;
    if (p->poly_ics != NULL) {
        for (uint32_t i = 0; i < p->poly_ic_count; i++) {
            free(p->poly_ics[i]);
        }
        free(p->poly_ics);
    }
    free(p);
}

void vtx_codecache_destroy_side_table(void *st_ptr)
{
    if (st_ptr == NULL) return;
    vtx_side_table_destroy((vtx_side_table_t *)st_ptr);
}

void vtx_codecache_destroy_deopt_info(void *di_ptr)
{
    /* deopt_info is a plain malloc'd struct (no internal sub-allocations
     * that need destruction beyond free). */
    free(di_ptr);
}

void vtx_codecache_destroy_bc_pc_map(void *map_ptr)
{
    free(map_ptr);
}
