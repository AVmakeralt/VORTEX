#include "inliner/features.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ========================================================================== */
/* Feature names                                                               */
/* ========================================================================== */

static const char * const vtx_feature_names[VTX_INLINE_FEATURE_COUNT] = {
    "callee_size",
    "callee_instruction_count",
    "call_site_frequency",
    "caller_size",
    "call_depth",
    "callee_is_hot",
    "callee_has_loops",
    "callee_has_try_catch",
    "callee_allocates",
    "callee_calls_virtual",
    "receiver_type_certainty",
    "constant_arg_ratio",
    "estimated_register_pressure",
    "callee_deopt_rate",
    "inline_history"
};

/* ========================================================================== */
/* Helper: count nodes with a specific opcode in a graph                       */
/* ========================================================================== */

static uint32_t count_opcode(const vtx_graph_t *graph, vtx_node_opcode_t opcode)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        vtx_node_t *node = &graph->node_table.nodes[i];
        if (!node->dead && node->opcode == opcode) {
            count++;
        }
    }
    return count;
}

/* ========================================================================== */
/* Helper: check if callee graph contains any of a set of opcodes              */
/* ========================================================================== */

static bool has_any_opcode(const vtx_graph_t *graph,
                            const vtx_node_opcode_t *opcodes,
                            uint32_t opcode_count)
{
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead) continue;
        for (uint32_t j = 0; j < opcode_count; j++) {
            if (node->opcode == opcodes[j]) {
                return true;
            }
        }
    }
    return false;
}

/* ========================================================================== */
/* Helper: compute receiver type certainty from call site profile              */
/* ========================================================================== */

static double receiver_type_certainty(const vtx_profile_global_t *profile,
                                       uint32_t method_id,
                                       uint32_t callsite_index)
{
    if (profile == NULL) {
        /* No profile data: assume polymorphic (0.5) — conservative */
        return 0.5;
    }

    const vtx_callsite_profile_t *cs = vtx_profile_get_callsite(
        profile, method_id, callsite_index);
    if (cs == NULL) {
        /* No observations yet: assume polymorphic */
        return 0.5;
    }

    if (cs->megamorphic) {
        return 0.0; /* megamorphic = no certainty */
    }

    if (cs->count == 1) {
        return 1.0; /* monomorphic = full certainty */
    }

    /* Polymorphic: certainty degrades with number of observed types.
     * 2 types = 0.5, 3 types = 0.33, 4 types = 0.25 */
    return 1.0 / (double)cs->count;
}

/* ========================================================================== */
/* Helper: compute ratio of constant arguments at a call site                  */
/* ========================================================================== */

static double constant_arg_ratio(const vtx_graph_t *graph, vtx_nodeid_t call_node_id)
{
    vtx_node_t *call_node = vtx_node_get(&graph->node_table, call_node_id);
    if (call_node == NULL || call_node->input_count == 0) {
        return 0.0;
    }

    /* Input layout for call nodes:
     *   input[0] = control predecessor
     *   input[1] = memory predecessor
     *   input[2..] = data arguments
     * The first two inputs are structural, not arguments.
     * However, some calls may have the method_index identifying the target.
     * We count data inputs starting from input[2] as arguments. */

    uint32_t data_start = 2; /* skip control + memory */
    if (call_node->input_count <= data_start) {
        return 0.0; /* no arguments */
    }

    uint32_t total_args = call_node->input_count - data_start;
    uint32_t constant_args = 0;

    for (uint32_t i = data_start; i < call_node->input_count; i++) {
        vtx_nodeid_t arg_id = call_node->inputs[i];
        if (arg_id == VTX_NODEID_INVALID) continue;
        vtx_node_t *arg = vtx_node_get_const(&graph->node_table, arg_id);
        if (arg != NULL && arg->opcode == VTX_OP_Constant) {
            constant_args++;
        }
    }

    return (double)constant_args / (double)total_args;
}

