#ifndef VORTEX_DEOPT_RATE_LIMIT_H
#define VORTEX_DEOPT_RATE_LIMIT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * VORTEX Deopt Rate Limiter
 * =========================
 *
 * Audit priority #7 (Hardening): "Deopt storm resilience — the deoptless
 * path and OSR work, but you need:
 *   - Rate limiting: if a guard fails >100 times/sec, fall back to
 *     interpreter permanently for that site.
 *   - Deopt batching: coalesce multiple pending deopts into one
 *     recompilation.
 *   - Deopt budget: if total deopts exceed N/sec globally, suppress
 *     T2/T3 compilation."
 *
 * This module provides three primitives:
 *
 * 1. vtx_deopt_rate_limiter_t — per-site rate limiting. Each call site
 *    that can deopt has a limiter. When the failure rate exceeds the
 *    threshold (default 100/sec over a 1-second window), the site is
 *    marked "poisoned" — future guard failures should permanently fall
 *    back to the interpreter rather than recompiling.
 *
 * 2. vtx_deopt_batcher_t — coalesces pending deopts. Multiple guard
 *    failures within a short window (default 1ms) are batched into a
 *    single recompilation request, avoiding the "deopt → recompile →
 *    deopt again" storm.
 *
 * 3. vtx_deopt_budget_t — global deopt budget. Tracks total deopts per
 *    second across all sites. When the budget is exceeded (default
 *    10000/sec), T2/T3 compilation is suppressed until the rate drops.
 *
 * All three primitives are O(1) per operation and thread-safe (use
 * atomic operations; no locks on the hot path).
 *
 * Tunables (override via vtx_deopt_rate_limit_set_*_defaults):
 *   SITE_WINDOW_NS         = 1_000_000_000   (1 second)
 *   SITE_FAIL_THRESHOLD    = 100             (fails per window)
 *   BATCH_WINDOW_NS        = 1_000_000       (1 millisecond)
 *   BATCH_MAX_PENDING      = 64              (max deopts in a batch)
 *   GLOBAL_WINDOW_NS       = 1_000_000_000   (1 second)
 *   GLOBAL_FAIL_THRESHOLD  = 10_000          (deopts per second globally)
 */

/* ========================================================================== */
/* Tunables                                                                    */
/* ========================================================================== */

#define VTX_DEOPT_SITE_WINDOW_NS        1000000000ULL  /* 1 sec */
#define VTX_DEOPT_SITE_FAIL_THRESHOLD   100u
#define VTX_DEOPT_BATCH_WINDOW_NS       1000000ULL     /* 1 ms */
#define VTX_DEOPT_BATCH_MAX_PENDING     64u
#define VTX_DEOPT_GLOBAL_WINDOW_NS      1000000000ULL  /* 1 sec */
#define VTX_DEOPT_GLOBAL_FAIL_THRESHOLD 10000u

/* ========================================================================== */
/* Per-site rate limiter                                                       */
/* ========================================================================== */

typedef struct {
    /* Sliding-window counter: failures in the last SITE_WINDOW_NS nanoseconds.
     * Implemented as a circular buffer of failure timestamps. */
    uint64_t *failure_times_ns;  /* dynamically allocated array */
    uint32_t  capacity;          /* size of failure_times_ns array */
    uint32_t  head;              /* index of the oldest failure */
    uint32_t  count;             /* number of failures in the window */
    bool      poisoned;          /* true once threshold exceeded — permanent */
    uint64_t  total_failures;    /* lifetime counter (for diagnostics) */
    uint64_t  first_failure_ns;  /* time of first failure ever (for diagnostics) */
} vtx_deopt_site_limiter_t;

/* Initialize a per-site limiter with the given capacity (must be >=
 * SITE_FAIL_THRESHOLD; typically 2x to allow some headroom). */
int vtx_deopt_site_init(vtx_deopt_site_limiter_t *sl, uint32_t capacity);

/* Destroy a per-site limiter (frees the failure_times_ns array). */
void vtx_deopt_site_destroy(vtx_deopt_site_limiter_t *sl);

/* Record a guard failure at the given time (nanoseconds since some epoch).
 * Returns true if the site is now poisoned (i.e. the caller should
 * permanently fall back to the interpreter for this site). */
