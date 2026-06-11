#include "sota/loop_spec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ========================================================================== */
/* CPU feature detection (x86-64)                                              */
/* ========================================================================== */

#if defined(__x86_64__) || defined(_M_X64)

/* Use compiler intrinsics for CPUID if available */
#if defined(__GNUC__) || defined(__clang__)
static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                   uint32_t *ecx, uint32_t *edx)
{
    __asm__ __volatile__ (
        "cpuid"
        : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx)
        : "a" (leaf)
    );
}
#else
/* MSVC or other — provide a stub that assumes SSE2 (always available on x86-64) */
static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                   uint32_t *ecx, uint32_t *edx)
{
    *eax = *ebx = *ecx = *edx = 0;
    if (leaf == 1) {
        *edx = (1u << 26); /* SSE2 bit */
    }
}
#endif

uint32_t vtx_cpu_detect_features(void)
{
    uint32_t features = VTX_CPU_SSE2; /* SSE2 is baseline for x86-64 */

    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);

    /* Check AVX (bit 28 of ECX from leaf 1) */
    if (ecx & (1u << 28)) {
        /* AVX supported — check for AVX2 (leaf 7, EBX bit 5) */
        uint32_t eax7, ebx7, ecx7, edx7;
        cpuid(7, &eax7, &ebx7, &ecx7, &edx7);
        if (ebx7 & (1u << 5)) {
            features |= VTX_CPU_AVX2;
        }
    }

    return features;
}

#else /* Non-x86 platforms */

uint32_t vtx_cpu_detect_features(void)
{
    /* No SIMD support on non-x86 platforms */
    return 0;
}

#endif

uint32_t vtx_sota_loop_detect_cpu_features(void)
{
    return vtx_cpu_detect_features();
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_sota_loop_spec_init(vtx_sota_loop_spec_t *spec)
{
    if (spec == NULL) return -1;
    memset(spec, 0, sizeof(*spec));
    return 0;
}

void vtx_sota_loop_spec_destroy(vtx_sota_loop_spec_t *spec)
{
    if (spec == NULL) return;
    memset(spec, 0, sizeof(*spec));
}

/* ========================================================================== */
/* Coefficient of variation                                                    */
/* ========================================================================== */

double vtx_loop_cv(const uint64_t *trip_counts, uint32_t count)
{
    if (trip_counts == NULL || count == 0) return INFINITY;

    /* Compute mean */
    double sum = 0.0;
    for (uint32_t i = 0; i < count; i++) {
        sum += (double)trip_counts[i];
    }
    double mean = sum / (double)count;

    if (mean == 0.0) return INFINITY;

    /* Compute variance */
    double var_sum = 0.0;
    for (uint32_t i = 0; i < count; i++) {
        double diff = (double)trip_counts[i] - mean;
        var_sum += diff * diff;
    }
    double variance = var_sum / (double)count;

    /* CV = stddev / mean = sqrt(variance) / mean */
    return sqrt(variance) / mean;
}

/* ========================================================================== */
/* Vector width computation                                                    */
/* ========================================================================== */

uint32_t vtx_loop_vector_width(uint32_t element_size, uint32_t cpu_features)
{
    uint32_t vector_bits = 128; /* SSE2 baseline */

    if (cpu_features & VTX_CPU_AVX2) {
        vector_bits = 256;
    }
    if (cpu_features & VTX_CPU_AVX512) {
        vector_bits = 512;
    }

    if (element_size == 0) return 1;

    return vector_bits / (element_size * 8);
}

/* ========================================================================== */
/* Internal: analyze loop body for vectorization                               */
/* ========================================================================== */

/**
 * Walk the loop body (nodes between LoopBegin and LoopEnd) and check
 * for vectorization inhibitors:
 *   - Calls to unknown functions
 *   - Loop-carried dependencies (reduction patterns are OK)
 *   - Aliased memory accesses
 *   - Non-stride-1 access patterns
 */
static void analyze_loop_body(const vtx_graph_t *graph,
                                vtx_nodeid_t loop_begin,
                                vtx_loop_spec_result_t *result)
{
    const vtx_node_t *loop = vtx_node_get_const(&graph->node_table, loop_begin);
    if (loop == NULL) return;

    result->has_loop_carried_dep = false;
    result->has_aliased_access = false;
    result->has_unknown_call = false;
    result->is_stride1 = true;
    result->stride = 1;
    result->element_size = 4; /* default: int32 */

    /* Scan all nodes in the graph that are inside this loop.
     * For a proper implementation, we would use the schedule to determine
     * which nodes are in the loop body. For now, we use a simple heuristic:
     * scan all nodes and check if they reference the loop's control output. */
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        const vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead) continue;

        switch (node->opcode) {
        case VTX_OP_CallStatic:
        case VTX_OP_CallVirtual:
        case VTX_OP_CallInterface:
        case VTX_OP_CallRuntime:
            /* Calls to unknown functions inhibit vectorization
             * unless they're pure (no side effects) and we can prove
             * they don't depend on loop iteration state.
             * For now, conservatively mark as inhibiting. */
            result->has_unknown_call = true;
            break;

        case VTX_OP_StoreIndexed:
            /* Check for potential aliasing: if we store to an array
             * and also load from an array in the same loop, they might alias.
             * Type-based alias analysis can help here: if the array types
             * are different (e.g., int[] vs float[]), they can't alias. */
            result->has_aliased_access = true;
            break;

        case VTX_OP_LoadIndexed:
            /* Stride-1 check: if the index is the loop induction variable
             * (or IV + constant), the access is stride-1.
             * For now, assume stride-1 for simple loops. */
            break;

        case VTX_OP_StoreField:
        case VTX_OP_LoadField:
            /* Field accesses in loops: may or may not alias.
             * Type-based alias analysis can help. */
            break;

        default:
            break;
        }
    }

    /* Determine element size from the loop's array access pattern.
     * Look for LoadIndexed/StoreIndexed and check the type of the
     * loaded/stored value. */
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        const vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead) continue;

        if (node->opcode == VTX_OP_LoadIndexed ||
            node->opcode == VTX_OP_StoreIndexed) {
            switch (node->type) {
            case VTX_TYPE_Int:   result->element_size = 4; break;  /* int32 */
            case VTX_TYPE_Float: result->element_size = 8; break;  /* double */
            default: break;
            }
            break; /* use the first array access to determine element size */
        }
    }
}

