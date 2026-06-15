#ifndef VORTEX_COMPILE_PIPELINE_H
#define VORTEX_COMPILE_PIPELINE_H

#include <stdint.h>
#include <stdbool.h>
#include "ir/graph.h"
#include "ir/gvn.h"
#include "ir/constant_prop.h"
#include "ir/dce.h"
#include "ir/schedule.h"
#include "ir/licm.h"
#include "ir/bounds_check.h"
#include "ir/verify.h"
#include "pea/analysis.h"
#include "pea/cross_object_sr.h"
#include "pea/materialize.h"
#include "pea/virtual.h"
#include "inliner/inference.h"
#include "inliner/transform.h"
#include "inliner/feedback.h"
#include "sota/loop_spec.h"
#include "sota/markov.h"
#include "midtier/midtier.h"
#include "lower/isel.h"
#include "lower/regalloc.h"
#include "lower/emit.h"
#include "lower/guard_emit.h"
#include "runtime/arena.h"
#include "interp/type_feedback.h"
#include "vortex_config.h"

/* Callee graph lookup callback for inlining.
 * Given a method_index (from the call node), returns the callee's SoN graph
 * or NULL if the callee is not available for inlining. */
typedef const vtx_graph_t *(*vtx_callee_lookup_fn)(uint32_t method_index, void *context);

/* Pipeline configuration */
typedef struct {
    bool run_gvn;               /* Global Value Numbering */
    bool run_sccp;              /* Sparse Conditional Constant Propagation */
    bool run_dce;               /* Dead Code Elimination */
    bool run_licm;              /* Loop-Invariant Code Motion */
    bool run_bounds_check;      /* Bounds Check Elimination */
    bool run_pea;               /* Partial Escape Analysis */
    bool run_inlining;          /* ML-guided inlining */
    bool run_verify;            /* IR verification between passes */
    bool run_speculative;       /* Speculative guard insertion (T3 only) */
    bool run_loop_spec;         /* run loop specialization/vectorization */
    bool run_vectorize;         /* actually emit SIMD instructions for vectorizable loops */
    bool run_midtier;           /* run mid-tier type specialization (T1.5) */
    int  gvn_iterations;        /* Max GVN iterations */
    int  sccp_iterations;       /* Max SCCP iterations */
    int  dce_iterations;        /* Max DCE iterations */
    int  inline_size_limit;     /* Max callee node count for inlining (0 = default) */

    /* Callee graph lookup for inlining.
     * If NULL, inlining decisions are recorded but the transform is skipped. */
    vtx_callee_lookup_fn  callee_lookup;
    void                 *callee_lookup_context;

    /* Type feedback for speculative guard insertion (T3 only).
     * If NULL, falls back to node->type_id as a speculative hint.
     * When provided, vtx_type_feedback_get_dominant_call_type() is used
     * to look up the actual observed dominant receiver type at each
     * call site, indexed by the node's bytecode_pc. */
    const vtx_type_feedback_t *type_feedback;

    /* Shared GBDT model for inlining decisions.
     * Initialized once and reused across compilations to avoid
     * creating and loading a new model on every compilation.
     * Call vtx_pipeline_config_init_shared_model() to initialize,
     * and vtx_pipeline_config_destroy() to clean up. */
    vtx_gbdt_model_t *shared_gbdt_model;
    bool              owns_gbdt_model;  /* true if pipeline should free the model */

    /* Phase transition Markov chain for predictive compilation.
     * If non-NULL, the pipeline checks for predicted phase transitions
     * after compilation and proactively compiles methods that will
     * be hot in the next phase. */
    vtx_markov_t     *markov;

    /* Code installation: when these are provided, the pipeline will
     * automatically install the compiled code into the code cache
     * and update the method's compiled_code pointer. Without these,
     * the compiled code is just stored in the result and freed on
     * destruction — it never becomes executable. */
    struct vtx_code_cache       *code_cache;
    struct vtx_method_registry  *method_registry;
    const vtx_method_desc_t     *method;           /* the method being compiled */
    vtx_arena_t                 *install_arena;    /* arena for installation allocations */
} vtx_pipeline_config_t;

/* Pipeline statistics */
typedef struct {
    uint32_t gvn_nodes_merged;
    uint32_t sccp_constants_propagated;
    uint32_t dce_nodes_removed;
    uint32_t licm_nodes_hoisted;
    uint32_t bounds_checks_eliminated;
    uint32_t pea_allocs_eliminated;
    uint32_t inlining_decisions;
    uint32_t inlines_performed;   /* number of call sites actually inlined */
    uint32_t loops_vectorized;    /* number of loops vectorized by loop spec */
    int64_t  total_pipeline_time_ns;
    int64_t  gvn_time_ns;
    int64_t  sccp_time_ns;
    int64_t  dce_time_ns;
    int64_t  licm_time_ns;
    int64_t  bounds_check_time_ns;
    int64_t  pea_time_ns;
    int64_t  inlining_time_ns;
    int64_t  loop_spec_time_ns;
    int64_t  schedule_time_ns;
    int64_t  lowering_time_ns;
} vtx_pipeline_stats_t;

/* Compilation result */
typedef struct {
    bool             success;
    vtx_graph_t     *graph;        /* optimized graph */
    vtx_schedule_t  *schedule;     /* schedule of optimized graph */
    vtx_pipeline_stats_t stats;
    uint8_t        *native_code;   /* emitted x86-64 machine code */
    uint32_t         native_size;  /* size of emitted code */
} vtx_compile_result_t;

/* Get default config for each tier */
vtx_pipeline_config_t vtx_pipeline_config_t1(void);   /* baseline: minimal opts */
vtx_pipeline_config_t vtx_pipeline_config_t1_5(void); /* mid-tier: type-specialized */
vtx_pipeline_config_t vtx_pipeline_config_t2(void);   /* optimizing: full opts */
vtx_pipeline_config_t vtx_pipeline_config_t3(void);   /* speculative: full + speculation */

/**
 * Initialize the shared GBDT model on a pipeline config.
 * Call this once before the first compilation to avoid creating
 * a new model on every compilation. The model is reused across
 * all subsequent compilations with this config.
 *
 * @param config  Pipeline configuration (must not be NULL)
 * @return        0 on success, -1 on failure
 */
int vtx_pipeline_config_init_shared_model(vtx_pipeline_config_t *config);

/**
 * Destroy a pipeline config's shared resources (GBDT model).
 * Call this when the config is no longer needed.
 *
 * @param config  Pipeline configuration (must not be NULL)
 */
void vtx_pipeline_config_destroy(vtx_pipeline_config_t *config);

/**
 * Compute heat-adapted speculation thresholds for a method.
 * The hotter a method, the more aggressive the speculation:
 *   - Very hot methods (E > 10M): lower inline threshold, stronger guards, more deoptless
 *   - Cold methods (E < 1000): default conservative thresholds
 *
 * Uses logarithmic scaling to avoid extreme values.
 *
 * @param execution_count  Method execution count from profile
 * @return                 Dynamically adapted pipeline config
 */
vtx_pipeline_config_t vtx_pipeline_config_heat_adapted(uint64_t execution_count);

/* Run the optimization pipeline on a graph */
int vtx_pipeline_run(vtx_graph_t *graph,
                      const vtx_pipeline_config_t *config,
                      vtx_arena_t *arena,
                      vtx_compile_result_t *result);

/* Destroy a compile result */
void vtx_compile_result_destroy(vtx_compile_result_t *result);

#endif
