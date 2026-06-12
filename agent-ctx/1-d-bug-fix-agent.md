# Task 1-d: Fix P0 bugs in VORTEX JIT instruction selection and register allocation

## Summary

Fixed 3 critical P0 bugs in the VORTEX JIT compiler's instruction selection and register allocation pipeline.

## Bugs Fixed

### Bug 1 (G6): Phi resolution completely missing
- **Location**: `src/lower/isel.c` lines 1185-1188
- **Root cause**: Phi node handler just allocated a vreg but never emitted MOV instructions to connect values from different predecessors
- **Fix**: Added `resolve_phis()` function that emits MOV phi_vreg, input_vreg at the end of each predecessor block (before terminal branch). Called after main isel loop, before branch resolution.

### Bug 2 (G1): Regalloc coalescing indexes compacted array by vreg number  
- **Location**: `src/lower/regalloc.c` lines 276-383 (coalesce_copies) and lines 207-229 (compaction)
- **Root cause**: `compute_live_intervals()` compacted the intervals array before returning it, but `coalesce_copies()` indexed it by vreg number — wrong after compaction
- **Fix**: Removed compaction from `compute_live_intervals()`. Moved compaction to `vtx_regalloc_run()` after coalescing completes.

### Bug 3 (G2): Memory operand resolution hack corrupts displacement
- **Location**: `src/lower/regalloc.c` lines 642-656 (vtx_regalloc_apply) and `src/lower/isel.h` (vtx_x86_memop_t) and `src/lower/emit.c` (emit_single_inst)
- **Root cause**: When base_vreg got a physical register, code overwrote `mem.disp = (int32_t)phys_reg`, destroying the actual displacement
- **Fix**: Added `base_phys` and `index_phys` fields to `vtx_x86_memop_t`. Updated regalloc_apply to store phys regs in proper fields. Updated emitter to use the new fields.

## Files Modified
- `src/lower/isel.h` — added base_phys/index_phys to memop; added VTX_INST_FLAG_PHI_COPY
- `src/lower/isel.c` — added resolve_phis(); updated all memop initializers  
- `src/lower/regalloc.c` — fixed compaction ordering; fixed memop phys reg storage
- `src/lower/emit.c` — updated MOV/LEA emission to use base_phys/index_phys

## Verification
- Build: 0 errors
- Self-tests: 10/10 pass
