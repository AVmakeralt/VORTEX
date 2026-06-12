# Task 1-c: Fix P0 bugs in VORTEX JIT guard emission and deoptimization system

**Agent:** Bug Fix Agent
**Status:** Completed
**Date:** 2026-03-04

## Summary

Fixed 3 P0 crash bugs in the VORTEX JIT compiler's guard emission and deoptimization system.

## Bugs Fixed

### Bug 1 (G4): Guard patching function is a complete no-op
- **File:** `src/lower/guard_emit.h`, `src/lower/guard_emit.c`
- **Root cause:** `vtx_guard_emit_patch()` just returned 0 without patching anything
- **Fix:**
  - Added `jcc_native_offset` and `deopt_stub_offset` fields to `vtx_guard_desc_t`
  - Added `find_guard_jcc()` helper to locate guard JCC instructions in the instruction stream
  - `vtx_guard_emit_lower` now records JCC native offsets from the instruction stream
  - `vtx_guard_emit_deopt_stubs` now stores stub offsets back in the guard descriptor
  - `vtx_guard_emit_patch` now computes rel32 displacements and patches JCC instructions in the code buffer

### Bug 2 (G5): Deopt stub jumps to address 0
- **File:** `src/lower/guard_emit.h`, `src/lower/guard_emit.c`, `src/runtime_stubs.c`
- **Root cause:** Deopt stub emitted `mov rax, 0; push rax; ret` jumping to NULL
- **Fix:**
  - Added `vtx_deopt_handler_stub()` to runtime_stubs.c (diagnostic abort)
  - Added `vtx_guard_emit_set_deopt_handler()`/`vtx_guard_emit_get_deopt_handler()` API
  - Replaced `mov rax, 0; push rax; ret` with `mov rax, <handler>; jmp rax` (FF E0)
  - Fixed RSI argument to pass `jcc_native_offset` instead of `bytecode_pc`

### Bug 3 (G7): VTX_COND_NEVER makes range-check guard a no-op
- **File:** `src/guard/merge.c`
- **Root cause:** Lower bound guard used `VTX_COND_NEVER`, causing isel to emit a JCC with the negated condition (ALWAYS), guaranteeing deoptimization
- **Fix:** Changed `lg->cond = VTX_COND_NEVER` to `lg->cond = VTX_COND_GE`

## Verification
- Full build completes with 0 errors
- All targets compile successfully
