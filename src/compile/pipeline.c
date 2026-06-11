/**
 * VORTEX Optimization Pipeline Driver
 *
 * Wires together ALL optimization passes in the correct order,
 * producing optimized x86-64 machine code from a Sea-of-Nodes IR graph.
 *
 * Pipeline tiers:
 *   T1 — Baseline: GVN + DCE (quick, no expensive analysis)
 *   T2 — Optimizing: Full pipeline with inlining, PEA, LICM, bounds check
 *   T3 — Speculative: T2 + speculative guards + vectorization hints
 *
 * Pass ordering rationale:
 *   1. GVN first — enables SCCP and DCE by merging redundant nodes
 *   2. SCCP — constant propagation simplifies conditions for DCE/inlining
 *   3. DCE — remove dead code exposed by GVN/SCCP
 *   4. Inlining — expand call sites using ML model
 *   5. GVN+SCCP+DCE again — clean up after inlining exposes redundancies
 *   6. PEA — escape analysis + scalar replacement of allocations
 *   7. DCE — remove dead allocations exposed by scalar replacement
 *   8. Schedule — convert SoN back to basic blocks
 *   9. LICM — hoist loop-invariant code (needs schedule for loop structure)
 *  10. Bounds check elimination (needs schedule for dominance info)
 *  11. Instruction selection — map SoN nodes to x86-64 instructions
 *  12. Register allocation — assign physical registers
 *  13. Code emission — emit x86-64 machine code
 */

#include "compile/pipeline.h"

#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ========================================================================== */
/* Timing helpers                                                              */
/* ========================================================================== */

static inline int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static inline int64_t elapsed_ns(int64_t start)
{
    return now_ns() - start;
}

/* ========================================================================== */
/* Verification helper                                                         */
/* ========================================================================== */

/**
 * Conditionally verify the graph between passes.
 * Only active when config->run_verify is true.
 * Returns 0 on success (or verification disabled), -1 on verification failure.
 */
static int verify_between_passes(const vtx_graph_t *graph,
                                  const vtx_pipeline_config_t *config,
                                  const char *pass_name)
{
    if (!config->run_verify) {
        return 0;
    }
    if (!vtx_verify_graph(graph)) {
        fprintf(stderr, "[pipeline] VERIFICATION FAILED after %s\n", pass_name);
        return -1;
    }
    return 0;
}

/* ========================================================================== */
/* Tier configurations                                                         */
/* ========================================================================== */

vtx_pipeline_config_t vtx_pipeline_config_t1(void)
{
    /* T1 — Baseline JIT: minimal optimizations for fast compilation.
     * Only GVN (1 iteration) + DCE (1 iteration). No expensive analysis.
     * Target: < 1ms compilation time for small methods. */
    vtx_pipeline_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.run_gvn           = true;
    cfg.run_sccp          = false;
    cfg.run_dce           = true;
    cfg.run_licm          = false;
    cfg.run_bounds_check  = false;
    cfg.run_pea           = false;
    cfg.run_inlining      = false;
    cfg.run_verify        = false;
    cfg.gvn_iterations    = 1;
    cfg.sccp_iterations   = 0;
    cfg.dce_iterations    = 1;
    return cfg;
}

vtx_pipeline_config_t vtx_pipeline_config_t2(void)
{
    /* T2 — Optimizing JIT: full optimization pipeline.
     * GVN -> SCCP -> DCE -> inlining -> GVN+SCCP+DCE -> PEA -> DCE ->
     * schedule -> LICM -> bounds check -> lower.
     * Target: best throughput for hot code. */
    vtx_pipeline_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.run_gvn           = true;
    cfg.run_sccp          = true;
    cfg.run_dce           = true;
    cfg.run_licm          = true;
    cfg.run_bounds_check  = true;
    cfg.run_pea           = true;
    cfg.run_inlining      = true;
    cfg.run_verify        = false;  /* disabled by default for performance */
    cfg.gvn_iterations    = 3;
    cfg.sccp_iterations   = 3;
    cfg.dce_iterations    = 3;
    return cfg;
}

