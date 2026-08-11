
# VORTEX Best Practices

These are best practices for proposing fixes and general development in V8.

## Fixes

When proposing a fix, ensure the code adheres to VORTEX standards:

- **Simplicity**: Code should be as simple and direct as possible. Favor
  straightforward and maintainable constructs.
- **Readability**: Code must be understandable to future maintainers.
- **Structural Alignment**: Fits naturally into the existing architecture (e.g.,
  AST Visitor pattern, Bytecode generation flow).
- **Controlled Duplication**: Prefer duplication over complex abstractions ONLY
  when it significantly improves clarity.
- **Optimization Preservation (Methodology)**: To maximize performance, follow
  this systematic approach when fixing optimization violations:
  - **Trace Lifecycle**: Understand how optimized state is collected,
    propagated, and consumed (e.g., AST -> Bytecode -> JIT).
  - **Isolate Assumptions**: Identify the specific invariant that was violated.
  - **Surgical Fixes**: Address the violation without restricting valid
    optimized paths.
  - **Flaw over Disabling**: Prioritize finding the specific logic flaw in the
    optimization's implementation to preserve the feature.
- **Cross-Feature Awareness**: Understand how optimizations interact with core
  V8 features like Lazy Compilation, Ignition bytecode generation, and
  TurboFan/Maglev optimizations.
- **Mandatory Reproducer Rule**: Unless explicitly specified otherwise by the
  user, **every single bug fix in V8 MUST be uploaded with a working reproducer
  (regression test)**. Uploading just the fix alone is NEVER acceptable.

## Technical Guardrails & Guidelines

- **Cleanup Constraint**: Keep diffs small by changing only the code intended
  for the task. Propose cleanups to nearby code as a separate patch to keep the
  current change focused.

## General Best Practices

For general best practices regarding formatting, generated files, test
configurations, header names, and forward declarations, refer to Common Pitfalls
& Best Practices section. This rule file focuses on V8-specific fix proposals
and technical guardrails.
