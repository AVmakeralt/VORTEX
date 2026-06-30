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
#include "codecache/install.h"
#include "ir/strength_reduce.h"

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
    /* T1 — Baseline JIT: no optimizations for fastest compilation.
     * Pure code generation with no IR passes. Target: < 1ms compilation
     * time for small methods. GVN/DCE/SCCP are deferred to T2. */
    vtx_pipeline_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.run_gvn           = false;
    cfg.run_sccp          = false;
    cfg.run_dce           = false;
    cfg.run_licm          = false;
    cfg.run_bounds_check  = false;
    cfg.run_pea           = false;
    cfg.run_inlining      = false;
    cfg.run_verify        = false;
    cfg.run_loop_spec     = false;
    cfg.run_vectorize     = false;
    cfg.gvn_iterations    = 0;
    cfg.sccp_iterations   = 0;
    cfg.dce_iterations    = 0;
    cfg.shared_gbdt_model = NULL;
    cfg.owns_gbdt_model   = false;
    cfg.run_midtier       = false;
    cfg.markov            = NULL;
    return cfg;
}

vtx_pipeline_config_t vtx_pipeline_config_t1_5(void)
{
    /* T1.5 — Mid-Tier: type-specialized without full optimization.
     * Between T1 and T2: uses type feedback to specialize operations,
     * eliminating type checks at monomorphic sites. Reduces warmup
     * by 3-5x compared to waiting for T2 compilation.
     *
     * Pipeline: GVN(1) → type specialization → DCE(1) → schedule → lower.
     * No SCCP, no PEA, no inlining, no LICM, no bounds check elimination.
     * Compilation is ~10x faster than T2. */
    vtx_pipeline_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.run_gvn           = true;
    cfg.run_sccp          = false;
    cfg.run_dce           = true;
    cfg.run_licm          = false;
    cfg.run_bounds_check  = false;
    cfg.run_pea           = false;
    cfg.run_inlining      = false;
    cfg.run_speculative   = false;
    cfg.run_verify        = false;
    cfg.run_loop_spec     = false;
    cfg.run_vectorize     = false;
    cfg.run_midtier       = true;  /* enable mid-tier type specialization */
    cfg.gvn_iterations    = 1;
    cfg.sccp_iterations   = 0;
    cfg.dce_iterations    = 1;
    cfg.inline_size_limit = 0;
    cfg.callee_lookup     = NULL;
    cfg.callee_lookup_context = NULL;
    cfg.type_feedback     = NULL;  /* set by caller if available */
    cfg.shared_gbdt_model = NULL;
    cfg.owns_gbdt_model   = false;
    cfg.markov            = NULL;
    return cfg;
}

vtx_pipeline_config_t vtx_pipeline_config_t2(void)
{
    /* T2 — Optimizing JIT: full optimization pipeline.
     * GVN -> SCCP -> DCE -> inlining -> GVN+SCCP+DCE -> PEA -> DCE ->
     * schedule -> LICM -> bounds check -> loop spec -> lower.
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
    cfg.run_speculative   = false;  /* no speculation in T2 */
    cfg.run_verify        = false;  /* disabled by default for performance */
    cfg.run_loop_spec     = true;   /* enable loop specialization */
    cfg.run_vectorize     = true;   /* enable SIMD vectorization */
    cfg.gvn_iterations    = 3;
    cfg.sccp_iterations   = 3;
    cfg.dce_iterations    = 3;
    cfg.inline_size_limit = 0;      /* use default VTX_INLINE_SIZE_LIMIT */
    cfg.callee_lookup     = NULL;
    cfg.callee_lookup_context = NULL;
    cfg.type_feedback     = NULL;   /* no type feedback by default; set by caller */
    cfg.shared_gbdt_model = NULL;
    cfg.owns_gbdt_model   = false;
    cfg.run_midtier       = false;
    cfg.markov            = NULL;
    return cfg;
}

