# VORTEX

A multi-tier speculative JIT compiler for a custom bytecode VM, targeting x86-64, written in C17.

## Architecture

| Tier | Role | Trigger |
|---|---|---|
| T0 | Computed-goto interpreter + ICs + profiling | startup |
| T1 | Baseline one-pass codegen | heat > 1000 |
| T1.5 | Type specialization, block layout | mid-tier |
| T2 | Sea-of-Nodes: PEA, GVN, SCCP, DCE, LICM, inlining | heat > 10000 |
| T3 | Speculative SIMD + deoptless continuations | phase prediction |

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
- **Representation inference** — explicit UnboxInt/BoxInt nodes for SMI tag elision
- **Profile-guided block layout** using branch probability data
- **Loop unrolling** (factor=2) with proper control-flow threading
- **Deoptimization** — OSR up/down, deoptless continuations, side tables
- **Concurrent compilation** — pthread threadpool + orchestrator + safepoints
- **Hand-written x86-64 emitter** (~5K LOC, REX/ModR-M/SIB per Intel SDM)



## Provenance

This codebase was originally written by a human developer and substantially
modified by an AI assistant (GLM/Z.ai) for bug fixes, performance improvements,
and feature additions. Modified files carry an "AI-MODIFIED CODE" banner.
See commit history for detailed change descriptions.
