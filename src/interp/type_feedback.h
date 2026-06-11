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
/* Per-site feedback structures                                                */
/* ========================================================================== */

/**
 * Call site feedback: circular buffer of call observations.
 */
typedef struct {
    vtx_tf_call_observation_t observations[VTX_TYPE_FEEDBACK_BUFFER_SIZE];
    uint8_t  write_index;   /* next write position in the circular buffer */
    uint8_t  count;         /* number of valid observations (max 32) */
} vtx_tf_call_site_t;

/**
 * Field access feedback: circular buffer of field observations.
 */
typedef struct {
    vtx_tf_field_observation_t observations[VTX_TYPE_FEEDBACK_BUFFER_SIZE];
    uint8_t  write_index;
    uint8_t  count;
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

#endif /* VORTEX_TYPE_FEEDBACK_H */
