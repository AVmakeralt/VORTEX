# VORTEX JIT Compiler — Complete Roadmap

> **Language**: C17 / C++20  
> **Build System**: CMake 3.25+  
> **Target**: x86-64 (Linux primary, macOS secondary)  
> **License**: Apache 2.0  
> **Estimated Total**: ~150K LoC (mid-estimate for production quality)

---

## Zero-Tolerance Policy

**NO STUBS. NO PLACEHOLDERS. NO HARD-CODED VALUES.**

Every function must contain real, working logic. Every constant must be derived from a principled calculation or documented measurement. Every `TODO` must be accompanied by a working implementation that can be improved later — never an empty body. If a component is too complex to implement fully in one pass, implement the simplest correct version, not a stub.

**Agents are welcome and expected.** Each component is designed to be independently implementable by an autonomous agent given the interface contracts defined here. Agents must read this roadmap before starting work. Agents must write to the shared worklog at `/home/z/my-project/worklog.md`.

---

## Architecture Overview

VORTEX is a multi-tier speculative JIT compiler with trace-based region formation, a Sea-of-Nodes intermediate representation, partial escape analysis with cross-object scalar replacement, and ML-guided inlining. It lowers to native x86-64 machine code via a custom code generator inspired by Cranelift's design principles.

### Compilation Tiers

| Tier | Name | Trigger | What It Does |
|------|------|---------|--------------|
| T0 | Interpreter | Startup | Bytecode dispatch loop with profiling counters, type feedback, branch frequency, call site monitoring |
| T1 | Baseline JIT | Method heat > threshold | Quick one-pass codegen with guards and instrumentation, no optimization |
| T2 | Optimizing JIT | Trace heat > threshold or T1 deopt count exceeds limit | Full SoN pipeline: PEA, inlining, constant prop, DCE, guard merging, lowering |
| T3 | Speculative JIT | Phase prediction or feedback-directed recompilation | Speculative loop transforms (vectorization, unrolling), adaptive guard strength, cross-object SR |

### Pipeline

```
Bytecode
   │
   ▼
┌──────────┐    heat > T1_threshold    ┌──────────────┐
│ Interpreter│ ──────────────────────► │ Baseline JIT  │
│   (T0)    │                          │    (T1)       │
└──────────┘                          └──────┬───────┘
     │                                       │
     │ profile data                          │ deopt > limit
     │                                       │ or trace heat
     ▼                                       ▼
┌──────────┐    trace selection    ┌──────────────┐
│  Profile  │ ─────────────────► │ Trace Recorder│
│ Collector │                     │  + Region Form│
└──────────┘                     └──────┬───────┘
                                        │
                                        ▼
                                 ┌──────────────┐
                                 │  Sea-of-Nodes │
                                 │  IR Builder   │
                                 └──────┬───────┘
                                        │
                          ┌─────────────┼─────────────┐
                          ▼             ▼             ▼
                    ┌──────────┐ ┌──────────┐ ┌──────────┐
                    │   PEA +  │ │   ML     │ │  Guard   │
                    │   SR     │ │ Inliner  │ │ Optimizer│
                    └────┬─────┘ └────┬─────┘ └────┬─────┘
                         │            │            │
                         └────────────┼────────────┘
                                      ▼
                               ┌──────────────┐
                               │  Optimized   │
                               │  SoN Graph   │
                               └──────┬───────┘
                                      │
                                      ▼
                               ┌──────────────┐
                               │   Lowering   │
                               │  x86-64 CG   │
                               └──────┬───────┘
                                      │
                                      ▼
                               ┌──────────────┐
                               │  Code Cache  │
                               │  + Install   │
                               └──────────────┘
```

---

## Component Breakdown

### 1. Runtime & Type System (`src/runtime/`)

**Estimated LoC**: 10,000–15,000  
**Priority**: CRITICAL — everything depends on this  
**Agent-assignable**: Yes

The runtime provides the object model, type hierarchy, garbage collector interface, and runtime helper functions that all other components rely on.

#### What must be implemented

- **Object Model** (`object.h/c`): Tagged value representation (SMI for small integers, NaN-boxing for doubles, tagged pointer for heap objects). Each heap object has a header with: type ID, GC mark bit, size field, and shape ID. No hard-coded object layouts — everything is derived from the type descriptor.

- **Type System** (`type_system.h/c`): Types are represented as TypeIDs that index into a type descriptor table. Type descriptors contain: name, field layout (offsets computed from alignment rules), method table (vtable pointer), parent type, implemented interfaces, and a monomorphic call cache per call site. Type identity is structural for interfaces, nominal for classes. The type system supports: monomorphic, polymorphic (up to 4 types tracked per site), megamorphic (type bitmap + fallback). The transition from polymorphic to megamorphic is based on a measured threshold (default 4, configurable via environment variable `VORTEX_POLY_LIMIT`).

- **GC Interface** (`gc.h/c`): Semi-space copying collector for young generation, mark-sweep for old generation. The interface is: `gc_alloc(size_t, typeid_t)` returns a tagged pointer; `gc_root_push/pop` for conservative stack scanning; `gc_safepoint()` checks for collection request; `gc_write_barrier(ptr, field, value)` for generational remember sets. The GC must support pinning objects for when JIT code holds raw pointers (deopt materialization windows).

- **Runtime Helpers** (`helpers.h/c`): Type check (`instanceof`), dynamic cast, null check with trap, array bounds check with trap, arithmetic overflow check, string comparison, property lookup cache (inline cache per call site, 4 entries monomorphic→polymorphic→megamorphic transition).

- **Bytecode Definition** (`bytecode.h/c`): The bytecode format is a variable-length instruction stream. Each opcode is 1 byte, followed by operand bytes. The instruction set covers: load/store local, load/store field, load constant, arithmetic (iadd, isub, imul, idiv, fadd, fsub, fmul, fdiv), comparison, branch, call (static, virtual, interface), return, new, newarray, checkcast, instanceof, throw, monitor enter/exit. Each opcode has a defined stack effect for verification.

- **Arena Allocator** (`arena.h/c`): Region-based allocator used throughout the compiler pipeline. Each compilation gets its own arena; when compilation finishes, the entire arena is freed. The arena allocates from system pages (4KB minimum, grows in 64KB chunks). No individual free — only wholesale reset.

#### Key invariants

- Tagged values are always 64 bits
- Heap objects are always 8-byte aligned (low 3 bits available for tags)
- No object is ever partially initialized (allocation writes header before any store)
- GC safe points only occur at backward branches and method calls

---

### 2. Interpreter T0 (`src/interp/`)

**Estimated LoC**: 5,000–8,000  
**Priority**: CRITICAL — needed to run any code  
**Agent-assignable**: Yes

A high-performance bytecode interpreter with integrated profiling.

#### What must be implemented

- **Dispatch Loop** (`dispatch.h/c`): Computed goto dispatch (GCC/Clang labels-as-values). The dispatch table is built at initialization from the opcode definition table. Each opcode handler is a labeled block that: reads operands, executes the operation, updates profiling counters, and jumps to the next handler via `goto *dispatch_table[next_opcode]`.

- **Stack Frame** (`frame.h/c`): Each frame contains: local variable array (fixed size from method max_locals), operand stack (fixed size from method max_stack), return address (bytecode PC), caller frame pointer, method pointer, monitored type state for deopt. Frame allocation uses a frame stack that grows in 256KB chunks. No malloc in the dispatch loop.

