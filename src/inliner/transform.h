#ifndef VORTEX_INLINER_TRANSFORM_H
#define VORTEX_INLINER_TRANSFORM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "ir/gvn.h"
#include "runtime/arena.h"
#include "interp/type_feedback.h"

/* Forward declaration — defined in compile/pipeline.h.
 * Avoids circular include between transform.h ↔ pipeline.h. */
typedef const vtx_graph_t *(*vtx_callee_lookup_fn)(uint32_t method_index, void *context);

/**
 * VORTEX ML Inliner — Inlining Transform
 *
 * Performs the actual inlining of a callee graph into a caller graph
 * at a specific call site. The transform follows the Sea-of-Nodes
 * inlining algorithm:
 *
 *   1. Clone the callee's SoN subgraph into the caller's node table
 *   2. Replace Parameter nodes in the clone with the call's argument nodes
 *   3. Replace Return nodes in the clone with Phi nodes merging into the caller
 *   4. Thread the callee's memory chain into the caller's memory chain
 *   5. Add FrameState at the inlined entry point for deoptimization
 *   6. Run GVN on the inlined subgraph to eliminate redundancies
 *
 * The caller graph is modified in place. The callee graph is NOT modified.
 *
 * Important: After inlining, the caller may need DCE and re-scheduling.
 * The transform does NOT run these passes automatically — the caller
 * must schedule them.
 */

/* ========================================================================== */
/* Inline result                                                               */
/* ========================================================================== */

typedef struct {
    bool    success;           /* true if inlining succeeded */
    uint32_t nodes_added;      /* number of nodes added to the caller graph */
    uint32_t nodes_removed;    /* number of nodes removed (call node, etc.) */
    uint32_t phis_created;     /* number of Phi nodes created for returns */

    /* The node that produces the inlined call's return value.
     * VTX_NODEID_INVALID if the callee returns void. */
    vtx_nodeid_t return_value_node;

    /* The new memory state after the inlined callee.
     * The caller should use this as its current memory state. */
    vtx_nodeid_t new_memory_node;
} vtx_inline_result_t;

/* ========================================================================== */
/* Chain inlining configuration (Proposal #6)                                    */
/* ========================================================================== */

/**
 * Maximum depth of recursive inlining for hyper-stable call chains.
 * Beyond this depth, further inlining is skipped even if the site
 * is hyper-stable. This prevents unbounded compile time.
 */
#define VTX_MAX_CHAIN_DEPTH 4

/**
 * Maximum cumulative node count for a chain inlining sequence.
 * If the total inlined node count exceeds this budget, the chain
 * is terminated even if more hyper-stable sites are available.
 */
#define VTX_CHAIN_INLINE_BUDGET 1024

/**
 * Result of a chain inlining operation.
 * Tracks the cumulative statistics across all levels of inlining.
 */
typedef struct {
    uint32_t chain_depth;           /* how many levels were inlined */
    uint32_t cumulative_node_count; /* total nodes added across all levels */
    uint32_t sites_inlined;         /* total call sites inlined in the chain */
    bool     budget_exhausted;      /* true if chain stopped due to budget */
    bool     depth_exhausted;       /* true if chain stopped due to max depth */
} vtx_chain_inline_result_t;

/* ========================================================================== */
/* Transform API                                                               */
/* ========================================================================== */

/**
 * Inline a callee graph into a caller graph at a specific call site.
 *
 * @param caller_graph  The caller's SoN graph (modified in place)
 * @param call_node     The call node (CallStatic/CallVirtual/CallInterface) NodeID
 * @param callee_graph  The callee's SoN graph (NOT modified, treated as read-only)
 * @param arena         Arena for temporary allocations during inlining
 * @return              Result structure indicating success/failure and metrics
 */
vtx_inline_result_t vtx_inline_transform(vtx_graph_t *caller_graph,
                                          vtx_nodeid_t call_node,
                                          const vtx_graph_t *callee_graph,
                                          vtx_arena_t *arena);

/**
 * Check whether a call node can be inlined.
 *
 * A call can be inlined if:
 *   - The call node exists and is not dead
 *   - The callee graph is non-empty
 *   - The callee is not too large (node count < VTX_INLINE_SIZE_LIMIT)
 *   - The inlining depth has not been exceeded
 *   - The call is not recursive (callee == caller)
 *
 * @param caller_graph  The caller's SoN graph
 * @param call_node     The call node to check
 * @param callee_graph  The callee's SoN graph
 * @param current_depth Current inlining depth
 * @return              true if the call can be inlined
 */
bool vtx_inline_can_inline(const vtx_graph_t *caller_graph,
                            vtx_nodeid_t call_node,
                            const vtx_graph_t *callee_graph,
                            uint32_t current_depth);

/**
 * Perform chain inlining for a hyper-stable call site.
 *
 * Recursively inlines the callee and scans the inlined subgraph for
 * further hyper-stable call sites. The chain terminates when:
 *   (a) a call site is not hyper-stable
 *   (b) cumulative inlined size exceeds VTX_CHAIN_INLINE_BUDGET
 *   (c) chain depth exceeds VTX_MAX_CHAIN_DEPTH
 *
 * Each level receives a composite guard (from Proposal #2), so the total
 * guard cost for a 4-deep chain is 4 compares regardless of argument count.
 *
 * @param graph           The SoN graph (modified in place)
 * @param call_node       The initial call site NodeID to start chaining from
 * @param type_feedback   Type feedback for checking hyper-stability
 * @param callee_lookup   Function to look up callee graphs by method_index
 * @param lookup_context  Context for callee_lookup
 * @param arena           Arena for temporary allocations
 * @return                Chain inline result with depth and statistics
 */
vtx_chain_inline_result_t vtx_inline_chain(
    vtx_graph_t *graph,
    vtx_nodeid_t call_node,
    const vtx_type_feedback_t *type_feedback,
    vtx_callee_lookup_fn callee_lookup,
    void *lookup_context,
    vtx_arena_t *arena);

#endif /* VORTEX_INLINER_TRANSFORM_H */
