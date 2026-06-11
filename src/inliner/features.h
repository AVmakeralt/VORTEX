#ifndef VORTEX_INLINER_FEATURES_H
#define VORTEX_INLINER_FEATURES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "profile/data.h"

/**
 * VORTEX ML Inliner — Feature Extraction
 *
 * Extracts 15 features per call site for the GBDT inlining model.
 * Features capture static properties (size, structure) and dynamic
 * properties (frequency, type certainty, deopt history) of the
 * caller/callee pair at each call site.
 *
 * Feature indices:
 *   0  callee_size             — callee bytecode size in bytes
 *   1  callee_instruction_count— number of IR nodes in callee graph
 *   2  call_site_frequency     — profiled invocation count at this site
 *   3  caller_size             — caller bytecode size in bytes
 *   4  call_depth              — inlining depth at this call site
 *   5  callee_is_hot           — 1.0 if callee invocation_count >= T2 threshold, else 0.0
 *   6  callee_has_loops        — 1.0 if callee contains LoopBegin nodes, else 0.0
 *   7  callee_has_try_catch    — 1.0 if callee contains Catch nodes, else 0.0
 *   8  callee_allocates        — 1.0 if callee contains NewObject/NewArray/Allocate, else 0.0
 *   9  callee_calls_virtual    — 1.0 if callee contains CallVirtual/CallInterface, else 0.0
 *  10  receiver_type_certainty — 1.0 monomorphic, 0.5 polymorphic, 0.0 megamorphic
 *  11  constant_arg_ratio      — fraction of arguments that are Constant nodes
 *  12  estimated_register_pressure — heuristic: (callee_node_count * 0.3 + caller_node_count * 0.1) / 100.0
 *  13  callee_deopt_rate       — (callee deopt count / callee invocation count), 0.0 if no invocations
 *  14  inline_history          — number of times this call site was previously inlined
 */

#define VTX_INLINE_FEATURE_COUNT 15

/* ========================================================================== */
/* Feature vector                                                              */
/* ========================================================================== */

typedef struct {
    double features[VTX_INLINE_FEATURE_COUNT];
} vtx_inline_features_t;

/* ========================================================================== */
/* Feature extraction context                                                  */
/* ========================================================================== */

/**
 * Context needed for feature extraction beyond the graph and profile.
 * The caller provides this to avoid the feature extractor needing
 * knowledge of the entire compilation infrastructure.
 */
typedef struct {
    /* Bytecode size of the caller method (in bytes) */
    uint32_t caller_bytecode_size;

    /* Current inlining depth at this call site */
    uint32_t call_depth;

    /* Number of times this specific call site has been inlined before */
    uint32_t inline_history;

    /* Callee bytecode size (looked up externally) */
    uint32_t callee_bytecode_size;

    /* Callee method ID for profile lookup */
    uint32_t callee_method_id;

    /* Callee deopt count (from runtime feedback) */
    uint64_t callee_deopt_count;

    /* Callee invocation count (from runtime feedback) */
    uint64_t callee_invocation_count;

    /* Caller graph node count (for register pressure estimate) */
    uint32_t caller_node_count;

    /* Callee graph — when available, enables precise feature extraction
     * for features 1 (callee_instruction_count), 6 (callee_has_loops),
     * 7 (callee_has_try_catch), 8 (callee_allocates), and
     * 9 (callee_calls_virtual). May be NULL if the callee graph
     * has not yet been constructed. */
    const vtx_graph_t *callee_graph;
} vtx_inline_context_t;

/* ========================================================================== */
/* Feature extraction API                                                      */
/* ========================================================================== */

/**
 * Extract features for a call site.
 *
 * @param graph    The caller's SoN graph
 * @param call_node The call node (CallStatic/CallVirtual/CallInterface) NodeID
 * @param profile  Global profile data (may be NULL if no profiling available)
 * @param context  Additional context for extraction
 * @return         Populated feature vector
 */
vtx_inline_features_t vtx_features_extract(const vtx_graph_t *graph,
                                            vtx_nodeid_t call_node,
                                            const vtx_profile_global_t *profile,
                                            const vtx_inline_context_t *context);

/**
 * Get the human-readable name of a feature by index.
 * Returns "unknown" if the index is out of range.
 */
const char *vtx_feature_name(uint32_t index);

/**
 * Normalize a feature vector to [0, 1] range using known bounds.
 * Each feature has a principled maximum derived from system limits:
 *   - sizes capped at VTX_INLINE_SIZE_LIMIT
 *   - frequencies capped at UINT64_MAX (saturating)
 *   - ratios already in [0, 1]
 *   - depths capped at VTX_MAX_TREE_DEPTH
 */
void vtx_features_normalize(vtx_inline_features_t *features);

/**
 * Write a feature vector as a CSV line (without trailing newline).
 * Format: f0,f1,...,f14
 * Writes at most buf_size bytes including null terminator.
 * Returns the number of characters written (excluding null), or -1 on error.
 */
int vtx_features_to_csv(const vtx_inline_features_t *features,
                         char *buf, size_t buf_size);

#endif /* VORTEX_INLINER_FEATURES_H */
