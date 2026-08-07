#ifndef VORTEX_IR_CFG_SIMPLIFY_H
#define VORTEX_IR_CFG_SIMPLIFY_H

#include "ir/graph.h"

/**
 * CFG simplification pass.
 *
 * Collapses single-input Regions, folds constant If conditions,
 * and resolves Goto→Goto chains.
 *
 * @param graph  The IR graph
 * @return       Number of nodes simplified
 */
uint32_t vtx_cfg_simplify_run(vtx_graph_t *graph);

#endif /* VORTEX_IR_CFG_SIMPLIFY_H */