- **Profiling** (`profiler.h/c`): Every backward branch increments a method-level execution counter. Every call site records the TypeID of the receiver. Every branch records taken/not-taken counts. Every field access records the shape ID observed. All profiling data is stored in per-method ProfileData structures. Heat is calculated as `execution_count * branch_density_weight`. The T1 compilation threshold is `VORTEX_T1_THRESHOLD` (default 1000, derived from empirical startup-vs-compile-time tradeoff studies). The T2 threshold is `VORTEX_T2_THRESHOLD` (default 10000).

- **Method Lookup** (`lookup.h/c`): Virtual dispatch uses an inline cache per call site. First call: monomorphic cache (type → method pointer). Cache miss: update to polymorphic (linear scan of up to 4 entries). Polymorphic overflow: megamorphic (type bitmap with fast path, vtable fallback). The inline cache is stored in the ProfileData, not in the bytecode.

- **Type Feedback Collection** (`type_feedback.h/c`): At each call site and field access, the observed type is recorded. For call sites: receiver TypeID + result TypeID. For field access: holder shape ID + value TypeID. For branches: taken count / total count. This data feeds the optimizer's speculative decisions. Type feedback is a circular buffer per site (last 32 observations, exponential decay weighting).

#### Key invariants

- The dispatch loop never allocates heap memory
- Profiling overhead is < 15% vs. unprofiled dispatch (measured, not assumed)
- All profiling counters are saturating (never overflow)
- Safe points are checked every backward branch (counter check, amortized cost)

---

### 3. Baseline JIT T1 (`src/baseline/`)

**Estimated LoC**: 8,000–12,000  
**Priority**: HIGH — needed to get past interpreter speed  
**Agent-assignable**: Yes

Quick one-pass code generation with guards and instrumentation. No optimization — just get machine code running fast.

#### What must be implemented

- **Code Generator** (`codegen.h/c`): Single-pass bytecode → x86-64 machine code translation. Each bytecode maps to a sequence of x86-64 instructions. Register allocation: R0-R3 for expression stack top, RDI/RSI/RDX/RCX for argument passing (System V AMD64 ABI), RBP for frame pointer, RSP for stack pointer. Expression stack values that don't fit in registers are spilled to the stack frame. No register allocation algorithm — just a fixed mapping.

- **Guard Emission** (`guards.h/c`): Every potentially speculative operation gets a guard. Type check guards: compare the object's TypeID against the expected TypeID; on mismatch, jump to a deopt stub that reconstructs interpreter state. Null check guards: test the pointer; on null, jump to a null-check deopt stub. Bounds check guards: compare index against array length; on out-of-bounds, jump to bounds-check deopt stub. Each guard records: bytecode PC, guard kind, expected value, deopt continuation.

- **Frame Layout** (`frame_layout.h/c`): JIT frames mirror interpreter frames but use machine registers for the operand stack top. Frame header: return address (native code pointer), caller frame pointer, method pointer, deopt info pointer (offset table mapping native PCs to bytecode PCs + stack states). Locals are at fixed offsets from RBP. Expression stack spills are at fixed offsets below locals.

- **Deopt Stub Generation** (`deopt_stubs.h/c`): For each guard, generate a deopt stub that: saves all live registers, reconstructs the interpreter operand stack from the JIT frame state, restores the interpreter frame pointer, and jumps to the interpreter dispatch loop at the correct bytecode PC. The mapping from native PC to interpreter state is stored in a side table (sorted array of (native_pc, bytecode_pc, stack_map) tuples, binary searched at deopt time).

- **Instrumentation** (`instrument.h/c`): T1 code includes the same profiling counters as the interpreter, so that T2 compilation can use profile data collected during T1 execution. The counters are stored in the same ProfileData structures used by the interpreter.

#### Key invariants

- T1 compilation time < 50μs per method (measured on 3GHz x86-64)
- T1 code runs at least 2x interpreter speed (conservative target)
- Every guard has a complete deopt stub — no "deopt not implemented" paths
- T1 code is always correct — it must produce the same results as the interpreter

---

### 4. Sea-of-Nodes IR (`src/ir/`)

**Estimated LoC**: 25,000–35,000  
**Priority**: CRITICAL — the core of the optimizer  
**Agent-assignable**: Yes (this is the largest single component)

The central intermediate representation. Everything in the optimizing pipeline is represented as a node in a graph.

#### What must be implemented

- **Node Taxonomy** (`node.h/c`): The base Node type contains: NodeID (32-bit), opcode (16-bit), input edges (variable-length array of NodeID), output edges (computed from input edges — the graph is doubly-linked), type (NodeType — see below), flags (control flow, side effects, etc.). Node subclasses are represented as opcodes, not as C++ inheritance. Each opcode has a handler table for: print, clone, hash, equals, simplify, idealize.

  Node categories and their opcodes:
  - **Control**: Start, End, If, Goto, Switch, LoopBegin, LoopEnd, Return, Unwind, Catch, Province
  - **Data**: Constant, Parameter, Add, Sub, Mul, Div, Mod, Shl, Shr, And, Or, Xor, Neg, Not, Cmp, CmpP, CmpF, CmpD
  - **Memory**: Load, Store, LoadField, StoreField, LoadIndexed, StoreIndexed, MemBar, Initialize
  - **Call**: CallStatic, CallVirtual, CallInterface, CallRuntime
  - **Type**: CheckCast, InstanceOf, Guard, Phi, Region, Proj
  - **Allocation**: NewObject, NewArray, Allocate, InitializeKlass
  - **Deopt**: Deopt, DeoptGuard, FrameState

- **Graph Construction** (`graph.h/c`): Build a SoN graph from bytecode. The graph starts with a Start node (parameters as projections). Each basic block becomes a Region node. Loops are represented as LoopBegin/LoopEnd pairs. Phis are inserted at Region nodes for variables that differ across predecessors. Memory state is threaded through the graph as a chain of memory nodes (load → store ordering). Control flow is a separate subgraph. Data and memory are connected only through the nodes that consume them — this is the "sea of nodes" property.

- **Global Value Numbering** (`gvn.h/c`): Hash-based GVN. Each node computes a hash from its opcode, input NodeIDs, and type. Two nodes are congruent if they have the same opcode, same input types, and same input values (recursively). GVN runs iteratively until no new congruences are found. GVN eliminates: redundant computations (CSE), redundant type checks, redundant null checks (if a dominating guard already proved non-null).

- **Constant Propagation & Folding** (`constant_prop.h/c`): Sparse conditional constant propagation (SCCP) on the SoN graph. Each node has a lattice value: Top (unreachable), Constant(value), Bottom (overdefined/variable). Propagation rules: arithmetic on constants → constant result; Phi with one input → that input; CheckCast with known type → no-op; Guard with always-true condition → remove. Fixed-point iteration until no lattice value changes.

- **Dead Code Elimination** (`dce.h/c`): Remove nodes with no output edges and no side effects. Side-effecting nodes (stores, calls, allocations) are kept even if their results are unused. DCE runs after every optimization pass. Iterative: removing dead nodes may expose more dead nodes.

- **Code Motion & Scheduling** (`schedule.h/c`): Convert the SoN graph back to a scheduled form for lowering. The scheduler: groups nodes into basic blocks, hoists loop-invariant nodes out of loops, sinks condition-dependent nodes into their target blocks, respects memory dependencies (load-after-store ordering), places guards as early as possible (to fail fast), places allocations as late as possible (to avoid unnecessary allocation on untaken paths).

