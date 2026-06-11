#ifndef VORTEX_GRAPH_H
#define VORTEX_GRAPH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/node.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/arena.h"

/**
 * VORTEX SoN Graph — Construction from Bytecode
 *
 * The graph is the central data structure for the Sea-of-Nodes IR.
 * It owns the node table and provides the entry point for building
 * the graph from a bytecode method.
 *
 * Construction algorithm:
 *  1. Create Start node + Parameter projections (one per method parameter).
 *  2. Walk bytecode in linear order, identifying basic block boundaries.
 *  3. Each basic block entry becomes a Region node (or LoopBegin for loop headers).
 *  4. Within each block, emit data nodes for arithmetic, memory nodes for
 *     loads/stores (threaded as a chain), and control nodes for branches.
 *  5. At Region merge points, insert Phi nodes for variables that differ
 *     across predecessors.
 *  6. Loop back-edges connect to LoopBegin nodes; LoopEnd marks the latch.
 *  7. Memory state is threaded: each memory-consuming node takes the previous
 *     memory node as input and produces a new memory state.
 */

/* ========================================================================== */
/* Basic block info (used during construction)                                 */
/* ========================================================================== */

typedef struct {
    size_t  start_pc;           /* bytecode PC where this block starts */
    size_t  end_pc;             /* bytecode PC of the last instruction (exclusive) */
    bool    is_loop_header;     /* true if this block is a loop header */
    bool    is_loop_end;        /* true if this block ends with a backward branch */
    bool    is_catch_handler;   /* true if this block starts at a catch handler */

    /* SoN nodes created for this block */
    vtx_nodeid_t region_node;   /* Region/LoopBegin node ID for this block */
    vtx_nodeid_t control_node;  /* current control output for this block */
    vtx_nodeid_t memory_node;   /* current memory state for this block */

    /* Per-block local variable map (local index → NodeID producing that value) */
    vtx_nodeid_t *locals;       /* array of size max_locals, arena-allocated */

    /* Predecessor block indices (into the blocks array) */
    uint32_t     *pred_indices; /* arena-allocated */
    uint32_t      pred_count;
    uint32_t      pred_capacity;

    /* Successor block indices */
    uint32_t     *succ_indices;
    uint32_t      succ_count;
    uint32_t      succ_capacity;
} vtx_block_info_t;

/* ========================================================================== */
/* Graph structure                                                             */
/* ========================================================================== */

typedef struct {
    vtx_node_table_t node_table;     /* owns all nodes */
    vtx_nodeid_t     start_node;     /* the Start node */
    vtx_nodeid_t     entry_control;  /* control output of Start (=Start itself) */
    vtx_nodeid_t     entry_memory;   /* initial memory state (Province after Start) */
    uint32_t         parameter_count;/* number of Parameter projections */
    vtx_nodeid_t    *parameters;     /* array of Parameter node IDs, malloc'd */

    /* Block info from construction (arena-allocated, valid during build) */
    vtx_block_info_t *blocks;        /* array of block descriptors */
    uint32_t          block_count;
    uint32_t          block_capacity;
} vtx_graph_t;

/**
 * Initialize an empty graph. Creates the node table and Start node.
 * Returns 0 on success, -1 on failure.
 */
int vtx_graph_init(vtx_graph_t *graph, uint32_t max_params);

/**
 * Destroy the graph and free all memory.
 */
void vtx_graph_destroy(vtx_graph_t *graph);

/**
 * Build the SoN graph from bytecode.
 *
 * This is the main entry point: walks the bytecode, identifies basic blocks,
 * constructs the Sea-of-Nodes IR with proper control flow, memory chains,
 * and Phi nodes at merge points.
 *
 * The graph must be initialized before calling this function.
 * The arena is used for temporary construction data (block info, locals).
 *
 * Returns 0 on success, -1 on failure.
 */
int vtx_graph_build(vtx_graph_t *graph,
                    const vtx_bytecode_t *bytecode,
                    const vtx_method_desc_t *method,
                    vtx_arena_t *arena);

/**
 * Look up a node in the graph by ID. Convenience wrapper.
 */
vtx_node_t *vtx_graph_node(vtx_graph_t *graph, vtx_nodeid_t id);

/**
 * Get the total number of nodes in the graph.
 */
uint32_t vtx_graph_node_count(const vtx_graph_t *graph);

/**
 * Print the graph to stderr for debugging.
 */
void vtx_graph_print(const vtx_graph_t *graph);

#endif /* VORTEX_GRAPH_H */
