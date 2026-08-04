# VORTEX

A multi-tier speculative JIT compiler for a custom bytecode VM, targeting x86-64 (ARM64 + RISC-V target descriptions ready), written in C17 with C++ embedding API.

## Architecture

| Tier | Role | Trigger |
|---|---|---|
| T0 | Computed-goto interpreter + ICs + profiling | startup |
| T1 | Baseline one-pass codegen | heat > 1000 |
| T1.5 | Type specialization, block layout | mid-tier |
| T2 | Sea-of-Nodes: PEA, GVN, SCCP, DCE, LICM, inlining | heat > 10000 |
| T3 | Speculative SIMD + deoptless continuations | phase prediction |
| **AOT** | **Background compilation with aggressive opts + bailout stubs** | **any tier** |

## Building

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

This produces:
- `src/vortex` main executable (runs .vtbc bytecode files or self-test)
- `benchmarks/bench_t2` T2 JIT benchmark suite
- Various test binaries in `tests/`

### C++ Embedding Library

```bash
# Build the C++ embedding API (libvortex_cpp.a)
cd cpp && mkdir build && cd build
cmake ..
make
./vortex_cpp_example    # demonstrates Runtime, Object, Array, HostFunction
./vortex_cpp_tests      # 12 tests
```

### Rust Bindings

```bash
cd rust-bindings
cargo build
cargo test
```

## Running

```bash
# Self-test
./src/vortex

# Run a bytecode file
./src/vortex program.vtbc

# Run benchmarks
./benchmarks/bench_t2
```

## Testing

```bash
cd build
ctest --output-on-failure
```

## Key Subsystems

- **Sea-of-Nodes IR** with ~60 opcodes and 14 optimization passes
- **Partial Escape Analysis** with cross-object scalar replacement
- **Representation inference** explicit UnboxInt/BoxInt nodes for SMI tag elision
- **Profile-guided block layout** using branch probability data
- **Loop unrolling** (factor=2) with proper control-flow threading
- **Deoptimization** OSR up/down, deoptless continuations, side tables
- **Concurrent compilation** pthread threadpool + orchestrator + safepoints
- **Hand-written x86-64 emitter** (5K LOC, REX/ModR-M/SIB per Intel SDM)

## AOT Background Compilation

VORTEX includes a background AOT compilation system (`compile/aot.h`) that compiles traces with aggressive optimizations on a worker thread:

- **Heap-allocated artifacts** — AOT artifacts survive arena scope, enabling background compilation
- **Aggressive optimization** — inline_size_limit=8192 (vs 4096 JIT), GVN 5 iters (vs 3), SCCP 10 iters (vs 5), speculative guards, loop specialization, SIMD vectorization
- **Bailout stubs** — pre-generated code that reconstructs interpreter state on guard failure
- **Guard failure → retrace** — failed guards feed into the trace re-tracing system, which generates a new specialized variant
- **Code installation** — compiled code installed in the code cache via `vtx_install_method`

```c
// Create an AOT artifact (heap-allocated, self-contained)
vtx_aot_artifact_t *art = vtx_aot_create_artifact(
    method_id, trace_id, 3 /* T3 */, bytecode, method);

// Submit for background compilation
vtx_aot_submit(&aot_manager, art);

// Worker thread compiles + installs automatically
// Guard failures feed into retrace system
```

## Trace-Based PGO with Re-Tracing

When a guard fails repeatedly, the trace's speculation is wrong. VORTEX doesn't just deopt — it re-records the trace with updated profile data (`trace/retrace.h`):

- **Per-method failure tracking** — hash table records guard failures per method
- **Threshold-based re-tracing** — 10+ failures triggers a re-trace
- **Profile feedback** — guard failures update branch probabilities so the next trace picks a different hot path
- **Cooldown** — 10 orchestrator checks between re-traces prevents storms
- **Max attempts** — 5 re-traces per method before giving up

## Retargetable RegAlloc

The register allocator uses a polymorphic `TargetDescription` interface (`lower/target.h`) instead of hardcoded x86-64 constants:

- **`vtx_target_x86_64()`** — 16 GPRs + 16 XMMs, System V ABI
- **`vtx_target_arm64()`** — 32 GPRs, AAPCS64 ABI (register layout ready, codegen TBD)
- **`vtx_target_riscv64()`** — 32 GPRs, standard ABI (register layout ready, codegen TBD)

```c
// Cross-compile for ARM64 (on an x86 host):
vtx_regalloc_run_target(stream, arena, vtx_target_arm64());
```

Adding a new architecture = writing a `TargetDescription` subclass (data-definition), not rewriting the regalloc algorithm.

## C++ Embedding API

```cpp
#include "vortex/runtime.hpp"
#include "vortex/object.hpp"

// Create runtime + enable JIT
auto rt = vortex::Runtime::create().value();
rt.enable_jit(2);

// Dynamic objects with prototype chain
auto animal = vortex::Object::create(rt);
animal.set(rt, "legs", vortex::Value::smi(4));

auto dog = vortex::Object::create(rt);
dog.set_prototype(rt, animal);
dog.get(rt, "legs");  // → 4 (inherited from prototype)

// Register host functions callable from bytecode
vortex::register_host_function("print", [](int argc, const vortex::Value* argv) {
    std::cout << argv[0].to_string() << "\n";
    return vortex::Value::undefined();
});
```

A C API (`vortex/embed.h`) is also available for C-only consumers.

## Provenance

This codebase was originally written by a human developer and
modified by an AI assistant (GLM/Z.ai) for bug fixes, performance improvements,
and feature additions. Modified files carry an "AI-MODIFIED CODE" banner.
See commit history for detailed change descriptions.