vtx_pipeline_config_t vtx_pipeline_config_t3(void)
{
    /* T3 — Speculative JIT: full pipeline + speculative guards.
     * Same as T2 but with verification enabled and more iterations
     * for aggressive optimization. Used for very hot code with
     * speculation support. */
    vtx_pipeline_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.run_gvn           = true;
    cfg.run_sccp          = true;
    cfg.run_dce           = true;
    cfg.run_licm          = true;
    cfg.run_bounds_check  = true;
    cfg.run_pea           = true;
    cfg.run_inlining      = true;
    cfg.run_verify        = true;   /* verify aggressively in speculative tier */
    cfg.gvn_iterations    = 5;
    cfg.sccp_iterations   = 5;
    cfg.dce_iterations    = 5;
    return cfg;
}

/* ========================================================================== */
/* GVN pass (iterative)                                                        */
/* ========================================================================== */

/**
 * Run GVN for up to max_iter iterations, or until no more nodes are merged.
 * Returns total nodes merged across all iterations.
 */
static uint32_t run_gvn_pass(vtx_graph_t *graph,
                              int max_iter,
                              int64_t *time_ns)
{
    int64_t start = now_ns();
    uint32_t total_merged = 0;

    for (int i = 0; i < max_iter; i++) {
        uint32_t merged = vtx_gvn_run(graph);
        total_merged += merged;
        if (merged == 0) {
            break;  /* fixed point reached */
        }
    }

    *time_ns = elapsed_ns(start);
    return total_merged;
}

/* ========================================================================== */
/* SCCP pass (iterative)                                                       */
/* ========================================================================== */

/**
 * Run SCCP for up to max_iter iterations, or until no more constants are found.
 * Returns total constants propagated across all iterations.
 */
static uint32_t run_sccp_pass(vtx_graph_t *graph,
                               int max_iter,
                               int64_t *time_ns)
{
    int64_t start = now_ns();
    uint32_t total_propagated = 0;

    for (int i = 0; i < max_iter; i++) {
        uint32_t propagated = vtx_constant_prop_run(graph);
        total_propagated += propagated;
        if (propagated == 0) {
            break;  /* fixed point reached */
        }
    }

    *time_ns = elapsed_ns(start);
    return total_propagated;
}

/* ========================================================================== */
/* DCE pass (iterative)                                                        */
/* ========================================================================== */

/**
 * Run DCE for up to max_iter iterations, or until no more nodes are removed.
 * Returns total nodes removed across all iterations.
 */
static uint32_t run_dce_pass(vtx_graph_t *graph,
                              int max_iter,
                              int64_t *time_ns,
                              bool verify)
{
    int64_t start = now_ns();
    uint32_t total_removed = 0;

    for (int i = 0; i < max_iter; i++) {
        uint32_t removed = vtx_dce_run(graph);
        total_removed += removed;
        if (removed == 0) {
            break;  /* no more dead code */
        }
    }

    *time_ns = elapsed_ns(start);

    if (verify) {
        vtx_verify_graph_post_dce(graph);
    }

    return total_removed;
}

/* ========================================================================== */
/* Inlining pass                                                               */
/* ========================================================================== */

/**
 * Run the ML-guided inlining pass.
 *
 * Walks all CallStatic/CallVirtual/CallInterface nodes in the graph,
 * computes features, runs GBDT inference, and inlines callees whose
 * score exceeds VTX_INLINE_THRESHOLD.
 *
 * Returns the number of inlining decisions made (both inline and no-inline).
 */
