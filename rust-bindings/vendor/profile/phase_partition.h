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

#ifndef VORTEX_PROFILE_PHASE_PARTITION_H
#define VORTEX_PROFILE_PHASE_PARTITION_H

/**
 * VORTEX Phase-Aware Profile Partitioning (Sprint 2)
 *
 * Problem: a workload that runs in distinct phases (e.g. a web server
 * with /api and /compute endpoints, or a batch job with parse → process
 * → emit stages) has very different type/branch/loop distributions in
 * each phase. The existing single global profile merges them all
 * together, so:
 *
 *   - Type distributions from /compute pollute /api's monomorphic sites
 *     → megamorphic transition → lost inlining → slower /api.
 *   - Branch probabilities from /compute (mostly taken) leak into /api
 *     (mostly not taken) → wrong branch hints.
 *   - Loop trip counts from the large /compute datasets leak into /api's
 *     small loops → wrong vectorization decisions.
 *
 * Fix: maintain a separate vtx_profile_global_t per detected phase. When
 * the phase predictor says "we're now in phase Y," swap the active
 * profile to phase Y's. Recording and recompilation decisions then use
 * phase Y's data, not the merged mess.
 *
 * This module owns:
 *   - A small map: phase_id → vtx_profile_global_t
 *   - The "active" phase (whose profile is currently being recorded to
 *     and read from by the JIT)
 *   - A "default" phase (phase_id == VTX_PHASE_NONE) for methods that
 *     don't belong to any detected phase
 *
 * Persistence: each phase's profile is saved to a separate file
 * (<hash>.<phase_id>.prof). This keeps the existing single-profile file
 * format unchanged and lets the partition grow/shrink without rewriting
 * a master file.
 *
 * Thread safety: the partition manager itself is NOT internally locked.
 * Callers (the orchestrator) must serialize access. The per-phase
 * vtx_profile_global_t objects are returned by reference and share the
 * same thread-safety contract as the existing profile module (i.e.,
 * the recording path is single-threaded; the orchestrator reads
 * periodically from a background thread).
 */

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "profile/data.h"
#include "profile/phase.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* Configuration                                                               */
/* ========================================================================== */

/**
 * Initial capacity of the phase → profile map.
 * Most programs have 2–8 phases; 16 leaves headroom.
 */
#define VTX_PHASE_PARTITION_INITIAL_CAPACITY 16

/**
 * Sentinel phase ID used for the "default" profile — methods that don't
 * belong to any detected phase. We reuse VTX_PHASE_NONE for this, since
 * a method with no phase is conceptually in the "default" phase.
 */

/* ========================================================================== */
/* Per-phase entry                                                             */
/* ========================================================================== */

/**
 * A single phase's profile storage.
 *
 * The `profile` is a fully-owned vtx_profile_global_t — the partition
 * manager initializes and destroys it.
 */
typedef struct {
    uint32_t                phase_id;     /* phase ID (or VTX_PHASE_NONE for default) */
    vtx_profile_global_t    profile;      /* owned profile data for this phase */
    uint64_t                transition_count;  /* how many times we entered this phase */
    uint64_t                last_transition_ns; /* monotonic time of last entry */
    bool                    valid;        /* false if this slot is unused */
} vtx_phase_profile_entry_t;

/* ========================================================================== */
/* Partition manager                                                           */
/* ========================================================================== */

/**
 * Phase-aware profile partition manager.
 *
 * Owns N per-phase profile globals and tracks which one is currently
 * "active" (i.e., the one the interpreter is recording to and the
 * orchestrator is reading from for recomp decisions).
 *
 * Lifecycle:
 *   1. init()  — creates the default phase (VTX_PHASE_NONE) and makes
 *                it active.
 *   2. transition(new_phase) — saves the current active phase's stats,
 *                              creates the new phase if needed, makes it
 *                              active.
 *   3. get_active() — returns the currently-active profile (used by
 *                     the recording path and the orchestrator).
 *   4. destroy() — destroys all per-phase profiles.
 */
typedef struct {
    /* Array of per-phase entries. Grown via realloc. */
    vtx_phase_profile_entry_t *entries;
    uint32_t                   entry_count;
    uint32_t                   entry_capacity;

    /* Index into entries[] of the currently-active phase.
     * Always valid after init() (points to the default phase entry). */
    uint32_t                   active_index;

    /* Statistics */
    uint64_t                   total_transitions;     /* total phase transitions */
    uint64_t                   total_phase_creations; /* new phases created */
} vtx_phase_partition_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the partition manager.
 *
 * Creates the default phase (phase_id == VTX_PHASE_NONE) and makes it
 * active. The default phase is where all recording goes until the first
 * explicit transition.
 *
 * Returns 0 on success, -1 on failure.
 */
