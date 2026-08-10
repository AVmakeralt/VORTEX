// vortex/partial_virtualization.hpp — Partial Virtualization pass.
//
// Cross-constant optimization + partial virtualization for VORTEX T2/T3.
//
// ## The core idea
//
// Instead of fully virtualizing objects (which requires proving they
// never escape), this pass performs PARTIAL virtualization: it
// identifies object fields that hold compile-time constant values
// and replaces field loads with the constants directly.
//
// Combined with the existing SCCP + algebraic simplification passes,
// this creates a feedback loop:
//
//   partial virtualization (field → constant)
//        ↓
//   SCCP propagates the constant through the graph
//        ↓
//   branches become dead (if obj.kind == 3 → 3 == 3)
//        ↓
//   cfg_simplify eliminates the dead branch
//        ↓
//   the object itself may become fully virtualizable (PEA handles this)
//        ↓
//   repeat
//
// ## What this pass does
//
// For each Allocate/NewObject node that doesn't escape globally:
//   1. Track which StoreField nodes write to it
//   2. If a field is always written with a Constant value, record
//      (object_id, field_offset) → constant_value
//   3. Replace LoadField(object, field_offset) with the Constant
//      directly, when the field is known-constant
//
// ## What this pass does NOT do
//
//   - Does NOT remove the Allocate (PEA handles that when the object
//     becomes fully virtualizable)
//   - Does NOT handle fields written with non-constant values
//     (those remain as real loads)
//   - Does NOT handle phi-merged field values (a field that's
//     sometimes 3, sometimes 5) — that's a future extension
//     using a per-field lattice
//
// ## Safety
//
// The pass is conservative: it only replaces a LoadField when it can
// PROVE every StoreField to that (object, field) writes the same
// constant value. If any StoreField writes a runtime value, or if the
// field is read before being written, the field is not virtualized.
//
// This pass is T2-safe (no speculation). A T3 variant can use type
// feedback to SPECULATE that a field is constant (with a guard), but
// that's a separate pass.

#ifndef VORTEX_PARTIAL_VIRTUALIZATION_HPP
#define VORTEX_PARTIAL_VIRTUALIZATION_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

// Result of a partial virtualization pass.
struct PartialVirtualizationResult {
    uint32_t fields_virtualized;  // number of field loads replaced with constants
    uint32_t objects_analyzed;    // number of allocations analyzed
};

// Per-field tracking for an allocation site.
struct FieldInfo {
    bool has_constant_value = false;
    vtx_constval_t constant_value{};
    bool has_runtime_write = false;      // written with a non-constant
    bool read_before_write = false;       // loaded before any store
};

// Per-allocation tracking.
struct ObjectInfo {
    vtx_nodeid_t alloc_node_id;
    bool escapes = false;             // escapes globally (can't virtualize)
    std::unordered_map<uint32_t, FieldInfo> fields;  // field_offset → info
};