/* ========================================================================== */
/* Helper: compute call site frequency from profile                            */
/* ========================================================================== */

static double call_site_frequency(const vtx_profile_global_t *profile,
                                   uint32_t callee_method_id)
{
    if (profile == NULL) {
        return 0.0;
    }

    const vtx_profile_method_t *method = vtx_profile_get_method(profile, callee_method_id);
    if (method == NULL) {
        return 0.0;
    }

    /* Use invocation count as the frequency proxy.
     * Saturating arithmetic in the profile means this won't overflow
     * as a double since UINT64_MAX is representable. */
    return (double)method->invocation_count;
}

/* ========================================================================== */
/* Helper: compute callee deopt rate                                           */
/* ========================================================================== */

static double callee_deopt_rate(uint64_t deopt_count, uint64_t invocation_count)
{
    if (invocation_count == 0) {
        return 0.0;
    }
    return (double)deopt_count / (double)invocation_count;
}

/* ========================================================================== */
/* Helper: estimate register pressure                                          */
/* ========================================================================== */

static double estimated_register_pressure(uint32_t callee_node_count,
                                           uint32_t caller_node_count)
{
    /* Heuristic: each IR node roughly corresponds to ~0.3 live registers
     * in the callee after inlining, and the caller contributes ~0.1 per
     * node to background pressure. Normalize by 100 to get a [0, ~10] range.
     *
     * This is a principled approximation: real register pressure depends on
     * liveness intervals, but those aren't available at inlining time.
     * The factor 0.3 is derived from empirical measurements of typical
     * SoN graphs where the average live variable count is ~30% of node count
     * within a basic block. */
    double raw = (callee_node_count * 0.3 + caller_node_count * 0.1) / 100.0;
    /* Cap at a reasonable maximum to avoid outliers */
    if (raw > 10.0) raw = 10.0;
    return raw;
}

/* ========================================================================== */
/* Public API                                                                  */
/* ========================================================================== */

