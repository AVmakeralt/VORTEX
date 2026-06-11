#ifndef VORTEX_GVN_H
#define VORTEX_GVN_H

#include <stdint.h>
#include <stdbool.h>
#include "ir/graph.h"

/**
 * VORTEX Global Value Numbering (GVN)
 *
 * Hash-based GVN that identifies and eliminates redundant computations.
 * Two nodes are congruent if they have:
 *   - Same opcode
 *   - Same type
 *   - Same number of inputs
 *   - All corresponding inputs are congruent (recursively)
 *   - Same auxiliary data (condition code, constant value, etc.)
 *
 * GVN eliminates:
 *   - Redundant pure computations (CSE)
 *   - Redundant type checks (if a dominating check proved the type)
 *   - Redundant null checks (if a dominating guard proved non-null)
 *
 * The algorithm:
 *   1. Partition all nodes into initial congruence classes by (opcode, type).
 *   2. Refine partitions iteratively: two nodes in the same class are split
 *      if their inputs belong to different classes.
 *   3. Repeat until no class changes (fixed point).
 *   4. For each class with >1 node, keep the first and redirect all uses
 *      of the others to it.
 *
 * The graph is modified in place: redundant nodes are disconnected (inputs
 * cleared, marked dead) but not removed from the table.
 */

/**
 * Run GVN on the graph. Modifies the graph in place.
 * Returns the number of nodes eliminated.
 */
uint32_t vtx_gvn_run(vtx_graph_t *graph);

/**
 * Compute a hash for a node suitable for GVN.
 * The hash considers: opcode, type, input value numbers, condition code,
 * constant value (if Constant opcode), and other auxiliary data.
 */
uint32_t vtx_gvn_node_hash(const vtx_node_t *node, const vtx_node_table_t *table);

/**
 * Check if two nodes are congruent (same value number).
 * Returns true if they are provably the same computation.
 */
bool vtx_gvn_nodes_congruent(const vtx_node_t *a, const vtx_node_t *b,
                              const vtx_node_table_t *table);

#endif /* VORTEX_GVN_H */
