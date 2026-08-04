#ifndef VORTEX_TYPE_FEEDBACK_H
#define VORTEX_TYPE_FEEDBACK_H

#include "vortex_config.h"
#include <stdint.h>
#include <stdbool.h>
#include "runtime/object.h"
#include "runtime/type_system.h"

/**
 * VORTEX Type Feedback Collection
 *
 * At each call site and field access, the observed type is recorded.
 * For call sites: receiver TypeID + result TypeID.
 * For field access: holder shape ID + value TypeID.
 * For branches: taken / total counts.
 *
 * Type feedback uses a circular buffer per site (last 32 observations)
 * with exponential decay weighting. Recent observations are weighted
 * more heavily than older ones.
 *
 * This data feeds the optimizer's speculative decisions.
 */

/* Number of observations per feedback site */
#define VTX_TYPE_FEEDBACK_BUFFER_SIZE VTX_PROFILE_TYPE_BUFFER_SIZE /* 32 */

/* ========================================================================== */
/* Type feedback observation types                                             */
/* ========================================================================== */

/**
 * A call site observation: receiver type and result type.
 */
typedef struct {
    vtx_typeid_t receiver_typeid;  /* type of the receiver object */
    vtx_typeid_t result_typeid;    /* type of the result value */
} vtx_tf_call_observation_t;

/**
 * A field access observation: holder shape and value type.
 */
typedef struct {
    vtx_shapeid_t holder_shapeid;  /* shape of the holder object */
    vtx_typeid_t  value_typeid;    /* type of the accessed field value */
} vtx_tf_field_observation_t;

/**
 * A branch observation: taken or not taken.
 */
typedef struct {
    bool taken;  /* true if the branch was taken */
} vtx_tf_branch_observation_t;

/* ========================================================================== */
/* Per-type frequency tracking (D5)                                            */
/* ========================================================================== */

/**
 * Maximum number of distinct types tracked per call site.
 * 8 slots captures >99% of real-world call sites (most are monomorphic
 * or polymorphic with 2-3 types). Sites exceeding this become megamorphic
 * and the frequency data degrades gracefully (oldest entry is simply not
 * updated but total_count still increments).
 */
#define VTX_TYPE_FREQ_MAX_SLOTS 8

/**
 * A single (type_id, count) entry in the per-type frequency table.
 * Tracks how many times a specific receiver type was observed at a call site.
 */
typedef struct {
    vtx_typeid_t type_id;    /* observed type */
    uint32_t     count;      /* how many times this type was seen */
} vtx_type_freq_entry_t;

/**
 * Per-type frequency distribution for a single call site.
 * Enables accurate KL divergence computation for recompilation decisions.
 * Without this, the KL divergence is meaningless because uniform weights
 * are used (each observed type gets equal weight regardless of actual
 * frequency).
 */
typedef struct {
    vtx_type_freq_entry_t entries[VTX_TYPE_FREQ_MAX_SLOTS];
    uint32_t              entry_count;   /* number of populated entries (<= VTX_TYPE_FREQ_MAX_SLOTS) */
    uint32_t              total_count;   /* total observations across all types */
} vtx_type_freq_t;

/* ========================================================================== */
/* Stable-type signatures for composite guard optimization (Proposal #2)        */
/* ========================================================================== */

/**
 * Maximum number of types in a stable-type signature.
 * Covers receiver + up to 7 arguments.
 */
#define VTX_TYPE_SIGNATURE_MAX_SLOTS 8

/**
 * A stable-type signature: the tuple (receiver_type, arg1_type, ..., result_type)
 * observed at a call site. When all types in the signature have been stable
 * for VTX_TYPE_STABILITY_WINDOW consecutive observations, the site is marked
 * as hyper-stable and receives a single composite guard.
 */
typedef struct {
    vtx_typeid_t types[VTX_TYPE_SIGNATURE_MAX_SLOTS]; /* type IDs: [0]=receiver, [1..7]=args */
    uint32_t     slot_count;     /* number of filled slots (1..8) */
    uint32_t     stability_count; /* consecutive observations matching this signature */
    bool         is_hyper_stable; /* true if stability_count >= VTX_TYPE_STABILITY_WINDOW */
} vtx_type_signature_t;

/**
 * Window of consecutive consistent observations required for hyper-stability.
 * After this many observations of the same type signature, the call site is
 * considered hyper-stable and receives a composite guard.
 */
#define VTX_TYPE_STABILITY_WINDOW 1000

