// vortex/cross_function_virtual.hpp — Cross-Function Virtual Object
// Continuation.
//
// Pushes PEA (Partial Escape Analysis) across call boundaries.
// Instead of materializing a virtual object when it's passed to
// another function, maintain its virtual representation across
// the inlined call chain.
//
// ## The idea
//
//   foo() creates virtual object
//         ↓
//   bar(obj)  ← inlined, obj stays virtual
//         ↓
//   baz(obj)  ← inlined, obj still virtual
//         ↓
//   returns field  ← field access on virtual object, no materialization
//
// Combined with cross-constant optimization:
//   virtual object → field invariant → cross-function propagation →
//   branch elimination → object disappears entirely
//
// ## Algorithm
//
// This pass runs AFTER inlining (so the call graph is flattened).
// It identifies virtual objects (from PEA) that flow through the
// inlined call chain and ensures they remain virtual.
//
// Specifically, it looks for patterns where:
//   1. An Allocate node is created (the virtual object)
//   2. StoreField nodes write constants to its fields
//   3. LoadField nodes read those fields
//   4. The object is NOT used in any way that would force
//      materialization (no escape to external calls, no storage
//      in a global, no return of the object itself)
//
// When all conditions are met, the object is "cross-function virtual"
// and all LoadField nodes are replaced with the stored constants.
// The Allocate itself becomes dead and is removed by DCE.
//
// ## Relation to partial_virtualization.hpp
//
// This pass is a SUPERSET of partial_virtualization. It performs the
// same field-to-constant replacement, but additionally:
//   - Tracks objects across what WERE call boundaries (now inlined)
//   - Handles the case where a field is written in one "function"
//     and read in another (after inlining, both are in the same graph)
//   - Removes the Allocate when all fields are virtualized
//
// The partial_virtualization pass handles the simple case (single
// function, no inlining). This pass handles the cross-function case
// (after inlining). Running both is safe — partial_virtualization
// runs first, then this pass catches any remaining opportunities.

#ifndef VORTEX_CROSS_FUNCTION_VIRTUAL_HPP
#define VORTEX_CROSS_FUNCTION_VIRTUAL_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "vortex/constval_equal.hpp"
#include <unordered_map>
#include <vector>

#define typeid typeid_
extern "C" {
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "ir/graph.h"
#include "ir/node.h"
}
#undef typeid

namespace vortex {

struct CrossFunctionVirtualResult {
    uint32_t objects_virtualized;  // objects fully virtualized (Allocate removed)
    uint32_t fields_replaced;       // LoadField nodes replaced with constants
};

// Run cross-function virtual object continuation.
//
// This pass is more aggressive than partial_virtualization: it also
// removes the Allocate node when ALL fields are virtualized (the
// object never needs to exist at runtime).
inline CrossFunctionVirtualResult cross_function_virtualize(vtx_graph_t* graph) {
    CrossFunctionVirtualResult result = {0, 0};
    if (!graph) return result;

    vtx_node_table_t* nt = &graph->node_table;

    // Phase 1: Find allocations and track escape status + field writes.
    struct FieldWrite {
        bool is_constant = false;
        vtx_constval_t value{};
        bool has_runtime_write = false;
    };

    struct ObjectState {
        vtx_nodeid_t alloc_id;
        bool escapes = false;
        std::unordered_map<uint32_t, FieldWrite> fields;
        uint32_t total_field_count = 0;
    };

    std::vector<ObjectState> objects;
    std::unordered_map<uint32_t, uint32_t> alloc_to_idx;

    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t* node = &nt->nodes[i];
        if (node->dead) continue;
        if (node->opcode != VTX_OP_NewObject && node->opcode != VTX_OP_Allocate)
            continue;

        ObjectState st;
        st.alloc_id = i;
        st.total_field_count = node->local_index;  // field count stored in local_index
        st.escapes = false;

        // Check escape: any use that isn't StoreField/LoadField
        for (uint32_t u = 0; u < node->use_count; u++) {
            vtx_use_entry_t* ue = &node->uses[u];
            if (ue->user_id >= nt->count) continue;
            vtx_node_t* user = &nt->nodes[ue->user_id];
            if (user->dead) continue;
            if (user->opcode == VTX_OP_StoreField || user->opcode == VTX_OP_LoadField) {
                if (user->input_count >= 1 && user->inputs[0] == i) continue;
            }
            st.escapes = true;
            break;
        }

        alloc_to_idx[i] = static_cast<uint32_t>(objects.size());
        objects.push_back(st);
    }

    if (objects.empty()) return result;

    // Phase 2: Record field writes.
    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t* node = &nt->nodes[i];
        if (node->dead) continue;
        if (node->opcode != VTX_OP_StoreField) continue;
        if (node->input_count < 2) continue;

        vtx_nodeid_t obj_id = node->inputs[0];
        vtx_nodeid_t val_id = node->inputs[1];
        uint32_t field_off = node->local_index;

