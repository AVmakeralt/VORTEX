#include "guard/ewma.h"
#include <math.h>

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

void vtx_ewma_init(vtx_ewma_t *ewma)
{
    if (ewma == NULL) return;

    ewma->value = 0.0;
    ewma->alpha = VTX_GUARD_ALPHA;
    ewma->update_count = 0;
    ewma->initialized = false;
}

void vtx_ewma_init_with_alpha(vtx_ewma_t *ewma, double alpha)
{
    if (ewma == NULL) return;

    vtx_ewma_init(ewma);

    /* Clamp alpha to (0, 1] */
    if (alpha <= 0.0) alpha = 0.001;
    if (alpha > 1.0) alpha = 1.0;

    ewma->alpha = alpha;
}

/* ========================================================================== */
/* Update                                                                      */
/* ========================================================================== */

double vtx_ewma_update(vtx_ewma_t *ewma, double failure_rate)
{
    if (ewma == NULL) return 0.0;

    /* Clamp failure rate to [0.0, 1.0] */
    if (failure_rate < 0.0) failure_rate = 0.0;
    if (failure_rate > 1.0) failure_rate = 1.0;

    if (!ewma->initialized) {
        /* First update: initialize EWMA to the observed rate.
         * This avoids the slow ramp-up that would occur if we
         * started at 0.0 and gradually incorporated observations. */
        ewma->value = failure_rate;
        ewma->initialized = true;
    } else {
        /* Standard EWMA formula:
         * ewma_new = alpha * observation + (1 - alpha) * ewma_old
         *
         * This gives more weight to recent observations while
         * maintaining a smooth estimate. The alpha parameter
         * controls the responsiveness:
         *   - alpha = 1.0: EWMA = latest observation (no smoothing)
         *   - alpha = 0.0: EWMA never changes (infinite smoothing)
         *   - alpha = 0.1: moderate smoothing (half-life ≈ 7 observations) */
        ewma->value = ewma->alpha * failure_rate +
                      (1.0 - ewma->alpha) * ewma->value;
    }

    /* Saturating: clamp to [0.0, 1.0] */
    if (ewma->value < 0.0) ewma->value = 0.0;
    if (ewma->value > 1.0) ewma->value = 1.0;

    ewma->update_count++;

    return ewma->value;
}

double vtx_ewma_update_counts(vtx_ewma_t *ewma,
                                uint64_t failures,
                                uint64_t executions)
{
    if (ewma == NULL) return 0.0;

    if (executions == 0) {
        /* No executions: don't update the EWMA.
         * Returning the current value is the most conservative choice —
         * it doesn't change the estimate based on zero information. */
        return ewma->value;
    }

    double failure_rate = (double)failures / (double)executions;
    return vtx_ewma_update(ewma, failure_rate);
}

/* ========================================================================== */
/* Query                                                                       */
/* ========================================================================== */

double vtx_ewma_value(const vtx_ewma_t *ewma)
{
    if (ewma == NULL) return 0.0;
    return ewma->value;
}

bool vtx_ewma_is_initialized(const vtx_ewma_t *ewma)
{
    if (ewma == NULL) return false;
    return ewma->initialized;
}

uint64_t vtx_ewma_update_count(const vtx_ewma_t *ewma)
{
    if (ewma == NULL) return 0;
    return ewma->update_count;
}

void vtx_ewma_reset(vtx_ewma_t *ewma)
{
    if (ewma == NULL) return;

    double alpha = ewma->alpha; /* preserve custom alpha */
    vtx_ewma_init(ewma);
    ewma->alpha = alpha;
}
