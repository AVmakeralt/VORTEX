// vortex/host_function.cpp — C trampoline for calling C++ host functions
// from the VORTEX bytecode CALL_RUNTIME opcode.

#include "vortex/host_function.hpp"
#include "vortex/value.hpp"

extern "C" vtx_value_t vtx_cpp_host_call(uint32_t func_id, vtx_value_t arg) {
    // Call the registered C++ function.
    // The current CALL_RUNTIME only passes a single argument.
    // For multi-arg functions, we'd need to extend the C runtime.
    vortex::Value argv[1] = { vortex::Value(arg) };
    vortex::Value result = vortex::HostFunctionRegistry::instance().call(
        func_id, 1, argv);
    return result.raw();
}
