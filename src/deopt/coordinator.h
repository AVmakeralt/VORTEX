#ifndef VORTEX_DEOPT_COORDINATOR_H
#define VORTEX_DEOPT_COORDINATOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "deopt/rate_limit.h"
#include "deopt/deoptless.h"

/**
 * VORTEX Deopt Coordinator
 * ========================
 *
 * Audit priority #7 (Hardening) — integration layer:
 *
 *   "Deopt storm resilience — the deoptless path and OSR work, but you
 *    need: rate limiting, deopt batching, deopt budget."
 *
 * The rate_limit.c module provides the three primitives (per-site limiter,
 * batcher, global budget) as standalone, tested units. This coordinator
 * WIRING them into the actual deopt path:
 *
 *   1. When a guard fails at runtime, the runtime calls
 *      vtx_deopt_coord_on_guard_fail(coord, site_id, now_ns).
 *
 *   2. The coordinator records the failure in:
 *      - the per-site limiter (poisons the site if rate > 100/sec)
 *      - the global budget (suppresses T2/T3 if total > 10000/sec)
 *      - the batcher (coalesces pending deopts into 1ms windows)
 *
 *   3. The coordinator returns a decision:
 *      - DEOPT_COORD_POISONED: site is poisoned → fall back to
 *        interpreter permanently for this site. Do NOT recompile.
 *      - DEOPT_COORD_BATCH: add to batch, defer recompilation.
 *      - DEOPT_COORD_DEOPTLESS: try the deoptless continuation path.
 *      - DEOPT_COORD_FULL_DEOPT: full deopt to interpreter (last resort).
 *      - DEOPT_COORD_SUPPRESSED: global budget exceeded → don't recompile,
 *        just fall back to interpreter for now.
 *
 *   4. When the batcher flushes, the coordinator triggers ONE recompilation
 *      for all pending sites (via a caller-provided callback).
 *
 * This coordinator is OPTIONAL — the runtime can call it if present, or
 * fall back to the old "always full deopt" path if not. This allows
 * incremental rollout.
 */

typedef enum {
    DEOPT_COORD_POISONED     = 0,  /* site permanently poisoned → interpreter */
    DEOPT_COORD_BATCH        = 1,  /* add to batch, defer recompilation */
    DEOPT_COORD_DEOPTLESS    = 2,  /* try deoptless continuation */
    DEOPT_COORD_FULL_DEOPT   = 3,  /* full deopt to interpreter */
    DEOPT_COORD_SUPPRESSED   = 4,  /* global budget exceeded → interpreter */
} vtx_deopt_decision_t;

/* Maximum number of per-site limiters we track. Each call site that can
 * deopt gets a slot. Sites are indexed by a uint32_t site_id (typically
 * the bytecode PC or guard node ID). */
#define VTX_DEOPT_COORD_MAX_SITES 4096

/* Recompilation callback. Called when the batcher flushes. The callback
 * receives an array of site IDs and a count. The callback should trigger
 * recompilation for each site (or coalesce them into one recompilation
 * if the sites belong to the same method). */
typedef void (*vtx_deopt_recompile_fn)(const uint32_t *site_ids, uint32_t count,
                                        void *user_data);

typedef struct {
    /* Per-site limiters, indexed by site_id % MAX_SITES.
     * Collisions are handled by overwriting (last-writer-wins), which is
     * acceptable because the worst case is that a hot site's limiter gets
     * reset, causing it to need more failures before poisoning. */
    vtx_deopt_site_limiter_t sites[VTX_DEOPT_COORD_MAX_SITES];
    bool                     sites_inited[VTX_DEOPT_COORD_MAX_SITES];

    /* Deopt batcher (coalesces pending deopts). */
    vtx_deopt_batcher_t      batcher;

    /* Global deopt budget (suppresses compilation when exceeded). */
    vtx_deopt_budget_t       budget;

    /* Recompilation callback (called when batcher flushes). */
    vtx_deopt_recompile_fn   recompile_fn;
    void                    *recompile_user_data;

    /* Statistics */
    uint64_t total_guard_failures;
    uint64_t total_poisoned;
    uint64_t total_deoptless;
    uint64_t total_full_deopts;
    uint64_t total_suppressed;
    uint64_t total_batches_flushed;
} vtx_deopt_coord_t;

/* Initialize the coordinator. The recompile_fn is called when the batcher
 * flushes; pass NULL if you don't want automatic recompilation. */
int vtx_deopt_coord_init(vtx_deopt_coord_t *coord,
                          vtx_deopt_recompile_fn recompile_fn,
                          void *user_data);

/* Destroy the coordinator (frees all per-site limiters). */
void vtx_deopt_coord_destroy(vtx_deopt_coord_t *coord);

/* Called when a guard fails at runtime. Returns the decision about what
 * to do next. The site_id identifies the call site (typically bytecode PC
 * or guard node ID). now_ns is the current time in nanoseconds. */
vtx_deopt_decision_t vtx_deopt_coord_on_guard_fail(
    vtx_deopt_coord_t *coord, uint32_t site_id, uint64_t now_ns);

/* Check if a site is poisoned (permanently falling back to interpreter).
 * This is called by the recompile thread before scheduling recompilation
 * — if the site is poisoned, don't bother recompiling. */
bool vtx_deopt_coord_is_poisoned(const vtx_deopt_coord_t *coord, uint32_t site_id);

/* Check if compilation is currently suppressed (global budget exceeded).
 * Called by the compile thread before starting T2/T3 compilation. */
bool vtx_deopt_coord_is_suppressed(const vtx_deopt_coord_t *coord, uint64_t now_ns);

/* Force-flush the batcher (e.g. at a safepoint). Returns the number of
 * sites in the flushed batch. */
uint32_t vtx_deopt_coord_flush(vtx_deopt_coord_t *coord, uint64_t now_ns);

/* Get statistics as a printable string. */
void vtx_deopt_coord_stats_str(const vtx_deopt_coord_t *coord, char *buf, size_t bufsize);

#endif /* VORTEX_DEOPT_COORDINATOR_H */
