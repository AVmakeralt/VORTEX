/*
 * VORTEX Deopt Coordinator — integration layer for rate_limit.c
 *
 * See coordinator.h for design and audit rationale.
 */

#include "coordinator.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SITE_INDEX(site_id) ((site_id) % VTX_DEOPT_COORD_MAX_SITES)

int vtx_deopt_coord_init(vtx_deopt_coord_t *coord,
                          vtx_deopt_recompile_fn recompile_fn,
                          void *user_data)
{
    if (coord == NULL) return -1;
    memset(coord, 0, sizeof(*coord));

    /* Per-site limiters are lazily initialized on first use. */
    for (uint32_t i = 0; i < VTX_DEOPT_COORD_MAX_SITES; i++) {
        coord->sites_inited[i] = false;
    }

    /* Init batcher with capacity for 256 pending deopts (4x BATCH_MAX_PENDING
     * to give headroom for bursts). */
    if (vtx_deopt_batcher_init(&coord->batcher, 256) != 0) return -1;

    /* Init global budget with capacity for 50000 deopts in the window
     * (5x GLOBAL_FAIL_THRESHOLD for headroom). */
    if (vtx_deopt_budget_init(&coord->budget, 5 * VTX_DEOPT_GLOBAL_FAIL_THRESHOLD) != 0) {
        vtx_deopt_batcher_destroy(&coord->batcher);
        return -1;
    }

    coord->recompile_fn = recompile_fn;
    coord->recompile_user_data = user_data;
    return 0;
}

void vtx_deopt_coord_destroy(vtx_deopt_coord_t *coord)
{
    if (coord == NULL) return;
    for (uint32_t i = 0; i < VTX_DEOPT_COORD_MAX_SITES; i++) {
        if (coord->sites_inited[i]) {
            vtx_deopt_site_destroy(&coord->sites[i]);
            coord->sites_inited[i] = false;
        }
    }
    vtx_deopt_batcher_destroy(&coord->batcher);
    vtx_deopt_budget_destroy(&coord->budget);
}

/* Lazily initialize a per-site limiter if not yet initialized. */
static vtx_deopt_site_limiter_t *get_site_limiter(vtx_deopt_coord_t *coord,
                                                    uint32_t site_id)
{
    uint32_t idx = SITE_INDEX(site_id);
    if (!coord->sites_inited[idx]) {
        /* Check if this slot was previously used by a different site_id.
         * If so, destroy the old limiter before re-initializing. This is
         * the "last-writer-wins" collision policy. */
        if (vtx_deopt_site_init(&coord->sites[idx], 256) != 0) {
            return NULL;
        }
        coord->sites_inited[idx] = true;
    }
    return &coord->sites[idx];
}

vtx_deopt_decision_t vtx_deopt_coord_on_guard_fail(
    vtx_deopt_coord_t *coord, uint32_t site_id, uint64_t now_ns)
{
    if (coord == NULL) return DEOPT_COORD_FULL_DEOPT;

    coord->total_guard_failures++;

    /* Step 1: Record in global budget. If suppressed, return immediately. */
    bool budget_exceeded = vtx_deopt_budget_record(&coord->budget, now_ns);
    if (budget_exceeded) {
        coord->total_suppressed++;
        return DEOPT_COORD_SUPPRESSED;
    }

    /* Step 2: Record in per-site limiter. If poisoned, fall back to
     * interpreter permanently for this site. */
    vtx_deopt_site_limiter_t *sl = get_site_limiter(coord, site_id);
    if (sl != NULL) {
        bool poisoned = vtx_deopt_site_record_failure(sl, now_ns);
        if (poisoned) {
            coord->total_poisoned++;
            return DEOPT_COORD_POISONED;
        }
    }

    /* Step 3: Add to batcher. If the batcher signals flush, trigger
     * recompilation via the callback. */
    bool should_flush = vtx_deopt_batcher_add(&coord->batcher, site_id, now_ns);
    if (should_flush) {
        uint32_t sites[VTX_DEOPT_BATCH_MAX_PENDING];
        uint32_t n = vtx_deopt_batcher_flush(&coord->batcher, sites,
                                              VTX_DEOPT_BATCH_MAX_PENDING, now_ns);
        if (n > 0 && coord->recompile_fn != NULL) {
            coord->recompile_fn(sites, n, coord->recompile_user_data);
        }
        coord->total_batches_flushed++;
    }

    /* Step 4: Default decision — try deoptless first.
     * The caller can check vtx_deoptless_can_deoptless() to decide whether
     * to actually use the deoptless path or fall back to full deopt. */
    coord->total_deoptless++;
    return DEOPT_COORD_DEOPTLESS;
}

bool vtx_deopt_coord_is_poisoned(const vtx_deopt_coord_t *coord, uint32_t site_id)
{
    if (coord == NULL) return false;
    uint32_t idx = SITE_INDEX(site_id);
    if (!coord->sites_inited[idx]) return false;
    return vtx_deopt_site_is_poisoned(&coord->sites[idx]);
}

bool vtx_deopt_coord_is_suppressed(const vtx_deopt_coord_t *coord, uint64_t now_ns)
{
    if (coord == NULL) return false;
    return vtx_deopt_budget_is_suppressed(&coord->budget, now_ns);
}

uint32_t vtx_deopt_coord_flush(vtx_deopt_coord_t *coord, uint64_t now_ns)
{
    if (coord == NULL) return 0;
    /* Force flush: ignore the should_flush check. The caller (e.g. a
     * safepoint handler) wants to flush NOW, regardless of window state. */
    if (coord->batcher.count == 0) return 0;
    uint32_t sites[VTX_DEOPT_BATCH_MAX_PENDING];
    uint32_t n = vtx_deopt_batcher_flush(&coord->batcher, sites,
                                          VTX_DEOPT_BATCH_MAX_PENDING, now_ns);
    if (n > 0 && coord->recompile_fn != NULL) {
        coord->recompile_fn(sites, n, coord->recompile_user_data);
    }
    coord->total_batches_flushed++;
    return n;
}

void vtx_deopt_coord_stats_str(const vtx_deopt_coord_t *coord, char *buf, size_t bufsize)
{
    if (coord == NULL || buf == NULL || bufsize == 0) return;
    snprintf(buf, bufsize,
        "deopt_coord: guard_fails=%llu poisoned=%llu deoptless=%llu "
        "full_deopts=%llu suppressed=%llu batches_flushed=%llu",
        (unsigned long long)coord->total_guard_failures,
        (unsigned long long)coord->total_poisoned,
        (unsigned long long)coord->total_deoptless,
        (unsigned long long)coord->total_full_deopts,
        (unsigned long long)coord->total_suppressed,
        (unsigned long long)coord->total_batches_flushed);
}
