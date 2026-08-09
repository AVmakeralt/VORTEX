// partial_virtualization.cpp — C-callable entry point for the partial
// virtualization pass.
//
// Per VORTEX Engineering Standards §1.1, new code is in C++. This file
// exposes a C-callable API so the C pipeline can invoke the pass.

#define typeid typeid_
extern "C" {
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "ir/graph.h"
#include "ir/node.h"
}
#undef typeid

#include "vortex/partial_virtualization.hpp"

#include <cstdlib>
#include <cstring>

extern "C" {

// Run partial virtualization on a graph.
// Returns the number of field loads replaced with constants.
// 0 means no virtualization was possible.
__attribute__((visibility("default"), used))
uint32_t vtx_partial_virtualize_run(vtx_graph_t* graph) {
    if (!graph) return 0;
    auto result = vortex::partial_virtualize(graph);
    return result.fields_virtualized;
}

}  // extern "C"
