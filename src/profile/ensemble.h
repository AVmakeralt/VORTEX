/* ============================================================================ *
 * AI-GENERATED CODE
 *
 * This file was written from scratch by an AI assistant (GLM/Z.ai).
 * It is part of the VORTEX JIT compiler project.
 *
 * Human-written code lives in: src/interp/ (dispatch loop), src/baseline/
 * (codegen), src/runtime/ (GC, type system, arena), src/main_new.c.
 *
 * If reviewing, please verify correctness independently.
 * ============================================================================ */

#ifndef VORTEX_PROFILE_ENSEMBLE_H
#define VORTEX_PROFILE_ENSEMBLE_H

/**
 * VORTEX Ensemble Profiles (Sprint 3)
 *
 * Problem: a single weird run (debugging, one-off input, OS noise) writes
 * a profile that doesn't represent the real workload. Every subsequent
 * run optimizes for the wrong thing. The existing merge (sum across
 * runs) lets a high-sample-count outlier dominate.
 *
 * Fix: store the last K runs' profiles separately. Compute a robust
 * aggregate:
 *   - Branch probabilities: MEDIAN across runs (outlier can't shift it)
 *   - Type distributions: MODE — types seen in >50% of runs are included
 *   - Shape sets: INTERSECTION — only shapes seen in ALL runs
 *   - Loop trip counts: MEDIAN across runs
 *   - Invocation counts: MEDIAN across runs
 *
 * A single outlier run can't dominate because the median/mode/intersection
 * operators are robust to outliers by construction.
 *
 * Quality scoring: each run's profile gets a quality score based on:
 *   - sample_count (more = higher quality)
 *   - deopt_rate (lower = higher quality)
 *   - runtime_duration (longer = higher quality)
 * Profiles below the quality threshold are DEMOTED — excluded from the
 * aggregate. This prevents a crash-prone or too-short run from polluting
 * the ensemble.
 *
 * Rollback: after computing a new aggregate, the ensemble tags it as
 * "pending validation." If the deopt rate in the first M seconds of use
 * exceeds a threshold, the ensemble rolls back to the previous validated
 * aggregate. Bad aggregates don't persist.
 *
 * Lifecycle:
 *   1. init()         — empty ensemble
 *   2. add_run()      — add a completed run's profile (with metadata)
 *   3. compute_aggregate() — produce the robust aggregate into a working
 *                            profile that the JIT uses
 *   4. validate()     — called after M seconds; if deopt rate is low,
 *                       mark the aggregate as validated
 *   5. rollback()     — called if deopt rate is high; reverts to the
 *                       previous validated aggregate
 *
 * The ensemble sits ABOVE the existing profile infrastructure. It does
 * NOT modify vtx_profile_global_t or vtx_profile_merge_into — it adds
 * a new layer that produces a "working" profile for the JIT to use.
 */

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "profile/data.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* Configuration                                                               */
/* ========================================================================== */

/**
 * Maximum number of recent runs stored in the ensemble.
 * 5 is enough to compute a meaningful median while keeping memory and
 * disk usage bounded. Older runs beyond K are evicted (FIFO).
 */
#define VTX_ENSEMBLE_MAX_RUNS 5

/**
 * Minimum number of runs required before ensemble aggregation kicks in.
 * With fewer than 2 runs, there's nothing to aggregate — we just use
 * the single run directly. Set to 2 so that the second run benefits
 * from cross-run robustness.
 */
#define VTX_ENSEMBLE_MIN_RUNS 2

/**
 * Quality thresholds for profile demotion.
 *
 * A run's quality score is computed as:
 *   quality = (sample_count / MIN_SAMPLES) * (1.0 - deopt_rate) * (duration / MIN_DURATION)
 * clamped to [0, 1].
 *
 * Runs with quality < VTX_ENSEMBLE_QUALITY_THRESHOLD are excluded from
 * the aggregate. This filters out:
 *   - Runs that crashed early (low duration, low sample count)
 *   - Runs with pathological deopt rates (bad optimization decisions)
 */
#define VTX_ENSEMBLE_MIN_SAMPLES     1000u
#define VTX_ENSEMBLE_MIN_DURATION_S  1.0
#define VTX_ENSEMBLE_QUALITY_THRESHOLD 0.3

