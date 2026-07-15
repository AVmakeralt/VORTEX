/* ============================================================================ *
 * AI-GENERATED CODE
 *
 * Representation inference pass — inserts explicit UnboxInt/BoxInt nodes
 * to eliminate SMI tag/untag overhead in hot arithmetic chains.
 *
 * Algorithm:
 *   1. Find arithmetic nodes (Add, Sub, Mul, Cmp) whose inputs are tagged SMIs.
 *   2. For each such chain, insert UnboxInt at the chain entry and BoxInt
 *      at every exit to a non-arithmetic consumer.
 *   3. Mark the arithmetic nodes as RAW_INT so the isel skips per-op untag/retag.
 *
 * The chain entry is where a tagged value enters arithmetic. The exits are
 * where an arithmetic result flows to a non-arithmetic consumer (Return,
 * Store, Call, Phi that exits the chain, If/Cmp boundary).
 * ============================================================================ */

#ifndef VORTEX_REP_INFER_H
#define VORTEX_REP_INFER_H

#include <stdint.h>
#include <stdbool.h>
#include "ir/graph.h"
#include "runtime/arena.h"

/**
 * Run representation inference on the graph.
 *
 * Inserts UnboxInt nodes before arithmetic chains and BoxInt nodes at
 * chain exits. Marks arithmetic nodes as RAW_INT so the isel emits
 * raw arithmetic instead of per-op untag/retag.
 *
 * @param graph   The IR graph (modified in-place)
 * @param arena   Arena for temporary allocations
 * @return        Number of UnboxInt/BoxInt nodes inserted
 */
uint32_t vtx_rep_infer_run(vtx_graph_t *graph, vtx_arena_t *arena);

#endif /* VORTEX_REP_INFER_H */
