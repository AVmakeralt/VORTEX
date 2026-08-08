

# VORTEX Engineering Standards

> **VORTEX code must be written as if it will be deployed to production immediately.**
>
> A feature is not complete when its implementation exists. It is complete when it is correctly integrated, tested, maintainable, and safe under the runtime conditions it will encounter.
>
> **Do not leave functionality dangling, unwired, or dependent on future integration.**
>
> Missing GC registration, stale metadata, incorrect deoptimization state, broken ownership, missing barriers, or incomplete runtime wiring are **correctness failures**, not cleanup tasks.

---

## 1. Language and Implementation

### 1.1 Language Requirements

- All new implementation code **MUST** be written in **C++**.
- Rust is permitted only for explicitly approved bindings or integration layers.
- C is permitted only for legacy code, maintenance, compatibility, or other explicitly justified cases.
- New C code **MUST NOT** be introduced simply because the surrounding code is written in C.

### 1.2 Modern C++

New C++ code should use modern C++ facilities appropriately.

- Use RAII for resource ownership.
- Prefer `std::unique_ptr` and `std::shared_ptr` when their ownership semantics are appropriate.
- Avoid raw owning pointers.
- Use references, pointers, `std::span`, views, and value types for non-owning access where appropriate.
- Use `enum class` instead of unscoped enums.
- Use `nullptr`, never `NULL`.
- Use `const`, `constexpr`, and `noexcept` where appropriate.
- Prefer compile-time validation when practical.
- Avoid unnecessary heap allocation in hot paths.
- Avoid exceptions in latency-sensitive runtime/JIT paths unless explicitly justified.
- C-style casts are prohibited in new code.
- Undefined behavior **MUST NOT** be used as an optimization technique.

### 1.3 Ownership

Ownership and lifetime must be obvious.

Every object must have a clear answer to:

1. Who owns it?
2. Who may mutate it?
3. When is it destroyed?
4. Can it move?
5. Can it be collected?
6. Can another thread access it?

If these questions cannot be answered from the surrounding code and API, the implementation is not sufficiently clear.

---

## 2. C++ Code Style

VORTEX follows the **[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)** as its baseline for formatting, naming, structure, and general C++ practices.

VORTEX-specific requirements override the Google style guide where explicitly stated.

### 2.1 Formatting

- Use `clang-format` with the repository's approved configuration.
- Formatting must be consistent across the codebase.
- Do not introduce formatting-only changes into unrelated files.
- Avoid excessively long functions and deeply nested control flow.

### 2.2 Naming

Follow Google's naming conventions.

#### Types

Use `PascalCase`:

```cpp
class GarbageCollector;
struct MachineState;
enum class CompilationTier;
````

#### Functions

Use `PascalCase`:

```cpp
CompileFunction();
CollectGarbage();
LowerInstruction();
```

#### Variables

Use `snake_case`:

```cpp
auto bytecode_offset = GetOffset();
auto compilation_state = GetState();
```

#### Constants

Use `kPascalCase`:

```cpp
constexpr int kMaxInlineDepth = 32;
constexpr size_t kPageSize = 4096;
```

#### Data Members

Use a trailing underscore:

```cpp
class Compiler {
 private:
    IRGraph* graph_;
    CompilationTier tier_;
};
```

Names must describe meaning.

Prefer:

```cpp
bool is_deoptimized = state.IsDeoptimized();
```

over:

```cpp
bool flag = state.IsDeoptimized();
```

### 2.3 Classes

Classes should have a clear responsibility.

Avoid classes that simultaneously handle unrelated concerns such as:

* parsing,
* IR construction,
* optimization,
* code generation,
* memory management,
* and runtime execution.

Split unrelated responsibilities into appropriate components.

Public interfaces should generally appear before implementation details:

```cpp
class Compiler {
 public:
    Compiler(Context* context);

    CompilationResult Compile(const Function& function);

 private:
    void Optimize();
    void Lower();