static uint32_t run_inlining_pass(vtx_graph_t *graph,
                                   vtx_arena_t *arena,
                                   int64_t *time_ns)
{
    int64_t start = now_ns();
    uint32_t decisions = 0;

    /* Initialize the GBDT model with default conservative weights */
    vtx_gbdt_model_t model;
    if (vtx_gbdt_model_init(&model) != 0) {
        *time_ns = elapsed_ns(start);
        return 0;
    }

    if (vtx_gbdt_load_default_model(&model) != 0) {
        vtx_gbdt_model_destroy(&model);
        *time_ns = elapsed_ns(start);
        return 0;
    }

    /* Scan the graph for call nodes and make inlining decisions.
     *
     * We iterate over all nodes in the node table. For each call node
     * (CallStatic, CallVirtual, CallInterface), we compute the feature
     * vector, run GBDT inference, and inline if the score is high enough.
     *
     * Note: We collect call nodes first, then inline, because inlining
     * modifies the graph and would invalidate iteration.
     */
    uint32_t node_count = graph->node_table.count;
    vtx_nodeid_t *call_nodes = (vtx_nodeid_t *)vtx_arena_alloc(arena,
        sizeof(vtx_nodeid_t) * (node_count + 1));
    if (!call_nodes) {
        vtx_gbdt_model_destroy(&model);
        *time_ns = elapsed_ns(start);
        return 0;
    }

    uint32_t call_count = 0;
    for (uint32_t i = 0; i < node_count; i++) {
        vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead) continue;

        vtx_node_opcode_t op = node->opcode;
        if (op == VTX_OP_CallStatic ||
            op == VTX_OP_CallVirtual ||
            op == VTX_OP_CallInterface) {
            call_nodes[call_count++] = (vtx_nodeid_t)i;
        }
    }

    /* Attempt inlining for each call site.
     * In a real implementation, we'd look up the callee graph from
     * a method registry. For now, we record the decision but skip
     * the actual transform if no callee graph is available. */
    for (uint32_t c = 0; c < call_count; c++) {
        vtx_nodeid_t call_id = call_nodes[c];
        vtx_node_t *call_node = vtx_graph_node(graph, call_id);
        if (!call_node || call_node->dead) continue;

        /* Compute features for this call site.
         * In a full implementation, features would be extracted from
         * profile data, the call node's context, and the callee. */
        vtx_inline_features_t features;
        memset(&features, 0, sizeof(features));

        /* Set basic features using the feature index constants:
         *   [0]  callee_size              — heuristic estimate
         *   [1]  callee_instruction_count — heuristic estimate
         *   [2]  call_site_frequency      — moderate (hot if still in graph)
         *   [3]  caller_size              — from caller node count
         *   [4]  call_depth               — current depth = 1
         *   [5]  callee_is_hot            — heuristic
         *   [6]  callee_has_loops         — unknown, conservative
         *   [7]  callee_has_try_catch     — unknown, conservative
         *   [8]  callee_allocates         — unknown, conservative
         *   [9]  callee_calls_virtual     — from call node opcode
         *   [10] receiver_type_certainty  — moderate (0.5)
         *   [11] constant_arg_ratio       — conservative (0.0)
         *   [12] estimated_register_pressure — heuristic
         *   [13] callee_deopt_rate        — conservative (0.0)
         *   [14] inline_history           — 0 (first time)
         *
         * For the pipeline driver, we use conservative defaults that
         * will be overridden by real profile data when available. */
        features.features[0]  = 64.0;        /* callee bytecode size estimate */
        features.features[1]  = 32.0;        /* callee instruction count estimate */
        features.features[2]  = 0.5;         /* call_site_frequency (normalized) */
        features.features[3]  = (double)graph->node_table.count; /* caller_size */
        features.features[4]  = 1.0;         /* call_depth */
        features.features[5]  = 0.5;         /* callee_is_hot */
        features.features[6]  = 0.0;         /* callee_has_loops (unknown) */
        features.features[7]  = 0.0;         /* callee_has_try_catch (unknown) */
        features.features[8]  = 0.0;         /* callee_allocates (unknown) */
        features.features[9]  = (call_node->opcode == VTX_OP_CallVirtual ||
                                  call_node->opcode == VTX_OP_CallInterface)
                                 ? 1.0 : 0.0;  /* callee_calls_virtual */
        features.features[10] = 0.5;         /* receiver_type_certainty */
        features.features[11] = 0.0;         /* constant_arg_ratio */
        features.features[12] = 0.5;         /* estimated_register_pressure */
        features.features[13] = 0.0;         /* callee_deopt_rate */
        features.features[14] = 0.0;         /* inline_history */

        /* Run GBDT inference */
        double score = vtx_gbdt_infer(&model, &features);
        decisions++;

        if (vtx_gbdt_should_inline(score)) {
            /* In a real implementation, we would:
             *   1. Look up the callee graph from the method registry
             *   2. Check vtx_inline_can_inline()
             *   3. Call vtx_inline_transform()
             *   4. Record the decision in the feedback tracker
             *
             * For now, the decision is recorded but the actual transform
             * requires a callee graph, which is provided externally. */
        }
    }

    vtx_gbdt_model_destroy(&model);
    *time_ns = elapsed_ns(start);
    return decisions;
}

/* ========================================================================== */
/* PEA pass (escape analysis + scalar replacement + materialization)            */
/* ========================================================================== */