int vtx_phase_partition_init(vtx_phase_partition_t *part);

/**
 * Destroy the partition manager and all per-phase profiles.
 */
void vtx_phase_partition_destroy(vtx_phase_partition_t *part);

/* ========================================================================== */
/* Active profile access                                                       */
/* ========================================================================== */

/**
 * Get the currently-active phase's profile.
 *
 * This is the profile that:
 *   - The interpreter records to (via the orchestrator's wiring)
 *   - The orchestrator reads from for recomp decisions
 *   - The compiler uses for speculative optimization
 *
 * Returns NULL only if init() failed.
 */
vtx_profile_global_t *vtx_phase_partition_get_active(vtx_phase_partition_t *part);

/**
 * Get the phase_id of the currently-active phase.
 * Returns VTX_PHASE_NONE for the default phase.
 */
uint32_t vtx_phase_partition_active_phase(const vtx_phase_partition_t *part);

/* ========================================================================== */
/* Phase transitions                                                           */
/* ========================================================================== */

/**
 * Transition to a new phase.
 *
 *   1. Records transition stats for the outgoing phase.
 *   2. Looks up (or creates) the entry for `new_phase_id`.
 *   3. Makes that entry the active phase.
 *
 * If `new_phase_id` is the same as the currently-active phase, this is
 * a no-op (returns the active profile without modifying state).
 *
 * After this call, vtx_phase_partition_get_active() returns the new
 * phase's profile. The interpreter's recording pointer must be updated
 * by the caller.
 *
 * @param part          Partition manager
 * @param new_phase_id  Phase to transition to (VTX_PHASE_NONE for default)
 * @param now_ns        Monotonic timestamp (pass 0 to use internal clock)
 * @return              The new active profile, or NULL on failure
 */
vtx_profile_global_t *vtx_phase_partition_transition(
    vtx_phase_partition_t *part,
    uint32_t new_phase_id,
    uint64_t now_ns);

/* ========================================================================== */
/* Per-phase queries                                                           */
/* ========================================================================== */

/**
 * Get the profile for a specific phase, or NULL if it doesn't exist.
 *
 * Does NOT change the active phase. Use this to inspect or save a
 * phase's profile (e.g., for persistence or preemptive recompilation
 * of the predicted-next phase).
 */
vtx_profile_global_t *vtx_phase_partition_get_phase(
    vtx_phase_partition_t *part,
    uint32_t phase_id);

/**
 * Get or create the profile for a specific phase.
 *
 * If the phase doesn't exist, it's created (but NOT made active —
 * call transition() for that). Returns the phase's profile.
 */
vtx_profile_global_t *vtx_phase_partition_get_or_create(
    vtx_phase_partition_t *part,
    uint32_t phase_id);

/**
 * Number of distinct phases tracked (including the default phase).
 */
uint32_t vtx_phase_partition_phase_count(const vtx_phase_partition_t *part);

/**
 * Iterate over all phase entries.
 *
 * Example:
 *   for (uint32_t i = 0; i < part->entry_count; i++) {
 *       if (!part->entries[i].valid) continue;
 *       // use &part->entries[i].profile
 *   }
 *
 * Caller must NOT reallocate the entries array while iterating.
 */

/* ========================================================================== */
/* Preemptive recompilation helper                                             */
/* ========================================================================== */

/**
 * Given a phase graph and a target phase, populate `out_methods` with
 * the method IDs that are hot in that phase and should be preemptively
 * recompiled when entering the phase.
 *
 * The methods are sorted by descending invocation_count in the phase's
 * profile (so the hottest come first). At most `out_capacity` methods
 * are written.
 *
 * @param part          Partition manager
 * @param graph         Phase graph (from vtx_phase_detect)
 * @param phase_id      Target phase
 * @param out_methods   Output array
 * @param out_capacity  Max methods to write
 * @return              Number of methods written (may be 0)
 */
uint32_t vtx_phase_partition_hot_methods_for_phase(
    vtx_phase_partition_t *part,
    const vtx_phase_graph_t *graph,
    uint32_t phase_id,
    uint32_t *out_methods,
    uint32_t out_capacity);

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

/**
 * Get aggregate partition statistics.
 *
 * @param part                 Partition manager
 * @param total_transitions    Out: total phase transitions
 * @param total_creations      Out: total phases created
 * @param active_phase_id      Out: currently-active phase ID
 */
void vtx_phase_partition_stats(const vtx_phase_partition_t *part,
                                 uint64_t *total_transitions,
                                 uint64_t *total_creations,
                                 uint32_t *active_phase_id);

#ifdef __cplusplus
}
#endif

#endif /* VORTEX_PROFILE_PHASE_PARTITION_H */
