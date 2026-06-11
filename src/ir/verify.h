#ifndef VORTEX_VERIFY_H
#define VORTEX_VERIFY_H

#include <stdint.h>
#include <stdbool.h>
#include "ir/graph.h"

/**
 * VORTEX IR Verification
 *
 * Validates the integrity of the Sea-of-Nodes graph after each
 * optimization pass. Only active when VORTEX_ENABLE_VERIFY is defined;
 * in release builds, vtx_verify_graph() is a no-op that returns true.
 *
 * Checks performed:
 *   1. All inputs of every live node are valid NodeIDs (in range, not dead).
 *   2. No data cycles: data edges do not form a cycle in the graph.
 *      (Control back-edges in loops are allowed.)
 *   3. Phi input count matches Region predecessor count:
 *      Phi(Region) has one data input per Region predecessor, plus the
 *      Region itself as a control input.
 *   4. Valid memory chains: memory nodes form a properly threaded chain.
 *   5. No dead nodes remain after DCE (if verify_post_dce flag is set).
 *   6. Output counts are consistent: for each node, its output_count
 *      equals the number of times it appears as an input of other live nodes.
 *   7. Start node has no inputs.
 *   8. All Region nodes have at least one input.
 *   9. If nodes have exactly 2 inputs (control + condition).
 *  10. Return nodes have 1-2 inputs (control + optional value).
 */

/**
 * Verify the graph integrity. Returns true if all checks pass.
 *
 * When VORTEX_ENABLE_VERIFY is not defined, this function returns true
 * without doing any work.
 */
bool vtx_verify_graph(const vtx_graph_t *graph);

/**
 * Verify the graph after DCE — additionally checks that no dead nodes
 * exist (since DCE should have removed them all).
 */
bool vtx_verify_graph_post_dce(const vtx_graph_t *graph);

#endif /* VORTEX_VERIFY_H */
