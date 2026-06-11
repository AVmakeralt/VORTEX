#ifndef VORTEX_SOTA_RECOMP_H
#define VORTEX_SOTA_RECOMP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "profile/data.h"
#include "compile/version.h"
#include "runtime/arena.h"

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

typedef struct {
    /* Per-method compilation-time profile snapshot.
     * Stored as a dense array indexed by method_id.
     * Each snapshot records the type distribution at each call site
     * at the time of compilation, so we can compare with current profile. */
    struct vtx_recomp_snapshot *snapshots;
    uint32_t                    snapshot_count;
    uint32_t                    snapshot_capacity;

    /* Recompilation queue: methods that need recompilation due to
     * profile divergence. Workers pick entries from this queue. */
    vtx_recomp_queue_entry_t *recomp_queue;
    uint32_t                  recomp_queue_count;
    uint32_t                  recomp_queue_capacity;

    /* Statistics */
    uint64_t total_checks;
    uint64_t total_recompilations_triggered;
    uint64_t total_false_positives;  /* recompiled but profile didn't actually change */
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

#endif /* VORTEX_SOTA_RECOMP_H */
