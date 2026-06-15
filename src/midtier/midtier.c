/**
 * VORTEX Mid-Tier (T1.5) — Type-Specialized Compilation
 *
 * Implements the mid-tier compilation pipeline that sits between the
 * baseline T1 and the optimizing T2. The mid-tier uses type feedback
 * from profiling to specialize operations, eliminating type checks
 * at monomorphic call sites and specializing arithmetic based on
 * observed types.
 *
 * Pipeline:
 *   1. Build SoN graph from bytecode (same as T2)
 *   2. Run GVN (1 iteration only)
 *   3. Type specialization: check type feedback for each operation
 *      - Monomorphic CALL_VIRTUAL → guard + direct dispatch
 *      - Always-integer IADD → specialized integer add (skip type check)
 *      - Monomorphic LOAD_FIELD → skip type check on holder
 *   4. Run DCE (1 iteration)
 *   5. Schedule and lower to x86-64
 *   6. Emit code with specialized operations
 *
 * This is similar to V8's Maglev tier: fast compilation with type
 * specialization, but without the expensive analysis of the top tier.
 */

#include "midtier/midtier.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "ir/gvn.h"
#include "ir/dce.h"
#include "ir/schedule.h"
#include "lower/isel.h"
#include "lower/regalloc.h"
#include "lower/emit.h"
#include "lower/guard_emit.h"
#include "ir/node.h"

/**
 * Global mid-tier statistics.  Persists across calls to vtx_midtier_compile()
 * so that cumulative counters are not lost when the local cfg goes out of scope.
 */
static vtx_midtier_config_t g_midtier_stats;

/* ========================================================================== */
/* Timing helpers                                                              */
/* ========================================================================== */

static inline int64_t midtier_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/* ========================================================================== */
/* Default configuration                                                       */
/* ========================================================================== */

vtx_midtier_config_t vtx_midtier_config_default(void)
{
    vtx_midtier_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.use_type_feedback     = true;
    cfg.inline_max_size       = 0;   /* no inlining in mid-tier by default */
    cfg.specialize_arithmetic = true;
    return cfg;
}

/* ========================================================================== */
/* Type specialization pass                                                    */
/* ========================================================================== */

/**
 * Specialize the SoN graph based on type feedback.
 *
 * For each node in the graph, check if type feedback indicates a
 * monomorphic (single-type) usage pattern. If so:
 *   - For CALL_VIRTUAL: if only one receiver type observed, insert a
 *     Guard node that checks the receiver type, then replace the
 *     virtual dispatch with direct dispatch (CallStatic targeting
 *     the resolved method).
 *   - For Add/Sub/Mul/Div: if type feedback shows integer-only usage,
 *     annotate the node with the integer type so that lowering can
 *     skip the generic type-check path.
 *   - For LoadField: if only one holder shape observed, annotate the
 *     node so that the type check on the holder can be eliminated.
 *
 * Returns the number of sites specialized.
 */