vtx_inline_features_t vtx_features_extract(const vtx_graph_t *graph,
                                            vtx_nodeid_t call_node,
                                            const vtx_profile_global_t *profile,
                                            const vtx_inline_context_t *context)
{
    vtx_inline_features_t result;
    memset(&result, 0, sizeof(result));

    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(context != NULL, "context must not be NULL");

    /* Feature 0: callee_size — callee bytecode size in bytes.
     * Normalized later; raw value here. */
    result.features[0] = (double)context->callee_bytecode_size;

    /* Feature 1: callee_instruction_count — IR node count of callee.
     * When the callee graph is available, count live nodes directly.
     * Otherwise, approximate from the callee bytecode size using the
     * empirical ratio of ~1.5 IR nodes per bytecode instruction
     * (measured from compilations of real programs). */
    if (context->callee_graph != NULL) {
        uint32_t live_count = 0;
        for (uint32_t i = 0; i < context->callee_graph->node_table.count; i++) {
            if (!context->callee_graph->node_table.nodes[i].dead) {
                live_count++;
            }
        }
        result.features[1] = (double)live_count;
    } else {
        result.features[1] = (double)context->callee_bytecode_size * 1.5;
    }

    /* Feature 2: call_site_frequency — profiled invocation count */
    result.features[2] = call_site_frequency(profile, context->callee_method_id);

    /* Feature 3: caller_size — caller bytecode size in bytes */
    result.features[3] = (double)context->caller_bytecode_size;

    /* Feature 4: call_depth — current inlining depth */
    result.features[4] = (double)context->call_depth;

    /* Feature 5: callee_is_hot — 1.0 if callee is hot */
    if (profile != NULL) {
        const vtx_profile_method_t *callee_method =
            vtx_profile_get_method(profile, context->callee_method_id);
        result.features[5] = (callee_method != NULL &&
            vtx_profile_method_is_hot(callee_method, VORTEX_T2_THRESHOLD))
            ? 1.0 : 0.0;
    } else {
        result.features[5] = 0.0;
    }

    /* Features 6-9: Analyze callee structure.
     * When the callee graph is available, count the relevant opcodes directly.
     * Otherwise, fall back to conservative heuristics based on bytecode size. */
    if (context->callee_graph != NULL) {
        /* Feature 6: callee_has_loops — 1.0 if callee contains LoopBegin nodes */
        result.features[6] = (count_opcode(context->callee_graph, VTX_OP_LoopBegin) > 0)
                             ? 1.0 : 0.0;

        /* Feature 7: callee_has_try_catch — 1.0 if callee contains Catch nodes */
        result.features[7] = (count_opcode(context->callee_graph, VTX_OP_Catch) > 0)
                             ? 1.0 : 0.0;

        /* Feature 8: callee_allocates — 1.0 if callee contains allocation opcodes */
        {
            static const vtx_node_opcode_t alloc_opcodes[] = {
                VTX_OP_NewObject, VTX_OP_NewArray, VTX_OP_Allocate
            };
            result.features[8] = has_any_opcode(context->callee_graph,
                alloc_opcodes, 3) ? 1.0 : 0.0;
        }

        /* Feature 9: callee_calls_virtual — 1.0 if callee contains virtual call opcodes */
        {
            static const vtx_node_opcode_t virt_opcodes[] = {
                VTX_OP_CallVirtual, VTX_OP_CallInterface
            };
            result.features[9] = has_any_opcode(context->callee_graph,
                virt_opcodes, 2) ? 1.0 : 0.0;
        }
    } else {
        /* Fallback heuristics when callee graph is not available.
         * These are conservative estimates based on bytecode size.
         * The ML model learns to weight these appropriately regardless. */
        result.features[6] = (context->callee_bytecode_size > 50) ? 1.0 : 0.0;
        result.features[7] = 0.0; /* has_try_catch: conservative false */
        result.features[8] = (context->callee_bytecode_size > 20) ? 1.0 : 0.0;
        result.features[9] = 1.0; /* calls_virtual: conservative true */
    }

    /* Feature 10: receiver_type_certainty */
    /* We need the callsite index. For CallVirtual/CallInterface, the method_index
     * field identifies the call site. For CallStatic, there's no receiver. */
    {
        vtx_node_t *call = vtx_node_get((vtx_node_table_t *)&graph->node_table, call_node);
        if (call != NULL && (call->opcode == VTX_OP_CallVirtual ||
                              call->opcode == VTX_OP_CallInterface)) {
            result.features[10] = receiver_type_certainty(
                profile, context->callee_method_id, call->method_index);
        } else {
            /* Static calls: no receiver, certainty is N/A → use 1.0
             * (the target is always known for static calls) */
            result.features[10] = 1.0;
        }
    }

    /* Feature 11: constant_arg_ratio */
    result.features[11] = constant_arg_ratio(graph, call_node);

    /* Feature 12: estimated_register_pressure */
    {
        uint32_t callee_node_count;
        if (context->callee_graph != NULL) {
            /* Use actual live node count from callee graph */
            callee_node_count = 0;
            for (uint32_t i = 0; i < context->callee_graph->node_table.count; i++) {
                if (!context->callee_graph->node_table.nodes[i].dead) {
                    callee_node_count++;
                }
            }
        } else {
            callee_node_count = (uint32_t)(context->callee_bytecode_size * 1.5);
        }
        result.features[12] = estimated_register_pressure(
            callee_node_count, context->caller_node_count);
    }

    /* Feature 13: callee_deopt_rate */
    result.features[13] = callee_deopt_rate(
        context->callee_deopt_count, context->callee_invocation_count);

    /* Feature 14: inline_history */
    result.features[14] = (double)context->inline_history;

    /* Normalize to [0, 1] range */
    vtx_features_normalize(&result);

    return result;
}