- **IR Verification** (`verify.h/c`): After each optimization pass, verify the graph is well-formed. Checks: all inputs are valid NodeIDs, no cycles in the data subgraph, all Phis have the same number of inputs as their Region's predecessors, all memory nodes form a valid chain, all control nodes have valid successors, no dead nodes remain after DCE. Verification is enabled in debug builds; in release builds, it's compiled out.

#### Key invariants

- The graph is always in a valid state after any operation
- NodeIDs are never reused within a compilation
- GVN runs to fixed point before scheduling
- Memory dependencies are never violated by code motion
- Verification passes after every optimization pass in debug mode

---

### 5. Trace Recorder & Trees (`src/trace/`)

**Estimated LoC**: 8,000–12,000  
**Priority**: HIGH — enables region-based optimization  
**Agent-assignable**: Yes

Records hot execution traces and forms trace trees for cross-iteration optimization.

#### What must be implemented

- **Trace Selection** (`selector.h/c`): Identify hot loops from profile data (backward branch counter > T2 threshold). Select the loop header as the trace anchor. Record starting from the loop header, following the hot path (taken branch = hot path, determined by branch profiling data). Stop when: loop header is reached again (closed trace), method exit is reached, maximum trace length is hit (VORTEX_MAX_TRACE_LENGTH, default 512 bytecodes, derived from compile-time budget), or an unsupported operation is encountered (e.g., indirect jump).

- **Trace Recording** (`recorder.h/c`): Walk the bytecode stream following the hot path. For each bytecode, emit the corresponding SoN node but mark it with the trace ID. Side exits (untaken branches) are recorded as Guard nodes with the branch condition negated. The trace is a linear sequence of SoN nodes with guard points. Each guard point has a side exit descriptor: the target bytecode PC, the interpreter stack state at that point, and the reason for the exit.

- **Trace Trees** (`tree.h/c`): When a side exit from a compiled trace becomes hot (exit count > VORTEX_SIDE_EXIT_THRESHOLD, default 50), record a new trace starting from the side exit point back to the loop header. Link the new trace as a branch of the tree. Trace trees enable cross-iteration optimization: the root trace is the hot loop body, branches handle cold paths that become hot. Tree depth is limited to VORTEX_MAX_TREE_DEPTH (default 8) to avoid compile-time explosion.

- **Side Exit Handling** (`side_exit.h/c`): Each side exit in compiled code jumps to a side exit stub. The stub: reconstructs interpreter state from the exit descriptor, increments the exit counter for that exit, and returns to the interpreter. If the exit counter exceeds the threshold, the side exit stub also triggers trace recording for a new branch.

#### Key invariants

- Traces are always acyclic (loop back edges are represented as guard + re-entry)
- Side exits always have complete deopt information
- Trace tree depth is bounded
- Trace recording never blocks the interpreter (recording is done on a copy of the profile data)

---

### 6. Region Formation / Hyperblocks (`src/region/`)

**Estimated LoC**: 5,000–8,000  
**Priority**: MEDIUM  
**Agent-assignable**: Yes

Stitches multiple traces and basic blocks into hyperblocks for whole-region optimization.

#### What must be implemented

- **Stitching Algorithm** (`stitch.h/c`): Given a trace tree, stitch the root trace and hot branches into a single hyperblock. The stitching algorithm: (1) start with the root trace, (2) for each side exit whose exit count exceeds the stitching threshold (VORTEX_STITCH_THRESHOLD, default 200), inline the branch trace at the guard point, replacing the guard with the branch's code, (3) add a new guard at the end of the inlined branch to rejoin the main path, (4) repeat for nested branches up to depth limit. The result is a single entry, multiple exit hyperblock.

- **Size Budget** (`budget.h/c`): The hyperblock size is limited by VORTEX_MAX_HYPERBLOCK_NODES (default 4096 SoN nodes, derived from compile-time budget and code cache pressure). If stitching would exceed the budget, use a greedy algorithm: prioritize branches with the highest exit count * estimated speedup, include as many as fit, drop the rest. Backtracking: if including a branch causes the schedule to exceed the native code size budget (VORTEX_MAX_NATIVE_SIZE, default 32KB), remove it and try the next candidate.

- **Cross-Trace Optimization** (`cross_trace.h/c`): After stitching, run GVN and constant propagation across the entire hyperblock. This enables optimizations that are impossible within a single trace: commoning of computations that appear in both the main path and a branch, constant propagation from the main path into branches, dead code elimination of branch-only computations whose results are never used on the rejoin path.

#### Key invariants

- Hyperblocks never exceed the node or native code size budget
- Every exit from a hyperblock has deopt information
- Cross-trace optimization preserves the semantics of each individual trace

---

### 7. Partial Escape Analysis + Scalar Replacement (`src/pea/`)

**Estimated LoC**: 10,000–15,000  
**Priority**: HIGH — this is where the performance magic lives  
**Agent-assignable**: Yes (complex but well-defined algorithm)

Flow-sensitive escape analysis with cross-object scalar replacement.

#### What must be implemented

- **Escape Analysis** (`analysis.h/c`): Flow-sensitive analysis that tracks whether each allocation escapes the current compilation unit. An allocation escapes if: it is stored into a field of an escaping object, it is passed as an argument to an unknown function, it is returned from the current function, it is stored into a global variable, or it is used in a monitor. The analysis uses a dataflow framework: each basic block has an entry state and exit state. The state maps each allocation to an escape state: NoEscape, ArgEscape (escapes through arguments but not globally), GlobalEscape. State merge at Phi nodes takes the join (max) of predecessor states.