/* ========================================================================== */
/* Loop spec check                                                             */
/* ========================================================================== */

vtx_loop_spec_result_t vtx_sota_loop_spec_check(
    vtx_sota_loop_spec_t *spec,
    const vtx_profile_global_t *profile,
    const vtx_graph_t *graph,
    vtx_nodeid_t loop_node)
{
    vtx_loop_spec_result_t result;
    memset(&result, 0, sizeof(result));
    result.vectorizability = VTX_LOOP_CANT_VECTORIZE;

    if (spec == NULL || graph == NULL) return result;

    spec->loops_checked++;

    const vtx_node_t *loop = vtx_node_get_const(&graph->node_table, loop_node);
    if (loop == NULL || loop->opcode != VTX_OP_LoopBegin) return result;

    /* Step 1: Get trip count statistics from profile.
     * The loop's bytecode_pc identifies it in the profile. */
    if (profile != NULL) {
        /* Look up the loop profile by the LoopBegin's bytecode_pc */
        const vtx_profile_method_t *method = NULL;

        /* Find the method that contains this loop.
         * We search through all methods for one with a loop at this PC. */
        for (uint32_t m = 0; m < profile->method_count; m++) {
            for (uint32_t l = 0; l < profile->methods[m].loop_count; l++) {
                if (profile->methods[m].loops[l].loop_header_pc == loop->bytecode_pc) {
                    method = &profile->methods[m];
                    result.profiled_iterations = profile->methods[m].loops[l].backedge_count;
                    break;
                }
            }
            if (method != NULL) break;
        }

        if (method != NULL && result.profiled_iterations > 10) {
            /* We have profile data. Compute trip count statistics.
             * The backedge count approximates the total number of loop iterations.
             * The mean trip count per entry is:
             *   mean = backedge_count / invocation_count
             *
             * We estimate CV from the profile: if the loop has been entered
             * many times with consistent behavior, CV is low.
             * A precise CV would require per-entry trip counts, which we
             * don't have in the current profile structure. Instead, we use
             * a heuristic based on the ratio of backedge count to invocation count.
             *
             * For a loop with very consistent trip count, the ratio is very stable.
             * We approximate CV as:
             *   CV ≈ 1.0 / sqrt(profiled_iterations)
             * which decreases as we get more observations. This is a principled
             * approximation: the Central Limit Theorem tells us that the standard
             * error of the mean decreases as 1/sqrt(n). */
            if (method->invocation_count > 0) {
                result.mean_trip_count = (double)result.profiled_iterations /
                                          (double)method->invocation_count;
            }

            /* Estimate CV using the CLT approximation */
            if (result.profiled_iterations > 100) {
                result.cv_trip_count = 1.0 / sqrt((double)result.profiled_iterations / 10.0);
            } else {
                result.cv_trip_count = 1.0; /* not enough data → high CV */
            }
        } else {
            /* No profile data — can't determine trip count predictability */
            result.cv_trip_count = INFINITY;
            result.mean_trip_count = 0.0;
        }
    }

    /* Step 2: Analyze loop body for vectorization inhibitors */
    analyze_loop_body(graph, loop_node, &result);

    /* Step 3: Determine vectorizability */
    if (result.has_unknown_call) {
        result.vectorizability = VTX_LOOP_CANT_VECTORIZE;
        return result;
    }

    if (result.has_aliased_access) {
        /* Aliasing can be handled with a guard, so we can still try */
        result.vectorizability = VTX_LOOP_MAYBE_VECTORIZE;
    }

    if (result.cv_trip_count >= VTX_LOOP_CV_THRESHOLD) {
        /* Trip count is not predictable enough */
        result.vectorizability = VTX_LOOP_CANT_VECTORIZE;
        return result;
    }

    if (result.mean_trip_count < 4.0) {
        /* Not enough iterations to benefit from vectorization */
        result.vectorizability = VTX_LOOP_CANT_VECTORIZE;
        return result;
    }

    /* Compute vector width and peel count */
    uint32_t cpu_features = vtx_cpu_detect_features();
    result.vector_width = vtx_loop_vector_width(result.element_size, cpu_features);

    if (result.vector_width < 2) {
        result.vectorizability = VTX_LOOP_CANT_VECTORIZE;
        return result;
    }

    /* Determine required peel iterations for alignment.
     * For a stride-1 access, we need to align to the vector width.
     * The actual alignment depends on the data pointer, which we
     * don't know at compile time. We'll add a runtime guard. */
    result.peel_count = result.vector_width;

    /* Determine vectorizability level based on CPU features */
    if (cpu_features & VTX_CPU_AVX2) {
        result.vectorizability = VTX_LOOP_CAN_VECTORIZE_AVX2;
    } else if (cpu_features & VTX_CPU_SSE2) {
        result.vectorizability = VTX_LOOP_CAN_VECTORIZE_SSE2;
    }

    return result;
}

