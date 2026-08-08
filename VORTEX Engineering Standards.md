### VORTEX Engineering Standards
1. Language and Implementation
1.1 New Code
All new implementation code MUST be written in C++.
Rust is permitted only for bindings/integration layers where explicitly approved.
C is permitted only for legacy code, maintenance, or compatibility work.
New C code MUST NOT be introduced merely because an existing component happens to be written in C.
1.2 Modern C++
Use RAII for resource ownership.
Prefer std::unique_ptr and std::shared_ptr where ownership semantics actually require them.
Avoid raw owning pointers.
Prefer references, spans, views, and value types for non-owning access.
Use enum class instead of unscoped enums.
Use nullptr, never NULL.
Use constexpr, const, and noexcept where appropriate.
Prefer compile-time validation over runtime validation when practical.
Avoid unnecessary heap allocation in hot paths.
Avoid exceptions in latency-sensitive runtime/JIT paths unless explicitly justified.
Do not use undefined behavior as an optimization strategy.
1.3 Ownership Must Be Obvious

Every object must have a clear answer to:

Who owns this? Who may mutate it? When does it die?

If that answer cannot be determined by reading the surrounding code, the implementation is not finished.

2. Production-Readiness Rule
Everything MUST Be Treated As Production Code

Code is not considered complete merely because:

it compiles,
a unit test passes,
the feature exists,
the API exists,
or the implementation “works” in isolation.

A feature is complete only when it is fully integrated into the system it belongs to.

This includes:

GC integration
object lifetime management
deoptimization
exception handling
safepoints
write barriers
threading
profiling
invalidation
code generation
serialization/deserialization
diagnostics
error propagation
shutdown/destruction
API registration
runtime registration
build configuration
2.1 No Dangling Features

Do not leave functionality dangling or partially wired.

For example:

class MyObject : public GCObject {
    Value child;
};

is not complete merely because child exists.

If child contains a GC-managed reference, the implementation must also correctly integrate that reference with:

tracing,
barriers,
relocation/forwarding if applicable,
object scanning,
lifetime rules,
and every other mechanism required by the collector.

A missing GC edge is not a minor bug.

It is heap corruption waiting for a sufficiently unlucky workload.

2.2 Integration Checklist

Every new subsystem/component must answer:

Where is it constructed?
Where is it registered?
Who owns it?
Who destroys it?
How does it interact with the GC?
How does it interact with threads?
How does it interact with exceptions?
How does it interact with deoptimization?
How does it interact with profiling?
What happens when initialization fails?
What happens during shutdown?
What happens under malformed input?
What happens when assumptions become invalid?

If any answer is “it doesn't,” that must be intentional and documented.

3. Implementation Logic Must Be Verified

VORTEX implementations MUST NOT be judged solely against their intended algorithm.

For significant compiler/runtime components, verify the implementation against established production systems.

Primary References
V8
HotSpot
GraalVM
PyPy

These are not necessarily implementation templates. They are sanity checks against decades of compiler engineering experience.

For relevant subsystems, compare:

algorithmic structure
optimization opportunities
invalidation behavior
deoptimization behavior
memory management
threading
representation choices
edge cases
correctness invariants
performance characteristics

If VORTEX behaves differently from these systems, the difference should be explainable.

“We did it differently” is not a justification.

The justification is why the difference is correct and beneficial.

4. Performance Validation

A performance-sensitive implementation MUST be benchmarked.

Do not optimize based on intuition.

For hot code:

Establish a baseline.
Implement the change.
Benchmark representative workloads.
Profile the generated code.
Identify regressions.
Compare against appropriate reference implementations.

For JIT/compiler work, comparisons should include appropriate combinations of:

VORTEX interpreter
VORTEX baseline JIT
VORTEX optimized tiers
native C/C++ where applicable
V8
HotSpot
GraalVM
PyPy where applicable
4.1 Performance Regression Rule

A change that makes a hot path slower MUST either:

be rejected,
be optimized further,
or include a documented reason why the regression is necessary.

Do not merge:

“It's only 8% slower.”

That sentence has murdered many perfectly good runtimes.

5. Tests Must Be Difficult
Tests MUST attempt to break the implementation.

Do not write tests merely demonstrating that the happy path works.

Tests should target:

malformed input
pathological input
integer overflow
unusual type combinations
empty structures
extremely large structures
deep recursion
recursion limits
invalid assumptions
GC pressure
allocation pressure
thread races
deoptimization
invalidation
exceptions
interrupted execution
repeated recompilation
tier transitions
unusual control flow
aliasing
speculative optimization failures
5.1 Tests Must Exercise Integration

A component test is insufficient when the feature interacts with the runtime.

For example:

allocate object
    ↓
store GC reference
    ↓
trigger collection
    ↓
move object
    ↓
continue execution
    ↓
read reference

That test is considerably more valuable than:

object.child == expected_child

because computers apparently enjoy behaving correctly until the garbage collector gets involved.

6. Compiler Correctness

Every optimization MUST preserve observable program behavior.

For an optimization:

Before:
A → B → C

After:
A → C

there must be a demonstrated reason that removing B cannot change:

program results,
exceptions,
memory effects,
observable ordering,
GC behavior,
deoptimization state,
synchronization,
or other runtime-visible behavior.
6.1 Speculation

Speculative optimizations MUST have:

an explicit assumption,
a mechanism for detecting assumption failure,
a recovery path,
and tests covering assumption failure.

Never write:

if (likely(is_integer(value))) {
    // assume integer forever
}

without answering what happens when that assumption stops being true.

7. IR Standards

Every IR transformation should preserve documented invariants.

Passes MUST clearly define:

Input invariants
Output invariants
Required analyses
Invalidated analyses
Required metadata

If a pass invalidates:

DominatorTree
AliasAnalysis
LoopAnalysis
TypeInformation

then that invalidation must be explicitly represented.

Do not silently leave stale analysis results lying around.

8. GC Standards

GC integration gets its own section because heap corruption is not a personality trait.

Every GC-managed reference MUST have a defined tracing/lifetime policy.

New object types must explicitly specify:

fields containing GC references,
tracing behavior,
write-barrier requirements,
allocation mechanism,
finalization behavior if applicable,
relocation behavior,
interaction with weak references,
interaction with thread-local roots,
interaction with JIT-generated references.
8.1 GC Safety Rule

If an object can contain a GC reference, the GC must know about it.

No exceptions without an explicit, documented reason.

8.2 JIT/GC Interaction

Generated code must correctly handle:

safepoints,
stack maps,
GC roots,
relocated objects,
barriers,
deoptimization metadata.

A JIT optimization that is mathematically correct but causes the collector to miss a live object is incorrect.

9. Threading

Shared mutable state MUST have explicit synchronization semantics.

Every shared object should make clear whether it is:

thread-local,
immutable,
externally synchronized,
internally synchronized,
lock-free,
atomic,
or otherwise concurrency-safe.

Do not introduce atomics simply because they make the compiler stop complaining.

Do not introduce locks into hot paths without measuring their cost.

9.1 Race Testing

Concurrent components should be tested under:

ThreadSanitizer
stress workloads
repeated scheduling
high contention
shutdown races
compilation/execution races
10. Error Handling

Errors MUST propagate correctly.

Do not:

bool compile(...) {
    if (!something()) {
        return false;
    }

    // continue anyway
}

unless ignoring the failure is explicitly correct.

Errors must not silently become:

corrupted state,
invalid IR,
stale metadata,
leaked resources,
invalid machine code,
or undefined behavior.
11. Assertions and Invariants

Use assertions aggressively for internal invariants.

assert(block->isLinked());
assert(value->type() != Type::Invalid);
assert(state.isConsistent());

Assertions should catch programmer errors as close to their origin as possible.

But:

Assertions MUST NOT be the only defense against malformed or hostile runtime input.

Internal invariant:

assert(index < table.size());

External input:

if (index >= table.size()) {
    return Error::InvalidIndex;
}

Different problems require different defenses.

12. Code Review Standards

Every non-trivial change should answer:

Correctness
What invariant does this rely on?
What invariant does it establish?
What happens when its assumptions fail?
Integration
What systems does this touch?
What systems need to know this exists?
Did every required registration/wiring point get updated?
Memory
Who owns the data?
Can it move?
Can it be collected?
Can another thread access it?
Compiler
Does it affect IR correctness?
Does it invalidate analysis?
Does it affect deoptimization?
Performance
Is this on a hot path?
What does profiling say?
What is the benchmark result?
Testing
What breaks this?
Is the failure tested?
Is the integration path tested?
13. Definition of Done