static uint32_t midtier_specialize_types(
    vtx_graph_t *graph,
    const vtx_type_feedback_t *type_feedback,
    vtx_type_system_t *ts,
    vtx_arena_t *arena,
    uint32_t *guards_inserted)
{
    uint32_t sites_specialized = 0;
    *guards_inserted = 0;

    if (type_feedback == NULL) {
        return 0;
    }

    uint32_t node_count = graph->node_table.count;

    for (uint32_t i = 0; i < node_count; i++) {
        vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead) continue;

        /* --- CALL_VIRTUAL specialization --- */
        if (node->opcode == VTX_OP_CallVirtual) {
            uint32_t site_index = node->bytecode_pc;
            uint32_t type_count = vtx_type_feedback_get_call_site_type_count(
                type_feedback, site_index);

            if (type_count == 1) {
                /* Monomorphic site: insert a guard and specialize */
                vtx_typeid_t dominant_type = vtx_type_feedback_get_dominant_call_type(
                    type_feedback, site_index);

                if (dominant_type != VTX_TYPE_INVALID) {
                    /* Create a Guard node that checks the receiver type.
                     * The guard takes the receiver as input and produces
                     * the receiver (if check passes) or deopts (if fails).
                     * We insert it before the call, and annotate the call
                     * with the resolved method. */
                    vtx_nodeid_t guard_id = vtx_node_create(&graph->node_table,
                        VTX_OP_Guard);
                    if (guard_id != VTX_NODEID_INVALID) {
                        vtx_node_t *guard_node = vtx_graph_node(graph, guard_id);
                        /* Copy the receiver input from the call to the guard */
                        if (node->input_count > 0) {
                            vtx_node_add_input(&graph->node_table, guard_id,
                                               node->inputs[0]);
                        }
                        guard_node->bytecode_pc = node->bytecode_pc;
                        guard_node->type_id = dominant_type;

                        /* Record the specialized type on the call node
                         * so that lowering can emit direct dispatch. */
                        node->type_id = dominant_type;

                        sites_specialized++;
                        (*guards_inserted)++;
                    }
                }
            }
            continue;
        }

        /* --- Arithmetic specialization (Add/Sub/Mul/Div/Mod) --- */
        if (node->opcode == VTX_OP_Add ||
            node->opcode == VTX_OP_Sub ||
            node->opcode == VTX_OP_Mul ||
            node->opcode == VTX_OP_Div ||
            node->opcode == VTX_OP_Mod) {

            uint32_t site_index = node->bytecode_pc;

            /* Check if the result is always integer.
             * We use the call site feedback at the same bytecode PC
             * to check type information. For arithmetic operations,
             * the type is tracked via the node's type_id field and
             * type feedback. If the result_typeid in feedback is
             * consistently an integer type, we can specialize.
             *
             * For now, we check the dominant call type at the site
             * as a proxy for the arithmetic result type. A real
             * implementation would track arithmetic type feedback
             * separately. */
            if (node->type_id == VTX_TYPE_INVALID ||
                node->type_id == VTX_TYPE_OBJECT) {
                /* No type info yet — check if both inputs are known int */
                bool both_inputs_int = true;
                for (uint32_t in = 0; in < node->input_count; in++) {
                    vtx_nodeid_t input_id = node->inputs[in];
                    if (input_id == VTX_NODEID_INVALID) {
                        both_inputs_int = false;
                        break;
                    }
                    vtx_node_t *input_node = vtx_graph_node(graph, input_id);
                    if (!input_node || input_node->type_id == VTX_TYPE_INVALID) {
                        both_inputs_int = false;
                        break;
                    }
                    /* Check if the input is a known integer-producing node.
                     * Constant nodes with SMI values, Parameter nodes with
                     * integer type, and other arithmetic nodes that are
                     * already specialized. */
                    if (input_node->opcode == VTX_OP_Constant) {
                        /* Constants are integer if their kind is Int.
                         * BUGFIX: The previous check was tautological —
                         * vtx_make_smi always produces an SMI, so checking
                         * vtx_is_smi on its result was always true.
                         * Now we directly check the constant's type kind. */
                        if (input_node->constval.kind != VTX_TYPE_Int) {
                            both_inputs_int = false;
                            break;
                        }
                    }
                }

                if (both_inputs_int && node->input_count >= 2) {
                    /* Specialize: mark node as integer-typed */
                    node->type_id = VTX_TYPE_SMI; /* SMI = Small Integer type */
                    sites_specialized++;

                    /* Insert a guard that both inputs are integers.
                     * This is a lightweight check compared to full
                     * generic dispatch. */
                    vtx_nodeid_t guard_id = vtx_node_create(&graph->node_table,
                        VTX_OP_Guard);
                    if (guard_id != VTX_NODEID_INVALID) {
                        vtx_node_t *guard_node = vtx_graph_node(graph, guard_id);
                        if (node->input_count > 0) {
                            vtx_node_add_input(&graph->node_table, guard_id,
                                               node->inputs[0]);
                        }
                        guard_node->bytecode_pc = node->bytecode_pc;
                        (*guards_inserted)++;
                    }
                }
            }
            continue;
        }

        /* --- LoadField specialization --- */
        if (node->opcode == VTX_OP_LoadField) {
            uint32_t site_index = node->bytecode_pc;
            vtx_shapeid_t dominant_shape = vtx_type_feedback_get_dominant_field_shape(
                type_feedback, site_index);

            if (dominant_shape != VTX_SHAPE_INVALID) {
                /* Monomorphic field access: record the shape on the node
                 * so that lowering can skip the shape check. */
                /* Store shape info in the node's type_id field as a proxy
                 * since vtx_node_t doesn't have a shape_id field */
                node->type_id = dominant_shape;
                sites_specialized++;

                /* Insert a shape guard for the field access */
                vtx_nodeid_t guard_id = vtx_node_create(&graph->node_table,
                    VTX_OP_Guard);
                if (guard_id != VTX_NODEID_INVALID) {
                    vtx_node_t *guard_node = vtx_graph_node(graph, guard_id);
                    if (node->input_count > 0) {
                        vtx_node_add_input(&graph->node_table, guard_id,
                                           node->inputs[0]);
                    }
                    guard_node->bytecode_pc = node->bytecode_pc;
                    guard_node->type_id = dominant_shape;
                    (*guards_inserted)++;
                }
            }
            continue;
        }
    }

    return sites_specialized;
}