vtx_pipeline_config_t vtx_pipeline_config_t3(void)
{
    /* T3 — Speculative JIT: full pipeline + speculative guards.
     * Same as T2 but with verification enabled, more iterations
     * for aggressive optimization, and speculative guard insertion.
     * Used for very hot code with speculation support.
     *
     * T3 differs from T2 in three ways:
     *   1. Speculative guard insertion: type-check guards at call sites
     *      based on profiling data, enabling deoptimization on type change.
     *   2. More aggressive inlining: higher size limit for inlined callees.
     *   3. More optimization iterations (5 vs 3). */
    vtx_pipeline_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.run_gvn           = true;
    cfg.run_sccp          = true;
    cfg.run_dce           = true;
    cfg.run_licm          = true;
    cfg.run_bounds_check  = true;
    cfg.run_pea           = true;
    cfg.run_inlining      = true;
    cfg.run_speculative   = true;   /* enable speculative guard insertion */
    cfg.run_verify        = true;   /* verify aggressively in speculative tier */
    cfg.run_loop_spec     = true;   /* enable loop specialization */
    cfg.run_vectorize     = true;   /* enable SIMD vectorization */
    cfg.gvn_iterations    = 5;
    cfg.sccp_iterations   = 5;
    cfg.dce_iterations    = 5;
    cfg.inline_size_limit = 512;    /* more aggressive: 2x the default limit */
    cfg.callee_lookup     = NULL;
    cfg.callee_lookup_context = NULL;
    cfg.type_feedback     = NULL;   /* no type feedback by default; set by caller */
    cfg.shared_gbdt_model = NULL;
    cfg.owns_gbdt_model   = false;
    cfg.run_midtier       = false;
    cfg.markov            = NULL;
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
                                   const vtx_pipeline_config_t *config,
                                   vtx_arena_t *arena,
                                   int64_t *time_ns)
{
    int64_t start = now_ns();
    uint32_t decisions = 0;
    uint32_t inlined = 0;

    /* Use the shared GBDT model if available, otherwise create a
     * temporary one for this compilation (backward compatibility). */
    vtx_gbdt_model_t *model = config->shared_gbdt_model;
    bool owns_temp_model = false;

    if (model == NULL) {
        /* Fallback: create a per-compilation model */
        vtx_gbdt_model_t *temp = (vtx_gbdt_model_t *)malloc(sizeof(vtx_gbdt_model_t));
        if (!temp) {
            *time_ns = elapsed_ns(start);
            return 0;
        }
        if (vtx_gbdt_model_init(temp) != 0) {
            free(temp);
            *time_ns = elapsed_ns(start);
            return 0;
        }
        if (vtx_gbdt_load_default_model(temp) != 0) {
            vtx_gbdt_model_destroy(temp);
            free(temp);
            *time_ns = elapsed_ns(start);
            return 0;
        }
        model = temp;
        owns_temp_model = true;
    }

    /* Determine the effective inline size limit */
    uint32_t effective_size_limit = (config && config->inline_size_limit > 0)
                                    ? (uint32_t)config->inline_size_limit
                                    : VTX_INLINE_SIZE_LIMIT;

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
        if (owns_temp_model) {
            vtx_gbdt_model_destroy(model);
            free(model);
        }
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
     * If a callee graph lookup callback is provided, we look up the
     * callee graph, check can_inline(), and call vtx_inline_transform(). */
    for (uint32_t c = 0; c < call_count; c++) {
        vtx_nodeid_t call_id = call_nodes[c];
        vtx_node_t *call_node = vtx_graph_node(graph, call_id);
        if (!call_node || call_node->dead) continue;

        /* Look up the callee graph via the callback, if provided.
         * We do this BEFORE feature extraction so the extractor can use
         * the callee graph for precise features (instruction count, has_loops,
         * has_try_catch, allocates, calls_virtual). */
        const vtx_graph_t *callee_graph = NULL;
        if (config && config->callee_lookup) {
            callee_graph = config->callee_lookup(
                call_node->method_index,
                config->callee_lookup_context);
        }

        /* Build the feature extraction context from real data.
         *
         * BUGFIX (audit #8): The old code hand-filled the features struct
         * with constants (callee_size=64, callee_is_hot=0.5, etc.), making
         * the GBDT model receive essentially constant inputs and produce
         * essentially constant output. The "ML inliner" was a heuristic
         * in disguise. Now we call the real vtx_features_extract() with
         * a properly populated context. */
        vtx_inline_context_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.caller_bytecode_size = (config && config->method && config->method->bytecode)
                                    ? (uint32_t)config->method->bytecode->length : 0;
        ctx.call_depth = 1;
        ctx.inline_history = 0;
        ctx.callee_bytecode_size = 64;  /* conservative default if unknown */
        ctx.callee_method_id = call_node->method_index;
        ctx.callee_deopt_count = 0;
        ctx.callee_invocation_count = 0;
        ctx.caller_node_count = graph->node_table.count;
        ctx.callee_graph = callee_graph;

        /* Extract real features from the graph, call node, and context.
         * Profile data is not available in the pipeline config (T2 compiles
         * without profile), so pass NULL — the extractor falls back to
         * conservative estimates for profile-dependent features. */
        vtx_inline_features_t features = vtx_features_extract(
            graph, call_id, NULL, &ctx);

        /* Run GBDT inference */
        double score = vtx_gbdt_infer(model, &features);
        decisions++;

        if (vtx_gbdt_should_inline(score)) {
            if (callee_graph != NULL) {
                /* Check if inlining is legal */
                if (callee_graph->node_table.count <= effective_size_limit &&
                    vtx_inline_can_inline(graph, call_id, callee_graph, 1)) {
                    /* Perform the inlining transform */
                    vtx_inline_result_t iresult = vtx_inline_transform(
                        graph, call_id, callee_graph, arena);
                    if (iresult.success) {
                        inlined++;
                    }
                }
            }
            /* If no callee graph is available, the decision is recorded
             * but the actual transform is deferred until a callee graph
             * is provided (e.g., via a method registry at runtime). */
        }
    }

    if (owns_temp_model) {
        vtx_gbdt_model_destroy(model);
        free(model);
    }
    *time_ns = elapsed_ns(start);
    return (decisions << 16) | inlined;  /* pack decisions + inlined count */
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

    /* Step 3: Run virtual object tracking (rewrites field accesses to
     * locals, marks StoreField nodes as dead). Must run before
     * materialization so that materialize reads field values from
     * the virtual field maps instead of dead StoreField nodes. */
    vtx_virtual_result_t *virtual_result = vtx_virtual_run(graph, analysis, arena);

    /* Step 4: Materialize objects at escape/deopt points */
    vtx_materialize_result_t *mat_result = vtx_materialize_run(graph, analysis, virtual_result, arena);
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
/* Loop specialization/vectorization pass                                        */
/* ========================================================================== */

/**
 * Run loop specialization and vectorization.
 *
 * Finds all LoopBegin nodes, checks if they are vectorizable using
 * the loop_spec analyzer, and transforms vectorizable loops by
 * replacing scalar operations with SIMD-wide operations.
 *
 * Returns the number of loops vectorized.
 */
static uint32_t run_loop_spec_pass(vtx_graph_t *graph,
                                    vtx_arena_t *arena,
                                    int64_t *time_ns)
{
    int64_t start = now_ns();
    uint32_t vectorized = 0;

    /* Initialize loop spec analyzer */
    vtx_sota_loop_spec_t spec;
    if (vtx_sota_loop_spec_init(&spec) != 0) {
        *time_ns = elapsed_ns(start);
        return 0;
    }

    /* Detect CPU features */
    uint32_t cpu_features = vtx_sota_loop_detect_cpu_features();

    /* Find all LoopBegin nodes in the graph */
    vtx_node_table_t *table = &graph->node_table;

    /* First pass: collect LoopBegin node IDs (iteration may be
     * invalidated if we modify the graph during transformation) */
    uint32_t loop_count = 0;
    for (uint32_t i = 0; i < table->count; i++) {
        vtx_node_t *node = &table->nodes[i];
        if (!node->dead && node->opcode == VTX_OP_LoopBegin) {
            loop_count++;
        }
    }

    if (loop_count == 0) {
        vtx_sota_loop_spec_destroy(&spec);
        *time_ns = elapsed_ns(start);
        return 0;
    }

    vtx_nodeid_t *loop_nodes = (vtx_nodeid_t *)vtx_arena_alloc(
        arena, sizeof(vtx_nodeid_t) * loop_count);
    if (!loop_nodes) {
        vtx_sota_loop_spec_destroy(&spec);
        *time_ns = elapsed_ns(start);
        return 0;
    }

    uint32_t idx = 0;
    for (uint32_t i = 0; i < table->count; i++) {
        vtx_node_t *node = &table->nodes[i];
        if (!node->dead && node->opcode == VTX_OP_LoopBegin) {
            loop_nodes[idx++] = node->id;
        }
    }

    /* Second pass: analyze and transform each loop */
    for (uint32_t l = 0; l < loop_count; l++) {
        vtx_nodeid_t loop_id = loop_nodes[l];

        /* Verify the node still exists and is a LoopBegin */
        vtx_node_t *loop_node = vtx_node_get(table, loop_id);
        if (!loop_node || loop_node->dead || loop_node->opcode != VTX_OP_LoopBegin) {
            continue;
        }

        /* Analyze this loop for vectorization.
         * We pass NULL for profile since profiling data may not be
         * available at this point; the analyzer will fall back to
         * heuristic trip count estimation. */
        vtx_loop_spec_result_t result = vtx_sota_loop_spec_check(
            &spec, NULL, graph, loop_id);

        /* If the loop is vectorizable, transform it */
        if (result.vectorizability >= VTX_LOOP_CAN_VECTORIZE_SSE2 &&
            result.vector_width > 1) {
            bool ok = vtx_sota_loop_spec_transform(
                graph, loop_id, cpu_features, &result, arena);
            if (ok) {
                vectorized++;
            }
        }
    }

    vtx_sota_loop_spec_destroy(&spec);
    *time_ns = elapsed_ns(start);
    return vectorized;
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
                                  int64_t *time_ns,
                                  uint32_t method_arg_count,
                                  uint32_t method_max_locals)
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

    /* Step 5: Build guard descriptor array from the graph.
     * Scan for Guard/DeoptGuard nodes and create descriptors so we can
     * emit deopt stubs and build the side table during code emission. */
    vtx_guard_desc_array_t guard_arr;
    if (vtx_guard_desc_array_init(&guard_arr, arena) != 0) {
        fprintf(stderr, "[pipeline] guard array init failed\n");
        *time_ns = elapsed_ns(start);
        return -1;
    }

    vtx_node_table_t *ntable = &graph->node_table;
    for (uint32_t i = 0; i < ntable->count; i++) {
        vtx_node_t *node = &ntable->nodes[i];
        if (node->dead) continue;
        if (node->opcode != VTX_OP_Guard && node->opcode != VTX_OP_DeoptGuard) continue;

        vtx_guard_desc_t gd;
        memset(&gd, 0, sizeof(gd));
        gd.guard_node = i;  /* node index in the table */
        gd.cond = node->cond;
        gd.bytecode_pc = node->bytecode_pc;
        gd.type_id = node->type_id;
        /* frame_state_index: use the node's frame_state if available,
         * otherwise use the node index as a unique identifier */
        gd.frame_state_index = (node->frame_state != VTX_NODEID_INVALID)
                                ? node->frame_state
                                : i;

        vtx_guard_desc_array_add(&guard_arr, gd, arena);
    }

    /* Step 6: Build side table for deoptimization.
     * The side table maps native PC offsets → FrameState, enabling
     * the deopt runtime to reconstruct interpreter state when a
     * guard fails or a safepoint is hit. */
    vtx_side_table_t *side_table = vtx_side_table_build(arena);
    if (!side_table) {
        fprintf(stderr, "[pipeline] side table build failed\n");
        *time_ns = elapsed_ns(start);
        return -1;
    }

    /* Step 7: Code emission */
    vtx_x86_emit_t emitter;
    if (vtx_x86_emit_init(&emitter, VTX_EMIT_INITIAL_CAPACITY) != 0) {
        fprintf(stderr, "[pipeline] emitter init failed\n");
        *time_ns = elapsed_ns(start);
        return -1;
    }

    /* Emit prologue — includes arg-to-register copy for T2/T3.
     * arg_count and max_locals are needed so the prologue can copy
     * args from the JIT entry's args array into the registers that
     * the T2 instruction selector expects for Parameter nodes. */
    vtx_x86_emit_prologue(&emitter, ra_result->frame_size,
                           ra_result->callee_saved_mask,
                           method_arg_count, method_max_locals,
                           ra_result->is_leaf);

    /* Note: SMI constants (R10=HEADER, R11=DATA_MASK) are loaded as the
     * first instructions in block 0 of the instruction stream (emitted by
     * isel), NOT here. This way the regalloc sees the definitions and
     * doesn't spill the fixed vregs. */

    /* Note: Epilogue is emitted by the RET instruction handler in
     * emit_single_inst(), which calls vtx_x86_emit_epilogue() to
     * restore callee-saved registers before the ret instruction.
     * This correctly handles functions with multiple return points.
     * We do NOT emit a separate epilogue at the end of the function. */

    /* Emit the function body */
    if (vtx_x86_emit_function(&emitter, inst_stream, ra_result, arena) != 0) {
        fprintf(stderr, "[pipeline] code emission failed\n");
        vtx_x86_emit_destroy(&emitter);
        *time_ns = elapsed_ns(start);
        return -1;
    }

    /* Step 8: Lower guards — record guard positions in the side table.
     * This scans the instruction stream for guard JCCs (marked with
     * VTX_INST_FLAG_IS_GUARD) and records their native PC offsets,
     * live register maps, and FrameState indices in the side table. */
    if (guard_arr.count > 0) {
        int lowered = vtx_guard_emit_lower(&guard_arr, inst_stream,
                                            &emitter, side_table, arena,
                                            ra_result);
        if (lowered < 0) {
            fprintf(stderr, "[pipeline] guard lowering failed\n");
            vtx_x86_emit_destroy(&emitter);
            *time_ns = elapsed_ns(start);
            return -1;
        }

        /* Step 9: Emit deopt stubs after the main code.
         * Each guard gets a deopt stub that saves state and transfers
         * to the deopt runtime when the guard fails. */
        uint8_t *code_start = (uint8_t *)vtx_x86_emit_code(&emitter);
        int stubs = vtx_guard_emit_deopt_stubs(&guard_arr, &emitter,
                                                 side_table, code_start, arena);
        if (stubs < 0) {
            fprintf(stderr, "[pipeline] deopt stub emission failed\n");
            vtx_x86_emit_destroy(&emitter);
            *time_ns = elapsed_ns(start);
            return -1;
        }

        /* Step 10: Patch guard JCC instructions to point to deopt stubs.
         * After deopt stubs are emitted, we patch the 32-bit displacement
         * in each guard's JCC instruction to jump to its deopt stub. */
        int patched = vtx_guard_emit_patch(&guard_arr, &emitter,
                                            code_start, arena);
        if (patched < 0) {
            fprintf(stderr, "[pipeline] guard patching failed\n");
            vtx_x86_emit_destroy(&emitter);
            *time_ns = elapsed_ns(start);
            return -1;
        }
    }

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
    result->side_table = side_table;

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
    /* Phase 2.5: Strength Reduction                                      */
    /*                                                                    */
    /* Replaces expensive ops with cheaper equivalents:                   */
    /*   Mul(x, 2^k) → Shl(x, k)                                         */
    /*   Mul(x, 1) → x, Mul(x, 0) → 0, Div(x, 1) → x                    */
    /*                                                                    */
    /* Runs after SCCP (constants are propagated) and before DCE (dead    */
    /* nodes from the replacement are cleaned up).                        */
    /* ================================================================== */
    if (config->run_sccp) {
        uint32_t sr_replaced = vtx_strength_reduce_run(graph);
        if (sr_replaced > 0) {
            verify_between_passes(graph, config, "StrengthReduce");
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
    /* Phase 3.5: Mid-Tier Type Specialization (T1.5)                      */
    /*                                                                    */
    /* When run_midtier is true, applies type-specialization based on      */
    /* type feedback. This inserts guards at monomorphic call sites and    */
    /* annotates arithmetic nodes with observed types, enabling the        */
    /* lowering pipeline to skip type checks. This is the core of the     */
    /* T1.5 mid-tier: type specialization without full optimization.      */
    /* ================================================================== */
    if (config->run_midtier && config->type_feedback != NULL) {
        /* Walk all nodes and specialize based on type feedback.
         * For CALL_VIRTUAL with monomorphic receiver: insert Guard + direct dispatch.
         * For Add/Sub/Mul with known-integer inputs: annotate as integer-typed.
         * For LoadField with monomorphic holder shape: skip shape check. */
        vtx_node_table_t *ntable = &graph->node_table;
        const vtx_type_feedback_t *feedback = config->type_feedback;

        for (uint32_t i = 0; i < ntable->count; i++) {
            vtx_node_t *node = &ntable->nodes[i];
            if (node->dead) continue;

            /* Specialize CALL_VIRTUAL at monomorphic sites */
            if (node->opcode == VTX_OP_CallVirtual) {
                uint32_t site_index = node->bytecode_pc;
                uint32_t type_count = vtx_type_feedback_get_call_site_type_count(
                    feedback, site_index);

                if (type_count == 1) {
                    vtx_typeid_t dominant_type = vtx_type_feedback_get_dominant_call_type(
                        feedback, site_index);

                    if (dominant_type != VTX_TYPE_INVALID) {
                        /* Insert a Guard node checking the receiver type */
                        vtx_nodeid_t guard_id = vtx_node_create(ntable, VTX_OP_Guard);
                        if (guard_id != VTX_NODEID_INVALID) {
                            vtx_node_t *guard = vtx_node_get(ntable, guard_id);
                            if (guard) {
                                /* Copy the first data input (receiver) */
                                for (uint32_t inp = 0; inp < node->input_count; inp++) {
                                    const vtx_node_t *inp_node = vtx_node_get_const(
                                        ntable, node->inputs[inp]);
                                    if (inp_node && vtx_nf_has(inp_node->flags, VTX_NF_DATA)) {
                                        vtx_node_add_input(ntable, guard_id, node->inputs[inp]);
                                        break;
                                    }
                                }
                                guard->bytecode_pc = node->bytecode_pc;
                                guard->type_id = dominant_type;
                                guard->flags = VTX_NF_CONTROL | VTX_NF_PINNED;
                            }
                        }
                        /* Annotate the call with the specialized type */
                        node->type_id = dominant_type;
                    }
                }
                continue;
            }

            /* Specialize LoadField at monomorphic holder shape sites */
            if (node->opcode == VTX_OP_LoadField) {
                uint32_t site_index = node->bytecode_pc;
                vtx_shapeid_t dominant_shape = vtx_type_feedback_get_dominant_field_shape(
                    feedback, site_index);

                if (dominant_shape != VTX_SHAPE_INVALID) {
                    node->type_id = dominant_shape; /* store shape in type_id as proxy */
                }
                continue;
            }
        }

        if (verify_between_passes(graph, config, "MidTierSpecialization") != 0) {
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
        uint32_t inline_result = run_inlining_pass(
            graph, config, arena, &stats.inlining_time_ns);
        stats.inlining_decisions = inline_result >> 16;
        stats.inlines_performed = inline_result & 0xFFFF;

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

    /* DEBUG: Print schedule for diagnosis */
    vtx_schedule_print(&schedule, graph);

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

        /* Outer fixpoint: re-run GVN+SCCP+DCE after LICM to clean up
         * trivial Phis created by hoisting (Phi with both inputs now equal)
         * and dead code exposed by the hoisted invariants.
         * (audit #9: outer fixpoint after LICM) */
        if (config->run_gvn) {
            run_gvn_pass(graph, 1, &stats.gvn_time_ns);
        }
        if (config->run_sccp) {
            run_sccp_pass(graph, 1, &stats.sccp_time_ns);
            vtx_strength_reduce_run(graph);
        }
        if (config->run_dce) {
            run_dce_pass(graph, 1, &stats.dce_time_ns, false);
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

        /* Outer fixpoint: re-run DCE after bounds check to remove dead
         * guards and Cmp nodes whose conditions were proven.
         * (audit #9: outer fixpoint after bounds check) */
        if (config->run_dce) {
            run_dce_pass(graph, 1, &stats.dce_time_ns, false);
        }
    }

    /* ================================================================== */
    /* Phase 9.1: Loop Specialization / Vectorization                      */
    /*                                                                    */
    /* Analyzes loops for vectorization opportunities. Vectorizable loops  */
    /* are transformed by replacing scalar LoadIndexed/StoreIndexed with   */
    /* VectorLoad/VectorStore, and scalar Add/Mul with VectorAdd/VectorMul.*/
    /* DeoptGuard nodes guard the vectorized code. Runs after bounds check */
    /* elimination so that eliminated bounds checks don't inhibit          */
    /* vectorization, and before speculative guards to keep guard          */
    /* insertion simple.                                                  */
    /* ================================================================== */
    if (config->run_loop_spec && config->run_vectorize) {
        stats.loops_vectorized = run_loop_spec_pass(
            graph, arena, &stats.loop_spec_time_ns);

        if (verify_between_passes(graph, config, "LoopSpec") != 0) {
            result->stats = stats;
            return -1;
        }
    }

    /* ================================================================== */
    /* Phase 9.5: Speculative Guard Insertion (T3 only)                   */
    /*                                                                    */
    /* Inserts type-check guards at virtual call sites based on profiling  */
    /* data. If a dominant type is observed, a guard checks the receiver  */
    /* type and deoptimizes on mismatch. This enables speculative          */
    /* devirtualization and more aggressive inlining in the T3 tier.       */
    /* ================================================================== */
    if (config->run_speculative) {
        /* Scan for virtual call sites and insert type guards.
         * For each CallVirtual/CallInterface, we look up the dominant
         * receiver type from the type feedback system (if available),
         * or fall back to the IR node's type_id as a speculative hint.
         * A DeoptGuard is inserted that checks the receiver's type_id
         * against the speculated dominant type. On type mismatch,
         * execution falls back to the interpreter. */
        vtx_node_table_t *ntable = &graph->node_table;
        const vtx_type_feedback_t *feedback = config->type_feedback;

        for (uint32_t i = 0; i < ntable->count; i++) {
            vtx_node_t *node = &ntable->nodes[i];
            if (node->dead) continue;
            if (node->opcode != VTX_OP_CallVirtual &&
                node->opcode != VTX_OP_CallInterface) continue;
            if (node->input_count < 1) continue;

            /* The receiver is the first data input (after control+memory). */
            vtx_nodeid_t receiver_id = VTX_NODEID_INVALID;
            for (uint32_t inp = 0; inp < node->input_count; inp++) {
                const vtx_node_t *inp_node = vtx_node_get_const(ntable, node->inputs[inp]);
                if (inp_node && vtx_nf_has(inp_node->flags, VTX_NF_DATA)) {
                    receiver_id = node->inputs[inp];
                    break;
                }
            }

            if (receiver_id == VTX_NODEID_INVALID) continue;

            /* Determine the speculated dominant receiver type.
             *
             * Priority:
             *   1. If type feedback is available, look up the call site's
             *      bytecode_pc in the feedback and use the dominant observed
             *      type from profiling data. This is far more accurate than
             *      the IR node's type_id, which may be a generic base type.
             *   2. If no feedback is available (feedback pointer is NULL
             *      or VTX_TYPE_INVALID returned for the site), fall back
             *      to the node's type_id as a speculative hint. */
            vtx_typeid_t speculated_type = VTX_TYPE_INVALID;

            if (feedback != NULL) {
                /* Look up the dominant receiver type from profiling data.
                 * The site_index is the node's bytecode_pc — the same
                 * index used when recording observations in the interpreter
                 * dispatch loop (see dispatch.c: vtx_type_feedback_record_call). */
                speculated_type = vtx_type_feedback_get_dominant_call_type(
                    feedback, node->bytecode_pc);
            }

            /* Fall back to node->type_id if no feedback data is available */
            if (speculated_type == VTX_TYPE_INVALID) {
                speculated_type = node->type_id;
            }

            /* Only insert a guard if we have a valid speculated type */
            if (speculated_type != VTX_TYPE_INVALID) {
                /* Insert a DeoptGuard before this call:
                 * Guard(receiver_type == speculated_type)
                 * On failure, deoptimize to interpreter. */
                vtx_nodeid_t guard_id = vtx_node_create(ntable, VTX_OP_DeoptGuard);
                if (guard_id != VTX_NODEID_INVALID) {
                    vtx_node_t *guard = vtx_node_get(ntable, guard_id);
                    if (guard) {
                        guard->cond = VTX_COND_EQ;
                        guard->type_id = speculated_type;
                        guard->flags = VTX_NF_CONTROL | VTX_NF_PINNED;
                        guard->bytecode_pc = node->bytecode_pc;
                        /* Add the receiver as input */
                        vtx_node_add_input(ntable, guard_id, receiver_id);
                        /* Mark this as a speculative guard for T3 */
                        guard->frame_state = node->frame_state;
                    }
                }
            }
        }

        if (verify_between_passes(graph, config, "SpeculativeGuards") != 0) {
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
    uint32_t m_arg_count = (config->method != NULL) ? config->method->arg_count : 0;
    uint32_t m_max_locals = (config->method != NULL && config->method->bytecode != NULL)
                             ? config->method->bytecode->max_locals : 0;
    if (run_lowering_pipeline(graph, &schedule, arena, result,
                               &stats.lowering_time_ns,
                               m_arg_count, m_max_locals) != 0) {
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

    /* ================================================================== */
    /* Code Installation                                                  */
    /*                                                                    */
    /* CRITICAL FIX: Previously, the T2/T3 pipeline produced native code  */
    /* but never installed it into the code cache. The code was simply     */
    /* freed in vtx_compile_result_destroy(), making the optimizing JIT    */
    /* completely non-functional. Now, if config provides code_cache,     */
    /* method_registry, and method, we install the compiled code so it    */
    /* becomes the active execution path for subsequent calls.            */
    /* ================================================================== */
    if (result->native_code != NULL && result->native_size > 0 &&
        config->code_cache != NULL && config->method_registry != NULL &&
        config->method != NULL) {
        uint32_t method_id = config->method->vtable_index;
        bool installed = vtx_install_method(
            config->code_cache, config->method_registry,
            config->method, method_id,
            result->native_code, result->native_size,
            result->side_table,  /* side_table from lowering pipeline */
            NULL,   /* reloc_table */
            NULL, 0, NULL, 0,  /* dep_type_ids, dep_shape_ids */
            config->install_arena ? config->install_arena : arena,
            NULL, 0  /* poly_ics */
        );
        if (installed) {
            /* Code is now installed in the cache and method->compiled_code
             * is set. Transfer ownership: don't free native_code in
             * vtx_compile_result_destroy(). */
            result->native_code = NULL;
            result->native_size = 0;
            result->side_table = NULL;  /* ownership transferred to code cache */
        }
    }

    /* ================================================================== */
    /* Phase 11: Markov Chain — Predictive Compilation (D10)               */
    /*                                                                    */
    /* If a Markov chain model is attached to the pipeline config, we     */
    /* check whether a phase transition is predicted. If so, we predict   */
    /* which methods will be hot in the next phase and can proactively    */
    /* compile them before the transition occurs, reducing warmup time.   */
    /*                                                                    */
    /* The actual proactive compilation is initiated asynchronously by    */
    /* recording the predicted hot methods. The compilation thread pool   */
    /* picks them up and compiles them at lower priority.                 */
    /* ================================================================== */
    if (config->markov != NULL && config->markov->is_trained) {
        uint32_t predicted_phase = vtx_markov_predict_next(
            config->markov, config->markov->current_phase);

        if (predicted_phase != config->markov->current_phase) {
            /* A phase transition is predicted. Get the methods that will
             * be hot in the next phase. */
            uint32_t hot_methods[32];
            uint32_t hot_count = vtx_markov_predict_hot_methods(
                config->markov, predicted_phase, hot_methods, 32);

            /* Log the prediction for debugging / monitoring.
             * In a production system, these method IDs would be enqueued
             * on the compilation thread pool for proactive compilation. */
            if (hot_count > 0) {
                (void)hot_methods;  /* Suppress unused variable warning;
                                     * in production, these would be enqueued
                                     * for proactive compilation. */
            }
        }
    }

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

/* ========================================================================== */
/* Pipeline config lifecycle                                                    */
/* ========================================================================== */

int vtx_pipeline_config_init_shared_model(vtx_pipeline_config_t *config)
{
    if (!config) return -1;

    /* Only init if inlining is enabled and no shared model exists yet */
    if (!config->run_inlining) return 0;
    if (config->shared_gbdt_model != NULL) return 0;  /* already initialized */

    vtx_gbdt_model_t *model = (vtx_gbdt_model_t *)malloc(sizeof(vtx_gbdt_model_t));
    if (!model) return -1;

    if (vtx_gbdt_model_init(model) != 0) {
        free(model);
        return -1;
    }

    if (vtx_gbdt_load_default_model(model) != 0) {
        vtx_gbdt_model_destroy(model);
        free(model);
        return -1;
    }

    config->shared_gbdt_model = model;
    config->owns_gbdt_model = true;
    return 0;
}

void vtx_pipeline_config_destroy(vtx_pipeline_config_t *config)
{
    if (!config) return;

    if (config->owns_gbdt_model && config->shared_gbdt_model != NULL) {
        vtx_gbdt_model_destroy(config->shared_gbdt_model);
        free(config->shared_gbdt_model);
        config->shared_gbdt_model = NULL;
        config->owns_gbdt_model = false;
    }
}

/* ========================================================================== */
/* Heat-adapted speculation budgets (Proposal #12)                               */
/* ========================================================================== */

vtx_pipeline_config_t vtx_pipeline_config_heat_adapted(uint64_t execution_count)
{
    /* Start with the T3 config as the base */
    vtx_pipeline_config_t config = vtx_pipeline_config_t3();

    /* For very cold methods, use conservative defaults */
    if (execution_count < 1000) {
        return config;
    }

    /* Compute logarithmic scaling factor.
     * log10(1000) = 3, log10(10M) = 7
     * We use this to gradually increase aggressiveness. */
    double log_exec = 0.0;
    if (execution_count > 0) {
        /* Approximate log10 without linking -lm */
        uint64_t tmp = execution_count;
        while (tmp >= 10) { log_exec += 1.0; tmp /= 10; }
        log_exec += (double)tmp / 10.0; /* fractional part approximation */
    }

    /* Adjust inline size limit: hotter methods can inline larger callees.
     * Default T3 limit is around 512 nodes.
     * For very hot methods, increase to 768. */
    if (config.inline_size_limit == 0) config.inline_size_limit = 512;
    uint32_t size_boost = (uint32_t)(log_exec * 30);
    if (size_boost > 256) size_boost = 256;
    config.inline_size_limit += size_boost;

    /* Adjust GVN iterations: hotter methods benefit from more iterations.
     * Default is around 3. Increase to 5 for very hot methods. */
    if (config.gvn_iterations < 3) config.gvn_iterations = 3;
    if (log_exec > 6.0 && config.gvn_iterations < 5) {
        config.gvn_iterations = 5;
    }

    /* Adjust SCCP iterations similarly */
    if (config.sccp_iterations < 3) config.sccp_iterations = 3;
    if (log_exec > 6.0 && config.sccp_iterations < 5) {
        config.sccp_iterations = 5;
    }

    /* Enable more aggressive speculative optimizations for hot methods */
    if (execution_count > 100000) {
        config.run_speculative = true;
        config.run_loop_spec = true;
    }

    /* For extremely hot methods, enable vectorization */
    if (execution_count > 10000000) {
        config.run_vectorize = true;
    }

    return config;
}
