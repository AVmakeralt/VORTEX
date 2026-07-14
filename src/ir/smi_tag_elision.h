/* ============================================================================ *
 * AI-MODIFIED CODE
 *
 * This file was originally written by a human developer. It has been
 * substantially modified by an AI assistant (GLM/Z.ai) for bug fixes,
 * performance improvements, and feature additions.
 *
 * Original human-written structure is preserved; AI changes are marked
 * with bug fix IDs (B1-B28) or perf notes (Perf 1-10) in comments.
 *
 * If reviewing, please verify AI changes against the original logic.
 * ============================================================================ */

#ifndef VORTEX_SMI_TAG_ELISION_H
#define VORTEX_SMI_TAG_ELISION_H

#include "ir/graph.h"

/**
 * SMI tag elision pass: marks straight-line arithmetic chains as RAW_INT
 * so the isel skips per-op untag/retag. One untag at chain entry, one
 * retag at chain exit, instead of untag+retag per op.
 *
 * @param graph  The IR graph
 * @return       Number of nodes marked RAW_INT
 */
uint32_t vtx_smi_tag_elision_run(vtx_graph_t *graph);

#endif /* VORTEX_SMI_TAG_ELISION_H */