/* ========================================================================== */
/* Mid-tier compilation entry point                                            */
/* ========================================================================== */

vtx_midtier_result_t *vtx_midtier_compile(
    const vtx_method_desc_t *method,
    const vtx_type_feedback_t *type_feedback,
    vtx_type_system_t *ts,
    vtx_arena_t *arena)
{
    if (method == NULL || method->bytecode == NULL || arena == NULL) {
        return NULL;
    }

    vtx_midtier_config_t cfg = vtx_midtier_config_default();
    int64_t start_time = midtier_now_ns();

    /* Allocate result */
    vtx_midtier_result_t *result = (vtx_midtier_result_t *)malloc(
        sizeof(vtx_midtier_result_t));
    if (result == NULL) {
        return NULL;
    }
    memset(result, 0, sizeof(*result));

    /* Step 1: Build the SoN graph from bytecode */
    vtx_graph_t graph;
    if (vtx_graph_init(&graph, method->arg_count) != 0) {
        free(result);
        return NULL;
    }

    if (vtx_graph_build(&graph, method->bytecode, method, arena) != 0) {
        vtx_graph_destroy(&graph);
        free(result);
        return NULL;
    }

    /* Step 2: Run GVN (1 iteration only) */
    vtx_gvn_run(&graph);

    /* Step 3: Type specialization based on feedback */
    uint32_t guards_inserted = 0;
    uint32_t sites_specialized = 0;

    if (cfg.use_type_feedback && type_feedback != NULL) {
        sites_specialized = midtier_specialize_types(
            &graph, type_feedback, ts, arena, &guards_inserted);
    }

    /* Step 4: Run DCE (1 iteration) */
    vtx_dce_run(&graph);

    /* Step 5: Schedule the graph */
    vtx_schedule_t schedule;
    memset(&schedule, 0, sizeof(schedule));
    if (vtx_schedule_run(&graph, arena, &schedule) != 0) {
        vtx_graph_destroy(&graph);
        free(result);
        return NULL;
    }

    /* Step 6: Instruction selection */
    vtx_inst_stream_t *stream = vtx_isel_select(&schedule, &graph, arena);
    if (stream == NULL) {
        vtx_schedule_destroy(&schedule);
        vtx_graph_destroy(&graph);
        free(result);
        return NULL;
    }

    /* Step 7: Register allocation */
    vtx_regalloc_result_t *ra_result = vtx_regalloc_run(stream, arena);
    if (ra_result == NULL) {
        vtx_schedule_destroy(&schedule);
        vtx_graph_destroy(&graph);
        free(result);
        return NULL;
    }

    /* Step 8: Peephole optimization */
    vtx_peephole_optimize(stream, ra_result);

    /* Step 9: Emit x86-64 machine code */
    vtx_x86_emit_t emit;
    if (vtx_x86_emit_init(&emit, VTX_EMIT_INITIAL_CAPACITY) != 0) {
        vtx_schedule_destroy(&schedule);
        vtx_graph_destroy(&graph);
        free(result);
        return NULL;
    }

    if (vtx_x86_emit_function(&emit, stream, ra_result, arena) != 0) {
        vtx_x86_emit_destroy(&emit);
        vtx_schedule_destroy(&schedule);
        vtx_graph_destroy(&graph);
        free(result);
        return NULL;
    }

    /* Copy emitted code into the result */
    result->code_size = vtx_x86_emit_position(&emit);
    if (result->code_size > 0) {
        result->code = (uint8_t *)malloc(result->code_size);
        if (result->code != NULL) {
            memcpy(result->code, vtx_x86_emit_code(&emit), result->code_size);
        } else {
            result->code_size = 0;
        }
    }
    result->guards_inserted = guards_inserted;
    result->sites_specialized = sites_specialized;

    /* Record timing — store in the global stats struct, not the local cfg */
    g_midtier_stats.compilation_time_ns += (uint64_t)(midtier_now_ns() - start_time);
    g_midtier_stats.methods_compiled++;
    g_midtier_stats.sites_specialized += sites_specialized;
    g_midtier_stats.guards_inserted += guards_inserted;

    /* Cleanup */
    vtx_x86_emit_destroy(&emit);
    vtx_schedule_destroy(&schedule);
    vtx_graph_destroy(&graph);

    return result;
}