A VORTEX change is not complete until all applicable conditions are satisfied:

[ ] Implementation is in the correct language.
[ ] Ownership is explicit.
[ ] Lifetime is correct.
[ ] All required subsystems are wired.
[ ] GC integration is complete.
[ ] Threading behavior is defined.
[ ] Error paths are handled.
[ ] Compiler invariants are preserved.
[ ] Relevant analyses are updated/invalidated.
[ ] Deoptimization behavior is correct.
[ ] Production reference implementations were consulted.
[ ] Performance was benchmarked where applicable.
[ ] Adversarial tests exist.
[ ] Integration tests exist.
[ ] Debug/assertion checks exist where appropriate.
[ ] No known dangling functionality remains.
[ ] No known TODO is required for basic correctness.
The Core Rule

I'd put this at the very top of the document in giant letters:

VORTEX code must be written as if it will be deployed to production immediately.

A feature is not complete when its implementation exists.
It is complete when it is correctly integrated, tested, observable, maintainable, and safe under the runtime conditions it will encounter.

Do not leave functionality dangling, unwired, or dependent on future integration.

Missing GC registration, stale metadata, incorrect deoptimization state, broken ownership, missing barriers, or incomplete runtime wiring are correctness failures, not cleanup tasks.

VORTEX C++ Code Style Standards

VORTEX follows the Google C++ Style Guide as the baseline standard for formatting, naming, structure, and general C++ practices.

Google C++ Style Guide

VORTEX-specific requirements take precedence where they differ from Google's recommendations.

1. Formatting

All C++ code MUST follow Google's formatting conventions.

Use clang-format with the repository's approved configuration.
Do not manually format code differently from the project standard.
Indentation, brace placement, spacing, line wrapping, and declaration formatting MUST be consistent.
Avoid excessively long functions or deeply nested control flow.
Do not introduce formatting-only changes into unrelated files.

Formatting should be automatically enforceable wherever practical.

2. Naming

Follow Google's naming conventions.

Types

Use PascalCase:

class GarbageCollector;
struct MachineState;
enum class CompilationTier;
Functions

Use PascalCase:

CompileFunction();
CollectGarbage();
LowerInstruction();
Variables

Use snake_case:

auto bytecode_offset = 42;
auto compilation_state = GetState();
Constants

Use kPascalCase:

constexpr int kMaxInlineDepth = 32;
constexpr size_t kPageSize = 4096;
Class Members

Use snake_case_:

class Compiler {
 private:
    IRGraph* graph_;
    CompilationTier tier_;
};
Names Must Describe Meaning

Prefer:

auto bytecode_offset = GetOffset();

over:

auto x = GetOffset();

Prefer:

bool is_deoptimized = state.IsDeoptimized();

over:

bool flag = state.IsDeoptimized();

Names should make code understandable without requiring the reader to inspect the implementation of every function.

3. Classes

Classes should have a clear responsibility.

Avoid giant “god classes” that simultaneously handle:

parsing,
IR construction,
optimization,
code generation,
memory management,
and runtime execution.

If a class has too many unrelated responsibilities, split it.

Public Interface First

Follow Google's convention of generally placing the public interface before implementation details:

class Compiler {
 public:
    Compiler(Context* context);

    CompilationResult Compile(const Function& function);

 private:
    void Optimize();
    void Lower();

    Context* context_;
};
4. Functions

Functions should do one coherent thing.

Prefer:

IRGraph* BuildGraph(const Bytecode& bytecode);
void RunOptimizationPasses(IRGraph* graph);
MachineCode GenerateMachineCode(IRGraph* graph);

over a 900-line function called:

CompileEverythingAndHopeNothingExplodes();
Function Length

There is no arbitrary hard line limit, but excessively large functions MUST be treated as a code-review warning.

A function should be split when:

it performs multiple logically distinct operations,
control flow becomes difficult to follow,
error handling becomes tangled,
local state becomes difficult to reason about,
or understanding one part requires mentally tracking unrelated parts.
5. Comments

Comments should explain why, not merely repeat what the code does.

Bad:

// Increment i.
++i;

Good:

