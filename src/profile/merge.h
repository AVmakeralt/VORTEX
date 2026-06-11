#ifndef VORTEX_PROFILE_MERGE_H
#define VORTEX_PROFILE_MERGE_H

#include "profile/data.h"

/**
 * VORTEX Profile Merging
 *
 * Merges profile data from multiple sources (e.g., previous runs loaded from
 * disk combined with the current run's data). Merging is:
 *   - Associative: merge(A, merge(B, C)) == merge(merge(A, B), C)
 *   - Commutative: merge(A, B) == merge(B, A)
 *
 * Merge rules:
 *   - Invocation counts: summed (saturating at UINT64_MAX)
 *   - Type observations: unioned (if combined distinct types exceed
 *     VTX_POLY_LIMIT, the site transitions to megamorphic)
 *   - Branch frequencies: weight-averaged by invocation count.
 *     Specifically: merged_taken = target_taken + source_taken,
 *     merged_not_taken = target_not_taken + source_not_taken.
 *     This is equivalent to averaging weighted by total observations.
 *   - Loop back-edge counts: summed (saturating)
 *   - Call edge frequencies: summed (saturating)
 *   - Field access shapes: unioned (same rule as type observations)
 *
 * When the source has a method that the target does not, it is added.
 * When both have the same method, their data is merged field-by-field.
 */

/* ========================================================================== */
/* Merge functions                                                            */
/* ========================================================================== */

/**
 * Merge all data from `source` into `target`. Target is modified in place.
 * Source is not modified.
 *
 * After this call, target contains the union of both profiles.
 */
void vtx_profile_merge_into(vtx_profile_global_t *target,
                             const vtx_profile_global_t *source);

/**
 * Merge a single method profile into the global target.
 * If the method does not exist in target, it is added.
 * If it exists, its data is merged.
 *
 * This is the function called by the persistence loader for each method.
 */
void vtx_profile_merge_method(vtx_profile_global_t *target,
                               const vtx_profile_method_t *source_method);

/**
 * Merge a single callsite profile: union the type observations from source
 * into target. If the combined type count exceeds VTX_POLY_LIMIT, target
 * becomes megamorphic.
 */
void vtx_profile_merge_callsite(vtx_callsite_profile_t *target,
                                 const vtx_callsite_profile_t *source);

/**
 * Merge branch profiles: add source counts into target (saturating).
 * This is equivalent to weight-averaging since the counts are additive.
 */
void vtx_profile_merge_branch(vtx_branch_profile_t *target,
                               const vtx_branch_profile_t *source);

/**
 * Merge field access profiles: union the shape observations from source
 * into target. Same rule as callsite merging.
 */
void vtx_profile_merge_field(vtx_field_profile_t *target,
                              const vtx_field_profile_t *source);

/**
 * Merge loop profiles: sum the back-edge counts (saturating).
 */
void vtx_profile_merge_loop(vtx_loop_profile_t *target,
                             const vtx_loop_profile_t *source);

#endif /* VORTEX_PROFILE_MERGE_H */
