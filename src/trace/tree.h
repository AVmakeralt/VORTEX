#ifndef VORTEX_TREE_H
#define VORTEX_TREE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/arena.h"
#include "interp/profiler.h"
#include "profile/data.h"
#include "trace/recorder.h"
#include "trace/side_exit.h"

/**
 * VORTEX Trace Trees
 *
 * A trace tree organizes a root trace (the hot loop body) and branch traces
 * (recorded from hot side exits) into a tree structure. When a side exit
 * from the root trace becomes hot (exit count > VTX_SIDE_EXIT_THRESHOLD),
 * a new trace is recorded from the side exit point back to the loop header.
 * This new trace is linked as a branch of the tree.
 *
 * Trace trees enable cross-iteration optimization:
 *   - The root trace represents the most frequently executed loop path.
 *   - Branch traces handle paths that became hot after initial compilation.
 *   - Tree depth is limited to VTX_MAX_TREE_DEPTH to prevent compile-time
 *     explosion.
 *
 * Tree structure:
 *
 *   [Loop Header]
 *       │
 *       ▼
 *   ┌──────────┐
 *   │ Root     │ ← recorded from loop header, follows hot path
 *   │ Trace    │
 *   └──┬───┬───┘
 *      │   │
 *   ┌──┘   └──┐
 *   ▼          ▼
 * ┌─────┐  ┌─────┐
 * │Branch│  │Branch│ ← recorded from hot side exits
 * │  0  │  │  1  │
 * └─────┘  └─────┘
 *
 * Each branch trace starts at the side exit point and is expected to
 * eventually loop back to the loop header (closed branch). If a branch
 * trace doesn't loop back, it's an open branch (method exit).
 */

/* ========================================================================== */
/* Trace tree branch                                                           */
/* ========================================================================== */

/**
 * A branch in the trace tree. Each branch is associated with a side exit
 * from a parent trace (root or another branch) and contains its own
 * recorded trace.
 */
typedef struct vtx_trace_branch vtx_trace_branch_t;

struct vtx_trace_branch {
    vtx_side_exit_id_t  parent_exit_id;  /* side exit that triggered this branch */
    vtx_trace_t        *trace;           /* the recorded trace for this branch */
    uint32_t            depth;           /* depth in the tree (root = 0) */
    vtx_trace_branch_t *parent;         /* parent branch (NULL for root-level branches) */

    /* Child branches: branches that extend from this branch's side exits */
    vtx_trace_branch_t **children;      /* arena-allocated array */
    uint32_t             child_count;
    uint32_t             child_capacity;
};

/* ========================================================================== */
/* Trace tree                                                                  */
/* ========================================================================== */

/**
 * A trace tree rooted at a loop header. Contains the root trace and
 * all branches recorded from hot side exits.
 */
typedef struct {
    vtx_trace_t        *root;           /* the root trace (loop body) */
    vtx_trace_branch_t *root_branch;    /* root branch wrapper (depth 0) */
    uint32_t            depth;          /* maximum depth of any branch */
    uint32_t            branch_count;   /* total number of branches (including nested) */

    /* Method identity for this tree */
    const vtx_method_desc_t *method;    /* method containing the loop */
    uint32_t            loop_header_pc; /* loop header bytecode PC */

    /* Flat list of all branches for easy iteration */
    vtx_trace_branch_t **all_branches;  /* arena-allocated array */
    uint32_t             all_branch_capacity;
} vtx_trace_tree_t;

/* ========================================================================== */
/* Tree building                                                               */
/* ========================================================================== */

/**
 * Build a trace tree starting with a root trace.
 *
 * Records the root trace from the loop header, following the hot path.
 * This creates the initial tree structure. Branches can be added later
 * with vtx_trace_tree_add_branch().
 *
 * Returns the trace tree, or NULL on failure.
 */
vtx_trace_tree_t *vtx_trace_tree_build_root(
    vtx_trace_recorder_t *recorder,
    vtx_graph_t *graph,
    const vtx_bytecode_t *bytecode,
    const vtx_method_desc_t *method,
    uint32_t entry_pc,
    const vtx_profiler_t *profiler,
    const vtx_profile_global_t *profile,
    vtx_arena_t *arena);

/**
 * Add a branch to the trace tree from a hot side exit.
 *
 * Records a new trace starting from the side exit's target PC back to
 * the loop header. The new trace is linked as a child of the branch
 * that owns the side exit.
 *
 * Fails if:
 *   - The tree has reached VTX_MAX_TREE_DEPTH
 *   - The side exit already has a branch
 *   - Recording fails
 *
 * Returns the new branch, or NULL on failure.
 */
vtx_trace_branch_t *vtx_trace_tree_add_branch(
    vtx_trace_recorder_t *recorder,
    vtx_graph_t *graph,
    const vtx_bytecode_t *bytecode,
    const vtx_method_desc_t *method,
    vtx_trace_tree_t *tree,
    vtx_side_exit_id_t side_exit_id,
    vtx_arena_t *arena);

/* ========================================================================== */
/* Tree queries                                                                */
/* ========================================================================== */

/**
 * Get the root trace of the tree.
 */
vtx_trace_t *vtx_trace_tree_root(const vtx_trace_tree_t *tree);

/**
 * Get the total number of traces in the tree (root + branches).
 */
uint32_t vtx_trace_tree_trace_count(const vtx_trace_tree_t *tree);

/**
 * Get the current depth of the tree.
 */
uint32_t vtx_trace_tree_depth(const vtx_trace_tree_t *tree);

/**
 * Check if the tree can accept more branches (depth < VTX_MAX_TREE_DEPTH).
 */
bool vtx_trace_tree_can_branch(const vtx_trace_tree_t *tree);

/**
 * Find all side exits in the tree that are hot enough to record branches.
 * Writes up to max_out side exit IDs into the out array.
 * Returns the actual number of hot exits found.
 */
uint32_t vtx_trace_tree_find_hot_exits(const vtx_trace_tree_t *tree,
                                         vtx_side_exit_id_t *out,
                                         uint32_t max_out);

/**
 * Get a branch by index from the flat branch list.
 * Returns NULL if index is out of bounds.
 */
vtx_trace_branch_t *vtx_trace_tree_get_branch(const vtx_trace_tree_t *tree,
                                                uint32_t index);

/**
 * Find a side exit descriptor by ID across all traces in the tree.
 * Searches the root trace and all branch traces.
 * Returns the side exit, or NULL if not found.
 */
vtx_side_exit_t *vtx_trace_tree_find_exit(const vtx_trace_tree_t *tree,
                                             vtx_side_exit_id_t exit_id);

#endif /* VORTEX_TREE_H */
