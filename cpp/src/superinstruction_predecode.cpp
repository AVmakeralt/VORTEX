// superinstruction_predecode.cpp — C-callable entry point for the
// superinstruction pre-decode pass.
//
// Per VORTEX Engineering Standards §1.1, new code is in C++. This file
// exposes a C-callable API so the C interpreter and benchmark harness
// can invoke the pass without changing their build.
//
// The pass takes a vtx_bytecode_t and returns a NEW vtx_bytecode_t
// with fused superinstructions. The caller owns the new struct and
// must free it with vtx_superinstruction_free().

#define typeid typeid_
extern "C" {
#include "runtime/bytecode.h"
#include "runtime/object.h"
}
#undef typeid

#include "vortex/superinstruction.hpp"

#include <cstdlib>
#include <cstring>

extern "C" {

// Mark these symbols as `used` to prevent -flto + --gc-sections from
// dropping them. The bench_v8_comparison binary calls them via an
// `extern` declaration; without `used`, LTO may inline the call away
// and then the linker thinks the symbol is dead code.
//
// `visibility("default")` ensures the symbol is exported even if the
// build uses hidden visibility by default (it doesn't here, but it's
// a defensive measure).

// Pre-decode a bytecode module: fuse qualifying opcode pairs into
// superinstructions. Returns a new vtx_bytecode_t or NULL on failure.
//
// The returned struct:
//   - owns the new code buffer (malloc)
//   - SHARES the constant pool with the input (no copy)
//   - shares max_locals / max_stack / constant_count
//
// The caller must free it with vtx_superinstruction_free().
__attribute__((visibility("default"), used))
vtx_bytecode_t* vtx_superinstruction_predecode(const vtx_bytecode_t* bc) {
    if (!bc || !bc->code || bc->length == 0) return nullptr;

    vortex::PreDecodeResult result;
    if (vortex::predecode(bc, &result) != 0) return nullptr;

    vtx_bytecode_t* out = static_cast<vtx_bytecode_t*>(
        std::calloc(1, sizeof(vtx_bytecode_t)));
    if (!out) {
        std::free(result.code);
        return nullptr;
    }
    out->code = result.code;
    out->length = result.length;
    out->constant_pool = const_cast<vtx_value_t*>(bc->constant_pool);  // shared
    out->constant_count = bc->constant_count;
    out->max_locals = bc->max_locals;
    out->max_stack = bc->max_stack;
    return out;
}

// Free a bytecode struct produced by vtx_superinstruction_predecode.
// Frees the code buffer but NOT the constant pool (which is shared
// with the original).
__attribute__((visibility("default"), used))
void vtx_superinstruction_free(vtx_bytecode_t* bc) {
    if (!bc) return;
    if (bc->code) std::free(const_cast<uint8_t*>(bc->code));
    std::free(bc);
}

// Query: how many pairs were fused? Returns 0 if not previously recorded.
// (This is informational — the bench harness uses it to verify the pass
// actually fired.)
__attribute__((visibility("default"), used))
uint32_t vtx_superinstruction_count(const vtx_bytecode_t* original,
                                      const vtx_bytecode_t* fused) {
    if (!original || !fused) return 0;
    // Estimate: each fusion removes (orig_len - 5) bytes from the code.
    // We can't know the exact count without re-running the pass, so
    // just return 0 here. The bench can print original vs fused length
    // to verify the pass is firing.
    (void)original; (void)fused;
    return 0;
}

}  // extern "C"