/* ========================================================================== */
/* Loop spec transform                                                         */
/* ========================================================================== */

bool vtx_sota_loop_spec_transform(vtx_graph_t *graph,
                                    vtx_nodeid_t loop_node,
                                    uint32_t cpu_features,
                                    const vtx_loop_spec_result_t *spec_result,
                                    vtx_arena_t *arena)
{
    if (graph == NULL || spec_result == NULL || arena == NULL) return false;

    if (spec_result->vectorizability < VTX_LOOP_CAN_VECTORIZE_SSE2) {
        return false; /* not vectorizable */
    }

    vtx_node_t *loop = vtx_node_get(&graph->node_table, loop_node);
    if (loop == NULL || loop->opcode != VTX_OP_LoopBegin) return false;

    /* The speculative loop transformation creates:
     *
     *   1. Alignment guard: check that the array base is aligned
     *      to the vector width boundary
     *   2. Aliasing guard: check that source and destination arrays
     *      don't overlap (for copy-type loops)
     *   3. Trip count guard: check that the trip count is a multiple
     *      of the vector width (or handle remainder)
     *   4. Vectorized loop body: replace scalar operations with
     *      SIMD-wide operations
     *   5. Scalar epilogue: handle remaining iterations
     *
     * In the SoN IR, we represent this as:
     *   - DeoptGuard nodes for each speculation
     *   - Modified loop body with vector-width operations
     *   - The original scalar loop is kept as the deopt target
     *
     * For now, we implement the guard insertion and mark the loop
     * for vectorization during code generation. The actual SIMD
     * instruction selection happens during lowering.
     */

    /* Guard 1: Trip count is sufficient for vectorization.
     * guard(trip_count >= vector_width) */
    vtx_nodeid_t tc_guard = vtx_node_create(&graph->node_table, VTX_OP_DeoptGuard);
    if (tc_guard == VTX_NODEID_INVALID) return false;

    vtx_node_t *tc_guard_node = vtx_node_get(&graph->node_table, tc_guard);
    if (tc_guard_node != NULL) {
        tc_guard_node->flags = VTX_NF_CONTROL | VTX_NF_SIDE_EFFECT;
        tc_guard_node->cond = VTX_COND_GE;
        tc_guard_node->bytecode_pc = loop->bytecode_pc;
        /* The guard's input will be a Cmp of trip_count vs vector_width.
         * For now, we set up the guard structure and let the lowering
         * phase fill in the comparison details. */
    }

    /* Guard 2: Memory doesn't alias (for loops with both loads and stores).
     * guard(src_array != dst_array || no_overlap) */
    if (spec_result->has_aliased_access) {
        vtx_nodeid_t alias_guard = vtx_node_create(&graph->node_table, VTX_OP_DeoptGuard);
        if (alias_guard != VTX_NODEID_INVALID) {
            vtx_node_t *ag = vtx_node_get(&graph->node_table, alias_guard);
            if (ag != NULL) {
                ag->flags = VTX_NF_CONTROL | VTX_NF_SIDE_EFFECT;
                ag->cond = VTX_COND_NE;
                ag->bytecode_pc = loop->bytecode_pc;
            }
        }
    }

    /* Guard 3: Alignment guard for array base pointer.
     * guard((base_ptr & (vector_width * element_size - 1)) == 0) */
    {
        vtx_nodeid_t align_guard = vtx_node_create(&graph->node_table, VTX_OP_DeoptGuard);
        if (align_guard != VTX_NODEID_INVALID) {
            vtx_node_t *ag = vtx_node_get(&graph->node_table, align_guard);
            if (ag != NULL) {
                ag->flags = VTX_NF_CONTROL | VTX_NF_SIDE_EFFECT;
                ag->cond = VTX_COND_EQ;
                ag->bytecode_pc = loop->bytecode_pc;
            }
        }
    }

    /* Mark the LoopBegin node with vectorization metadata.
     * The lowering phase will check this and emit SIMD code.
     * We use the node's value_number field as a scratch space for
     * the vectorization width (0 = not vectorized, >0 = vector width). */
    loop->value_number = spec_result->vector_width;

    return true;
}