    Context* context_;
};
```

### 2.4 Functions

Functions should perform one coherent operation.

Prefer:

```cpp
IRGraph* BuildGraph(const Bytecode& bytecode);
void RunOptimizationPasses(IRGraph* graph);
MachineCode GenerateMachineCode(IRGraph* graph);
```

over a single function responsible for the entire compilation pipeline.

There is no arbitrary maximum function length, but excessive size is a code-review warning. Split functions when:

* they perform multiple logically distinct operations,
* control flow becomes difficult to follow,
* error handling becomes tangled,
* local state becomes difficult to reason about,
* or understanding one section requires tracking unrelated state.

### 2.5 Comments

Comments should explain **why**, not merely repeat **what** the code does.

Bad:

```cpp
// Increment i.
++i;
```

Good:

```cpp
// Keep this index stable because the deoptimizer stores the bytecode
// offset before entering this loop.
++bytecode_index;
```

Document surprising behavior caused by:

* compiler constraints,
* GC requirements,
* ABI requirements,
* hardware behavior,
* JIT assumptions,
* or other non-obvious implementation constraints.

Comments must not be used to justify unnecessarily complicated or incorrect code.

### 2.6 Header Hygiene

* Include what you use.
* Do not rely on transitive includes.
* Prefer forward declarations where appropriate.
* Avoid unnecessary header dependencies.

A source file should compile correctly using its own explicit dependencies.

### 2.7 Type Safety

Prefer strong types when values represent different concepts.

Avoid:

```cpp
void Compile(int offset, int size, int tier);
```

when the values have distinct meanings.

Prefer:

```cpp
void Compile(BytecodeOffset offset,
             BytecodeSize size,
             CompilationTier tier);
```

The type system should prevent invalid states whenever practical.

### 2.8 `const` Correctness

Use `const` consistently.

```cpp
const IRNode* node;
```

Member functions that do not modify object state should be `const`:

```cpp
bool IsConstant() const;
```

Do not remove `const` merely for convenience.

### 2.9 Avoid Clever C++

Code should prioritize correctness, readability, and maintainability over cleverness.

Use advanced C++ features when they provide a real benefit.

Do not introduce complex template metaprogramming, abstractions, or language tricks merely because they are possible.

The preferred implementation is the simplest implementation that correctly satisfies the requirements.

### 2.10 Control Flow

Prefer straightforward control flow and early returns where they improve readability.

Prefer:

```cpp
if (!IsValid(node)) {
    return Error::InvalidNode;
}

if (!CanOptimize(node)) {
    return KeepNode(node);
}

