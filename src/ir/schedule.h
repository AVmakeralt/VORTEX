#ifndef VORTEX_SCHEDULE_H
#define VORTEX_SCHEDULE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ir/graph.h"
#include "runtime/arena.h"

/**
 * VORTEX SoN → Basic Block Scheduling
 *
 * Converts the Sea-of-Nodes graph back into a scheduled form (basic blocks)
 * suitable for lowering to machine code.
 *
 * Algorithm:
 *   1. Identify control nodes and build the basic block skeleton.
 *   2. Assign each data node to its earliest legal block:
 *      - Must be in a block that dominates all its uses.
 *      - Must respect memory dependencies (load after store).
 *      - Must be in a block that is dominated by all its inputs.
 *   3. Hoist loop-invariant nodes out of loops.
 *   4. Sink condition-dependent nodes into their target blocks.
 *   5. Place guards as early as possible (fail-fast).
 *   6. Place allocations as late as possible (avoid unnecessary allocation).
 *
 * The schedule is a list of basic blocks, each containing an ordered list
 * of node IDs. The ordering respects data dependencies within the block.
 */

/* ========================================================================== */
/* Scheduled block                                                             */
/* ========================================================================== */

typedef struct {
    vtx_nodeid_t  *nodes;        /* ordered list of node IDs in this block */
    uint32_t       node_count;   /* number of nodes */
    uint32_t       node_capacity;/* allocated capacity */

    /* Block metadata */
    vtx_nodeid_t   region_node;  /* the Region/LoopBegin node for this block */
    uint32_t       block_id;     /* sequential block index */
    uint32_t       loop_depth;   /* nesting depth (0 = not in a loop) */
    bool           is_loop_header;
    bool           is_catch;

    /* Successor / predecessor block indices */
    uint32_t      *succ_blocks;  /* indices into the schedule's blocks array */
    uint32_t       succ_count;
    uint32_t      *pred_blocks;
    uint32_t       pred_count;
} vtx_schedule_block_t;

/* ========================================================================== */
/* Schedule structure                                                          */
/* ========================================================================== */

typedef struct {
    vtx_schedule_block_t *blocks;   /* array of scheduled blocks */
    uint32_t              count;    /* number of blocks */
    uint32_t              capacity; /* allocated capacity */

    /* Per-node block assignment: node_id → block index.
     * Allocated for all nodes in the graph. */
    uint32_t             *node_block;  /* array indexed by node ID */
    uint32_t              node_block_count; /* size of node_block array */
} vtx_schedule_t;

/**
 * Run the scheduler on the graph. Produces a schedule structure.
 * The arena is used for temporary allocations during scheduling.
 *
 * Returns 0 on success, -1 on failure. The caller must free the
 * schedule with vtx_schedule_destroy.
 */
int vtx_schedule_run(vtx_graph_t *graph, vtx_arena_t *arena, vtx_schedule_t *schedule);

/**
 * Destroy a schedule and free all memory.
 */
void vtx_schedule_destroy(vtx_schedule_t *schedule);

/**
 * Get the block index that a node is scheduled in.
 * Returns (uint32_t)-1 if the node is not scheduled.
 */
uint32_t vtx_schedule_node_block(const vtx_schedule_t *schedule, vtx_nodeid_t node_id);

/**
 * Print the schedule to stderr for debugging.
 */
void vtx_schedule_print(const vtx_schedule_t *schedule, const vtx_graph_t *graph);

#endif /* VORTEX_SCHEDULE_H */
