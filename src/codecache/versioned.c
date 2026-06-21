/*
 * VORTEX Versioned Code Cache implementation.
 *
 * See versioned.h for design and audit rationale.
 *
 * N+1 versioning: at any time, a method has at most 1 active version
 * plus up to MAX_RETIRED retired versions. Retired versions are kept
 * alive until no thread is executing them (on_stack_count == 0).
 */

#include "versioned.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define METHOD_INDEX(method_id) ((method_id) % VTX_VERSIONED_CACHE_MAX_METHODS)

int vtx_versioned_cache_init(vtx_versioned_cache_t *vc, vtx_code_cache_t *cache)
{
    if (vc == NULL || cache == NULL) return -1;
    memset(vc, 0, sizeof(*vc));
    vc->cache = cache;
    return 0;
}

void vtx_versioned_cache_destroy(vtx_versioned_cache_t *vc)
{
    if (vc == NULL) return;
    for (uint32_t i = 0; i < VTX_VERSIONED_CACHE_MAX_METHODS; i++) {
        vtx_code_version_t *v = vc->versions[i];
        while (v != NULL) {
            vtx_code_version_t *next = v->next;
            /* Note: we don't free code_ptr here — the underlying cache
             * owns the memory and will free it on its own destroy. */
            free(v);
            v = next;
        }
        vc->versions[i] = NULL;
    }
}

/* Find the version list for a method. Returns the head, or NULL if none. */
static vtx_code_version_t *find_version_list(vtx_versioned_cache_t *vc, uint32_t method_id)
{
    return vc->versions[METHOD_INDEX(method_id)];
}

/* Force-free the oldest retired version for a method (used when we exceed
 * MAX_RETIRED). This is potentially unsafe but better than unbounded growth. */
static void force_free_oldest_retired(vtx_versioned_cache_t *vc, uint32_t method_id)
{
    uint32_t idx = METHOD_INDEX(method_id);
    vtx_code_version_t *oldest = NULL;
    vtx_code_version_t **oldest_ptr = NULL;
    vtx_code_version_t **prev_ptr = &vc->versions[idx];
    vtx_code_version_t *v = vc->versions[idx];
    while (v != NULL) {
        if (v->is_retired) {
            if (oldest == NULL || v->retire_time_ns < oldest->retire_time_ns) {
                oldest = v;
                oldest_ptr = prev_ptr;
            }
        }
        prev_ptr = &v->next;
        v = v->next;
    }
    if (oldest != NULL) {
        *oldest_ptr = oldest->next;
        if (oldest->on_stack_count > 0) {
            vc->total_force_frees++;
        }
        vc->total_retired--;
        /* Free the code from the underlying cache. */
        if (oldest->code_ptr != NULL) {
            vtx_code_cache_free(vc->cache, oldest->code_ptr, oldest->code_size);
        }
        free(oldest);
    }
}

uint32_t vtx_versioned_cache_install(vtx_versioned_cache_t *vc,
                                      uint32_t method_id,
                                      void *code_ptr, uint32_t code_size)
{
    if (vc == NULL) return 0;
    uint32_t idx = METHOD_INDEX(method_id);

    /* Retire the current active version (if any). */
    vtx_code_version_t *old_active = NULL;
    for (vtx_code_version_t *v = vc->versions[idx]; v != NULL; v = v->next) {
        if (v->is_active) {
            old_active = v;
            break;
        }
    }
    if (old_active != NULL) {
        old_active->is_active = false;
        old_active->is_retired = true;
        /* retire_time_ns = 0 means "unknown time"; caller can patch later.
         * In production, pass now_ns via a separate API. */
        old_active->retire_time_ns = 1;  /* non-zero marker */
        vc->total_active--;
        vc->total_retired++;
    }

    /* Count retired versions; if too many, force-free the oldest. */
    uint32_t retired_count = 0;
    for (vtx_code_version_t *v = vc->versions[idx]; v != NULL; v = v->next) {
        if (v->is_retired) retired_count++;
    }
    while (retired_count > VTX_VERSIONED_CACHE_MAX_RETIRED) {
        force_free_oldest_retired(vc, method_id);
        retired_count--;
    }

    /* Create the new active version. */
    vtx_code_version_t *new_v = (vtx_code_version_t *)calloc(1, sizeof(vtx_code_version_t));
    if (new_v == NULL) return 0;
    new_v->method_id = method_id;
    new_v->version_number = ++vc->next_version_number[idx];
    new_v->code_ptr = code_ptr;
    new_v->code_size = code_size;
    new_v->is_active = true;
    new_v->is_retired = false;
    new_v->on_stack_count = 0;
    new_v->retire_time_ns = 0;
    new_v->next = vc->versions[idx];
    vc->versions[idx] = new_v;
    vc->total_active++;

    return new_v->version_number;
}

