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

#ifndef VORTEX_SOTA_RECOMP_H
#define VORTEX_SOTA_RECOMP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "profile/data.h"
#include "compile/version.h"
#include "runtime/arena.h"
#include "profile/phase.h"
#include "interp/type_feedback.h"

/**
 * VORTEX SOTA — Continuous Background Recompilation
 *
 * Monitors profile data in real-time and triggers recompilation when
 * type profiles diverge from the assumptions used during the previous
 * compilation.
 *
 * The key metric is KL divergence (Kullback-Leibler) between the
 * type profile at compilation time and the current type profile.
 * When KL divergence exceeds VTX_PROFILE_DIVERGENCE_THRESHOLD (0.5),
 * the method is queued for recompilation with updated profile data.
 *
 * The new version is compiled in the background and installed at the
 * next safe point, with zero pause to the executing application.
 *
 * KL divergence measures how much information is lost when using the
 * old profile to approximate the current profile. A value of 0 means
 * identical distributions; values > 0.5 indicate significant divergence.
 *
 * Formula:
 *   KL(P || Q) = Σ P(i) * ln(P(i) / Q(i))
 * where P = current profile, Q = compilation-time profile
 *
 * Smoothing: to avoid ln(0), we add a small epsilon (1e-6) to both
 * P(i) and Q(i) before computing the ratio.
 */

/* ========================================================================== */
/* Recompilation check result                                                  */
/* ========================================================================== */

typedef struct {
    bool     should_recompile;       /* true if recompilation is recommended */
    double   kl_divergence;          /* KL divergence value */
    uint32_t divergent_call_sites;   /* number of call sites with high divergence */
    uint32_t method_id;              /* method to recompile */
} vtx_recomp_check_t;

/* ========================================================================== */
/* Recompilation state                                                         */
/* ========================================================================== */

/* ========================================================================== */
/* Recompilation queue entry                                                  */
/* ========================================================================== */

typedef struct {
    uint32_t method_id;         /* method to recompile */
    double   kl_divergence;     /* divergence that triggered recompilation */
    uint64_t enqueue_time_ns;   /* when the entry was enqueued */
    bool     processed;         /* true if already picked up by a worker */
} vtx_recomp_queue_entry_t;

/* Forward declaration — defined in recomp.c */
typedef struct vtx_recomp_snapshot vtx_recomp_snapshot_t;

/* ========================================================================== */
/* Sprint 1.2: Hysteresis per-method state                                     */
/* ========================================================================== */

/**
 * Per-method hysteresis state for KL-divergence recompilation.
 *
 * Without hysteresis, the orchestrator fires recomp on every divergent
 * sample. A workload that oscillates between two phases causes the same
 * method to be recompiled repeatedly — thrashing.
 *
 * Hysteresis requires VTX_RECOMP_HYSTERESIS_CONSECUTIVE consecutive
 * divergent samples before triggering recompilation. A non-divergent
 * sample resets the counter to zero.
 *
 * The state is keyed by method_id and stored in a small hash table
 * inside vtx_sota_recomp_t. Lookups are O(1) amortized.
 */
typedef struct {
    uint32_t method_id;              /* method this state is for */
    uint32_t consecutive_divergent;  /* consecutive divergent samples seen */
    uint64_t last_recomp_time_ns;    /* last time this method was recompiled */
    uint64_t recomp_count_in_window; /* recompiles within the coalesce window */
    uint64_t window_start_ns;        /* start of the current coalesce window */
    bool     valid;                  /* true if this slot is in use */
} vtx_recomp_hysteresis_t;

typedef struct {
    /* Per-method compilation-time profile snapshot.
     * Stored as a dense array indexed by method_id.
     * Each snapshot records the type distribution at each call site
     * at the time of compilation, so we can compare with current profile. */
    vtx_recomp_snapshot_t *snapshots;
    uint32_t                    snapshot_count;
    uint32_t                    snapshot_capacity;

    /* Recompilation queue: methods that need recompilation due to
     * profile divergence. Workers pick entries from this queue. */
    vtx_recomp_queue_entry_t *recomp_queue;
    uint32_t                  recomp_queue_count;
    uint32_t                  recomp_queue_capacity;

    /* Sprint 1.2: Hysteresis state per method.
     * Small open-addressing hash table keyed by method_id. */
    vtx_recomp_hysteresis_t *hysteresis;
    uint32_t                 hysteresis_count;
    uint32_t                 hysteresis_capacity;

    /* Sprint 1.3: Backpressure statistics.
     * Tracks how many queue entries were dropped due to soft/hard cap. */
    uint64_t total_dropped_soft_cap;   /* dropped because queue > soft cap */
    uint64_t total_dropped_hard_cap;   /* rejected because queue >= hard cap */
    uint64_t total_coalesced;          /* older entries merged into newer */

    /* Statistics */
    uint64_t total_checks;
    uint64_t total_recompilations_triggered;
    uint64_t total_false_positives;  /* recompiled but profile didn't actually change */
    uint64_t total_hysteresis_blocks; /* Sprint 1.2: would-have-fired but blocked by hysteresis */
} vtx_sota_recomp_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the recompilation monitor.
 * Returns 0 on success, -1 on failure.
 */