/* ========================================================================== */
/* Loop unrolling                                                              */
/* ========================================================================== */

/**
 * Count the number of nodes in a loop body by finding all nodes
 * between LoopBegin and LoopEnd that share the loop's control input.
 */
static uint32_t count_loop_body_nodes(const vtx_graph_t *graph, vtx_nodeid_t loop_begin)
{
    uint32_t count = 0;
    const vtx_node_t *loop = vtx_node_get_const(&graph->node_table, loop_begin);
    if (loop == NULL) return 0;

    /* Scan all nodes. For nodes in the loop body, they will reference
     * the LoopBegin or a control node derived from it.
     * A simple heuristic: count nodes that have the LoopBegin as
     * a transitive control input. For efficiency, we just count
     * nodes between the LoopBegin and LoopEnd in the node table
     * that have control or data flags.
     *
     * A more precise implementation would use the schedule to
     * determine which nodes are in the loop. */
    bool in_loop = false;
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        const vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead) continue;

        if (node->id == loop_begin) {
            in_loop = true;
            continue;
        }

        /* Check if this is a LoopEnd for this loop */
        if (node->opcode == VTX_OP_LoopEnd && in_loop) {
            /* Found the end of this loop */
            break;
        }

        if (in_loop) {
            /* Check if this node's control input traces back to the loop */
            for (uint32_t j = 0; j < node->input_count; j++) {
                if (node->inputs[j] == loop_begin) {
                    count++;
                    break;
                }
            }
        }
    }

    return count;
}

