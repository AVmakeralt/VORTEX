// representation_selection.cpp — C-callable entry point.

#define typeid typeid_
extern "C" {
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "ir/graph.h"
#include "ir/node.h"
}
#undef typeid

#include "vortex/representation_selection.hpp"

extern "C" {

// Run V8-style representation selection.
// Returns the number of nodes marked RAW_INT (including Phis).
__attribute__((visibility("default"), used))
uint32_t vtx_representation_selection_run(vtx_graph_t* graph) {
    if (!graph) return 0;
    auto result = vortex::run_representation_selection(graph);
    return result.nodes_marked_raw;
}

}  // extern "C"