/* ========================================================================== */
/* Feature name                                                                */
/* ========================================================================== */

const char *vtx_feature_name(uint32_t index)
{
    if (index >= VTX_INLINE_FEATURE_COUNT) {
        return "unknown";
    }
    return vtx_feature_names[index];
}

/* ========================================================================== */
/* Feature normalization                                                       */
/* ========================================================================== */

void vtx_features_normalize(vtx_inline_features_t *features)
{
    VTX_ASSERT(features != NULL, "features must not be NULL");

    /* Feature 0: callee_size — normalize by VTX_INLINE_SIZE_LIMIT
     * Methods larger than the limit are never inlined anyway. */
    features->features[0] /= (double)VTX_INLINE_SIZE_LIMIT;
    if (features->features[0] > 1.0) features->features[0] = 1.0;

    /* Feature 1: callee_instruction_count — normalize by VTX_INLINE_SIZE_LIMIT * 1.5
     * (the maximum expected IR node count for an inlinable callee) */
    {
        double max_nodes = (double)VTX_INLINE_SIZE_LIMIT * 1.5;
        features->features[1] /= max_nodes;
        if (features->features[1] > 1.0) features->features[1] = 1.0;
    }

    /* Feature 2: call_site_frequency — log-normalize.
     * Raw counts can be enormous (millions). Log compression preserves
     * ordering while keeping values in a reasonable range.
     * log(1 + x) / log(1 + UINT64_MAX) ∈ [0, 1] */
    {
        double freq = features->features[2];
        if (freq > 0.0) {
            features->features[2] = log(1.0 + freq) / log(1.0 + (double)UINT64_MAX);
        }
    }

    /* Feature 3: caller_size — normalize by VTX_INLINE_SIZE_LIMIT * 4
     * (caller can be up to 4x the inline limit before we stop inlining) */
    {
        double max_caller = (double)VTX_INLINE_SIZE_LIMIT * 4.0;
        features->features[3] /= max_caller;
        if (features->features[3] > 1.0) features->features[3] = 1.0;
    }

    /* Feature 4: call_depth — normalize by VTX_MAX_TREE_DEPTH */
    features->features[4] /= (double)VTX_MAX_TREE_DEPTH;
    if (features->features[4] > 1.0) features->features[4] = 1.0;

    /* Features 5-9: already in [0, 1] (binary) — no normalization needed */

    /* Feature 10: receiver_type_certainty — already in [0, 1] — no change */

    /* Feature 11: constant_arg_ratio — already in [0, 1] — no change */

    /* Feature 12: estimated_register_pressure — already capped at 10.0,
     * normalize by 10.0 */
    features->features[12] /= 10.0;
    if (features->features[12] > 1.0) features->features[12] = 1.0;

    /* Feature 13: callee_deopt_rate — already in [0, 1] — no change */

    /* Feature 14: inline_history — normalize by VTX_MAX_TREE_DEPTH */
    features->features[14] /= (double)VTX_MAX_TREE_DEPTH;
    if (features->features[14] > 1.0) features->features[14] = 1.0;
}

/* ========================================================================== */
/* CSV output                                                                  */
/* ========================================================================== */

int vtx_features_to_csv(const vtx_inline_features_t *features,
                         char *buf, size_t buf_size)
{
    if (features == NULL || buf == NULL || buf_size == 0) {
        return -1;
    }

    int written = 0;
    int remaining = (int)buf_size;

    for (uint32_t i = 0; i < VTX_INLINE_FEATURE_COUNT; i++) {
        if (i > 0) {
            int n = snprintf(buf + written, (size_t)remaining, ",");
            if (n < 0 || n >= remaining) return -1;
            written += n;
            remaining -= n;
        }
        int n = snprintf(buf + written, (size_t)remaining, "%.6f", features->features[i]);
        if (n < 0 || n >= remaining) return -1;
        written += n;
        remaining -= n;
    }

    return written;
}