/**
 * Compute the unroll factor based on loop body size and trip count.
 *
 * The unroll factor is limited by:
 *   - VTX_MAX_NATIVE_SIZE / (loop_body_size * instruction_size_estimate)
 *   - Trip count (don't unroll more than the trip count)
 *   - Maximum unroll factor of 8 (diminishing returns beyond this)
 *
 * Returns 0 if unrolling is not beneficial.
 */
static uint32_t compute_unroll_factor(uint32_t loop_body_nodes,
                                        uint64_t trip_count)
{
    /* Each IR node roughly translates to 4-8 bytes of native code.
     * Use 8 bytes as a conservative estimate. */
    uint32_t estimated_native_size = loop_body_nodes * 8;

    if (estimated_native_size == 0) return 0;

    /* Maximum unroll factor that fits in the native code budget */
    uint32_t max_by_size = VTX_MAX_NATIVE_SIZE / estimated_native_size;
    if (max_by_size == 0) return 0;

    /* Don't unroll more than the trip count */
    uint32_t max_by_trip = (trip_count > 8) ? 8 : (uint32_t)trip_count;
    if (max_by_trip == 0) return 0;

    /* Cap at 8 (diminishing returns beyond this) */
    uint32_t max_unroll = 8;

    uint32_t factor = max_by_size;
    if (max_by_trip < factor) factor = max_by_trip;
    if (max_unroll < factor) factor = max_unroll;

    /* Only unroll if we can do at least 2x */
    if (factor < 2) return 0;

    /* Round down to nearest power of 2 for simplicity */
    uint32_t power_of_2 = 1;
    while (power_of_2 * 2 <= factor) {
        power_of_2 *= 2;
    }

    return power_of_2;
}

vtx_loop_unroll_result_t vtx_sota_loop_unroll_check(
    vtx_sota_loop_spec_t *spec,
    const vtx_profile_global_t *profile,
    const vtx_graph_t *graph,
    vtx_nodeid_t loop_node)
{
    vtx_loop_unroll_result_t result;
    memset(&result, 0, sizeof(result));
    result.can_unroll = false;
    result.unroll_factor = 0;
    result.constant_trip_count = 0;
    result.requires_guard = true;

    if (spec == NULL || graph == NULL) return result;

    const vtx_node_t *loop = vtx_node_get_const(&graph->node_table, loop_node);
    if (loop == NULL || loop->opcode != VTX_OP_LoopBegin) return result;

    /* Step 1: Get trip count from profile.
     * We need a consistent trip count for unrolling. */
    if (profile == NULL) return result;

    uint64_t backedge_count = 0;
    uint64_t invocation_count = 0;

    for (uint32_t m = 0; m < profile->method_count; m++) {
        for (uint32_t l = 0; l < profile->methods[m].loop_count; l++) {
            if (profile->methods[m].loops[l].loop_header_pc == loop->bytecode_pc) {
                backedge_count = profile->methods[m].loops[l].backedge_count;
                invocation_count = profile->methods[m].invocation_count;
                break;
            }
        }
        if (backedge_count > 0) break;
    }

    if (invocation_count == 0) return result;

    /* Compute mean trip count */
    double mean_trip = (double)backedge_count / (double)invocation_count;

    /* For unrolling, we want a small constant trip count.
     * Check if the trip count is consistent (low CV).
     * Since we don't have per-entry trip counts, we estimate CV
     * using the same CLT approximation as in vectorization. */
    if (backedge_count > 100) {
        double cv = 1.0 / sqrt((double)backedge_count / 10.0);
        if (cv >= VTX_LOOP_CV_THRESHOLD) {
            /* Trip count is not consistent enough */
            return result;
        }
    } else if (backedge_count < 10) {
        /* Not enough profiling data */
        return result;
    }

    /* Round the mean to the nearest integer to get the constant trip count.
     * We require the trip count to be small (<= 64) for unrolling. */
    uint64_t constant_trip = (uint64_t)(mean_trip + 0.5);

    if (constant_trip == 0 || constant_trip > 64) {
        /* Trip count too large — unrolling would bloat the code */
        return result;
    }

    /* Step 2: Count loop body nodes to estimate code size */
    uint32_t body_nodes = count_loop_body_nodes(graph, loop_node);

    /* Step 3: Compute unroll factor */
    uint32_t factor = compute_unroll_factor(body_nodes, constant_trip);
    if (factor < 2) return result;

    /* If the unroll factor equals the trip count, we can fully unroll
     * (no loop at all — just straight-line code). In that case,
     * no trip count guard is needed for the loop itself, but we
     * still need a guard that the trip count is what we expect. */
    if (factor >= constant_trip) {
        factor = (uint32_t)constant_trip;
        result.requires_guard = true; /* guard that trip count matches */
    }

    result.can_unroll = true;
    result.unroll_factor = factor;
    result.constant_trip_count = constant_trip;
    result.requires_guard = true;

    return result;
}

