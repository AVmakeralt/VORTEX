/* ============================================================================ *
 * AI-GENERATED CODE NOTICE
 *
 * This file was written or substantially modified by an AI assistant
 * (GLM/Z.ai). It is part of the VORTEX JIT compiler project.
 *
 * Human-written original code exists in the interpreter dispatch loop
 * (src/interp/), baseline codegen (src/baseline/codegen.c), runtime
 * (src/runtime/), and the main entry point (src/main_new.c).
 *
 * AI-generated components include: IR construction, optimization passes
 * (GVN, SCCP, DCE, LICM, strength reduction, SMI tag elision, PEA),
 * instruction selection, register allocation, code emission, the
 * compilation pipeline, the decision engine, PGO subsystems (phase
 * partitioning, ensemble aggregation, input-shape-keyed profiles, patch
 * logging, T1 code persistence), deopt/OSR, trace recording, guard
 * optimization, and the inliner.
 *
 * If you are reviewing this code, please verify correctness independently.
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