int vtx_sota_recomp_init(vtx_sota_recomp_t *recomp);

/**
 * Destroy the recompilation monitor and free memory.
 */
void vtx_sota_recomp_destroy(vtx_sota_recomp_t *recomp);

/* ========================================================================== */
/* Snapshot management                                                         */
/* ========================================================================== */

/**
 * Save a profile snapshot for a method at compilation time.
 * This snapshot is used later for KL divergence comparison.
 *
 * @param recomp     Recompilation monitor
 * @param method_id  Method that was compiled
 * @param profile    Global profile data at compilation time
 * @return           0 on success, -1 on failure
 */
int vtx_sota_recomp_save_snapshot(vtx_sota_recomp_t *recomp,
                                    uint32_t method_id,
                                    const vtx_profile_global_t *profile);

/**
 * Remove the snapshot for a method (e.g., when the method is invalidated).
 */
void vtx_sota_recomp_remove_snapshot(vtx_sota_recomp_t *recomp,
                                       uint32_t method_id);

/* ========================================================================== */
/* Recompilation check                                                         */
/* ========================================================================== */

/**
 * Check if a compiled method should be recompiled based on
 * profile divergence.
 *
 * Compares the current type profile at each call site with the
 * snapshot taken at compilation time. If the KL divergence at
 * any call site exceeds VTX_PROFILE_DIVERGENCE_THRESHOLD,
 * recommends recompilation.
 *
 * @param recomp     Recompilation monitor
 * @param profile    Current global profile data
 * @param method_id  Method to check
 * @return           Check result with should_recompile flag and details
 */
vtx_recomp_check_t vtx_sota_recomp_check(const vtx_sota_recomp_t *recomp,
                                            const vtx_profile_global_t *profile,
                                            uint32_t method_id);

/* ========================================================================== */
/* KL divergence computation                                                   */
/* ========================================================================== */

/**
 * Compute KL divergence between two type distributions.
 *
 * Each distribution is represented as an array of (type_id, frequency) pairs.
 * The frequencies are normalized to probabilities internally.
 *
 * @param types_a    Type IDs in distribution A (current)
 * @param freqs_a    Frequencies for distribution A
 * @param count_a    Number of entries in distribution A
 * @param types_b    Type IDs in distribution B (compilation-time)
 * @param freqs_b    Frequencies for distribution B
 * @param count_b    Number of entries in distribution B
 * @return           KL divergence value (>= 0)
 */
double vtx_kl_divergence(const vtx_typeid_t *types_a, const uint64_t *freqs_a, uint32_t count_a,
                           const vtx_typeid_t *types_b, const uint64_t *freqs_b, uint32_t count_b);

/**
 * Compute KL divergence between two call site profiles.
 * Convenience wrapper that extracts type distributions.
 */
double vtx_kl_divergence_callsite(const vtx_callsite_profile_t *current,
                                    const vtx_callsite_profile_t *compiled);

/**
 * Compute KL divergence between two per-type frequency distributions.
 * Uses the D5 per-type frequency data for accurate divergence measurement.
 * New types not seen at compilation time receive a large penalty (10.0).
 *
 * @param current   Current per-type frequency distribution
 * @param compiled  Per-type frequency distribution at compilation time
 * @return          KL divergence value (>= 0)
 */
double vtx_recomp_kl_divergence_freq(const vtx_type_freq_t *current,
                                       const vtx_type_freq_t *compiled);

/* ========================================================================== */
/* Profile divergence computation                                              */
/* ========================================================================== */

/**
 * Compute KL divergence between two method profiles.
 *
 * This compares the type distributions at each call site between
 * the old (compilation-time) profile and the new (current) profile.
 * Returns the maximum KL divergence across all call sites.
 *
 * A high divergence means the current execution pattern has shifted
 * significantly from what was assumed at compilation time, and
 * recompilation would likely produce better code.
 *
 * @param old_profile  Method profile at compilation time
 * @param new_profile  Current method profile
 * @return             Maximum KL divergence across call sites (>= 0)
 */
double vtx_sota_recomp_compute_divergence(const vtx_profile_method_t *old_profile,
                                            const vtx_profile_method_t *new_profile);

/* ========================================================================== */
/* Recompilation queue                                                         */
/* ========================================================================== */

/**
 * Queue a method for recompilation with updated profile data.
 *
 * The method is added to the recompilation queue. A background
 * compilation worker will pick it up, compile it with the
 * current profile, and install the new version at the next
 * safe point.
 *
 * If the method is already in the queue, this is a no-op
 * (we don't queue duplicate compilations).
 *
 * @param recomp      Recompilation monitor
 * @param method_id   Method to recompile
 * @param new_profile Current profile data (snapshot is taken internally)
 */
