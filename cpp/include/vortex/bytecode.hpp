// vortex/bytecode.hpp — RAII wrapper for .vtbc bytecode files.
//
// A Bytecode owns the underlying vtx_bytecode_t (code + constant pool)
// and frees it on destruction. Bytecode objects are move-only — copying
// would double-free the malloc'd code/constant_pool.

#ifndef VORTEX_BYTECODE_HPP
#define VORTEX_BYTECODE_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#define typeid typeid_
extern "C" {
#include "runtime/bytecode.h"
}
#undef typeid
#include "vortex/result.hpp"

namespace vortex {

class Bytecode {
public:
    /* ---- Construction / loading ---- */

    // Load a .vtbc file from disk.
    static Result<Bytecode> load(const std::string& path) {
        vtx_bytecode_t* bc = vtx_bytecode_load(path.c_str());
        if (!bc) {
            return Result<Bytecode>::err("failed to load bytecode: " + path);
        }
        return Bytecode(bc);
    }

    // Wrap an existing raw vtx_bytecode_t* without taking ownership.
    // Useful for interop with C code that owns the bytecode.
    static Bytecode borrow(const vtx_bytecode_t* bc) {
        // const_cast is safe: we mark ourselves as non-owning.
        return Bytecode(const_cast<vtx_bytecode_t*>(bc), /*own=*/false);
    }

    // Build bytecode from in-memory bytes (does NOT copy — borrows).
    // The caller must keep `code` and `consts` alive for the Bytecode's
    // lifetime. For owned bytecode, use load() or build_owned().
    static Bytecode borrow(const uint8_t* code, size_t length,
                           const vtx_value_t* consts = nullptr,
                           uint32_t const_count = 0,
                           uint16_t max_locals = 0,
                           uint16_t max_stack = 0) {
        // Allocate a vtx_bytecode_t struct on the heap that points to
        // the borrowed buffers. We own the struct but not the buffers.
        vtx_bytecode_t* bc = static_cast<vtx_bytecode_t*>(
            std::calloc(1, sizeof(vtx_bytecode_t)));
        bc->code = const_cast<uint8_t*>(code);
        bc->length = length;
        bc->constant_pool = const_cast<vtx_value_t*>(consts);
        bc->constant_count = const_count;
        bc->max_locals = max_locals;
        bc->max_stack = max_stack;
        // We own `bc` (the struct) but NOT `code` or `constant_pool`.
        // Mark as owning the struct but with a flag to skip freeing buffers.
        return Bytecode(bc, /*own_struct=*/true, /*own_buffers=*/false);
    }

    /* ---- RAII ---- */

    ~Bytecode() { release(); }

    // Move-only
    Bytecode(Bytecode&& other) noexcept
        : bc_(other.bc_), own_struct_(other.own_struct_),
          own_buffers_(other.own_buffers_) {
        other.bc_ = nullptr;
    }
    Bytecode& operator=(Bytecode&& other) noexcept {
        if (this != &other) {
            release();
            bc_ = other.bc_;
            own_struct_ = other.own_struct_;
            own_buffers_ = other.own_buffers_;
            other.bc_ = nullptr;
        }
        return *this;
    }
    Bytecode(const Bytecode&) = delete;
    Bytecode& operator=(const Bytecode&) = delete;

    /* ---- Accessors ---- */

    const vtx_bytecode_t* raw() const { return bc_; }
    vtx_bytecode_t* raw_mut() const { return bc_; }

    size_t      code_length()    const { return bc_ ? bc_->length : 0; }
    const uint8_t* code()        const { return bc_ ? bc_->code : nullptr; }
    uint32_t    constant_count()  const { return bc_ ? bc_->constant_count : 0; }
    const vtx_value_t* constants() const { return bc_ ? bc_->constant_pool : nullptr; }
    uint16_t    max_locals()      const { return bc_ ? bc_->max_locals : 0; }
    uint16_t    max_stack()       const { return bc_ ? bc_->max_stack : 0; }

    // Get a constant by index. Returns undefined if out of bounds.
    vtx_value_t constant(uint32_t index) const {
        if (bc_ && index < bc_->constant_count) {
            return bc_->constant_pool[index];
        }
        return VTX_VALUE_UNDEFINED;
    }

    /* ---- Disassembly ---- */

    // Disassemble the bytecode to a human-readable string.
    std::string disassemble() const {
        std::string out;
        if (!bc_) return out;
        for (size_t pc = 0; pc < bc_->length; ) {
            char buf[256];
            size_t next = vtx_bytecode_disassemble_op(bc_, pc, buf, sizeof(buf));
            out += std::to_string(pc) + ": " + buf + "\n";
            if (next <= pc) break;
            pc = next;
        }
        return out;
    }

private:
    Bytecode(vtx_bytecode_t* bc, bool own_struct = true, bool own_buffers = true)
        : bc_(bc), own_struct_(own_struct), own_buffers_(own_buffers) {}

    void release() {
        if (!bc_) return;
        if (own_buffers_) {
            // vtx_bytecode_load uses malloc, so we free with free.
            // Note: vtx_bytecode_load's bc->code and constant_pool are
            // malloc'd by vtx_bytecode_load.
            if (bc_->code) std::free(const_cast<uint8_t*>(bc_->code));
            if (bc_->constant_pool) std::free(bc_->constant_pool);
        }
        if (own_struct_) std::free(bc_);
        bc_ = nullptr;
    }

    vtx_bytecode_t* bc_ = nullptr;
    bool own_struct_  : 1;
    bool own_buffers_ : 1;
};

} // namespace vortex

#endif // VORTEX_BYTECODE_HPP