return Optimize(node);
```

over deeply nested conditionals.

### 2.11 Dead Code

Do not leave commented-out implementations in the codebase.

Use version control instead.

Unused variables, functions, parameters, classes, fields, includes, and compatibility code should be removed unless their existence is intentional and documented.

---

## 3. Production-Readiness

### 3.1 Production Rule

Everything must be treated as production code.

Code is not complete merely because:

* it compiles,
* a unit test passes,
* the API exists,
* the feature exists,
* or it works in isolation.

A feature is complete only when it is fully integrated into every system that depends on it.

Depending on the component, this may include:

* GC integration
* object lifetime management
* deoptimization
* exception handling
* safepoints
* write barriers
* threading
* profiling
* invalidation
* code generation
* serialization/deserialization
* diagnostics
* error propagation
* shutdown/destruction
* API registration
* runtime registration
* build configuration

### 3.2 No Dangling Features

Functionality must not be left partially implemented or unwired.

For example:

```cpp
class MyObject : public GCObject {
    Value child;
};
```

is not complete if `child` contains a GC-managed reference but the collector cannot discover or correctly manage it.

Required integration may include:

* tracing,
* write barriers,
* relocation/forwarding,
* object scanning,
* lifetime management,
* root registration,
* and any other collector-specific requirements.

A missing GC edge is a **correctness bug**, not a cleanup task.

### 3.3 Integration Checklist

Every new subsystem or runtime component must have defined answers for:

* Where is it constructed?
* Where is it registered?
* Who owns it?
* Who destroys it?
* How does it interact with the GC?
* How does it interact with threads?
* How does it interact with exceptions?
* How does it interact with deoptimization?
* How does it interact with profiling?
* What happens if initialization fails?
* What happens during shutdown?
* What happens with malformed input?
* What happens when an assumption becomes invalid?

If a subsystem does not interact with one of these mechanisms, that should be intentional and documented.

---

## 4. Implementation Verification

VORTEX implementations must not be judged solely against the intended algorithm.

For significant compiler and runtime components, compare the design and implementation against established production systems:

* V8
* HotSpot
* GraalVM
* PyPy

These systems are reference points, not necessarily implementation templates.

Comparison should consider:

* algorithmic structure,
* optimization opportunities,
* representation choices,
* memory management,
* invalidation,
* deoptimization,
* threading,
* edge cases,
* correctness invariants,
* and performance.

If VORTEX deliberately differs from an established implementation, the difference must have a technical justification.

> **“We did it differently” is not a justification.**
>
> The justification is why the difference is correct, necessary, or beneficial.

---

## 5. Performance

Performance-sensitive code must be benchmarked.

Do not optimize based solely on intuition.

For significant performance changes:

1. Establish a baseline.
2. Implement the change.
3. Benchmark representative workloads.
4. Profile the relevant code.
5. Identify regressions.
6. Compare against appropriate reference implementations.

Depending on the workload, comparisons should include:

* VORTEX interpreter
* VORTEX baseline JIT
* VORTEX optimized tiers
* native C/C++
* V8
* HotSpot
* GraalVM
* PyPy

### 5.1 Performance Regressions

A change that slows a hot path must be:

* rejected,
* optimized further,
* or accompanied by a documented technical justification.

Performance regressions must not be dismissed merely because they appear small.

---

## 6. Testing

Tests must attempt to break the implementation, not merely demonstrate the happy path.

Tests should cover applicable cases including:

* malformed input,
* pathological input,
* integer overflow,
* unusual type combinations,
* empty structures,
* extremely large structures,
* deep recursion,
* recursion limits,
* invalid assumptions,
* GC pressure,
* allocation pressure,
* race conditions,
* deoptimization,
* invalidation,
* exceptions,
* interrupted execution,
* repeated recompilation,
* tier transitions,
* unusual control flow,
* aliasing,
* speculative optimization failures.

### 6.1 Integration Testing

Component tests are insufficient for features that interact with the runtime.

For example, a GC-sensitive object should be tested through a lifecycle such as:

```text
Allocate object
    ↓
Store GC reference
    ↓
Trigger collection
    ↓
Relocate object
    ↓
Continue execution
    ↓
Read reference
```

The test must verify the complete runtime behavior, not merely that the reference works immediately after assignment.

---

## 7. Compiler Correctness

Every optimization must preserve observable program behavior.

For example:

```text
Before:
A → B → C

