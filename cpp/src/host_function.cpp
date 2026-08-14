// vortex/host_function.cpp — C trampoline for calling C++ host functions
// from the VORTEX bytecode CALL_RUNTIME opcode.
//
// CPP-008 fix: The old vtx_cpp_host_call(func_id, arg) signature was
// called via the legacy single-arg path, so multi-arg host functions
// never received more than one argument. The runtime exposes
// vtx_set_runtime_callback(callback, user_data) which gives the callback
// a vtx_value_t** sp_ptr — direct access to the operand stack — and lets
// the callback push multiple return values.
//
// We now register vtx_cpp_host_trampoline as the runtime callback at
// embed init time. The trampoline reads N arguments off the stack
// (where N is configurable via CALL_RUNTIME's operand extension — for
// now, we read until we hit argc 1 which is what the legacy opcode
// supports, but the API is forward-compatible).

#include "vortex/host_function.hpp"
#include "vortex/value.hpp"

// CPP-003 workaround: type_system.h (pulled in by dispatch.h) uses
// 'typeid' as a parameter name, which is a reserved C++ keyword.
// Use the same #define shim as runtime.hpp and bytecode.hpp.
#define typeid typeid_
extern "C" {
#include "interp/dispatch.h"
}
#undef typeid

#include <vector>

// New-style trampoline: receives the stack pointer so we can read
// multiple arguments. Returns the number of values pushed (0 = void,
// 1 = single return value). The C runtime pushes/pops the stack
// according to this return count.
//
// Convention: the top-of-stack value is argv[0] (the LAST pushed arg).
// For a function expecting (a, b, c), the bytecode pushes c, then b,
// then a — so sp[-1] = c, sp[-2] = b, sp[-3] = a.
//
// We currently pop exactly 1 argument (matching the legacy opcode's
// single-operand semantics), but the API can be extended to read more.
// Callers that need multi-arg can use the runtime callback mechanism
// directly via vtx_set_runtime_callback().
extern "C" int32_t vtx_cpp_host_trampoline(uint32_t func_id,
                                             vtx_value_t **sp_ptr,
                                             void *user_data) {
    (void)user_data;
    if (sp_ptr == nullptr || *sp_ptr == nullptr) return 0;

    // C18 FIX: Extract argc from the operand. The operand is packed as
    // (fn_id << 6) | argc by the luajit-2 frontend. We unpack argc here.
    uint32_t fn_id = func_id >> 6;
    uint32_t argc = func_id & 0x3F;
    if (argc == 0) argc = 1; /* default to 1 for legacy callers */

    vtx_value_t *sp = *sp_ptr;
    if (sp == nullptr) return 0;

    // Pop argc arguments from the stack.
    vortex::Value argv[64];
    for (uint32_t i = 0; i < argc && i < 64; i++) {
        argv[i] = vortex::Value(*--sp);
    }

    vortex::Value result = vortex::HostFunctionRegistry::instance().call(
        fn_id, static_cast<int>(argc), argv);

    // Push result back onto the stack (single return value).
    *sp++ = result.raw();
    *sp_ptr = sp;
    return 1;
}

// Legacy single-arg entry point — kept for backward compatibility with
// any embed code that calls it directly. New code should go through the
// trampoline via vtx_set_runtime_callback().
extern "C" vtx_value_t vtx_cpp_host_call(uint32_t func_id, vtx_value_t arg) {
    vortex::Value argv[1] = { vortex::Value(arg) };
    vortex::Value result = vortex::HostFunctionRegistry::instance().call(
        func_id, 1, argv);
    return result.raw();
}

// Register the trampoline with the C runtime. Call this once at embed
// startup so that CALL_RUNTIME opcodes dispatch to the C++ host function
// registry. Safe to call multiple times — the C runtime stores only one
// callback, so the last registration wins.
extern "C" void vtx_cpp_host_init(void) {
    vtx_set_runtime_callback(vtx_cpp_host_trampoline, nullptr);
}