void vtx_sota_recomp_queue(vtx_sota_recomp_t *recomp,
                             uint32_t method_id,
                             const vtx_profile_global_t *new_profile);

/**
 * Dequeue the next method to recompile.
 *
 * Returns the method_id of the next unprocessed entry, or
 * VTX_PHASE_NONE if the queue is empty.
 *
 * @param recomp Recompilation monitor
 * @return       Method ID to recompile, or VTX_PHASE_NONE
 */
uint32_t vtx_sota_recomp_dequeue(vtx_sota_recomp_t *recomp);

/**
 * Check if the recompilation queue has pending entries.
 */
bool vtx_sota_recomp_has_pending(const vtx_sota_recomp_t *recomp);

/* ========================================================================== */
/* Sprint 1.2: Hysteresis-aware check                                          */
/* ========================================================================== */

/**
 * Hysteresis-aware recompilation check.
 *
 * This is the same as vtx_sota_recomp_check(), but it tracks consecutive
 * divergent samples per method and only returns should_recompile=true
 * after VTX_RECOMP_HYSTERESIS_CONSECUTIVE consecutive divergent checks.
 * A non-divergent check resets the per-method counter.
 *
 * Use this in the orchestrator instead of the raw check to prevent
 * recomp thrashing under oscillating workloads.
 *
 * @param recomp     Recompilation monitor
 * @param profile    Current global profile data
 * @param method_id  Method to check
 * @return           Check result; should_recompile is true only if the
 *                   hysteresis threshold has been reached
 */
vtx_recomp_check_t vtx_sota_recomp_check_hysteresis(vtx_sota_recomp_t *recomp,
                                                       const vtx_profile_global_t *profile,
                                                       uint32_t method_id);

/**
 * Reset the hysteresis counter for a method (e.g., after a successful
 * recompilation, so the counter doesn't immediately fire again).
 *
 * @param recomp     Recompilation monitor
 * @param method_id  Method whose counter to reset
 */
void vtx_sota_recomp_hysteresis_reset(vtx_sota_recomp_t *recomp,
                                        uint32_t method_id);

/**
 * Get the current consecutive-divergent count for a method.
 * Returns 0 if no state exists for the method.
 */
uint32_t vtx_sota_recomp_hysteresis_count(const vtx_sota_recomp_t *recomp,
                                            uint32_t method_id);

/* ========================================================================== */
/* Sprint 1.3: Backpressure-aware queue                                        */
/* ========================================================================== */

/**
 * Backpressure-aware queue submission.
 *
 * Same as vtx_sota_recomp_queue(), but enforces backpressure:
 *
 *   - HARD CAP: if the queue is at VTX_RECOMP_QUEUE_HARD_CAP, the entry
 *     is rejected (returns false). The orchestrator will re-check the
 *     method on the next tick.
 *
 *   - SOFT CAP: if the queue is above VTX_RECOMP_QUEUE_SOFT_CAP, the
 *     lowest-priority unprocessed entry (lowest KL divergence) is
 *     evicted before the new entry is added. This keeps the queue
 *     drained under sustained divergence.
 *
 *   - COALESCE: if the same method is already queued (unprocessed) AND
 *     was recompiled at least VTX_RECOMP_COALESCE_MAX_DUPLICATES times
 *     within VTX_RECOMP_COALESCE_WINDOW_SEC, the older entry is removed
 *     and replaced with the newer one (with updated KL divergence).
 *
 * @param recomp       Recompilation monitor
 * @param method_id    Method to recompile
 * @param new_profile  Current profile data
 * @param now_ns       Current monotonic time in nanoseconds (used for
 *                     coalesce windowing; pass 0 to disable coalescing)
 * @return             true if the entry was queued, false if rejected
 *                     by the hard cap
 */
bool vtx_sota_recomp_queue_backpressure(vtx_sota_recomp_t *recomp,
                                          uint32_t method_id,
                                          const vtx_profile_global_t *new_profile,
                                          uint64_t now_ns);

/**
 * Get backpressure statistics.
 *
 * @param recomp                Recompilation monitor
 * @param dropped_soft_cap      Out: entries dropped because queue > soft cap
 * @param dropped_hard_cap      Out: entries rejected because queue >= hard cap
 * @param coalesced             Out: older entries merged into newer
 * @param hysteresis_blocks     Out: would-have-fired but blocked by hysteresis
 */
void vtx_sota_recomp_backpressure_stats(const vtx_sota_recomp_t *recomp,
                                          uint64_t *dropped_soft_cap,
                                          uint64_t *dropped_hard_cap,
                                          uint64_t *coalesced,
                                          uint64_t *hysteresis_blocks);

#endif /* VORTEX_SOTA_RECOMP_H */
