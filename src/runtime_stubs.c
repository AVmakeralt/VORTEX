#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"

/* Stubs for baseline JIT runtime functions not used in T0 interpreter benchmark.
 * These are only called from JIT-compiled code (T1+), not from the interpreter. */

static vtx_gc_t *the_gc = NULL;

vtx_gc_t *vtx_get_current_gc(void) { return the_gc; }
void vtx_set_current_gc(vtx_gc_t *gc) { the_gc = gc; }

vtx_typeid_t vtx_runtime_typeof(vtx_value_t v) { (void)v; return 0; }
void vtx_runtime_call_static(const void *m, ...) { (void)m; }
void vtx_runtime_call_virtual(uint32_t x, const void *m, ...) { (void)x; (void)m; }
void vtx_runtime_call_interface(uint32_t x, const void *m, ...) { (void)x; (void)m; }
void vtx_runtime_monitor_enter(vtx_value_t obj) { (void)obj; }
void vtx_runtime_monitor_exit(vtx_value_t obj) { (void)obj; }
void vtx_runtime_throw(vtx_value_t exc) { (void)exc; abort(); }

/* ========================================================================== */
/* Deoptimization handler stub                                                 */
/* ========================================================================== */

/**
 * Default deopt handler — called from JIT-compiled code when a guard fails.
 *
 * This is the fallback handler used when no custom deopt handler has been
 * registered via vtx_guard_emit_set_deopt_handler(). It prints diagnostic
 * information about the guard failure and terminates the process.
 *
 * Calling convention (System V AMD64 ABI):
 *   RDI = frame_state_index  (which FrameState to use for reconstitution)
 *   RSI = native_pc_offset   (where in the compiled code the failure occurred)
 *
 * A full implementation would:
 *   1. Use the frame_state_index to look up the FrameState in the side table
 *   2. Use the register map to reconstruct interpreter values from machine registers
 *   3. Resume execution in the interpreter at the corresponding bytecode PC
 *
 * For now, this stub provides a deterministic crash with diagnostics instead
 * of jumping to address 0 (which was the previous behavior).
 */
void vtx_deopt_handler_stub(uint32_t frame_state_index, uint32_t native_pc)
{
    fprintf(stderr,
            "VORTEX: deoptimization triggered at native_pc=%u, "
            "frame_state_index=%u\n",
            native_pc, frame_state_index);
    fprintf(stderr,
            "VORTEX: No deopt handler registered — cannot resume interpretation.\n");
    abort();
}