/**
 * Rollback configuration.
 *
 * After computing a new aggregate, the ensemble marks it as "pending
 * validation." If the deopt rate observed in the first
 * VTX_ENSEMBLE_VALIDATION_WINDOW_S seconds exceeds
 * VTX_ENSEMBLE_ROLLBACK_DEOPT_RATE, the ensemble rolls back to the
 * previous validated aggregate.
 *
 * The window is short (30s) because bad aggregates manifest quickly —
 * if every method is deopting, you know within seconds.
 */
#define VTX_ENSEMBLE_VALIDATION_WINDOW_S 30
#define VTX_ENSEMBLE_ROLLBACK_DEOPT_RATE 0.10  /* 10% deopt rate = rollback */

/* ========================================================================== */
/* Run metadata                                                                */
/* ========================================================================== */

/**
 * Metadata for a single run's profile.
 *
 * This is what gets stored alongside each profile snapshot in the
 * ensemble. The quality score is derived from these fields.
 */
typedef struct {
    uint64_t sample_count;      /* total profile samples (invocations + branches + ...) */
    uint64_t deopt_count;       /* deoptimizations observed during this run */
    double   deopt_rate;        /* deopt_count / sample_count (0.0 if no samples) */
    double   runtime_duration_s;/* wall-clock duration of this run in seconds */
    uint64_t timestamp_ns;      /* monotonic timestamp when the run completed */
    double   quality;           /* computed quality score in [0, 1] */
    bool     demoted;           /* true if excluded from aggregate due to low quality */
} vtx_ensemble_run_meta_t;

/* ========================================================================== */
/* Ensemble entry                                                              */
/* ========================================================================== */

/**
 * A single entry in the ensemble: a profile snapshot + its metadata.
 */
typedef struct {
    vtx_profile_global_t      profile;   /* owned profile data */
    vtx_ensemble_run_meta_t   meta;      /* quality + timing metadata */
    bool                      valid;     /* false if this slot is unused */
} vtx_ensemble_entry_t;

/* ========================================================================== */
/* Ensemble manager                                                            */
/* ========================================================================== */

/**
 * Ensemble profile manager.
 *
 * Stores the last K runs and computes a robust aggregate. The aggregate
 * is stored in `working_profile` — this is what the JIT reads from.
 *
 * The `previous_aggregate` is kept so that rollback can restore it.
 */
typedef struct {
    /* Ring buffer of the last K runs. FIFO eviction. */
    vtx_ensemble_entry_t runs[VTX_ENSEMBLE_MAX_RUNS];
    uint32_t             run_count;     /* number of valid entries (<= K) */
    uint32_t             next_slot;     /* next write position in the ring */

    /* The current working profile (the robust aggregate). This is what
     * the JIT reads from. NULL if no aggregate has been computed yet. */
    vtx_profile_global_t *working_profile;

    /* The previous validated aggregate, kept for rollback.
     * NULL if there is no previous aggregate to roll back to. */
    vtx_profile_global_t *previous_aggregate;

    /* Owned profiles (allocated by compute_aggregate). */
    vtx_profile_global_t *owned_working;
    vtx_profile_global_t *owned_previous;

    /* Validation state.
     * True if the current working_profile is pending validation (i.e.,
     * it was just computed and hasn't been confirmed good yet). */
    bool    pending_validation;
    uint64_t validation_start_ns;  /* when the current aggregate was computed */

    /* Statistics */
    uint64_t total_aggregates_computed;
    uint64_t total_rollbacks;
    uint64_t total_demotions;
} vtx_ensemble_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the ensemble manager.
 *
 * Does NOT allocate the working profile — that happens on the first
 * call to compute_aggregate().
 *
 * Returns 0 on success, -1 on failure.
 */
int vtx_ensemble_init(vtx_ensemble_t *ens);

/**
 * Destroy the ensemble and free all owned profiles.
 */
void vtx_ensemble_destroy(vtx_ensemble_t *ens);

/* ========================================================================== */
/* Adding runs                                                                 */
/* ========================================================================== */

/**
 * Add a completed run's profile to the ensemble.
 *
 * The run's profile is COPIED into the ensemble (the caller retains
 * ownership of the original). The metadata is stored alongside it.
 *
 * If the ensemble is full (K runs), the OLDEST run is evicted (FIFO).
 *
 * After adding, the run's quality score is computed automatically.
 * Runs below the quality threshold are marked as demoted and excluded
 * from future aggregates.
 *
 * @param ens       Ensemble manager
 * @param profile   The run's profile data (copied)
 * @param meta      Run metadata (quality field is computed internally)
 * @return          0 on success, -1 on failure
 */