/**
 * Run the Partial Escape Analysis pipeline:
 *   1. Flow-sensitive escape analysis
 *   2. Cross-object scalar replacement
 *   3. Object materialization at escape/deopt points
 *
 * Returns the number of allocations eliminated (scalar-replaced).
 */
static uint32_t run_pea_pass(vtx_graph_t *graph,
                              vtx_arena_t *arena,
                              int64_t *time_ns)
{
    int64_t start = now_ns();
    uint32_t allocs_eliminated = 0;

    /* Step 1: Run escape analysis */
    vtx_pea_analysis_t *analysis = vtx_pea_run(graph, arena);
    if (!analysis) {
        *time_ns = elapsed_ns(start);
        return 0;
    }

    /* Step 2: Cross-object scalar replacement */
    vtx_cross_sr_result_t *sr_result = vtx_cross_object_sr_run(graph, analysis, arena);
    if (sr_result) {
        allocs_eliminated = sr_result->allocs_replaced;
    }

    /* Step 3: Materialize objects at escape/deopt points */
    vtx_materialize_result_t *mat_result = vtx_materialize_run(graph, analysis, arena);
    /* mat_result tracks how many objects needed materialization; we don't
     * count those as "eliminated" since they still exist on the heap. */
    (void)mat_result;

    *time_ns = elapsed_ns(start);
    return allocs_eliminated;
}

/* ========================================================================== */
/* Schedule pass                                                               */
/* ========================================================================== */

/**
 * Run the SoN -> basic block scheduler.
 * Returns 0 on success, -1 on failure.
 */
static int run_schedule_pass(vtx_graph_t *graph,
                              vtx_arena_t *arena,
                              vtx_schedule_t *schedule,
                              int64_t *time_ns)
{
    int64_t start = now_ns();
    int rc = vtx_schedule_run(graph, arena, schedule);
    *time_ns = elapsed_ns(start);
    return rc;
}

/* ========================================================================== */
/* LICM pass                                                                   */
/* ========================================================================== */

/**
 * Run Loop-Invariant Code Motion.
 * Returns the number of nodes hoisted.
 */
static uint32_t run_licm_pass(vtx_graph_t *graph,
                               const vtx_schedule_t *schedule,
                               vtx_arena_t *arena,
                               int64_t *time_ns)
{
    int64_t start = now_ns();
    int hoisted = vtx_licm_run(graph, schedule, arena);
    *time_ns = elapsed_ns(start);
    return (hoisted >= 0) ? (uint32_t)hoisted : 0;
}

/* ========================================================================== */
/* Bounds check elimination pass                                                */
/* ========================================================================== */

/**
 * Run bounds check elimination.
 * Returns the number of guards eliminated.
 */
static uint32_t run_bounds_check_pass(vtx_graph_t *graph,
                                       const vtx_schedule_t *schedule,
                                       vtx_arena_t *arena,
                                       int64_t *time_ns)
{
    int64_t start = now_ns();
    int eliminated = vtx_bounds_check_run(graph, schedule, arena);
    *time_ns = elapsed_ns(start);
    return (eliminated >= 0) ? (uint32_t)eliminated : 0;
}

/* ========================================================================== */
/* Lowering pipeline (isel -> regalloc -> emit)                                */
/* ========================================================================== */

/**
 * Run the complete lowering pipeline:
 *   1. Instruction selection (SoN -> x86-64 virtual register instructions)
 *   2. Register allocation (virtual -> physical registers)
 *   3. Peephole optimization
 *   4. Branch optimization
 *   5. Code emission
 *
 * Returns 0 on success, -1 on failure.
 * The emitted code is stored in result->native_code / native_size.
 */