After:
A → C
```

The optimization must demonstrate that removing `B` cannot incorrectly change:

* program results,
* exceptions,
* memory effects,
* observable ordering,
* GC behavior,
* deoptimization state,
* synchronization,
* or other runtime-visible behavior.

### 7.1 Speculative Optimization

Every speculative optimization must define:

1. The assumption being made.
2. How assumption failure is detected.
3. The recovery/deoptimization path.
4. Tests covering assumption failure.

Speculation without a valid failure path is incorrect.

---

## 8. IR Standards

Every IR transformation must preserve documented invariants.

Each pass must clearly define:

* input invariants,
* output invariants,
* required analyses,
* invalidated analyses,
* required metadata.

For example, if a pass invalidates:

```text
DominatorTree
AliasAnalysis
LoopAnalysis
TypeInformation
```

that invalidation must be explicitly represented.

Stale analysis results must never be silently reused.

---

## 9. Garbage Collection

GC correctness is a hard requirement.

Every GC-managed reference must have a defined tracing and lifetime policy.

New GC-managed object types must account for:

* GC reference fields,
* tracing,
* write barriers,
* allocation,
* finalization where applicable,
* relocation,
* forwarding,
* weak references,
* thread-local roots,
* JIT-generated references.

### 9.1 GC Safety Rule

> **If an object can contain a GC reference, the GC must know about it.**

Exceptions require explicit documentation and a demonstrated reason they are safe.

### 9.2 JIT/GC Interaction

Generated code must correctly handle:

* safepoints,
* stack maps,
* GC roots,
* relocated objects,
* write barriers,
* deoptimization metadata.

A mathematically correct optimization that causes the collector to lose track of a live object is **incorrect**.

---

## 10. Threading

Shared mutable state must have explicit synchronization semantics.

Every shared object should have a clearly defined concurrency model:

* thread-local,
* immutable,
* externally synchronized,
* internally synchronized,
* atomic,
* lock-free,
* or another explicitly defined model.

Do not introduce atomics or locks without understanding their correctness and performance implications.

Locks in hot paths must be benchmarked.

### 10.1 Concurrency Testing

Concurrent components should be tested with:

* ThreadSanitizer,
* stress workloads,
* repeated scheduling,
* high contention,
* shutdown races,
* compilation/execution races.

---

## 11. Error Handling

Errors must propagate correctly.

Do not silently ignore failures.

Bad:

```cpp
bool Compile(...) {
    if (!something()) {
        return false;
    }

    // Continue anyway.
}
```

unless continuing is explicitly correct.

Errors must not silently become:

* corrupted state,
* invalid IR,
* stale metadata,
* leaked resources,
* invalid machine code,
* or undefined behavior.

Every failure path must leave the system in a valid state.

---

## 12. Assertions and Invariants

Use assertions to enforce internal invariants close to their source.

```cpp
assert(block->isLinked());
assert(value->type() != Type::Invalid);
assert(state.isConsistent());
```

Assertions are not a substitute for validating external or malformed input.

For internal invariants:

```cpp
assert(index < table.size());
```

For external input:

```cpp
if (index >= table.size()) {
    return Error::InvalidIndex;
}
```

The distinction must be preserved.

---

## 13. TODOs

TODOs must not hide incomplete production functionality.

Bad:

```cpp
// TODO: connect this to the GC.
```

when the feature is already being merged.

If GC integration is required for correctness, the implementation is incomplete until the integration exists.

TODOs are acceptable for genuinely non-blocking future improvements when the remaining work and reason are clear.

---

## 14. Code Review

Every non-trivial change should be reviewable in terms of:

### Correctness

* What invariants does this rely on?
* What invariants does it establish?
* What happens when its assumptions fail?

### Integration

* What systems does this touch?
* What systems need to know it exists?
* Has every required registration and wiring point been updated?

### Memory

* Who owns the data?
* Can it move?
* Can it be collected?
* Can another thread access it?

### Compiler

* Does it affect IR correctness?
* Does it invalidate analysis?
* Does it affect deoptimization?

### Performance

* Is this on a hot path?
* What does profiling show?
* What are the benchmark results?

### Testing

* What breaks this?
* Is that failure tested?
* Is the integration path tested?

A reviewer should be able to understand the implementation without reverse-engineering the entire subsystem.

---

## 15. Definition of Done

A VORTEX change is complete only when all applicable requirements are satisfied:

* [ ] Implementation uses the correct language.
* [ ] Code follows the VORTEX/Google C++ style standards.
* [ ] Ownership and lifetime are explicit.
* [ ] Required subsystems are fully wired.
* [ ] GC integration is complete.
* [ ] Threading behavior is defined.
* [ ] Error paths are handled.
* [ ] Compiler invariants are preserved.
* [ ] Relevant analyses are updated or invalidated.
* [ ] Deoptimization behavior is correct.
* [ ] Reference implementations were consulted where applicable.
* [ ] Performance was benchmarked where applicable.
* [ ] Adversarial tests exist where applicable.
* [ ] Integration tests exist where applicable.
* [ ] Assertions and invariant checks exist where appropriate.
* [ ] No dangling functionality remains.
* [ ] No TODO is required for basic correctness.

---

## Core Principle

> **Clean code is code whose behavior, ownership, invariants, and integration points can be understood without relying on tribal knowledge.**

The Google C++ Style Guide defines how VORTEX code should be structured and presented.

These engineering standards define what that code must **do**.

**Readable code that corrupts the heap is not clean code.
Fast code that violates compiler invariants is not good code.
A correctly implemented feature that is not wired into the runtime is not a finished feature.**

**VORTEX code must be correct, integrated, measurable, and maintainable.**