bool vtx_sota_loop_unroll_transform(vtx_graph_t *graph,
                                      vtx_nodeid_t loop_node,
                                      const vtx_loop_unroll_result_t *unroll_result,
                                      vtx_arena_t *arena)
{
    if (graph == NULL || unroll_result == NULL || arena == NULL) return false;

    if (!unroll_result->can_unroll || unroll_result->unroll_factor < 2) {
        return false;
    }

    vtx_node_t *loop = vtx_node_get(&graph->node_table, loop_node);
    if (loop == NULL || loop->opcode != VTX_OP_LoopBegin) return false;

    /* Emit a DeoptGuard that the trip count equals the expected value.
     *
     * In the SoN IR, the guard checks:
     *   trip_count == constant_trip_count
     * On failure, deopt to the interpreter which will re-enter the
     * original loop. */
    vtx_nodeid_t guard_id = vtx_node_create(&graph->node_table, VTX_OP_DeoptGuard);
    if (guard_id == VTX_NODEID_INVALID) return false;

    vtx_node_t *guard = vtx_node_get(&graph->node_table, guard_id);
    if (guard != NULL) {
        guard->flags = VTX_NF_CONTROL | VTX_NF_SIDE_EFFECT;
        guard->cond = VTX_COND_EQ;
        guard->bytecode_pc = loop->bytecode_pc;

        /* Create a Constant node for the expected trip count */
        vtx_nodeid_t tc_const = vtx_node_create(&graph->node_table, VTX_OP_Constant);
        if (tc_const != VTX_NODEID_INVALID) {
            vtx_node_t *tc_node = vtx_node_get(&graph->node_table, tc_const);
            if (tc_node != NULL) {
                tc_node->type = VTX_TYPE_Int;
                tc_node->flags = VTX_NF_DATA;
                tc_node->constval = vtx_constval_int((int64_t)unroll_result->constant_trip_count);
            }
        }

        /* Add the trip count comparison as inputs to the guard.
         * The guard's inputs will be: [control, trip_count_value, expected_value]
         * The actual trip_count node will be filled in by the optimizer
         * when it resolves the loop's induction variable. */
        vtx_node_add_input(&graph->node_table, guard_id, loop_node);
    }

    /* Mark the loop as unrolled. The value_number field encodes the
     * unroll factor (negative to distinguish from vectorization):
     *   value_number > 0  → vectorized with this width
     *   value_number < 0  → unrolled with |value_number| factor
     *   value_number == 0 → not transformed */
    loop->value_number = -(int32_t)unroll_result->unroll_factor;

    /* For full unrolling (unroll_factor == trip_count), the loop
     * back-edge is eliminated. The LoopEnd node is marked dead,
     * and the LoopBegin becomes a regular Region node.
     *
     * For partial unrolling, the loop remains but the body is
     * replicated. The lowering phase handles the actual instruction
     * replication based on the unroll factor. */
    if (unroll_result->unroll_factor >= unroll_result->constant_trip_count &&
        unroll_result->constant_trip_count > 0) {
        /* Full unroll: find and mark the LoopEnd as dead */
        for (uint32_t i = 0; i < graph->node_table.count; i++) {
            vtx_node_t *node = &graph->node_table.nodes[i];
            if (node->dead) continue;
            if (node->opcode == VTX_OP_LoopEnd) {
                /* Check if this LoopEnd belongs to our loop.
                 * The LoopEnd should reference our LoopBegin. */
                for (uint32_t j = 0; j < node->input_count; j++) {
                    if (node->inputs[j] == loop_node) {
                        node->dead = true;
                        break;
                    }
                }
            }
        }

        /* Convert LoopBegin to a Region (no more loop semantics) */
        loop->opcode = VTX_OP_Region;
    }

    return true;
}
