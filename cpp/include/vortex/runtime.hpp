// vortex/runtime.hpp — RAII wrapper around the VORTEX runtime.
//
// A Runtime bundles:
//   - Type system
//   - Garbage collector (generational)
//   - Interpreter (T0)
//   - JIT compilers (T1 baseline, T2 optimizing, T3 speculative)
//   - Code cache
//   - Compilation threadpool
//
// Threading: A Runtime is NOT thread-safe. Use one Runtime per thread,
// or wrap all access with an external mutex.
//
// Usage:
//   auto rt_result = vortex::Runtime::create();
//   if (!rt_result) { ... }
//   vortex::Runtime rt = std::move(rt_result.value());
//   rt.enable_jit(2);  // 2 compile threads
//   vortex::Bytecode bc = ...;
//   vortex::Value result = rt.run(bc);

#ifndef VORTEX_RUNTIME_HPP
#define VORTEX_RUNTIME_HPP

#include <cstdint>
#include <cstdlib>
#include <vector>
#include <string>
#include <functional>
#include <unordered_map>
#include <memory>

#define typeid typeid_
extern "C" {
#include "runtime/vortex_runtime.h"
}
#undef typeid
#include "vortex/value.hpp"
#include "vortex/bytecode.hpp"
#include "vortex/result.hpp"

namespace vortex {

class HostFunctionRegistry;

class Runtime {
public:
    /* ---- Lifecycle ---- */

    /* CPP-001 fix: vtx_runtime_t contains internal self-pointers
     * (gc.type_system = &type_system, interp->gc = &gc, etc.).
     * Moving the struct by value (as the old code did) shifts it to
     * a new address, invalidating every internal pointer → UAF.
     * Fix: heap-allocate vtx_runtime_t via unique_ptr so the struct
     * itself never moves; only the unique_ptr is moved. */

    static Result<Runtime> create() {
        /* Heap-allocate the raw struct so its address is stable. */
        Runtime rt;
        rt.raw_ptr_ = static_cast<vtx_runtime_t*>(std::malloc(sizeof(vtx_runtime_t)));
        if (!rt.raw_ptr_) {
            return Result<Runtime>::err("out of memory");
        }
        if (vtx_runtime_create(rt.raw_ptr_) != 0) {
            std::free(rt.raw_ptr_);
            rt.raw_ptr_ = nullptr;
            return Result<Runtime>::err("failed to create runtime");
        }
        rt.owns_ = true;
        return rt;
    }

    ~Runtime() {
        if (owns_ && raw_ptr_) {
            vtx_runtime_destroy(raw_ptr_);
            std::free(raw_ptr_);
            raw_ptr_ = nullptr;
        }
    }

    // Move-only — moving the unique_ptr-like wrapper doesn't relocate raw_.
    Runtime(Runtime&& other) noexcept
        : raw_ptr_(other.raw_ptr_), owns_(other.owns_) {
        other.raw_ptr_ = nullptr;
        other.owns_ = false;
    }
    Runtime& operator=(Runtime&& other) noexcept {
        if (this != &other) {
            if (owns_ && raw_ptr_) {
                vtx_runtime_destroy(raw_ptr_);
                std::free(raw_ptr_);
            }
            raw_ptr_ = other.raw_ptr_;
            owns_ = other.owns_;
            other.raw_ptr_ = nullptr;
            other.owns_ = false;
        }
        return *this;
    }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    /* ---- JIT control ---- */

    // Enable the JIT: starts background compilation threads.
    // nthreads = 0 for auto-detect (defaults to 2).
    void enable_jit(uint32_t nthreads = 0) {
        vtx_runtime_enable_jit(raw_ptr_, nthreads);
    }

    // Eagerly compile a method at T1 (baseline JIT).
    Result<void> compile_t1(Bytecode& bc) {
        vtx_method_desc_t m = make_method_desc(bc);
        int rc = vtx_runtime_compile(raw_ptr_, &m, 1);
        if (rc != 0) return Result<void>::err("T1 compilation failed");
        return {};
    }