// Keep this index stable because the deoptimizer stores the bytecode
// offset before entering this loop.
++bytecode_index;

Comments MUST NOT be used to justify incorrect or unnecessarily complicated code.

If something is surprising because of a compiler, GC, ABI, or hardware constraint, document the reason.

6. Header Hygiene

Headers are expensive dependencies in a large C++ codebase.

Prefer forward declarations when appropriate:

class GarbageCollector;
class ThreadState;

over unnecessarily including entire headers.

Include what you use.

Do not rely on transitive includes.

A file should generally compile correctly based on its own explicit dependencies.

7. Ownership

Ownership MUST be explicit.

Prefer:

std::unique_ptr<IRGraph> graph;

when a single owner exists.

Use raw pointers for non-owning references where appropriate:

IRNode* parent_;

The ownership semantics must be obvious from the API.

Do not use:

void* data;

as a substitute for designing an actual type.

8. Avoid Clever C++

Code should optimize for maintainability and correctness, not how impressive it looks in a code review.

Prefer:

if (node->IsConstant()) {
    return FoldConstant(node);
}

over an unnecessarily clever template/metaprogramming construction that requires three PhDs and a blood sacrifice to understand.

Use advanced C++ features when they provide a real benefit.

Do not use them merely because they exist.

9. Explicit Control Flow

Prefer straightforward control flow.

Good:

if (!IsValid(node)) {
    return Error::InvalidNode;
}

if (!CanOptimize(node)) {
    return KeepNode(node);
}

return Optimize(node);

Avoid deeply nested structures:

if (valid) {
    if (optimized) {
        if (has_type) {
            if (reachable) {
                ...
            }
        }
    }
}

Use early returns where they make the logic clearer.

10. Type Safety

Prefer strong types over primitive values when values have different meanings.

Avoid:

void Compile(int offset, int size, int tier);

when those values represent fundamentally different concepts.

Prefer dedicated types where appropriate:

void Compile(BytecodeOffset offset,
             BytecodeSize size,
             CompilationTier tier);

The compiler should prevent invalid states whenever practical.

11. const Correctness

Use const consistently.

Prefer:

const IRNode* node;

when mutation is not required.

Member functions that do not modify object state should be const:

bool IsConstant() const;

Do not remove const merely because it is inconvenient.

12. C++ Features

Prefer modern C++ facilities over C-style constructs.

Use:

nullptr

not:

NULL

Use:

enum class

rather than unscoped enums.

Use:

std::array
std::vector
std::string
std::span
std::unique_ptr

where appropriate rather than reinventing equivalent structures.

C-style casts are prohibited in new code.

Prefer:

static_cast<uint32_t>(value)

over:

(uint32_t)value
13. Error Handling

Errors should be explicit and predictable.

Do not silently ignore failures.

Avoid APIs where the caller cannot determine whether an operation succeeded.

For operations where failure is expected, use the project's standard result/error type rather than relying on obscure side effects.

Every failure path must leave the system in a valid state.

14. Dead Code

Do not leave commented-out implementations in the codebase.

Bad:

// Old implementation.
// auto result = DoSomethingOld();
// ...

Use version control.

Likewise, unused:

variables,
functions,
parameters,
classes,
fields,
includes,
compatibility hacks

should be removed unless there is a documented reason for their existence.

15. TODOs

TODOs MUST NOT be used to hide incomplete production functionality.

Bad:

// TODO: connect this to the GC.

if the feature is already being merged.

If GC integration is required for correctness, the implementation is not finished until it is connected to the GC.

TODOs may be used for genuinely non-blocking future improvements, preferably with enough context to explain what remains and why.

16. Code Review Standard

A reviewer should be able to answer:

Can I understand what this code does without reverse-engineering the entire subsystem?

If not, the code needs improvement.

Reviewers should specifically look for:

unclear ownership
excessive complexity
unnecessary abstraction
duplicated logic
hidden state
misleading names
stale comments
unsafe casts
missing error handling
missing integration
incomplete lifecycle handling
unnecessary allocations
accidental hot-path costs
VORTEX Rule of Clean Code

Clean code is code whose behavior, ownership, invariants, and integration points can be understood without relying on tribal knowledge.

Google's style guide gives us the syntax and conventions.

VORTEX's engineering standards define the correctness bar.