int vtx_ensemble_add_run(vtx_ensemble_t *ens,
                           const vtx_profile_global_t *profile,
                           vtx_ensemble_run_meta_t meta);

/* ========================================================================== */
/* Quality scoring                                                             */
/* ========================================================================== */

/**
 * Compute the quality score for a run given its metadata.
 *
 *   quality = (sample_count / MIN_SAMPLES) * (1.0 - deopt_rate) * (duration / MIN_DURATION)
 *   clamped to [0, 1]
 *
 * A run with 1000+ samples, 0% deopt rate, and 1+ second duration has
 * quality 1.0. A run with 100 samples, 50% deopt rate, and 0.5s duration
 * has quality = 0.1 * 0.5 * 0.5 = 0.025 — demoted.
 *
 * @param meta  Run metadata (quality field is updated)
 */
void vtx_ensemble_compute_quality(vtx_ensemble_run_meta_t *meta);

/**
 * Check if a run is demoted (quality below threshold).
 */
bool vtx_ensemble_run_is_demoted(const vtx_ensemble_run_meta_t *meta);

/* ========================================================================== */
/* Aggregate computation                                                       */
/* ========================================================================== */

/**
 * Compute the robust aggregate from the non-demoted runs.
 *
 * The aggregate is stored in the ensemble's working_profile (allocated
 * if needed). The previous working_profile is moved to
 * previous_aggregate for potential rollback.
 *
 * Aggregate rules:
 *   - Branch probabilities: median P(taken) across non-demoted runs
 *   - Type distributions: mode — types seen in >50% of runs
 *   - Shape sets: intersection — shapes seen in ALL non-demoted runs
 *   - Loop backedge counts: median across runs
 *   - Invocation counts: median across runs
 *
 * If fewer than VTX_ENSEMBLE_MIN_RUNS non-demoted runs exist, the
 * aggregate is just the most recent non-demoted run (no robustness
 * benefit, but still usable).
 *
 * After computing, the working_profile is marked as "pending
 * validation." Call vtx_ensemble_validate() after
 * VTX_ENSEMBLE_VALIDATION_WINDOW_S seconds to confirm it.
 *
 * @param ens  Ensemble manager
 * @return     The working profile, or NULL on failure
 */
vtx_profile_global_t *vtx_ensemble_compute_aggregate(vtx_ensemble_t *ens);

/**
 * Get the current working profile (the aggregate the JIT should use).
 * Returns NULL if no aggregate has been computed yet.
 */
vtx_profile_global_t *vtx_ensemble_get_working(vtx_ensemble_t *ens);

/* ========================================================================== */
/* Validation and rollback                                                     */
/* ========================================================================== */

/**
 * Validate the current working profile.
 *
 * Called after VTX_ENSEMBLE_VALIDATION_WINDOW_S seconds of use. If the
 * observed deopt rate is below the rollback threshold, the working
 * profile is marked as validated. If it exceeds the threshold, the
 * ensemble rolls back to the previous validated aggregate.
 *
 * @param ens              Ensemble manager
 * @param observed_deopt_rate  Deopt rate observed during the validation window
 * @return                 true if validated, false if rolled back
 */
bool vtx_ensemble_validate(vtx_ensemble_t *ens, double observed_deopt_rate);

/**
 * Force an immediate rollback to the previous validated aggregate.
 *
 * Called when the system detects that the current aggregate is bad
 * (e.g., a burst of deopts before the validation window expires).
 *
 * @param ens  Ensemble manager
 * @return     true if rolled back, false if no previous aggregate exists
 */
bool vtx_ensemble_rollback(vtx_ensemble_t *ens);

/**
 * Check if the current working profile is pending validation.
 */
bool vtx_ensemble_is_pending_validation(const vtx_ensemble_t *ens);

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

/**
 * Get ensemble statistics.
 *
 * @param ens                    Ensemble manager
 * @param run_count              Out: number of runs stored
 * @param demoted_count          Out: number of demoted runs
 * @param total_aggregates       Out: total aggregates computed
 * @param total_rollbacks        Out: total rollbacks performed
 * @param total_demotions        Out: total runs demoted
 */
void vtx_ensemble_stats(const vtx_ensemble_t *ens,
                          uint32_t *run_count,
                          uint32_t *demoted_count,
                          uint64_t *total_aggregates,
                          uint64_t *total_rollbacks,
                          uint64_t *total_demotions);

#ifdef __cplusplus
}
#endif

#endif /* VORTEX_PROFILE_ENSEMBLE_H */
