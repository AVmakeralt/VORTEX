/*
 * VORTEX Deopt Rate Limiter implementation.
 *
 * See rate_limit.h for design and audit rationale.
 *
 * All operations are O(1) amortized. The sliding window is implemented
 * as a circular buffer of timestamps; on each insert, we first evict
 * expired entries from the head, then insert at the tail.
 *
 * Thread-safety: callers must hold an external lock if they share these
 * structures across threads. For the global budget, the compile thread
 * typically owns it; for per-site limiters, the executing thread owns
 * them. A future version may use atomics for lock-free operation.
 */

#include "rate_limit.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/* Per-site rate limiter                                                       */
/* ========================================================================== */

int vtx_deopt_site_init(vtx_deopt_site_limiter_t *sl, uint32_t capacity)
{
    if (sl == NULL || capacity == 0) return -1;
    sl->failure_times_ns = (uint64_t *)calloc(capacity, sizeof(uint64_t));
    if (sl->failure_times_ns == NULL) return -1;
    sl->capacity = capacity;
    sl->head = 0;
    sl->count = 0;
    sl->poisoned = false;
    sl->total_failures = 0;
    sl->first_failure_ns = 0;
    return 0;
}

void vtx_deopt_site_destroy(vtx_deopt_site_limiter_t *sl)
{
    if (sl == NULL) return;
    free(sl->failure_times_ns);
    sl->failure_times_ns = NULL;
    sl->capacity = 0;
    sl->count = 0;
}

/* Evict expired entries (those older than now - WINDOW_NS).
 *
 * An entry is "expired" if its timestamp is strictly less than the cutoff
 * (i.e. the entry is older than the window). Entries with timestamp
 * exactly == cutoff are kept (they're at the edge of the window). */
static void site_evict_expired(vtx_deopt_site_limiter_t *sl, uint64_t now_ns)
{
    if (sl->count == 0) return;
    uint64_t cutoff = (now_ns > VTX_DEOPT_SITE_WINDOW_NS) ?
                        (now_ns - VTX_DEOPT_SITE_WINDOW_NS) : 0;
    while (sl->count > 0) {
        uint64_t oldest = sl->failure_times_ns[sl->head];
        if (oldest >= cutoff) break;  /* still within window */
        /* Evict this entry */
        sl->head = (sl->head + 1) % sl->capacity;
        sl->count--;
    }
}

bool vtx_deopt_site_record_failure(vtx_deopt_site_limiter_t *sl, uint64_t now_ns)
{
    if (sl == NULL || sl->poisoned) return sl->poisoned;

    if (sl->total_failures == 0) {
        sl->first_failure_ns = now_ns;
    }
    sl->total_failures++;

    /* Evict expired entries first. */
    site_evict_expired(sl, now_ns);

    /* Insert the new failure. If the buffer is full, we evict the oldest
     * (overwriting it). This is correct because the oldest is by definition
     * the closest to expiring anyway, and we want to count THIS failure. */
    if (sl->count == sl->capacity) {
        /* Buffer full — overwrite the oldest. */
        sl->failure_times_ns[sl->head] = now_ns;
        sl->head = (sl->head + 1) % sl->capacity;
        /* count stays at capacity */
    } else {
        uint32_t tail = (sl->head + sl->count) % sl->capacity;
        sl->failure_times_ns[tail] = now_ns;
        sl->count++;
    }

    /* Check threshold. */
    if (sl->count >= VTX_DEOPT_SITE_FAIL_THRESHOLD) {
        sl->poisoned = true;
        return true;
    }
    return false;
}

bool vtx_deopt_site_is_poisoned(const vtx_deopt_site_limiter_t *sl)
{
    return sl != NULL && sl->poisoned;
}

double vtx_deopt_site_rate_per_sec(const vtx_deopt_site_limiter_t *sl, uint64_t now_ns)
{
    if (sl == NULL || sl->count == 0) return 0.0;
    /* Count how many entries are still within the window. We can't mutate
     * the const struct, so we just use the count field which is updated
     * lazily on insert. For a more accurate reading, the caller should
     * call vtx_deopt_site_record_failure(0) periodically to force eviction. */
    (void)now_ns;
    /* count is the number of failures in the last window (approx). */
    return (double)sl->count * 1e9 / (double)VTX_DEOPT_SITE_WINDOW_NS;
}

void vtx_deopt_site_reset(vtx_deopt_site_limiter_t *sl)
{
    if (sl == NULL) return;
    sl->head = 0;
    sl->count = 0;
    sl->poisoned = false;
    /* Don't reset total_failures — it's a lifetime counter. */
}

/* ========================================================================== */
/* Deopt batcher                                                               */
/* ========================================================================== */

int vtx_deopt_batcher_init(vtx_deopt_batcher_t *b, uint32_t capacity)
{
    if (b == NULL || capacity == 0) return -1;
    b->pending_sites = (uint32_t *)calloc(capacity, sizeof(uint32_t));
    b->pending_times_ns = (uint64_t *)calloc(capacity, sizeof(uint64_t));
    if (b->pending_sites == NULL || b->pending_times_ns == NULL) {
        free(b->pending_sites);
        free(b->pending_times_ns);
        return -1;
    }
    b->capacity = capacity;
    b->count = 0;
    b->batch_start_ns = 0;
    b->total_batches = 0;
    b->total_deopts_coalesced = 0;
    return 0;
}