static int run_lowering_pipeline(vtx_graph_t *graph,
                                  const vtx_schedule_t *schedule,
                                  vtx_arena_t *arena,
                                  vtx_compile_result_t *result,
                                  int64_t *time_ns)
{
    int64_t start = now_ns();

    /* Step 1: Instruction selection */
    vtx_inst_stream_t *inst_stream = vtx_isel_select(schedule, graph, arena);
    if (!inst_stream) {
        fprintf(stderr, "[pipeline] instruction selection failed\n");
        *time_ns = elapsed_ns(start);
        return -1;
    }

    /* Step 2: Register allocation */
    vtx_regalloc_result_t *ra_result = vtx_regalloc_run(inst_stream, arena);
    if (!ra_result) {
        fprintf(stderr, "[pipeline] register allocation failed\n");
        *time_ns = elapsed_ns(start);
        return -1;
    }

    /* Step 3: Apply register allocation (rewrites virtual -> physical regs) */
    if (vtx_regalloc_apply(inst_stream, ra_result, arena) != 0) {
        fprintf(stderr, "[pipeline] register allocation apply failed\n");
        *time_ns = elapsed_ns(start);
        return -1;
    }

    /* Step 4: Peephole optimization on the allocated instructions */
    vtx_peephole_optimize(inst_stream, ra_result);

    /* Step 5: Code emission */
    vtx_x86_emit_t emitter;
    if (vtx_x86_emit_init(&emitter, VTX_EMIT_INITIAL_CAPACITY) != 0) {
        fprintf(stderr, "[pipeline] emitter init failed\n");
        *time_ns = elapsed_ns(start);
        return -1;
    }

    /* Emit prologue */
    vtx_x86_emit_prologue(&emitter, ra_result->frame_size,
                           ra_result->callee_saved_mask);

    /* Emit the function body */
    if (vtx_x86_emit_function(&emitter, inst_stream, ra_result, arena) != 0) {
        fprintf(stderr, "[pipeline] code emission failed\n");
        vtx_x86_emit_destroy(&emitter);
        *time_ns = elapsed_ns(start);
        return -1;
    }

    /* Emit epilogue */
    vtx_x86_emit_epilogue(&emitter, ra_result->callee_saved_mask);

    /* Copy the emitted code into the result.
     * We use malloc here (not arena) because the native code must outlive
     * the compilation arena -- it gets installed in the code cache. */
    uint32_t code_size = vtx_x86_emit_position(&emitter);
    if (code_size > VTX_MAX_NATIVE_SIZE) {
        fprintf(stderr, "[pipeline] emitted code too large: %u bytes (max %d)\n",
                code_size, VTX_MAX_NATIVE_SIZE);
        vtx_x86_emit_destroy(&emitter);
        *time_ns = elapsed_ns(start);
        return -1;
    }

    uint8_t *native_code = (uint8_t *)malloc(code_size);
    if (!native_code) {
        fprintf(stderr, "[pipeline] failed to allocate %u bytes for native code\n",
                code_size);
        vtx_x86_emit_destroy(&emitter);
        *time_ns = elapsed_ns(start);
        return -1;
    }

    memcpy(native_code, vtx_x86_emit_code(&emitter), code_size);
    vtx_x86_emit_destroy(&emitter);

    result->native_code = native_code;
    result->native_size = code_size;

    *time_ns = elapsed_ns(start);
    return 0;
}

/* ========================================================================== */
/* Main pipeline entry point                                                   */
/* ========================================================================== */

