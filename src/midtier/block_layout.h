/* ============================================================================ *
 * AI-GENERATED CODE
 *
 * This file was created by an AI assistant (GLM/Z.ai) to implement
 * profile-guided block layout for the VORTEX JIT.
 *
 * Algorithm: "Chain Layout" (also known as "Extended Trace Layout")
 *   1. Start from the entry block.
 *   2. At each block, pick the hottest successor (highest branch probability)
 *      as the fallthrough — place it next in the layout.
 *   3. Cold successors are placed later, after the current hot chain ends.
 *   4. This produces long runs of hot code that are I-cache friendly.
 *
 * The layout reorders the schedule->blocks array in-place. All successor
 * and predecessor indices are remapped to the new positions.
 * ============================================================================ */

#ifndef VORTEX_BLOCK_LAYOUT_H
#define VORTEX_BLOCK_LAYOUT_H

#include <stdint.h>
#include <stdbool.h>
#include "ir/schedule.h"
#include "ir/graph.h"
#include "interp/profiler.h"

/**
 * Run profile-guided block layout on the schedule.
 *
 * Reorders blocks so that hot paths fall through (no branch needed)
 * and cold paths are pushed to the end. Uses branch probability data
 * from the profiler to decide which successor is "hot".
 *
 * @param schedule   The schedule to reorder (modified in-place)
 * @param graph      The SoN graph (for looking up If node bytecode PCs)
 * @param profiler   Profiler data (may be NULL — falls back to heuristic)
 * @param method     Method descriptor (for profiler lookup)
 * @return           Number of blocks reordered (0 if no change)
 */
uint32_t vtx_block_layout_run(vtx_schedule_t *schedule,
                                const vtx_graph_t *graph,
                                const vtx_profiler_t *profiler,
                                const vtx_method_desc_t *method);

#endif /* VORTEX_BLOCK_LAYOUT_H */