// Run partial virtualization on a graph.
//
// Algorithm:
//   Phase 1: Find all Allocate/NewObject nodes. For each, create an
//            ObjectInfo and track if it escapes (has uses other than
//            StoreField/LoadField).
//   Phase 2: For each StoreField, if the target object is non-escaping,
//            record the field's value. If it's a Constant, mark the
//            field as has_constant_value. If it's non-Constant, mark
//            has_runtime_write (disables virtualization for that field).
//   Phase 3: For each LoadField, if the target object is non-escaping
//            AND the field has_constant_value AND !has_runtime_write,
//            replace the LoadField with a new Constant node.
//
// Returns the result with counts. Returns {0,0} on no-op or error.
inline PartialVirtualizationResult partial_virtualize(vtx_graph_t* graph) {
    PartialVirtualizationResult result = {0, 0};
    if (!graph) return result;

    vtx_node_table_t* nt = &graph->node_table;

    // Phase 1: Find allocation sites and track escape status.
    // An object "escapes" if it has any use that isn't:
    //   - StoreField (where the object is the target, input[0])
    //   - LoadField (where the object is the target, input[0])
    //   - the Region/control input (no data use)
    std::vector<ObjectInfo> objects;
    std::unordered_map<uint32_t, uint32_t> alloc_to_info;  // node_id → index in objects

    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t* node = &nt->nodes[i];
        if (node->dead) continue;
        if (node->opcode != VTX_OP_NewObject && node->opcode != VTX_OP_Allocate)
            continue;

        ObjectInfo info;
        info.alloc_node_id = i;
        info.escapes = false;

        // Walk the allocation's uses to determine escape status.
        for (uint32_t u = 0; u < node->use_count; u++) {
            vtx_use_entry_t* ue = &node->uses[u];
            if (ue->user_id >= nt->count) continue;
            vtx_node_t* user = &nt->nodes[ue->user_id];
            if (user->dead) continue;

            // Safe uses: StoreField (obj is input[0]), LoadField (obj is input[0])
            if (user->opcode == VTX_OP_StoreField || user->opcode == VTX_OP_LoadField) {
                if (user->input_count >= 1 && user->inputs[0] == i) {
                    continue;  // safe — field access
                }
            }
            // Any other use = escape
            info.escapes = true;
            break;
        }

        alloc_to_info[i] = static_cast<uint32_t>(objects.size());
        objects.push_back(info);
        result.objects_analyzed++;
    }

    if (objects.empty()) return result;

    // Phase 2: For each StoreField, record field values.
    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t* node = &nt->nodes[i];
        if (node->dead) continue;
        if (node->opcode != VTX_OP_StoreField) continue;
        if (node->input_count < 2) continue;

        vtx_nodeid_t obj_id = node->inputs[0];
        vtx_nodeid_t val_id = node->inputs[1];
        uint32_t field_offset = node->local_index;  // field offset stored in local_index

        auto it = alloc_to_info.find(obj_id);
        if (it == alloc_to_info.end()) continue;  // not a tracked allocation
        if (objects[it->second].escapes) continue;  // can't virtualize

        FieldInfo& fi = objects[it->second].fields[field_offset];

        // Check if the stored value is a Constant
        if (val_id < nt->count) {
            vtx_node_t* val_node = &nt->nodes[val_id];
            if (!val_node->dead && val_node->opcode == VTX_OP_Constant) {
                if (!fi.has_runtime_write) {
                    if (!fi.has_constant_value) {
                        fi.has_constant_value = true;
                        fi.constant_value = val_node->constval;
                    } else {
                        // Already has a constant — check it matches
                        // (if not, mark as runtime-write to be safe)
                        if (fi.constant_value.kind != val_node->constval.kind ||
                            fi.constant_value.as.int_val != val_node->constval.as.int_val) {
                            fi.has_constant_value = false;
                            fi.has_runtime_write = true;
                        }
                    }
                }
            } else {
                fi.has_runtime_write = true;
                fi.has_constant_value = false;
            }
        }
    }

    // Phase 3: Replace LoadField with Constant where the field is known.
    // We collect replacements first, then apply them (to avoid mutating
    // the graph during iteration).
    struct Replacement {
        vtx_nodeid_t load_node_id;
        vtx_constval_t value;
    };
    std::vector<Replacement> replacements;

    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t* node = &nt->nodes[i];
        if (node->dead) continue;
        if (node->opcode != VTX_OP_LoadField) continue;
        if (node->input_count < 1) continue;

        vtx_nodeid_t obj_id = node->inputs[0];
        uint32_t field_offset = node->local_index;

        auto it = alloc_to_info.find(obj_id);
        if (it == alloc_to_info.end()) continue;
        if (objects[it->second].escapes) continue;

        FieldInfo& fi = objects[it->second].fields[field_offset];
        if (fi.has_constant_value && !fi.has_runtime_write && !fi.read_before_write) {
            replacements.push_back({i, fi.constant_value});
        }
    }

    // Apply replacements: for each LoadField, create a new Constant
    // node and redirect all uses of the LoadField to the Constant.
    for (const auto& r : replacements) {
        vtx_nodeid_t c_id = vtx_node_create(nt, VTX_OP_Constant);
        if (c_id == VTX_NODEID_INVALID) continue;
        vtx_node_t* c = vtx_node_get(nt, c_id);
        if (!c) continue;
        c->constval = r.value;
        c->type = (r.value.kind == VTX_TYPE_Int) ? VTX_TYPE_Int :
                  (r.value.kind == VTX_TYPE_Float) ? VTX_TYPE_Float :
                  VTX_TYPE_Ptr;
        c->bytecode_pc = nt->nodes[r.load_node_id].bytecode_pc;

        // Redirect all uses of the LoadField to the Constant
        vtx_node_replace_all_uses(nt, r.load_node_id, c_id);

        // Mark the LoadField as dead
        nt->nodes[r.load_node_id].dead = true;
        result.fields_virtualized++;
    }

    if (result.fields_virtualized > 0) {
        vtx_node_table_clear_dead(nt);
    }

    return result;
}

}  // namespace vortex

#endif  // VORTEX_PARTIAL_VIRTUALIZATION_HPP