void vtx_deopt_batcher_destroy(vtx_deopt_batcher_t *b)
{
    if (b == NULL) return;
    free(b->pending_sites);
    free(b->pending_times_ns);
    b->pending_sites = NULL;
    b->pending_times_ns = NULL;
    b->capacity = 0;
    b->count = 0;
}

bool vtx_deopt_batcher_add(vtx_deopt_batcher_t *b, uint32_t site_id, uint64_t now_ns)
{
    if (b == NULL) return false;

    /* If this is the first entry in a new batch, record the start time. */
    if (b->count == 0) {
        b->batch_start_ns = now_ns;
    }

    /* If the buffer is full, force a flush by returning true. */
    if (b->count >= b->capacity) {
        return true;
    }

    /* Add the entry. */
    b->pending_sites[b->count] = site_id;
    b->pending_times_ns[b->count] = now_ns;
    b->count++;

    /* Check if window has elapsed or buffer is full. */
    if (b->count >= VTX_DEOPT_BATCH_MAX_PENDING) {
        return true;
    }
    if (now_ns - b->batch_start_ns >= VTX_DEOPT_BATCH_WINDOW_NS) {
        return true;
    }
    return false;
}

bool vtx_deopt_batcher_should_flush(const vtx_deopt_batcher_t *b, uint64_t now_ns)
{
    if (b == NULL || b->count == 0) return false;
    if (b->count >= VTX_DEOPT_BATCH_MAX_PENDING) return true;
    if (now_ns - b->batch_start_ns >= VTX_DEOPT_BATCH_WINDOW_NS) return true;
    return false;
}

uint32_t vtx_deopt_batcher_flush(vtx_deopt_batcher_t *b,
                                  uint32_t *out_sites, uint32_t out_capacity,
                                  uint64_t now_ns)
{
    if (b == NULL || b->count == 0) return 0;
    (void)now_ns;

    uint32_t to_copy = b->count;
    if (to_copy > out_capacity) to_copy = out_capacity;
    if (to_copy > 0 && out_sites != NULL) {
        memcpy(out_sites, b->pending_sites, to_copy * sizeof(uint32_t));
    }

    /* Track stats. */
    b->total_batches++;
    b->total_deopts_coalesced += b->count;

    /* Reset batch. */
    b->count = 0;
    b->batch_start_ns = 0;

    return to_copy;
}

/* ========================================================================== */
/* Global deopt budget                                                         */
/* ========================================================================== */

int vtx_deopt_budget_init(vtx_deopt_budget_t *gb, uint32_t capacity)
{
    if (gb == NULL || capacity == 0) return -1;
    gb->deopt_times_ns = (uint64_t *)calloc(capacity, sizeof(uint64_t));
    if (gb->deopt_times_ns == NULL) return -1;
    gb->capacity = capacity;
    gb->head = 0;
    gb->count = 0;
    gb->budget_exceeded = false;
    gb->total_deopts = 0;
    gb->suppress_start_ns = 0;
    return 0;
}

void vtx_deopt_budget_destroy(vtx_deopt_budget_t *gb)
{
    if (gb == NULL) return;
    free(gb->deopt_times_ns);
    gb->deopt_times_ns = NULL;
    gb->capacity = 0;
    gb->count = 0;
}

static void budget_evict_expired(vtx_deopt_budget_t *gb, uint64_t now_ns)
{
    if (gb->count == 0) return;
    uint64_t cutoff = (now_ns > VTX_DEOPT_GLOBAL_WINDOW_NS) ?
                        (now_ns - VTX_DEOPT_GLOBAL_WINDOW_NS) : 0;
    while (gb->count > 0) {
        uint64_t oldest = gb->deopt_times_ns[gb->head];
        if (oldest >= cutoff) break;  /* still within window */
        gb->head = (gb->head + 1) % gb->capacity;
        gb->count--;
    }
}

bool vtx_deopt_budget_record(vtx_deopt_budget_t *gb, uint64_t now_ns)
{
    if (gb == NULL) return false;
    gb->total_deopts++;
    budget_evict_expired(gb, now_ns);

    if (gb->count == gb->capacity) {
        gb->deopt_times_ns[gb->head] = now_ns;
        gb->head = (gb->head + 1) % gb->capacity;
    } else {
        uint32_t tail = (gb->head + gb->count) % gb->capacity;
        gb->deopt_times_ns[tail] = now_ns;
        gb->count++;
    }

    if (gb->count >= VTX_DEOPT_GLOBAL_FAIL_THRESHOLD) {
        if (!gb->budget_exceeded) {
            gb->budget_exceeded = true;
            gb->suppress_start_ns = now_ns;
        }
        return true;
    }
    /* If we were suppressing but the rate has dropped, clear the flag. */
    if (gb->budget_exceeded && gb->count < VTX_DEOPT_GLOBAL_FAIL_THRESHOLD / 2) {
        gb->budget_exceeded = false;
        gb->suppress_start_ns = 0;
    }
    return gb->budget_exceeded;
}

bool vtx_deopt_budget_is_suppressed(const vtx_deopt_budget_t *gb, uint64_t now_ns)
{
    if (gb == NULL) return false;
    (void)now_ns;
    return gb->budget_exceeded;
}

double vtx_deopt_budget_rate_per_sec(const vtx_deopt_budget_t *gb, uint64_t now_ns)
{
    if (gb == NULL || gb->count == 0) return 0.0;
    (void)now_ns;
    return (double)gb->count * 1e9 / (double)VTX_DEOPT_GLOBAL_WINDOW_NS;
}