    // Eagerly compile a method at T2 (optimizing JIT).
    // T2 handles floats and most opcodes; falls back to T1 on failure.
    Result<void> compile_t2(Bytecode& bc) {
        vtx_method_desc_t m = make_method_desc(bc);
        int rc = vtx_runtime_compile(raw_ptr_, &m, 2);
        if (rc != 0) return Result<void>::err("T2 compilation failed");
        return {};
    }

    /* ---- Execution ---- */

    // Run a bytecode module. Returns the result value.
    // If the method has been compiled (via compile_t1/t2 or tier-up),
    // the interpreter dispatches to JIT-compiled native code.
    Value run(const Bytecode& bc) {
        return Value(vtx_runtime_run(raw_ptr_, bc.raw()));
    }

    // Run with arguments. The bytecode's entry method should accept
    // `args.size()` arguments.
    Value run_with_args(const Bytecode& bc, const std::vector<Value>& args) {
        std::vector<vtx_value_t> raw_args;
        raw_args.reserve(args.size());
        for (auto& a : args) raw_args.push_back(a.raw());
        return Value(vtx_runtime_run_with_args(
            raw_ptr_, bc.raw(), raw_args.data(), raw_args.size()));
    }

    // Compile + run in one call.
    Result<Value> compile_and_run(Bytecode& bc, uint32_t tier = 2) {
        auto r = (tier <= 1) ? compile_t1(bc) : compile_t2(bc);
        if (!r) return Result<Value>::err(r.error());
        return run(bc);
    }

    /* ---- Accessors (for advanced use) ---- */

    vtx_runtime_t& raw() { return *raw_ptr_; }
    const vtx_runtime_t& raw() const { return *raw_ptr_; }

    vtx_type_system_t* type_system() { return vtx_runtime_type_system(raw_ptr_); }
    vtx_gc_t*          gc()          { return vtx_runtime_gc(raw_ptr_); }
    vtx_interp_t*      interp()      { return vtx_runtime_interp(raw_ptr_); }
    vtx_code_cache_t*  code_cache()  { return vtx_runtime_code_cache(raw_ptr_); }

    /* ---- GC control ---- */

    // Force a garbage collection cycle (young generation).
    void gc_collect() {
        vtx_gc_collect_young(&raw_ptr_->gc);
    }

    // Force a full GC (young + old generation).
    void gc_collect_full() {
        vtx_gc_collect_young(&raw_ptr_->gc);
        vtx_gc_collect_old(&raw_ptr_->gc);
    }

    // Get heap statistics.
    struct HeapStats {
        size_t young_used;
        size_t young_size;
        size_t old_used;
        size_t old_size;
        size_t collections_done;
        size_t total_allocations;
        size_t total_collections;
    };
    HeapStats heap_stats() const {
        HeapStats s{};
        s.young_used = raw_ptr_->gc.young_from.current - raw_ptr_->gc.young_from.start;
        s.young_size = raw_ptr_->gc.young_from.size;
        s.old_used  = raw_ptr_->gc.old_gen.used;
        s.old_size  = raw_ptr_->gc.old_gen.size;
        s.collections_done = raw_ptr_->gc.collections_done;
        s.total_allocations = 0;  // GC doesn't track this directly
        s.total_collections = raw_ptr_->gc.collections_done;
        return s;
    }

private:
    Runtime() = default;

    static vtx_method_desc_t make_method_desc(const Bytecode& bc) {
        vtx_method_desc_t m{};
        m.bytecode = const_cast<vtx_bytecode_t*>(bc.raw());
        m.compiled_code = nullptr;
        m.arg_count = 0;
        m.is_virtual = false;
        m.name = "main";
        m.signature = "()I";
        m.vtable_index = 0; // runtime_compile derives from bytecode ptr
        return m;
    }

    vtx_runtime_t* raw_ptr_ = nullptr;
    bool owns_ = false;
};

} // namespace vortex

#endif // VORTEX_RUNTIME_HPP