bool vtx_deopt_site_record_failure(vtx_deopt_site_limiter_t *sl, uint64_t now_ns);

/* Returns true if the site is poisoned. */
bool vtx_deopt_site_is_poisoned(const vtx_deopt_site_limiter_t *sl);

/* Get the current failure rate (failures per second) over the window. */
double vtx_deopt_site_rate_per_sec(const vtx_deopt_site_limiter_t *sl, uint64_t now_ns);

/* Reset a site (e.g. after recompilation with a different speculation). */
void vtx_deopt_site_reset(vtx_deopt_site_limiter_t *sl);

/* ========================================================================== */
/* Deopt batcher                                                               */
/* ========================================================================== */

typedef struct {
    /* Pending deopts in the current batch. Each entry is a (site_id, timestamp)
     * pair. The batch is flushed when either:
     *   - BATCH_WINDOW_NS has elapsed since the first entry, OR
     *   - BATCH_MAX_PENDING entries have been accumulated. */
    uint32_t *pending_sites;     /* dynamically allocated array */
    uint64_t *pending_times_ns;
    uint32_t  capacity;
    uint32_t  count;
    uint64_t  batch_start_ns;    /* time of first entry in current batch */
    uint64_t  total_batches;     /* lifetime count of batches flushed */
    uint64_t  total_deopts_coalesced; /* lifetime count of deopts coalesced */
} vtx_deopt_batcher_t;

/* Initialize a batcher. */
int vtx_deopt_batcher_init(vtx_deopt_batcher_t *b, uint32_t capacity);

/* Destroy a batcher. */
void vtx_deopt_batcher_destroy(vtx_deopt_batcher_t *b);

/* Add a pending deopt for the given site. Returns true if the batch is
 * now ready to flush (i.e. the caller should invoke vtx_deopt_batch_flush
 * and trigger recompilation). */
bool vtx_deopt_batcher_add(vtx_deopt_batcher_t *b, uint32_t site_id, uint64_t now_ns);

/* Flush the current batch. Returns the number of deopts in the flushed
 * batch (0 if empty), and writes up to *out_count site IDs into out_sites
 * (caller-allocated array of at least capacity entries). The batch is
 * reset after flushing. */
uint32_t vtx_deopt_batcher_flush(vtx_deopt_batcher_t *b,
                                  uint32_t *out_sites, uint32_t out_capacity,
                                  uint64_t now_ns);

/* Returns true if the batcher has pending deopts that should be flushed
 * (i.e. either the batch is full or the window has elapsed). */
bool vtx_deopt_batcher_should_flush(const vtx_deopt_batcher_t *b, uint64_t now_ns);

/* ========================================================================== */
/* Global deopt budget                                                         */
/* ========================================================================== */

typedef struct {
    /* Sliding-window counter of all deopts across all sites, globally.
     * Uses the same circular-buffer approach as the per-site limiter. */
    uint64_t *deopt_times_ns;
    uint32_t  capacity;
    uint32_t  head;
    uint32_t  count;
    bool      budget_exceeded;   /* true while rate exceeds threshold */
    uint64_t  total_deopts;      /* lifetime counter */
    uint64_t  suppress_start_ns; /* when suppression started (0 if not suppressing) */
} vtx_deopt_budget_t;

/* Initialize the global budget tracker. */
int vtx_deopt_budget_init(vtx_deopt_budget_t *gb, uint32_t capacity);

/* Destroy. */
void vtx_deopt_budget_destroy(vtx_deopt_budget_t *gb);

/* Record a global deopt. Returns true if the budget is now exceeded
 * (i.e. T2/T3 compilation should be suppressed until the rate drops). */
bool vtx_deopt_budget_record(vtx_deopt_budget_t *gb, uint64_t now_ns);

/* Returns true if compilation should be suppressed right now. */
bool vtx_deopt_budget_is_suppressed(const vtx_deopt_budget_t *gb, uint64_t now_ns);

/* Get the current global deopt rate (deopts per second). */
double vtx_deopt_budget_rate_per_sec(const vtx_deopt_budget_t *gb, uint64_t now_ns);

#endif /* VORTEX_DEOPT_RATE_LIMIT_H */