- **Cross-Object Scalar Replacement** (`cross_object_sr.h/c`): This is the SOTA feature. When object A escapes but object B (reachable only through A's fields) does not escape independently, B can be scalar-replaced. The analysis: (1) build an object graph from field stores (A.field = B creates an edge A→B), (2) for each object, compute the "effective escape" — does the object escape through any path that actually uses its value, or only through paths that use the container's reference?, (3) for objects with effective NoEscape, replace the allocation with scalar locals for each field, (4) for field accesses through the container, replace A.field with the scalar local. Example: `Node a = new Node(x); Node b = new Node(y); a.next = b; return a.value + b.value;` — a escapes through `a.next = b`, but b's value is only used through `b.value` which can be rewritten to `y`. So b is scalar-replaced even though a escapes.

- **Materialization** (`materialize.h/c`): At points where a scalar-replaced object must be reified (deopt point, call to unknown function, escape through a newly discovered path), generate materialization code: allocate the object on the heap, store all scalar-replaced field values into it. Materialization is lazy — only done at the points where the object actually escapes, not speculatively. The materialization code is inserted into the SoN graph as a sequence of NewObject + StoreField nodes.

- **Virtual Object Tracking** (`virtual.h/c`): During PEA, objects that are confirmed non-escaping are marked as "virtual." Virtual objects exist only in the compiler's analysis — no allocation is generated. All field accesses to virtual objects are rewritten to local variable accesses. Virtual objects are tracked through Phis (a Phi of two virtual objects is virtual if both inputs are virtual with the same type).

#### Key invariants

- PEA never incorrectly eliminates an allocation that escapes
- Materialization always produces an object that is indistinguishable from a heap-allocated one
- Cross-object SR only eliminates objects whose effective escape is NoEscape
- PEA runs before inlining (so inlined code can benefit from the analysis)

---

### 8. Deoptimization System (`src/deopt/`)

**Estimated LoC**: 12,000–18,000  
**Priority**: CRITICAL — bugs here are security vulnerabilities  
**Agent-assignable**: Yes (but requires extreme care)

The system that allows the JIT to speculatively optimize code and safely fall back to the interpreter when speculation fails.

#### What must be implemented

- **Frame States** (`frame_state.h/c`): At each deopt point (guard, call, allocation), the compiler records a FrameState that captures: the bytecode PC, the local variable values (as NodeIDs in the SoN graph), the operand stack values (as NodeIDs), the monitor state, and the exception handler state. FrameStates form a chain — each deopt point knows the FrameState of its caller. This chain is used to reconstruct the entire interpreter stack at deopt time.

- **OSR (On-Stack Replacement)** (`osr.h/c`): Two directions: OSR up (interpreter → compiled code during a loop) and OSR down (compiled code → interpreter during deopt). OSR up: when a loop is compiled while the interpreter is running it, the interpreter frame is converted to a compiled frame at the loop header. OSR down: when a guard fails, the compiled frame is converted to an interpreter frame at the guard's bytecode PC.

- **Deoptless Continuation** (`deoptless.h/c`): Instead of deoptimizing all the way back to the interpreter, generate a continuation version of the compiled code with the failed speculation removed. When a guard fails, instead of going to the interpreter, jump to the continuation version. The continuation version has: the same entry point structure, the failed guard removed (replaced with the slow path), all dependent optimizations invalidated. This avoids the cost of interpreter re-execution and re-compilation.

- **Side Tables** (`side_table.h/c`): The mapping from native PC to FrameState is stored in a side table, not embedded in the code. The side table is a sorted array of (native_pc_offset, frame_state_index) tuples. At deopt time, binary search for the native PC to find the FrameState. The side table also contains the register map: which registers contain which NodeIDs at each native PC.

- **Stack Walking** (`stack_walk.h/c`): Walk the compiled frame stack to collect all FrameStates. Starting from the current frame, follow the frame pointer chain. For each frame, look up the side table to find the FrameState. Reconstruct the interpreter stack from the FrameState chain. Stack walking must handle: mixed interpreter/compiled frames, frames with inlined methods (multiple logical frames in one physical frame), and frames with deoptless continuations.

#### Key invariants

- Deoptimization always produces the same result as if the interpreter had run from the deopt point
- Stack walking handles all frame types (interpreted, T1, T2, T3)
- Deoptless continuations are always a valid optimization of the full deopt path
- Frame state reconstruction never reads uninitialized memory

---

### 9. ML Inlining (`src/inliner/`)

**Estimated LoC**: 5,000–10,000  
**Priority**: MEDIUM  
**Agent-assignable**: Yes

Machine-learning-guided inlining decisions. Replaces years of heuristic tuning with a learned model.

#### What must be implemented

- **Feature Extraction** (`features.h/c`): For each call site in the SoN graph, extract features: callee method size (in bytecodes), callee method instruction count, call site frequency (from profiling), caller method size, call depth, callee is hot (yes/no), callee has loops (yes/no), callee has try/catch (yes/no), callee allocates (yes/no), callee calls virtual (yes/no), receiver type certainty (monomorphic=1.0, polymorphic=0.5, megamorphic=0.0), ratio of constant arguments, estimated register pressure impact. Total: 15 features per call site.

- **Inference Engine** (`inference.h/c`): The model is a gradient-boosted decision tree (GBDT) with 100 trees, max depth 5. This is deliberately simple — it needs to run in microseconds during compilation. The model is serialized as a flat array of tree nodes (feature_index, threshold, left_child, right_child, leaf_value). Inference traverses each tree from root to leaf and sums the leaf values. The output is a score in [0, 1] — above VORTEX_INLINE_THRESHOLD (default 0.6) means inline.

- **Model Training** (`train.py`): Python script that trains the GBDT model offline. Input: CSV of call sites with features + label (1 = inlining was profitable, 0 = not). Profitability is measured by: (speedup of compiled code with inlining) - (compile time increase). The script uses scikit-learn's GradientBoostingClassifier. Output: the flat array format consumed by inference.h. This runs offline, not in the JIT.

- **Online Feedback** (`feedback.h/c`): Track the outcome of every inlining decision. When a deoptimization occurs at an inlined call site's guard, record that the inlining was potentially unprofitable. When compiled code runs stably for VORTEX_FEEDBACK_WINDOW (default 10000 executions), record that the inlining was profitable. Feed this data back into the model by updating the feature→label CSV. The model is retrained periodically (offline).

- **Inlining Transform** (`transform.h/c`): Given a call site and an inline decision, perform the inlining in the SoN graph: (1) clone the callee's SoN subgraph, (2) replace Parameter nodes with the call's arguments, (3) replace Return nodes with Phi nodes merging into the caller, (4) thread the callee's memory chain into the caller's, (5) add FrameState nodes at the inlined method's entry for deopt, (6) run GVN on the inlined subgraph.

#### Key invariants

- Inlining decisions are deterministic for the same feature vector and model
- Online feedback never blocks compilation
- Model retraining is always offline
- Inlining never changes the program's observable behavior

---

### 10. Profile Collection & Persistence (`src/profile/`)

**Estimated LoC**: 5,000–8,000  
**Priority**: MEDIUM  
**Agent-assignable**: Yes

Collects, persists, and loads execution profiles for Jump-Start compilation.

#### What must be implemented

- **Profile Data Structures** (`data.h/c`): Per-method profile: call site types (receiver TypeID per site, up to VORTEX_POLY_LIMIT entries), branch frequencies (taken/not-taken per branch bytecode PC), method invocation count, loop back-edge counts, field access shapes (holder ShapeID per field site). Global profile: method call graph (who calls whom, with frequency), phase transition graph (method → method transitions at the top level).

- **Jump-Start Serialization** (`persist.h/c`): Serialize profile data to a binary file. Format: magic number (0xVORTEX01), version, method count, then per-method: method ID, profile data blob. The file is written at program exit (atexit handler) and loaded at next startup. If the profile version doesn't match, the file is ignored (no crash, no corruption). Profile files are checksummed (CRC32).

- **Profile Merging** (`merge.h/c`): When loading a profile from a previous run, merge it with the current run's profile. Merging: invocation counts are summed, type observations are unioned, branch frequencies are averaged (weighted by invocation count). This allows the profile to improve over multiple runs.

- **Phase Detection** (`phase.h/c`): Detect program phases from the profile's call graph. A phase is a set of methods that are frequently called together. Phase detection: (1) build the call graph from profile data, (2) find strongly connected components, (3) each SCC with > VORTEX_PHASE_MIN_METHODS (default 3) methods and > VORTEX_PHASE_MIN_FREQUENCY (default 1000) total invocations is a phase. Phase transitions are edges between SCCs. This data feeds the SOTA phase detection feature.

#### Key invariants

- Profile data is never used if it doesn't match the current program version
- Profile loading never blocks program startup (load in background thread)
- Profile merging is associative and commutative (order doesn't matter)
- Phase detection is conservative — only identifies phases with strong evidence

---

### 11. x86-64 Lowering & Code Generation (`src/lower/`)

**Estimated LoC**: 8,000–12,000  
**Priority**: HIGH — needed to produce executable code  
**Agent-assignable**: Yes

Translates the scheduled SoN graph to x86-64 machine code. This is a custom code generator (not using Cranelift as a library, since we're in C/C++ — instead, inspired by Cranelift's design).

#### What must be implemented

- **Instruction Selection** (`isel.h/c`): Map each SoN node to one or more x86-64 instructions. Simple one-to-one mappings for arithmetic (Add → addq/addl, Sub → subq/subl, etc.), memory (Load → movq, Store → movq [dst+off], src), comparisons (Cmp → cmpq + conditional set). Complex mappings: Mul → imulq, Div → idivq (requires RDX:RAX), Shl → shlq by constant or CL, function calls → callq + argument moves. Each selected instruction records: native opcode bytes, operand registers, stack slot assignments.

- **Register Allocation** (`regalloc.h/c`): Linear scan register allocator. Input: the instruction stream with virtual registers. Output: physical register assignments and spill code. Algorithm: (1) compute live intervals for each virtual register, (2) sort intervals by start point, (3) iterate: assign a free register, or evict the interval with the furthest end point. Caller-saved registers (RAX, RCX, RDX, RSI, RDI, R8-R11) are used first; callee-saved (RBX, R12-R15) require save/restore. RBP is reserved for frame pointer, RSP for stack pointer. Spill slots are at fixed offsets from RBP.

- **Code Emission** (`emit.h/c`): Emit x86-64 machine code bytes into a code buffer. Each instruction is encoded according to the x86-64 specification: REX prefix (if using extended registers or 64-bit operands), opcode byte(s), ModR/M + SIB bytes (for memory operands), displacement, immediate. The emitter handles all addressing modes: register-register, register-immediate, register-memory (base+displacement, base+index+displacement), RIP-relative (for constants and global data). All encodings are verified against the Intel SDM.

- **Guard & Deopt Emission** (`guard_emit.h/c`): For each guard node, emit: (1) the guard check (compare and conditional jump), (2) if the guard fails, jump to the deopt stub. The deopt stub: saves all live registers to the frame, loads the deopt handler address, jumps to the deopt runtime. The guard's side table entry records: native PC of the guard check, live register set, FrameState index.

- **Relocations** (`reloc.h/c`): Handle fixups for: forward branches (patched after the target is emitted), call targets (patched after the callee is compiled), constant pool references (RIP-relative), and deopt handler addresses (absolute). Relocations are recorded in a relocation table and patched in a final pass.

#### Key invariants

- All x86-64 encodings are correct per Intel SDM
- Register allocation never assigns RBP or RSP to virtual registers
- Every guard has a corresponding side table entry
- Emitted code is position-independent (no absolute addresses except deopt handlers)

---

### 12. Code Cache Management (`src/codecache/`)

**Estimated LoC**: 3,000–5,000  
**Priority**: MEDIUM  
**Agent-assignable**: Yes

Manages the compiled code cache: allocation, eviction, and invalidation.

#### What must be implemented

- **Segmented Cache** (`cache.h/c`): The code cache is divided into segments (VORTEX_CACHE_SEGMENT_SIZE, default 1MB, derived from page size and TLB considerations). Each segment is a contiguous block of executable memory (mmap'd with PROT_EXEC | PROT_WRITE, then mprotected to PROT_EXEC | PROT_READ after filling). Segments are allocated on demand and freed when empty. Each segment tracks: write position, number of methods, total method size.

- **Method Installation** (`install.h/c`): After compilation, the method's native code is copied into the current segment. If the segment doesn't have enough space, allocate a new segment. The method's entry point is the start address of the code in the segment. The method's metadata (side table, deopt info) is stored in a separate metadata area (malloc'd, not executable). Installation is atomic: the method's code pointer is updated with a release store, and all subsequent calls use the new code.

- **LRU Eviction** (`evict.h/c`): When the code cache exceeds VORTEX_CACHE_MAX_SIZE (default 256MB, derived from typical working set sizes), evict the least-recently-used methods. LRU tracking: each method has a timestamp that is updated on every call (amortized — updated every VORTEX_LRU_UPDATE_INTERVAL calls, default 100, to avoid contention). Eviction: (1) find the method with the oldest timestamp, (2) mark it as not-compiled (code pointer → null), (3) free its side table and deopt info, (4) the code in the segment is not freed (segments are freed only when completely empty). Evicted methods will be recompiled if they become hot again.

- **Dependency-Set Invalidation** (`invalidate.h/c`): When a class is loaded or redefined, all methods that contain guards depending on that class must be invalidated. Each compiled method records a dependency set: the set of TypeIDs and ShapeIDs that its guards depend on. When a class with TypeID T is loaded: (1) find all methods whose dependency set contains T (inverted index: TypeID → set of methods), (2) mark those methods as not-compiled, (3) free their metadata. The next call to those methods will go through the interpreter and potentially be recompiled.

#### Key invariants

- Code cache never exceeds the maximum size
- Eviction is safe — evicted methods are always re-callable through the interpreter
- Invalidation is complete — all dependent methods are found
- Code segments are always executable after installation

---

### 13. Concurrent Compilation (`src/compile/`)

**Estimated LoC**: 5,000–8,000  
**Priority**: MEDIUM  
**Agent-assignable**: Yes

Compiles methods in background threads while the program continues executing.

#### What must be implemented

- **Thread Pool** (`threadpool.h/c`): Fixed-size thread pool (VORTEX_COMPILE_THREADS, default = number of CPU cores - 1, minimum 1). Worker threads pull compilation tasks from a priority queue. Thread pool threads are created at startup and persist for the program's lifetime. Each thread has its own arena allocator for compilation.

- **Priority Queue** (`priority.h/c`): Compilation tasks are prioritized by: (1) tier (T3 > T2 > T1 — higher tier = higher priority because more code depends on it), (2) method heat (hotter methods compiled first), (3) wait time (starvation prevention — tasks that have waited too long get a priority boost). The priority queue is a binary heap with a lock-free fast path for single-producer/single-consumer (the application thread submits, one worker thread consumes).

- **Safe Points** (`safepoint.h/c`): The application thread checks for safe points at: backward branches, method calls, and allocation points. At a safe point: (1) check if a compilation has completed and needs to be installed, (2) if so, update the method's code pointer, (3) check if an invalidation has occurred that affects the current method, (4) if so, switch to the interpreter. Safe point checks are cheap: read a global flag, branch if set.

- **Code Version Management** (`version.h/c`): A method may have multiple compiled versions simultaneously: T1 code still running while T2 compiles, or a deoptless continuation coexisting with the original. Each version has a state: Compiling, Active, Deprecated (will be replaced at next safe point), Invalidated. Version transitions: Compiling → Active (installation), Active → Deprecated (new version installed), Deprecated → freed (after all threads have exited the deprecated code), Active → Invalidated (class load/reshape).

#### Key invariants

- Compilation never blocks the application thread (submission is O(1))
- Safe point checks are < 5ns (measured)
- Only one version of a method is active at any time
- Deprecated code is never freed while any thread is executing it

---

### 14. Adaptive Guard Strength (`src/guard/`)

**Estimated LoC**: 3,000–5,000  
**Priority**: MEDIUM (SOTA feature #3)  
**Agent-assignable**: Yes

Continuous, adaptive guard strength based on runtime failure rates.

#### What must be implemented

- **Guard Metadata** (`metadata.h/c`): Each guard in compiled code tracks: execution count (saturating counter), failure count, last failure timestamp, current strength level. Strength levels: Unconditional (no check, per-class invalidation hook), FastCheck (single comparison), FullCheck (full type walk), DeoptAlways (always deoptimize — the guard has failed too often). Transitions: Unconditional → FastCheck (on first failure), FastCheck → FullCheck (failure rate > VORTEX_GUARD_WEAKEN_THRESHOLD, default 1%), FullCheck → DeoptAlways (failure rate > VORTEX_GUARD_ABANDON_THRESHOLD, default 25%).

- **EWMA Tracking** (`ewma.h/c`): Exponentially weighted moving average of guard failure rate. Formula: `ewma = alpha * (failure_count / execution_count) + (1 - alpha) * ewma`. Alpha is VORTEX_GUARD_ALPHA (default 0.1, giving a half-life of ~7 executions). The EWMA smooths out transient failures and detects sustained changes in guard behavior.

- **Guard Hoisting** (`hoist.h/c`): During SoN optimization, hoist guards that are loop-invariant out of loops. A guard is loop-invariant if all its inputs are defined outside the loop. Hoisted guards are placed at the loop preheader. If a hoisted guard fails, the entire loop is deoptimized — but this is rare for truly invariant guards.

- **Guard Merging** (`merge.h/c`): Merge adjacent guards that check the same value. Two type checks on the same receiver can be merged into a single check against a type set. Two null checks on the same value can be merged into one. Guard merging reduces the overhead of multiple guards and creates opportunities for further optimization.

#### Key invariants

- Guard strength transitions are one-way (can only weaken, never strengthen, within a compilation)
- EWMA never overflows (saturating arithmetic)
- Hoisted guards always dominate the original guard position
- Merged guards are at least as strong as the strongest original guard

---

### 15. SOTA Features (`src/sota/`)

**Estimated LoC**: 8,000–12,000 (across all 6 features)  
**Priority**: MEDIUM — differentiating features  
**Agent-assignable**: Yes (each feature is independent)

The six features that make VORTEX genuinely state-of-the-art.

#### 15.1 Phase Detection + Preemptive Compilation (`phase.h/c`)

Maintain a phase transition graph per program. At program startup, load the phase graph from the previous run's profile. During execution, detect phase entry by monitoring method call patterns. When a phase entry is detected (the top-N method call signature matches a known phase), preemptively compile the phase's hot methods at T2/T3 before they are individually hot. If the prediction is wrong, the compiled code is never used — zero cost. The phase transition graph is updated at every top-level method call.

#### 15.2 Allocation Graph Elimination (`alloc_graph.h/c`)

Build an allocation graph: nodes are allocations, edges are field stores between them. For each allocation, compute "effective escape": trace all paths from the allocation to escape points. If the only escaping path goes through a field store (e.g., `a.next = b`), and the field is never read at the escape point, then b's effective escape is NoEscape. Apply cross-object scalar replacement to all allocations with effective NoEscape. This extends PEA to eliminate entire object graphs, not just individual objects.

#### 15.3 Continuous Background Recompilation (`recomp.h/c`)

A background thread monitors execution profiles in real-time. When a method's observed type profile diverges from the profile used for compilation (measured by Kullback-Leibler divergence > VORTEX_PROFILE_DIVERGENCE_THRESHOLD, default 0.5), queue the method for recompilation with the updated profile. Recompilation happens in the background. The new version is installed at the next safe point with zero pause. The old version is deprecated and freed when no thread is executing it.

#### 15.4 Speculative Loop Transformations (`loop_spec.h/c`)

Profile loop trip counts and access patterns. When a loop has: predictable trip count (CV < VORTEX_LOOP_CV_THRESHOLD, default 0.2), no aliasing between array accesses (proven by type-based alias analysis + runtime guard), and no dependencies between iterations, speculatively emit SIMD code (SSE2/AVX2 based on CPU feature detection). The SIMD code includes: alignment guard (check that the base address is aligned), aliasing guard (check that array ranges don't overlap), and trip count guard (check that the trip count matches the expected value). On guard failure, deoptimize to the scalar loop.

#### 15.5 Feedback-Directed Inlining Recompilation (`fdi.h/c`)

Track the outcome of every inlining decision: did the inlined code cause excessive register pressure (measured by spill count > VORTEX_SPILL_THRESHOLD, default 10% of instructions), or high deopt rate (deopt count > VORTEX_INLINE_DEOPT_THRESHOLD, default 5% of executions)? When an inlining decision proves suboptimal, queue a targeted recompilation with that call site treated differently (force no-inline). Feed the outcome back into the ML model immediately (online learning). Maintain multiple compiled versions of hot methods with different inlining strategies, and switch between them based on observed performance.

#### 15.6 Adaptive Guard Strength (detailed in component 14)

---

### 16. Tests & Benchmarks (`tests/`, `benchmarks/`)

**Estimated LoC**: 20,000–30,000  
**Priority**: HIGH — no tests = no trust  
**Agent-assignable**: Yes

#### What must be implemented

- **Unit Tests** (`tests/unit/`): One test file per component. Each test: creates the component in isolation, exercises every public function, asserts correct results. Tests use a custom test framework (no external dependency) with: assert_equal, assert_not_equal, assert_null, assert_not_null, assert_true, assert_false, assert_throws. Test runner discovers tests by function name prefix (vtx_test_*), runs them in order, reports pass/fail with file:line.

- **Integration Tests** (`tests/integration/`): End-to-end tests that compile and run small programs. Test programs are written in VORTEX bytecode (assembled from a simple text format). Each test: assembles the bytecode, runs it through the interpreter, runs it through each JIT tier, asserts the results match. Tests cover: arithmetic, control flow, method calls, virtual dispatch, exception handling, allocation, array access, type checks, deoptimization, OSR, concurrent compilation.

- **Regression Tests** (`tests/regression/`): Tests for specific bugs that have been found and fixed. Each test is named after the issue number or a short description. The test reproduces the bug's input and asserts that the output is correct (i.e., the bug doesn't recur).

- **Benchmarks** (`benchmarks/`): Micro-benchmarks and macro-benchmarks. Micro: individual operations (add, call, allocate, type check) measured in nanoseconds. Macro: small programs (fibonacci, sort, hash map, tree traversal) measured in milliseconds. Benchmarks compare: interpreter, T1, T2, T3. Benchmarks are run with statistically rigorous methodology: warmup (VORTEX_BENCH_WARMUP, default 10 iterations), measurement (VORTEX_BENCH_ITERATIONS, default 100 iterations), report median and 95th percentile.

#### Key invariants

- All tests pass before any commit
- No test uses hard-coded timing expectations (only functional correctness)
- Benchmarks report statistics, not single runs
- Test coverage > 80% for all components (measured by gcov)

---

## Directory Structure

```
VORTEX/
├── CMakeLists.txt                  # Top-level CMake
├── roadmap.md                      # This file
├── LICENSE                         # Apache 2.0
├── src/
│   ├── CMakeLists.txt
│   ├── runtime/
│   │   ├── CMakeLists.txt
│   │   ├── object.h                # Tagged value, heap object layout
│   │   ├── object.c
│   │   ├── type_system.h           # TypeID, type descriptors, vtables
│   │   ├── type_system.c
│   │   ├── gc.h                    # Garbage collector interface
│   │   ├── gc.c
│   │   ├── helpers.h               # Runtime helper functions
│   │   ├── helpers.c
│   │   ├── bytecode.h              # Opcode definitions, format spec
│   │   ├── bytecode.c
│   │   ├── arena.h                 # Region allocator
│   │   └── arena.c
│   ├── interp/
│   │   ├── CMakeLists.txt
│   │   ├── dispatch.h              # Computed goto dispatch
│   │   ├── dispatch.c
│   │   ├── frame.h                 # Interpreter stack frame
│   │   ├── frame.c
│   │   ├── profiler.h              # Profiling counters
│   │   ├── profiler.c
│   │   ├── lookup.h                # Inline cache method lookup
│   │   ├── lookup.c
│   │   ├── type_feedback.h         # Type feedback collection
│   │   └── type_feedback.c
│   ├── baseline/
│   │   ├── CMakeLists.txt
│   │   ├── codegen.h               # Bytecode → x86-64 one-pass
│   │   ├── codegen.c
│   │   ├── guards.h                # Guard emission
│   │   ├── guards.c
│   │   ├── frame_layout.h          # JIT frame layout
│   │   ├── frame_layout.c
│   │   ├── deopt_stubs.h           # Deopt stub generation
│   │   ├── deopt_stubs.c
│   │   ├── instrument.h            # Profiling instrumentation
│   │   └── instrument.c
│   ├── ir/
│   │   ├── CMakeLists.txt
│   │   ├── node.h                  # Node taxonomy, opcodes
│   │   ├── node.c
│   │   ├── graph.h                 # Graph construction from bytecode
│   │   ├── graph.c
│   │   ├── gvn.h                   # Global value numbering
│   │   ├── gvn.c
│   │   ├── constant_prop.h         # Sparse conditional constant prop
│   │   ├── constant_prop.c
│   │   ├── dce.h                   # Dead code elimination
│   │   ├── dce.c
│   │   ├── schedule.h              # Code motion & scheduling
│   │   ├── schedule.c
│   │   ├── verify.h                # IR verification
│   │   └── verify.c
│   ├── trace/
│   │   ├── CMakeLists.txt
│   │   ├── selector.h              # Hot trace selection
│   │   ├── selector.c
│   │   ├── recorder.h              # Trace recording
│   │   ├── recorder.c
│   │   ├── tree.h                  # Trace trees
│   │   ├── tree.c
│   │   ├── side_exit.h             # Side exit handling
│   │   └── side_exit.c
│   ├── region/
│   │   ├── CMakeLists.txt
│   │   ├── stitch.h                # Trace stitching
│   │   ├── stitch.c
│   │   ├── budget.h                # Size budget management
│   │   ├── budget.c
│   │   ├── cross_trace.h           # Cross-trace optimization
│   │   └── cross_trace.c
│   ├── pea/
│   │   ├── CMakeLists.txt
│   │   ├── analysis.h              # Escape analysis
│   │   ├── analysis.c
│   │   ├── cross_object_sr.h       # Cross-object scalar replacement
│   │   ├── cross_object_sr.c
│   │   ├── materialize.h           # Object materialization
│   │   ├── materialize.c
│   │   ├── virtual.h               # Virtual object tracking
│   │   └── virtual.c
│   ├── deopt/
│   │   ├── CMakeLists.txt
│   │   ├── frame_state.h           # Frame state capture
│   │   ├── frame_state.c
│   │   ├── osr.h                   # On-stack replacement
│   │   ├── osr.c
│   │   ├── deoptless.h             # Deoptless continuation
│   │   ├── deoptless.c
│   │   ├── side_table.h            # PC → FrameState mapping
│   │   ├── side_table.c
│   │   ├── stack_walk.h            # Stack walking
│   │   └── stack_walk.c
│   ├── inliner/
│   │   ├── CMakeLists.txt
│   │   ├── features.h              # Feature extraction
│   │   ├── features.c
│   │   ├── inference.h             # GBDT inference
│   │   ├── inference.c
│   │   ├── feedback.h              # Online feedback
│   │   ├── feedback.c
│   │   ├── transform.h             # Inlining transform
│   │   └── transform.c
│   ├── profile/
│   │   ├── CMakeLists.txt
│   │   ├── data.h                  # Profile data structures
│   │   ├── data.c
│   │   ├── persist.h               # Jump-Start serialization
│   │   ├── persist.c
│   │   ├── merge.h                 # Profile merging
│   │   ├── merge.c
│   │   ├── phase.h                 # Phase detection
│   │   └── phase.c
│   ├── lower/
│   │   ├── CMakeLists.txt
│   │   ├── isel.h                  # Instruction selection
│   │   ├── isel.c
│   │   ├── regalloc.h              # Linear scan register allocator
│   │   ├── regalloc.c
│   │   ├── emit.h                  # x86-64 machine code emission
│   │   ├── emit.c
│   │   ├── guard_emit.h            # Guard & deopt emission
│   │   ├── guard_emit.c
│   │   ├── reloc.h                 # Relocations & fixups
│   │   └── reloc.c
│   ├── codecache/
│   │   ├── CMakeLists.txt
│   │   ├── cache.h                 # Segmented code cache
│   │   ├── cache.c
│   │   ├── install.h               # Method installation
│   │   ├── install.c
│   │   ├── evict.h                 # LRU eviction
│   │   ├── evict.c
│   │   ├── invalidate.h            # Dependency-set invalidation
│   │   └── invalidate.c
│   ├── compile/
│   │   ├── CMakeLists.txt
│   │   ├── threadpool.h            # Compilation thread pool
│   │   ├── threadpool.c
│   │   ├── priority.h              # Priority queue
│   │   ├── priority.c
│   │   ├── safepoint.h             # Safe point checks
│   │   ├── safepoint.c
│   │   ├── version.h               # Code version management
│   │   └── version.c
│   ├── guard/
│   │   ├── CMakeLists.txt
│   │   ├── metadata.h              # Guard metadata & tracking
│   │   ├── metadata.c
│   │   ├── ewma.h                  # EWMA failure rate
│   │   ├── ewma.c
│   │   ├── hoist.h                 # Guard hoisting
│   │   ├── hoist.c
│   │   ├── merge.h                 # Guard merging
│   │   └── merge.c
│   └── sota/
│       ├── CMakeLists.txt
│       ├── phase.h                  # SOTA #1: Phase detection
│       ├── phase.c
│       ├── alloc_graph.h            # SOTA #2: Allocation graph elimination
│       ├── alloc_graph.c
│       ├── recomp.h                 # SOTA #3: Continuous recompilation
│       ├── recomp.c
│       ├── loop_spec.h              # SOTA #4: Speculative loop transforms
│       ├── loop_spec.c
│       ├── fdi.h                    # SOTA #5: Feedback-directed inlining
│       ├── fdi.c
│       └── train.py                 # ML model training script
├── tests/
│   ├── CMakeLists.txt
│   ├── test_framework.h             # Minimal test framework
│   ├── test_framework.c
│   ├── unit/
│   │   ├── test_arena.c
│   │   ├── test_object.c
│   │   ├── test_type_system.c
│   │   ├── test_bytecode.c
│   │   ├── test_dispatch.c
│   │   ├── test_node.c
│   │   ├── test_graph.c
│   │   ├── test_gvn.c
│   │   ├── test_constant_prop.c
│   │   ├── test_dce.c
│   │   ├── test_schedule.c
│   │   ├── test_pea.c
│   │   ├── test_deopt.c
│   │   ├── test_isel.c
│   │   ├── test_regalloc.c
│   │   ├── test_emit.c
│   │   └── ...
│   ├── integration/
│   │   ├── test_interp_basics.c
│   │   ├── test_jit_t1.c
│   │   ├── test_jit_t2.c
│   │   ├── test_deopt_roundtrip.c
│   │   └── ...
│   └── regression/
│       └── ...
├── benchmarks/
│   ├── CMakeLists.txt
│   ├── bench_framework.h
│   ├── bench_framework.c
│   ├── micro/
│   │   ├── bench_arith.c
│   │   ├── bench_call.c
│   │   ├── bench_alloc.c
│   │   └── ...
│   └── macro/
│       ├── bench_fib.c
│       ├── bench_sort.c
│       ├── bench_hashmap.c
│       └── ...
└── tools/
    ├── CMakeLists.txt
    ├── assembler.h                  # Bytecode assembler (text → bytecode)
    ├── assembler.c
    ├── disassembler.h               # Bytecode disassembler
    └── disassembler.c
```

---

## Build Instructions

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DVORTEX_ENABLE_ASSERTIONS=ON
make -j$(nproc)
ctest --output-on-failure
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `VORTEX_ENABLE_ASSERTIONS` | ON | Enable debug assertions in all components |
| `VORTEX_ENABLE_VERIFY` | ON | Enable IR verification after each optimization pass |
| `VORTEX_ENABLE_PROFILING` | ON | Enable profiling in interpreter and T1 |
| `VORTEX_ENABLE_SOTA` | ON | Enable SOTA features (phase detection, speculative loops, etc.) |
| `VORTEX_CACHE_MAX_SIZE` | 268435456 (256MB) | Maximum code cache size in bytes |
| `VORTEX_T1_THRESHOLD` | 1000 | Method execution count to trigger T1 compilation |
| `VORTEX_T2_THRESHOLD` | 10000 | Trace heat to trigger T2 compilation |
| `VORTEX_COMPILE_THREADS` | 0 (auto) | Number of compilation threads (0 = cores - 1) |

---

## Implementation Order

The order matters — later components depend on earlier ones.

| Phase | Components | Rationale |
|-------|-----------|-----------|
| **1** | Runtime, Arena, Bytecode | Everything depends on the object model and memory |
| **2** | Interpreter T0 | Need to execute code to profile |
| **3** | Profile Collection | Need profiles to drive compilation |
| **4** | Baseline JIT T1 | Quick wins — get past interpreter speed |
| **5** | Deoptimization (core) | Need deopt before T2 (T2 is speculative) |
| **6** | Sea-of-Nodes IR | The optimizer's representation |
| **7** | Trace Recorder | Need traces for T2 |
| **8** | PEA + SR | The biggest performance win |
| **9** | x86-64 Lowering | Need to generate optimized code |
| **10** | Code Cache | Need to manage compiled code |
| **11** | Region Formation | Cross-trace optimization |
| **12** | Concurrent Compilation | Performance polish |
| **13** | ML Inliner | Smart inlining decisions |
| **14** | Adaptive Guards | Guard optimization |
| **15** | SOTA Features | The differentiating features |
| **16** | Tests & Benchmarks | Ongoing — write tests alongside code |

---

## Configuration Constants

All configuration constants are derived from principled measurements or well-known systems. No magic numbers.

| Constant | Value | Derivation |
|----------|-------|------------|
| `VTX_TAG_BITS` | 3 | Heap alignment is 8 bytes → 3 low bits available |
| `VTX_SMI_MAX` | 2^46 - 1 | NaN-boxing: 46 bits for small integer (sign + 45 magnitude) |
| `VTX_POLY_LIMIT` | 4 | Empirical: polymorphic IC with >4 types is slower than megamorphic |
| `VTX_T1_THRESHOLD` | 1000 | HotSpot C1 threshold is ~1000; LuaJIT traces at ~600 |
| `VTX_T2_THRESHOLD` | 10000 | 10x T1: enough profile data for accurate speculation |
| `VTX_MAX_TRACE_LENGTH` | 512 | LuaJIT limit is ~64K bytecodes; we're more conservative |
| `VTX_MAX_TREE_DEPTH` | 8 | Trace tree depth >8 causes exponential compile time |
| `VTX_MAX_HYPERBLOCK_NODES` | 4096 | Fits in 32KB native code budget |
| `VTX_CACHE_SEGMENT_SIZE` | 1048576 (1MB) | Page-aligned; fits ~200 average methods |
| `VTX_CACHE_MAX_SIZE` | 268435456 (256MB) | Typical server working set; configurable down |
| `VTX_ARENA_PAGE_SIZE` | 65536 (64KB) | 16x system page; reduces mmap calls |
| `VTX_FRAME_ALIGNMENT` | 16 | System V AMD64 ABI requirement |
| `VTX_INLINE_THRESHOLD` | 0.6 | GBDT output threshold; 60% confidence to inline |
| `VTX_GUARD_ALPHA` | 0.1 | EWMA smoothing; half-life of ~7 samples |
| `VTX_GUARD_WEAKEN_THRESHOLD` | 0.01 | 1% failure rate → strengthen guard |
| `VTX_GUARD_ABANDON_THRESHOLD` | 0.25 | 25% failure rate → always deopt |
| `VTX_PROFILE_DIVERGENCE_THRESHOLD` | 0.5 | KL divergence threshold for recompilation |
| `VTX_LOOP_CV_THRESHOLD` | 0.2 | Coefficient of variation for predictable loops |
| `VTX_SPILL_THRESHOLD` | 0.1 | 10% of instructions being spills → unprofitable inline |
| `VTX_INLINE_DEOPT_THRESHOLD` | 0.05 | 5% deopt rate → unprofitable inline |
| `VTX_SIDE_EXIT_THRESHOLD` | 50 | 50 side exits before recording a branch trace |
| `VTX_STITCH_THRESHOLD` | 200 | 200 exits before stitching a branch |
| `VTX_PHASE_MIN_METHODS` | 3 | Minimum methods to constitute a phase |
| `VTX_PHASE_MIN_FREQUENCY` | 1000 | Minimum total invocations for phase detection |
| `VTX_LRU_UPDATE_INTERVAL` | 100 | Update LRU timestamp every 100 calls (amortized) |
| `VTX_BENCH_WARMUP` | 10 | Benchmark warmup iterations |
| `VTX_BENCH_ITERATIONS` | 100 | Benchmark measurement iterations |

---

## Agent Instructions

Each agent that works on VORTEX must:

1. **Read this roadmap** before starting any work
2. **Read the worklog** at `/home/z/my-project/worklog.md` to see what previous agents have done
3. **Write to the worklog** after completing work (append, never overwrite)
4. **Follow the no-stubs policy**: every function must contain real, working logic
5. **Follow the interface contracts**: use the exact type names, function signatures, and file names specified here
6. **Write tests**: each component must have unit tests in `tests/unit/`
7. **Compile cleanly**: `gcc -Wall -Wextra -Werror -std=c17` or `g++ -Wall -Wextra -Werror -std=c++20`
8. **No external dependencies**: only libc, libm, libpthread, and POSIX APIs

---

## Success Criteria

VORTEX is successful when:

1. It can execute bytecode programs correctly at all tier levels (T0, T1, T2, T3)
2. Deoptimization always produces the same result as the interpreter
3. T1 code runs at least 2x interpreter speed
4. T2 code runs at least 5x interpreter speed (with PEA + inlining)
5. T3 code runs at least 10x interpreter speed (with speculative vectorization)
6. The ML inliner makes better decisions than a simple size-heuristic inliner
7. Profile persistence (Jump-Start) reduces warmup time by >50% on repeat runs
8. Concurrent compilation adds < 5% overhead while reducing compilation pauses by >80%
9. All tests pass, all benchmarks report statistics
10. No stubs, no placeholders, no hard-coded values remain