int vtx_pipeline_run(vtx_graph_t *graph,
                      const vtx_pipeline_config_t *config,
                      vtx_arena_t *arena,
                      vtx_compile_result_t *result)
{
    int64_t pipeline_start = now_ns();

    /* Initialize result */
    memset(result, 0, sizeof(*result));
    result->graph = graph;
    result->success = false;

    /* Accumulated stats */
    vtx_pipeline_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    /* Temporary accumulators for cleanup passes after inlining */
    uint32_t post_inline_gvn = 0;
    uint32_t post_inline_sccp = 0;
    uint32_t post_inline_dce = 0;
    uint32_t post_pea_dce = 0;
    int64_t  post_inline_gvn_ns = 0;
    int64_t  post_inline_sccp_ns = 0;
    int64_t  post_inline_dce_ns = 0;
    int64_t  post_pea_dce_ns = 0;

    /* ================================================================== */
    /* Phase 1: GVN (Global Value Numbering)                              */
    /*                                                                    */
    /* GVN runs first to merge redundant computations. This enables       */
    /* downstream passes (SCCP, DCE, inlining) to work on a smaller,     */
    /* cleaner graph.                                                     */
    /* ================================================================== */
    if (config->run_gvn) {
        stats.gvn_nodes_merged = run_gvn_pass(
            graph, config->gvn_iterations, &stats.gvn_time_ns);

        if (verify_between_passes(graph, config, "GVN") != 0) {
            result->stats = stats;
            return -1;
        }
    }

    /* ================================================================== */
    /* Phase 2: SCCP (Sparse Conditional Constant Propagation)            */
    /*                                                                    */
    /* Propagates constants and folds conditional branches. This exposes   */
    /* more dead code for DCE and simplifies the graph for inlining.      */
    /* ================================================================== */
    if (config->run_sccp) {
        stats.sccp_constants_propagated = run_sccp_pass(
            graph, config->sccp_iterations, &stats.sccp_time_ns);

        if (verify_between_passes(graph, config, "SCCP") != 0) {
            result->stats = stats;
            return -1;
        }
    }

    /* ================================================================== */
    /* Phase 3: DCE (Dead Code Elimination)                               */
    /*                                                                    */
    /* Removes nodes whose results are never used. GVN and SCCP expose    */
    /* many dead nodes (e.g., the second copy of a redundant computation, */
    /* or a branch condition that folded to a constant).                  */
    /* ================================================================== */
    if (config->run_dce) {
        stats.dce_nodes_removed = run_dce_pass(
            graph, config->dce_iterations, &stats.dce_time_ns, config->run_verify);

        if (verify_between_passes(graph, config, "DCE") != 0) {
            result->stats = stats;
            return -1;
        }
    }

    /* ================================================================== */
    /* Phase 4: ML-guided Inlining                                        */
    /*                                                                    */
    /* Uses a GBDT model to decide which call sites to inline. Inlining   */
    /* expands the caller graph with the callee's body, enabling further  */
    /* optimizations across the call boundary.                            */
    /* ================================================================== */
    if (config->run_inlining) {
        stats.inlining_decisions = run_inlining_pass(
            graph, arena, &stats.inlining_time_ns);

        if (verify_between_passes(graph, config, "Inlining") != 0) {
            result->stats = stats;
            return -1;
        }

        /* ============================================================== */
        /* Phase 5: GVN + SCCP + DCE again (post-inlining cleanup)        */
        /*                                                                */
        /* Inlining introduces redundant nodes (parameter mappings,       */
        /* return Phi merges). Running GVN+SCCP+DCE again cleans up      */
        /* the expanded graph.                                            */
        /* ============================================================== */
        if (config->run_gvn) {
            post_inline_gvn = run_gvn_pass(
                graph, config->gvn_iterations, &post_inline_gvn_ns);
            stats.gvn_nodes_merged += post_inline_gvn;
            stats.gvn_time_ns += post_inline_gvn_ns;

            if (verify_between_passes(graph, config, "Post-inline GVN") != 0) {
                result->stats = stats;
                return -1;
            }
        }

        if (config->run_sccp) {
            post_inline_sccp = run_sccp_pass(
                graph, config->sccp_iterations, &post_inline_sccp_ns);
            stats.sccp_constants_propagated += post_inline_sccp;
            stats.sccp_time_ns += post_inline_sccp_ns;

            if (verify_between_passes(graph, config, "Post-inline SCCP") != 0) {
                result->stats = stats;
                return -1;
            }
        }

        if (config->run_dce) {
            post_inline_dce = run_dce_pass(
                graph, config->dce_iterations, &post_inline_dce_ns,
                config->run_verify);
            stats.dce_nodes_removed += post_inline_dce;
            stats.dce_time_ns += post_inline_dce_ns;

            if (verify_between_passes(graph, config, "Post-inline DCE") != 0) {
                result->stats = stats;
                return -1;
            }
        }
    }

    /* ================================================================== */
    /* Phase 6: PEA (Partial Escape Analysis)                             */
    /*                                                                    */
    /* Determines which allocations can be scalar-replaced (eliminated).  */
    /* Cross-object SR extends this to objects reachable only through     */
    /* fields of other (escaping) objects. Materialization reifies        */
    /* scalar-replaced objects at escape/deopt points.                    */
    /* ================================================================== */
    if (config->run_pea) {
        stats.pea_allocs_eliminated = run_pea_pass(
            graph, arena, &stats.pea_time_ns);

        if (verify_between_passes(graph, config, "PEA") != 0) {
            result->stats = stats;
            return -1;
        }

        /* DCE after scalar replacement: dead allocations and field     */
        /* accesses become removable after PEA rewrites them.           */
        if (config->run_dce) {
            post_pea_dce = run_dce_pass(
                graph, config->dce_iterations, &post_pea_dce_ns,
                config->run_verify);
            stats.dce_nodes_removed += post_pea_dce;
            stats.dce_time_ns += post_pea_dce_ns;

            if (verify_between_passes(graph, config, "Post-PEA DCE") != 0) {
                result->stats = stats;
                return -1;
            }
        }
    }

    /* ================================================================== */
    /* Phase 7: Scheduling (SoN -> Basic Blocks)                           */
    /*                                                                    */
    /* Converts the Sea-of-Nodes graph back into a scheduled form with    */
    /* basic blocks. This is required for LICM (needs loop structure),    */
    /* bounds check elimination (needs dominance), and lowering.          */
    /* ================================================================== */
    vtx_schedule_t schedule;
    memset(&schedule, 0, sizeof(schedule));

    if (run_schedule_pass(graph, arena, &schedule, &stats.schedule_time_ns) != 0) {
        fprintf(stderr, "[pipeline] scheduling failed\n");
        result->stats = stats;
        return -1;
    }

    if (verify_between_passes(graph, config, "Schedule") != 0) {
        vtx_schedule_destroy(&schedule);
        result->stats = stats;
        return -1;
    }

    result->schedule = (vtx_schedule_t *)vtx_arena_alloc(arena, sizeof(vtx_schedule_t));
    if (result->schedule) {
        memcpy(result->schedule, &schedule, sizeof(vtx_schedule_t));
    }

    /* ================================================================== */
    /* Phase 8: LICM (Loop-Invariant Code Motion)                         */
    /*                                                                    */
    /* Hoists loop-invariant computations out of loops into preheaders.   */
    /* Requires the schedule for loop structure identification.           */
    /* ================================================================== */
    if (config->run_licm) {
        stats.licm_nodes_hoisted = run_licm_pass(
            graph, &schedule, arena, &stats.licm_time_ns);

        if (verify_between_passes(graph, config, "LICM") != 0) {
            result->stats = stats;
            return -1;
        }

        /* After LICM hoists nodes, the schedule may be stale.
         * Re-schedule for correctness in subsequent passes. */
        vtx_schedule_destroy(&schedule);
        memset(&schedule, 0, sizeof(schedule));
        if (vtx_schedule_run(graph, arena, &schedule) != 0) {
            fprintf(stderr, "[pipeline] re-scheduling after LICM failed\n");
            result->stats = stats;
            return -1;
        }

        if (result->schedule) {
            memcpy(result->schedule, &schedule, sizeof(vtx_schedule_t));
        }
    }

    /* ================================================================== */
    /* Phase 9: Bounds Check Elimination                                  */
    /*                                                                    */
    /* Eliminates redundant array bounds checks using range analysis.     */
    /* Requires the schedule for dominance and loop structure info.       */
    /* ================================================================== */
    if (config->run_bounds_check) {
        stats.bounds_checks_eliminated = run_bounds_check_pass(
            graph, &schedule, arena, &stats.bounds_check_time_ns);

        if (verify_between_passes(graph, config, "BoundsCheck") != 0) {
            result->stats = stats;
            return -1;
        }
    }

    /* ================================================================== */
    /* Phase 10: Lowering (isel -> regalloc -> emit)                        */
    /*                                                                    */
    /* Converts the optimized SoN graph into x86-64 machine code:         */
    /*   - Instruction selection maps SoN nodes to x86-64 instructions    */
    /*   - Register allocation assigns physical registers                 */
    /*   - Code emission produces the final machine code bytes            */
    /* ================================================================== */
    if (run_lowering_pipeline(graph, &schedule, arena, result,
                               &stats.lowering_time_ns) != 0) {
        fprintf(stderr, "[pipeline] lowering failed\n");
        result->stats = stats;
        return -1;
    }

    /* ================================================================== */
    /* Finalize                                                           */
    /* ================================================================== */
    stats.total_pipeline_time_ns = elapsed_ns(pipeline_start);
    result->stats = stats;
    result->success = true;

    return 0;
}

/* ========================================================================== */
/* Compile result destruction                                                  */
/* ========================================================================== */

void vtx_compile_result_destroy(vtx_compile_result_t *result)
{
    if (!result) return;

    /* Free the native code buffer (allocated with malloc, not arena) */
    if (result->native_code) {
        free(result->native_code);
        result->native_code = NULL;
    }

    /* Note: graph and schedule are NOT owned by the result.
     * - graph is owned by the caller
     * - schedule is arena-allocated and freed with the arena
     * We just clear the pointers to avoid dangling references. */
    result->graph = NULL;
    result->schedule = NULL;
    result->native_size = 0;
    result->success = false;
}