/* ========================================================================== */
/* Per-site feedback structures                                                */
/* ========================================================================== */

/**
 * Call site feedback: circular buffer of call observations.
 */
typedef struct {
    vtx_tf_call_observation_t observations[VTX_TYPE_FEEDBACK_BUFFER_SIZE];
    uint8_t  write_index;   /* next write position in the circular buffer */
    uint8_t  count;         /* number of valid observations (max 32) */

    /* D5: Per-type frequency distribution.
     * Complementary to the circular buffer: the buffer captures raw
     * observations with temporal ordering (for decay-weighted queries),
     * while the frequency table captures cumulative counts per type
     * (for accurate KL divergence). */
    vtx_type_freq_t type_freq;

    /* Stable-type signature for composite guard optimization.
     * Tracks the full (receiver, args..., result) type tuple. */
    vtx_type_signature_t stable_signature;
} vtx_tf_call_site_t;

/**
 * Field access feedback: circular buffer of field observations.
 */
typedef struct {
    vtx_tf_field_observation_t observations[VTX_TYPE_FEEDBACK_BUFFER_SIZE];
    uint8_t  write_index;
    uint8_t  count;
    /* Shape stability tracking (Proposal #9) */
    vtx_shapeid_t       last_shapeid;       /* shape from most recent observation */
    uint32_t            shape_stability_count; /* consecutive observations with same shape */
    bool                is_shape_stable;     /* true if stability >= VTX_SHAPE_STABILITY_WINDOW */
} vtx_tf_field_site_t;

/**
 * Branch feedback: circular buffer of branch observations.
 */
typedef struct {
    vtx_tf_branch_observation_t observations[VTX_TYPE_FEEDBACK_BUFFER_SIZE];
    uint8_t  write_index;
    uint8_t  count;
} vtx_tf_branch_site_t;

/* ========================================================================== */
/* Type feedback collector                                                     */
/* ========================================================================== */

/**
 * The type feedback collector manages arrays of per-site feedback.
 * Sites are indexed by an integer (typically derived from bytecode PC).
 */
typedef struct {
    /* Call site feedback */
    vtx_tf_call_site_t  *call_sites;    /* array of call site feedback */
    uint32_t             call_site_count;

    /* Field access feedback */
    vtx_tf_field_site_t *field_sites;   /* array of field site feedback */
    uint32_t             field_site_count;

    /* Branch feedback */
    vtx_tf_branch_site_t *branch_sites;  /* array of branch site feedback */
    uint32_t              branch_site_count;
} vtx_type_feedback_t;

/**
 * Initialize the type feedback collector with the given maximum
 * number of sites for each category.
 * Returns 0 on success, -1 on failure.
 */
int vtx_type_feedback_init(vtx_type_feedback_t *feedback, uint32_t max_sites);

/**
 * Destroy the type feedback collector and release all memory.
 */
void vtx_type_feedback_destroy(vtx_type_feedback_t *feedback);

/**
 * Ensure the feedback arrays can accommodate the given site index.
 * Grows the arrays if necessary.
 * Returns 0 on success, -1 on failure.
 */
int vtx_type_feedback_ensure_site(vtx_type_feedback_t *feedback,
                                   uint32_t site_index);

/**
 * Record a call site observation: receiver type and result type.
 * site_index identifies which call site this observation belongs to.
 */
void vtx_type_feedback_record_call(vtx_type_feedback_t *feedback,
                                    uint32_t site_index,
                                    vtx_typeid_t receiver_typeid,
                                    vtx_typeid_t result_typeid);

/**
 * Record a field access observation: holder shape and value type.
 * site_index identifies which field site this observation belongs to.
 */
void vtx_type_feedback_record_field(vtx_type_feedback_t *feedback,
                                     uint32_t site_index,
                                     vtx_shapeid_t holder_shapeid,
                                     vtx_typeid_t value_typeid);

/**
 * Record a branch observation.
 * site_index identifies which branch site this observation belongs to.
 */
void vtx_type_feedback_record_branch(vtx_type_feedback_t *feedback,
                                      uint32_t site_index,
                                      bool taken);

/**
 * Get the dominant receiver type at a call site, weighted by recency.
 * Uses exponential decay weighting:
 *   weight[i] = 0.9^distance, where distance is how many positions
 *   ago the observation was written (most recent = distance 1).
 *
 * Returns VTX_TYPE_INVALID if no observations.
 */
