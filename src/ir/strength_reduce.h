#ifndef VORTEX_STRENGTH_REDUCE_H
#define VORTEX_STRENGTH_REDUCE_H

#include "ir/graph.h"

/**
 * Strength reduction pass: replaces expensive ops with cheaper equivalents
 * when the divisor/multiplier is a power of two.
 *
 *   Mul(x, 2^k)  → Shl(x, k)
 *   Div(x, 1)    → x
 *   Mul(x, 1)    → x
 *   Mul(x, 0)    → 0
 *
 * Note: Div(x, 2^k) → Shr(x, k) and Mod(x, 2^k) → And(x, 2^k-1) are
 * NOT applied because C's / and % truncate toward zero, while SAR and
 * AND round toward -infinity. This causes mismatches for negative
 * dividends. A future fix can add range analysis to prove non-negativity.
 *
 * @param graph  The IR graph
 * @return       Number of nodes replaced
 */
uint32_t vtx_strength_reduce_run(vtx_graph_t *graph);

#endif /* VORTEX_STRENGTH_REDUCE_H */
