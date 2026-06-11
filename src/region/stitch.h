#ifndef VORTEX_STITCH_H
#define VORTEX_STITCH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "runtime/arena.h"
#include "trace/tree.h"
#include "trace/side_exit.h"
#include "region/budget.h"

/**
 * VORTEX Hyperblock Stitching
 *
 * Stitches a trace tree's root trace and hot branches into a single
 * hyperblock — a single-entry, multiple-exit SoN subgraph that enables
 * cross-trace optimization.
 *
 * Stitching algorithm:
 *   1. Start with the root trace's node sequence.
 *   2. For each side exit whose exit count exceeds VTX_STITCH_THRESHOLD:
 *      a. Find the Guard node at the side exit point.
 *      b. Replace the Guard with the branch trace's nodes.
 *      c. Add a rejoinder Guard at the end of the inlined branch to
 *         rejoin the main path (the target is the node after the
 *         original guard in the root trace).
 *   3. Repeat for nested branches up to depth limit.
 *   4. Result: a single-entry, multiple-exit hyperblock.
 *
 * The hyperblock preserves the semantics of each individual trace:
 *   - The main path (root trace) is unchanged.
 *   - Branches are only inlined at guard points where the side exit
 *     was hot enough.
 *   - Every exit from the hyperblock has deopt information.
 */

/* ========================================================================== */
/* Hyperblock exit descriptor                                                  */
/* ========================================================================== */

/**
 * Describes an exit point from the hyperblock. Each exit corresponds
 * to either an un-inlined side exit (cold path) or a rejoinder guard
 * at the end of an inlined branch.
 */
typedef struct {
    vtx_nodeid_t        exit_node;     /* the node that causes the exit (Guard/If) */
    uint32_t            target_pc;     /* bytecode PC to resume at */
    vtx_nodeid_t       *stack_state;   /* operand stack NodeIDs at exit */
    uint32_t            stack_depth;   /* number of stack entries */
    vtx_side_exit_reason_t reason;     /* why this exit exists */
} vtx_hyperblock_exit_t;

/* ========================================================================== */
/* Hyperblock structure                                                        */
/* ========================================================================== */

/**
 * A hyperblock: a single-entry, multiple-exit SoN subgraph produced
 * by stitching a trace tree.
 */
typedef struct {
    /* Entry point */
    vtx_nodeid_t        entry_node;    /* the LoopBegin node (single entry) */

    /* All nodes in the hyperblock, in topological order */
    vtx_nodeid_t       *nodes;         /* arena-allocated array */
    uint32_t            node_count;
    uint32_t            node_capacity;

    /* Exit points */
    vtx_hyperblock_exit_t *exits;      /* arena-allocated array */
    uint32_t              exit_count;
    uint32_t              exit_capacity;

    /* Source trace tree */
    const vtx_trace_tree_t *source_tree;

    /* Estimated native code size (updated during stitching) */
    uint32_t            estimated_native_size;

    /* Control and memory state at the end of the main path */
    vtx_nodeid_t        terminal_control;
    vtx_nodeid_t        terminal_memory;
} vtx_hyperblock_t;

/* ========================================================================== */
/* Stitcher                                                                    */
/* ========================================================================== */

/**
 * The stitcher combines trace tree traces into hyperblocks.
 */
typedef struct {
    uint32_t branches_stitched;  /* number of branches successfully stitched */
    uint32_t branches_skipped;   /* number of branches skipped (too cold or over budget) */
} vtx_stitcher_t;

/* ========================================================================== */
/* Stitcher lifecycle                                                          */
/* ========================================================================== */

/**
 * Initialize the stitcher.
 */
int vtx_stitcher_init(vtx_stitcher_t *stitcher);

/**
 * Destroy the stitcher.
 */
void vtx_stitcher_destroy(vtx_stitcher_t *stitcher);

/* ========================================================================== */
/* Stitching                                                                   */
/* ========================================================================== */

/**
 * Stitch a trace tree into a hyperblock.
 *
 * The root trace forms the main path. Hot branches (side exit count >
 * VTX_STITCH_THRESHOLD) are inlined at guard points. Each inlined branch
 * gets a rejoinder guard at its end to fall back to the interpreter if
 * the branch doesn't loop back.
 *
 * The budget checker is used to ensure the resulting hyperblock doesn't
 * exceed size limits. Branches that would cause the budget to be exceeded
 * are skipped.
 *
 * Returns the hyperblock, or NULL on failure.
 */
vtx_hyperblock_t *vtx_stitcher_stitch(vtx_stitcher_t *stitcher,
                                       const vtx_trace_tree_t *tree,
                                       vtx_graph_t *graph,
                                       vtx_arena_t *arena);

/* ========================================================================== */
/* Hyperblock access                                                           */
/* ========================================================================== */

/**
 * Get the entry node of the hyperblock.
 */
vtx_nodeid_t vtx_hyperblock_entry(const vtx_hyperblock_t *hb);

/**
 * Get the number of nodes in the hyperblock.
 */
uint32_t vtx_hyperblock_node_count(const vtx_hyperblock_t *hb);

/**
 * Get the number of exit points in the hyperblock.
 */
uint32_t vtx_hyperblock_exit_count(const vtx_hyperblock_t *hb);

/**
 * Get a node from the hyperblock by index.
 * Returns VTX_NODEID_INVALID if out of bounds.
 */
vtx_nodeid_t vtx_hyperblock_get_node(const vtx_hyperblock_t *hb, uint32_t index);

/**
 * Get an exit descriptor from the hyperblock by index.
 * Returns NULL if out of bounds.
 */
const vtx_hyperblock_exit_t *vtx_hyperblock_get_exit(const vtx_hyperblock_t *hb,
                                                       uint32_t index);

#endif /* VORTEX_STITCH_H */
