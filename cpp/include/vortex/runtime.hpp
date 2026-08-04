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

    static Result<Runtime> create() {
        Runtime rt;
        if (vtx_runtime_create(&rt.raw_) != 0) {
            return Result<Runtime>::err("failed to create runtime");
        }
        return rt;
    }

    ~Runtime() {
        if (owns_) vtx_runtime_destroy(&raw_);
    }

    // Move-only
    Runtime(Runtime&& other) noexcept
        : raw_(other.raw_), owns_(other.owns_) {
        other.owns_ = false;
    }
    Runtime& operator=(Runtime&& other) noexcept {
        if (this != &other) {
            if (owns_) vtx_runtime_destroy(&raw_);
            raw_ = other.raw_;
            owns_ = other.owns_;
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
        vtx_runtime_enable_jit(&raw_, nthreads);
    }

    // Eagerly compile a method at T1 (baseline JIT).
    Result<void> compile_t1(Bytecode& bc) {
        vtx_method_desc_t m = make_method_desc(bc);
        int rc = vtx_runtime_compile(&raw_, &m, 1);
        if (rc != 0) return Result<void>::err("T1 compilation failed");
        return {};
    }

    // Eagerly compile a method at T2 (optimizing JIT).
    // T2 handles floats and most opcodes; falls back to T1 on failure.
    Result<void> compile_t2(Bytecode& bc) {
        vtx_method_desc_t m = make_method_desc(bc);
        int rc = vtx_runtime_compile(&raw_, &m, 2);
        if (rc != 0) return Result<void>::err("T2 compilation failed");
        return {};
    }

    /* ---- Execution ---- */

    // Run a bytecode module. Returns the result value.
    // If the method has been compiled (via compile_t1/t2 or tier-up),
    // the interpreter dispatches to JIT-compiled native code.
    Value run(const Bytecode& bc) {
        return Value(vtx_runtime_run(&raw_, bc.raw()));
    }

    // Run with arguments. The bytecode's entry method should accept
    // `args.size()` arguments.
    Value run_with_args(const Bytecode& bc, const std::vector<Value>& args) {
        std::vector<vtx_value_t> raw_args;
        raw_args.reserve(args.size());
        for (auto& a : args) raw_args.push_back(a.raw());
        return Value(vtx_runtime_run_with_args(
            &raw_, bc.raw(), raw_args.data(), raw_args.size()));
    }

    // Compile + run in one call.
    Result<Value> compile_and_run(Bytecode& bc, uint32_t tier = 2) {
        auto r = (tier <= 1) ? compile_t1(bc) : compile_t2(bc);
        if (!r) return Result<Value>::err(r.error());
        return run(bc);
    }

    /* ---- Accessors (for advanced use) ---- */

    vtx_runtime_t& raw() { return raw_; }
    const vtx_runtime_t& raw() const { return raw_; }

    vtx_type_system_t* type_system() { return vtx_runtime_type_system(&raw_); }
    vtx_gc_t*          gc()          { return vtx_runtime_gc(&raw_); }
    vtx_interp_t*      interp()      { return vtx_runtime_interp(&raw_); }
    vtx_code_cache_t*  code_cache()  { return vtx_runtime_code_cache(&raw_); }

    /* ---- GC control ---- */

    // Force a garbage collection cycle (young generation).
    void gc_collect() {
        vtx_gc_collect_young(&raw_.gc);
    }

    // Force a full GC (young + old generation).
    void gc_collect_full() {
        vtx_gc_collect_young(&raw_.gc);
        vtx_gc_collect_old(&raw_.gc);
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
        s.young_used = raw_.gc.young_from.current - raw_.gc.young_from.start;
        s.young_size = raw_.gc.young_from.size;
        s.old_used  = raw_.gc.old_gen.used;
        s.old_size  = raw_.gc.old_gen.size;
        s.collections_done = raw_.gc.collections_done;
        s.total_allocations = 0;  // GC doesn't track this directly
        s.total_collections = raw_.gc.collections_done;
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

    vtx_runtime_t raw_{};
    bool owns_ = false;
};

} // namespace vortex

#endif // VORTEX_RUNTIME_HPP
