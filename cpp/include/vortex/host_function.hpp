// vortex/host_function.hpp — Register C++ functions callable from bytecode.
//
// Host functions allow bytecode to call into C++ code via the
// VT_OP_CALL_RUNTIME opcode. The host function receives VORTEX values
// as arguments and returns a VORTEX value.
//
// Usage:
//   vortex::Runtime rt = ...;
//
//   // Register a host function
//   rt.register_host_function("add", [](int argc, vortex::Value* argv) {
//       if (argc < 2) return vortex::Value::undefined();
//       return vortex::Value::smi(argv[0].to_int() + argv[1].to_int());
//   });
//
//   // Bytecode can call it via CALL_RUNTIME with the function's ID
//   // (which is looked up by name at registration time).
//
// The host function registry is global (process-wide) for simplicity,
// matching VORTEX's existing CALL_RUNTIME mechanism which uses a global
// function ID table.

#ifndef VORTEX_HOST_FUNCTION_HPP
#define VORTEX_HOST_FUNCTION_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "vortex/value.hpp"
#include "vortex/result.hpp"

namespace vortex {

// A host function takes (argc, argv) and returns a Value.
// argv[0] is the first argument, argv[1] the second, etc.
using HostFunction = std::function<Value(int argc, const Value* argv)>;

// Registry for host functions. Functions are identified by a numeric
// ID (used in bytecode's CALL_RUNTIME operand) and a name (for lookup).
//
// Thread safety: registration is NOT thread-safe. Register all host
// functions at startup, before running any bytecode. Once registered,
// lookup (via the C trampoline) is read-only and thread-safe.
class HostFunctionRegistry {
public:
    // Get the global registry instance.
    static HostFunctionRegistry& instance() {
        static HostFunctionRegistry r;
        return r;
    }

    // Register a host function by name.
    // Returns the function ID (used in CALL_RUNTIME bytecode operand).
    // If a function with this name is already registered, returns the
    // existing ID (does NOT replace — use unregister first if needed).
    uint32_t register_function(const std::string& name, HostFunction fn) {
        auto it = name_to_id_.find(name);
        if (it != name_to_id_.end()) {
            return it->second;
        }
        uint32_t id = static_cast<uint32_t>(functions_.size());
        functions_.push_back(std::move(fn));
        name_to_id_[name] = id;
        id_to_name_[id] = name;
        return id;
    }

    // Unregister a function by name. Subsequent calls to this ID from
    // bytecode will return undefined.
    void unregister(const std::string& name) {
        auto it = name_to_id_.find(name);
        if (it == name_to_id_.end()) return;
        uint32_t id = it->second;
        functions_[id] = nullptr;  // keep slot (IDs are stable)
        name_to_id_.erase(it);
        id_to_name_.erase(id);
    }

    // Look up a function ID by name. Returns UINT32_MAX if not found.
    uint32_t lookup(const std::string& name) const {
        auto it = name_to_id_.find(name);
        return (it != name_to_id_.end()) ? it->second : UINT32_MAX;
    }

    // Get the name for a function ID (for debugging).
    const std::string& name(uint32_t id) const {
        static const std::string empty;
        auto it = id_to_name_.find(id);
        return (it != id_to_name_.end()) ? it->second : empty;
    }

    // Call a function by ID. This is the trampoline called from
    // the C CALL_RUNTIME handler.
    Value call(uint32_t id, int argc, const Value* argv) const {
        if (id >= functions_.size()) return Value::undefined();
        const auto& fn = functions_[id];
        if (!fn) return Value::undefined();
        return fn(argc, argv);
    }

    // Number of registered functions.
    size_t count() const { return functions_.size(); }

private:
    HostFunctionRegistry() = default;

    std::vector<HostFunction> functions_;
    std::unordered_map<std::string, uint32_t> name_to_id_;
    std::unordered_map<uint32_t, std::string> id_to_name_;
};

// Helper: register a host function on the global registry.
inline uint32_t register_host_function(const std::string& name, HostFunction fn) {
    return HostFunctionRegistry::instance().register_function(name, std::move(fn));
}

// C-callable trampoline for the VORTEX runtime's CALL_RUNTIME handler.
// This bridges the C runtime's vtx_runtime_builtin_call to our C++ registry.
// The runtime calls vtx_runtime_builtin_call(func_id, arg), and this
// trampoline dispatches to the registered C++ function.
//
// NOTE: The current CALL_RUNTIME only passes a single argument. For
// multi-arg host functions, we'd need to extend the C runtime to pass
// a value array. For now, single-arg host functions are supported.
extern "C" vtx_value_t vtx_cpp_host_call(uint32_t func_id, vtx_value_t arg);

} // namespace vortex

#endif // VORTEX_HOST_FUNCTION_HPP
