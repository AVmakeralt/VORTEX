# Development Standards

These standards apply to all contributors.

## 1. Language Policy

- All new core development **must** be written in **C++**.
- **Rust is reserved for bindings, tooling, and FFI layers only.**
- **C is legacy-only.** Existing C code may be maintained or bug-fixed, but new core features must not be implemented in C.
- Introducing another language requires maintainer approval.

---

## 2. Benchmark Against Industry

Before implementing an optimization or compiler feature:

- Study how **HotSpot**, **V8**, or other mature production JITs solve the problem.
- Understand *why* their design exists.
- If your design performs worse, is less maintainable, or lacks justification, revise it before merging.
- Reinventing the wheel is acceptable only when there is measurable improvement.

---

## 3. Tests Should Be Difficult

Tests should attempt to break the implementation.

Every feature should include:

- Normal cases
- Edge cases
- Invalid input
- Stress tests
- Randomized/fuzz tests where appropriate

Avoid writing tests that only prove the happy path works.

---

## 4. Measure Before Optimizing

Never assume code is slow.

Use profiling data before making optimization decisions.

If performance is cited as the reason for a change, benchmarks should accompany the pull request.

---

## 5. Keep the Fast Path Simple

Avoid adding complexity to common execution paths.

Small regressions in hot code matter more than improvements in rare code.

---

## 6. Correctness First

A fast incorrect compiler is still a broken compiler.

Every optimization must preserve correctness.

When in doubt, choose correctness over speed.

---

## 7. Document Non-Obvious Decisions

If an implementation is unusual, clever, or relies on subtle invariants:

- Explain why it exists.
- Explain why simpler alternatives were rejected.
- Document assumptions.

Future contributors should not need to reverse-engineer intent.

---

## 8. No Magic Numbers

Named constants, enums, or configuration values are preferred over unexplained literals.

---

## 9. Keep PRs Focused

One pull request should solve one logical problem.

Avoid mixing:
- Refactoring
- Bug fixes
- New features
- Formatting changes

unless absolutely necessary.

---

## 10. Every Optimization Must Be Reversible

Optimizations should be isolated and easy to disable.

If debugging becomes impossible because of an optimization, the optimization is too invasive.

---

## 11. Readability Matters

Code is read far more often than it is written.

Prefer clear implementations over clever ones unless measurable performance requires otherwise.

---

## 12. Don't Guess

If you're unsure:

- Read the specification.
- Read the implementation.
- Read existing compiler literature.
- Ask.

Assumptions create bugs.

---

## 13. CI Must Stay Green

Do not merge code that breaks:
- compilation
- tests
- benchmarks (without justification)

Red CI is not "someone else's problem."

---

## 14. Leave the Code Better

When modifying code:

- remove dead code
- improve comments
- simplify logic where possible
- avoid increasing technical debt

The project should improve with every commit.

## Reference Implementations

Before implementing major compiler or JIT features, contributors are encouraged to study existing production runtimes.

These projects represent years of engineering and real-world optimization:

- **V8 (Google)**
  - https://github.com/v8/v8
  - JavaScript engine used by Chrome and Node.js.

- **OpenJDK HotSpot**
  - https://github.com/openjdk/jdk
  - One of the world's most mature optimizing JIT compilers.

- **GraalVM**
  - https://github.com/oracle/graal
  - Modern optimizing compiler with advanced partial evaluation and polyglot support.

- **PyPy**
  - https://github.com/pypy/pypy
  - Meta-tracing JIT demonstrating alternative optimization strategies.

 **luaJIT** 
 needs no introduction
    https://github.com/LuaJIT/LuaJIT

Studying these implementations before designing new features is strongly encouraged.

Do not copy code directly. Instead, understand the design decisions, tradeoffs, and algorithms that make these systems successful.
