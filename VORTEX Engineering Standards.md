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
