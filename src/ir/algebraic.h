#ifndef VORTEX_IR_ALGEBRAIC_H
#define VORTEX_IR_ALGEBRAIC_H

#include "ir/graph.h"

/**
 * Algebraic simplification pass.
 *
 * Folds trivial identities: x+0, x*1, x*0, x-x, !!x, Neg(Neg(x)), etc.
 * Runs after SCCP and before DCE.
 *
 * @param graph  The IR graph
 * @return       Number of nodes simplified
 */
uint32_t vtx_algebraic_simplify_run(vtx_graph_t *graph);

#endif /* VORTEX_IR_ALGEBRAIC_H */
