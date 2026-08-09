// advanced_passes.cpp — C-callable entry points for advanced optimization
// passes:
//   1. Temporal Constant Propagation
//   2. Cross-Function Virtual Object Continuation
//
// Per VORTEX Engineering Standards §1.1, new code is in C++.

#define typeid typeid_
extern "C" {
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "ir/graph.h"
#include "ir/node.h"
}
#undef typeid

#include "vortex/temporal_constant_propagation.hpp"
#include "vortex/cross_function_virtual.hpp"

#include <cstdlib>
#include <cstring>

extern "C" {

// Run temporal constant propagation.
// Returns the number of LoadField nodes replaced with constants.
__attribute__((visibility("default"), used))
uint32_t vtx_temporal_constant_run(vtx_graph_t* graph) {
    if (!graph) return 0;
    auto result = vortex::temporal_constant_propagate(graph);
    return result.loads_replaced;
}

// Run cross-function virtual object continuation.
// Returns the number of objects fully virtualized (Allocate removed).
__attribute__((visibility("default"), used))
uint32_t vtx_cross_function_virtual_run(vtx_graph_t* graph) {
    if (!graph) return 0;
    auto result = vortex::cross_function_virtualize(graph);
    return result.objects_virtualized;
}

}  // extern "C"
