#ifndef VORTEX_GUARD_EWMA_H
#define VORTEX_GUARD_EWMA_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"

/**
 * VORTEX Adaptive Guards — Exponentially Weighted Moving Average
 *
 * Tracks the failure rate of a guard using an EWMA (Exponentially
 * Weighted Moving Average). The EWMA provides a smooth, responsive
 * estimate of the current failure rate that adapts quickly to changes
 * while filtering out noise.
 *
 * Formula:
 *   ewma = alpha * (failures / executions) + (1 - alpha) * ewma
 *
 * Where alpha = VTX_GUARD_ALPHA (0.1), which gives a half-life of
 * approximately 7 observations:
 *   half-life = ln(0.5) / ln(1 - alpha) ≈ 6.58
 *
 * The EWMA is saturating: it never goes below 0.0 or above 1.0.
 * This prevents floating-point drift from accumulating.
 *
 * The EWMA is used by the guard metadata system to make strength
 * transition decisions. It provides a more stable signal than
 * the raw failure rate, which can be noisy for low execution counts.
 */

/* ========================================================================== */
/* EWMA state                                                                  */
/* ========================================================================== */

typedef struct {
    double   value;           /* current EWMA value, in [0.0, 1.0] */
    double   alpha;           /* smoothing factor (VTX_GUARD_ALPHA = 0.1) */
    uint64_t update_count;    /* number of updates applied */
    bool     initialized;     /* true after first update */
} vtx_ewma_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize an EWMA tracker with the default alpha (VTX_GUARD_ALPHA).
 */
void vtx_ewma_init(vtx_ewma_t *ewma);

/**
 * Initialize an EWMA tracker with a custom alpha.
 * Alpha must be in (0, 1]. Values outside this range are clamped.
 */
void vtx_ewma_init_with_alpha(vtx_ewma_t *ewma, double alpha);

/* ========================================================================== */
/* Update                                                                      */
/* ========================================================================== */

/**
 * Update the EWMA with a new failure rate observation.
 *
 * If this is the first update, the EWMA is set to the observed rate.
 * Subsequent updates apply the EWMA formula:
 *   ewma = alpha * failure_rate + (1 - alpha) * ewma
 *
 * The result is clamped to [0.0, 1.0] (saturating).
 *
 * @param ewma          EWMA tracker
 * @param failure_rate  Observed failure rate (failures / executions) for this period.
 *                      Must be in [0.0, 1.0]; values outside are clamped.
 * @return              The new EWMA value
 */
double vtx_ewma_update(vtx_ewma_t *ewma, double failure_rate);

/**
 * Update the EWMA with raw failure and execution counts.
 * Computes the failure rate internally and calls vtx_ewma_update.
 *
 * @param ewma        EWMA tracker
 * @param failures    Number of failures in this period
 * @param executions  Number of executions in this period
 * @return            The new EWMA value
 */
double vtx_ewma_update_counts(vtx_ewma_t *ewma,
                                uint64_t failures,
                                uint64_t executions);

/* ========================================================================== */
/* Query                                                                       */
/* ========================================================================== */

/**
 * Get the current EWMA value.
 * Returns 0.0 if the EWMA has never been updated.
 */
double vtx_ewma_value(const vtx_ewma_t *ewma);

/**
 * Check if the EWMA has been initialized (at least one update).
 */
bool vtx_ewma_is_initialized(const vtx_ewma_t *ewma);

/**
 * Get the number of updates applied to this EWMA.
 */
uint64_t vtx_ewma_update_count(const vtx_ewma_t *ewma);

/**
 * Reset the EWMA to its initial state.
 */
void vtx_ewma_reset(vtx_ewma_t *ewma);

#endif /* VORTEX_GUARD_EWMA_H */