vtx_code_version_t *vtx_versioned_cache_get_active(
    const vtx_versioned_cache_t *vc, uint32_t method_id)
{
    if (vc == NULL) return NULL;
    uint32_t idx = METHOD_INDEX(method_id);
    for (vtx_code_version_t *v = vc->versions[idx]; v != NULL; v = v->next) {
        if (v->is_active) return v;
    }
    return NULL;
}

void vtx_versioned_cache_on_enter(vtx_versioned_cache_t *vc, uint32_t method_id)
{
    if (vc == NULL) return;
    vtx_code_version_t *active = vtx_versioned_cache_get_active(vc, method_id);
    if (active != NULL) {
        active->on_stack_count++;
    }
}

void vtx_versioned_cache_on_exit(vtx_versioned_cache_t *vc, uint32_t method_id)
{
    if (vc == NULL) return;
    uint32_t idx = METHOD_INDEX(method_id);
    /* Decrement on_stack_count for ALL versions of this method that have
     * a positive count. In practice, only one version should have a
     * positive count (the one being exited), but if there's a race,
     * decrementing all positive ones is safe. */
    for (vtx_code_version_t *v = vc->versions[idx]; v != NULL; v = v->next) {
        if (v->on_stack_count > 0) {
            v->on_stack_count--;
        }
    }
}

int vtx_versioned_cache_patch(vtx_versioned_cache_t *vc, uint32_t method_id,
                               uint32_t patch_offset, const uint8_t *new_bytes,
                               uint32_t len)
{
    if (vc == NULL || new_bytes == NULL) return -1;
    vtx_code_version_t *active = vtx_versioned_cache_get_active(vc, method_id);
    if (active == NULL || active->code_ptr == NULL) return -1;
    if (patch_offset + len > active->code_size) return -1;

    uint8_t *patch_addr = (uint8_t *)active->code_ptr + patch_offset;

    /* Make the range writable. */
    if (vtx_code_cache_make_writable(vc->cache, patch_addr, len) != 0) return -1;

    /* Apply the patch. */
    memcpy(patch_addr, new_bytes, len);

    /* Make the range executable again. */
    if (vtx_code_cache_make_exec(vc->cache, patch_addr, len) != 0) return -1;

    return 0;
}

uint32_t vtx_versioned_cache_reclaim(vtx_versioned_cache_t *vc)
{
    if (vc == NULL) return 0;
    uint32_t reclaimed = 0;
    for (uint32_t i = 0; i < VTX_VERSIONED_CACHE_MAX_METHODS; i++) {
        vtx_code_version_t **prev_ptr = &vc->versions[i];
        vtx_code_version_t *v = vc->versions[i];
        while (v != NULL) {
            if (v->is_retired && v->on_stack_count == 0) {
                /* Safe to reclaim. */
                *prev_ptr = v->next;
                if (v->code_ptr != NULL) {
                    vtx_code_cache_free(vc->cache, v->code_ptr, v->code_size);
                }
                free(v);
                vc->total_retired--;
                reclaimed++;
                v = *prev_ptr;
            } else {
                prev_ptr = &v->next;
                v = v->next;
            }
        }
    }
    return reclaimed;
}

uint64_t vtx_versioned_cache_compact(vtx_versioned_cache_t *vc)
{
    if (vc == NULL) return 0;
    /* Reclaim first, then report. The underlying cache handles segment
     * freeing when segments become empty. */
    uint32_t reclaimed = vtx_versioned_cache_reclaim(vc);
    vc->total_compactions++;
    /* We don't have a direct way to report bytes freed from the underlying
     * cache, so we return the count of versions reclaimed. */
    return (uint64_t)reclaimed;
}

void vtx_versioned_cache_stats_str(const vtx_versioned_cache_t *vc,
                                    char *buf, size_t bufsize)
{
    if (vc == NULL || buf == NULL || bufsize == 0) return;
    snprintf(buf, bufsize,
        "versioned_cache: active=%u retired=%u compactions=%llu force_frees=%llu",
        vc->total_active, vc->total_retired,
        (unsigned long long)vc->total_compactions,
        (unsigned long long)vc->total_force_frees);
}