        auto it = alloc_to_idx.find(obj_id);
        if (it == alloc_to_idx.end()) continue;
        if (objects[it->second].escapes) continue;

        FieldWrite& fw = objects[it->second].fields[field_off];
        if (val_id < nt->count) {
            vtx_node_t* val = &nt->nodes[val_id];
            if (!val->dead && val->opcode == VTX_OP_Constant) {
                if (!fw.has_runtime_write) {
                    if (!fw.is_constant) {
                        fw.is_constant = true;
                        fw.value = val->constval;
                    } else if (!vortex::vtx_constval_equal(fw.value, val->constval)) {
                        fw.is_constant = false;
                        fw.has_runtime_write = true;
                    }
                }
            } else {
                fw.has_runtime_write = true;
                fw.is_constant = false;
            }
        }
    }

    // Phase 3: Replace LoadField with constants.
    struct Replacement {
        vtx_nodeid_t load_id;
        vtx_constval_t value;
    };
    std::vector<Replacement> replacements;

    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t* node = &nt->nodes[i];
        if (node->dead) continue;
        if (node->opcode != VTX_OP_LoadField) continue;
        if (node->input_count < 1) continue;

        vtx_nodeid_t obj_id = node->inputs[0];
        uint32_t field_off = node->local_index;

        auto it = alloc_to_idx.find(obj_id);
        if (it == alloc_to_idx.end()) continue;
        if (objects[it->second].escapes) continue;

        FieldWrite& fw = objects[it->second].fields[field_off];
        if (fw.is_constant && !fw.has_runtime_write) {
            replacements.push_back({i, fw.value});
        }
    }

    for (const auto& r : replacements) {
        vtx_nodeid_t c_id = vtx_node_create(nt, VTX_OP_Constant);
        if (c_id == VTX_NODEID_INVALID) continue;
        vtx_node_t* c = vtx_node_get(nt, c_id);
        if (!c) continue;
        c->constval = r.value;
        c->type = (r.value.kind == VTX_TYPE_Int) ? VTX_TYPE_Int :
                  (r.value.kind == VTX_TYPE_Float) ? VTX_TYPE_Float :
                  VTX_TYPE_Ptr;
        c->bytecode_pc = nt->nodes[r.load_id].bytecode_pc;
        vtx_node_replace_all_uses(nt, r.load_id, c_id);
        nt->nodes[r.load_id].dead = true;
        result.fields_replaced++;
    }

    // Phase 4: Remove Allocate nodes for fully-virtualized objects.
    // An object is "fully virtualized" if:
    //   - It doesn't escape
    //   - All its fields have constant writes
    //   - All LoadField nodes have been replaced
    //   - The only remaining uses are dead StoreField nodes
    for (auto& obj : objects) {
        if (obj.escapes) continue;
        if (obj.total_field_count == 0) continue;

        // Check all fields are constant
        bool all_constant = true;
        for (uint32_t f = 0; f < obj.total_field_count; f++) {
            auto fit = obj.fields.find(f);
            if (fit == obj.fields.end() || !fit->second.is_constant || fit->second.has_runtime_write) {
                all_constant = false;
                break;
            }
        }
        if (!all_constant) continue;

        // Check no live uses remain (all StoreField/LoadField should be dead
        // or replaceable). The LoadFields are already replaced. The
        // StoreFields are side-effect-free when the object is dead
        // (they only write to a dead object).
        vtx_node_t* alloc = &nt->nodes[obj.alloc_id];
        bool has_live_use = false;
        for (uint32_t u = 0; u < alloc->use_count; u++) {
            vtx_use_entry_t* ue = &alloc->uses[u];
            if (ue->user_id >= nt->count) continue;
            vtx_node_t* user = &nt->nodes[ue->user_id];
            if (!user->dead) {
                // StoreField to a dead object is safe to remove
                if (user->opcode == VTX_OP_StoreField) continue;
                // Any other live use = can't remove
                has_live_use = true;
                break;
            }
        }
        if (!has_live_use) {
            // Mark all StoreField nodes as dead
            for (uint32_t u = 0; u < alloc->use_count; u++) {
                vtx_use_entry_t* ue = &alloc->uses[u];
                if (ue->user_id >= nt->count) continue;
                vtx_node_t* user = &nt->nodes[ue->user_id];
                if (!user->dead && user->opcode == VTX_OP_StoreField) {
                    user->dead = true;
                }
            }
            // Mark the Allocate as dead
            alloc->dead = true;
            result.objects_virtualized++;
        }
    }

    if (result.fields_replaced > 0 || result.objects_virtualized > 0) {
        vtx_node_table_clear_dead(nt);
    }

    return result;
}

}  // namespace vortex

#endif  // VORTEX_CROSS_FUNCTION_VIRTUAL_HPP