vtx_typeid_t vtx_type_feedback_get_dominant_call_type(
    const vtx_type_feedback_t *feedback, uint32_t site_index);

/**
 * Get the dominant field shape at a field access site, weighted by recency.
 * Uses exponential decay weighting like call site queries.
 *
 * Returns VTX_SHAPE_INVALID if no observations.
 */
vtx_shapeid_t vtx_type_feedback_get_dominant_field_shape(
    const vtx_type_feedback_t *feedback, uint32_t site_index);

/**
 * Get the dominant value type at a field access site, weighted by recency.
 * Uses exponential decay weighting.
 *
 * Returns VTX_TYPE_INVALID if no observations.
 */
vtx_typeid_t vtx_type_feedback_get_dominant_field_value_type(
    const vtx_type_feedback_t *feedback, uint32_t site_index);

/**
 * Get the branch probability (taken / total) at a branch site.
 * Uses exponential decay weighting for recency.
 * Returns 0.5 (unknown) if no observations.
 */
double vtx_type_feedback_get_branch_probability(
    const vtx_type_feedback_t *feedback, uint32_t site_index);

/**
 * Get the number of unique receiver types observed at a call site.
 */
uint32_t vtx_type_feedback_get_call_site_type_count(
    const vtx_type_feedback_t *feedback, uint32_t site_index);

/**
 * Get the per-type frequency distribution for a call site.
 * Returns a pointer to the type_freq structure embedded in the call site.
 * Returns NULL if the site does not exist.
 *
 * The returned pointer is valid as long as the feedback structure is valid
 * and the site is not re-allocated (i.e., until vtx_type_feedback_destroy).
 */
const vtx_type_freq_t *vtx_type_feedback_get_type_freq(
    const vtx_type_feedback_t *feedback, uint32_t site_index);

/**
 * Compute KL divergence between two per-type frequency distributions.
 * KL(P || Q) = sum_i P(i) * log(P(i) / Q(i))
 * where P = current distribution, Q = compiled-time distribution.
 *
 * New types not seen at compilation time receive a large penalty (10.0)
 * to ensure they trigger recompilation.
 *
 * Returns 0.0 if either distribution has zero total observations.
 */
double vtx_type_freq_kl_divergence(const vtx_type_freq_t *current,
                                     const vtx_type_freq_t *compiled);

/* ========================================================================== */
/* Stable-type signature management                                             */
/* ========================================================================== */

/**
 * Update the stable-type signature for a call site with a new observation.
 * If the new observation matches the existing signature, increment stability.
 * If it differs, reset the signature and stability counter.
 *
 * @param site       The call site to update
 * @param receiver   Receiver type ID
 * @param result     Result type ID
 */
void vtx_tf_call_site_update_signature(vtx_tf_call_site_t *site,
                                        vtx_typeid_t receiver,
                                        vtx_typeid_t result);

/**
 * Check if a call site is hyper-stable (signature stable for
 * VTX_TYPE_STABILITY_WINDOW observations).
 *
 * @param site  The call site
 * @return      true if hyper-stable
 */
bool vtx_tf_call_site_is_hyper_stable(const vtx_tf_call_site_t *site);

/**
 * Get the stable-type signature for a call site.
 * Returns NULL if the site has no observations yet.
 *
 * @param site  The call site
 * @return      Pointer to the signature, or NULL
 */
const vtx_type_signature_t *vtx_tf_call_site_get_signature(
    const vtx_tf_call_site_t *site);

/**
 * Compute a 64-bit hash of a type signature for fast comparison.
 * Used by composite guards to check the entire signature in one CMP.
 *
 * @param sig   The type signature
 * @return      64-bit hash value
 */
uint64_t vtx_type_signature_hash(const vtx_type_signature_t *sig);

/* ========================================================================== */
/* Shape stability queries (Proposal #9)                                         */
/* ========================================================================== */

#define VTX_SHAPE_STABILITY_WINDOW 500

/**
 * Check if a field site's shape has been stable for long enough
 * to warrant shape-specialized code generation.
 */
bool vtx_tf_field_site_is_shape_stable(const vtx_tf_field_site_t *site);

/**
 * Get the stable shape ID for a field site.
 * Returns VTX_SHAPE_INVALID if the site is not shape-stable.
 */
vtx_shapeid_t vtx_tf_field_site_get_stable_shape(const vtx_tf_field_site_t *site);

#endif /* VORTEX_TYPE_FEEDBACK_H */
