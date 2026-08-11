#ifndef VORTEX_OSR_TEST_SETUP_H
#define VORTEX_OSR_TEST_SETUP_H

/*
 * Shared setup helpers for the OSR Cluster A regression tests.
 *
 * The tests exercise vtx_osr_up's failure-path logic (NULL checks,
 * field-validation gates, stack-depth verification) without needing
 * to actually compile real JIT code. We construct synthetic
 * vtx_compiled_code_t / vtx_interp_frame_t structs, point the OSR
 * entry at a deliberately-invalid trap address, and verify that
 * vtx_osr_up returns instead of jumping.
 *
 * If vtx_osr_up fails to gate a bad input, it will reach the asm
 * trampoline and jump to the trap address, crashing the test
 * process — which is exactly the failure mode we want to catch.
 */

#include "test_framework.h"
#include "vortex_config.h"
#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "deopt/osr.h"
#include "codecache/types.h"
#include "baseline/frame_layout.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * A fake OSR entry "target" that we never want to reach.
 * If vtx_osr_up jumps here, the test process crashes loudly,
 * indicating the gate under test did not refuse the bad input.
 */
#define VTX_OSR_TRAP_TARGET ((void *)0xdeadbeef)

/*
 * Build a vtx_compiled_code_t with the fields vtx_osr_up actually
 * inspects before the asm jump. The caller can mutate the returned
 * struct to inject the bug-specific bad value (e.g., set
 * has_inlined_frames = true for OSR-16).
 *
 * The frame_layout is computed from a real method descriptor so the
 * asm wouldn't crash on memory access if it ever reached the trampoline
 * (it would crash on the trap-target jump instead, which is the
 * intended loud failure).
 */
static inline void vtx_osr_test_make_cc(vtx_compiled_code_t *cc,
                                         const vtx_method_desc_t *method,
                                         uint8_t *code_ptr,
                                         uint32_t code_size)
{
    memset(cc, 0, sizeof(*cc));
    cc->entry_point = VTX_OSR_TRAP_TARGET;
    cc->code = code_ptr;
    cc->code_size = code_size;
    cc->frame_layout = vtx_frame_layout_compute(method);
    cc->method_id = method->vtable_index;
    cc->stack_slots = cc->frame_layout.max_stack;
    cc->local_slots = cc->frame_layout.max_locals;
    cc->has_inlined_frames = false;
    cc->entry_register_convention = VTX_OSR_CONV_DEFAULT;
    cc->profile_data = NULL;
    cc->method = method;
}

/*
 * Build a vtx_interp_frame_t describing an interpreter paused at a
 * loop header with no values on the operand stack.
 */
static inline void vtx_osr_test_make_frame(vtx_interp_frame_t *frame,
                                             const vtx_method_desc_t *method,
                                             uint32_t loop_header_pc,
                                             vtx_value_t *locals_buf,
                                             uint32_t local_count)
{
    memset(frame, 0, sizeof(*frame));
    frame->method_id = method->vtable_index;
    frame->bytecode_pc = loop_header_pc;
    frame->locals = locals_buf;
    frame->local_count = local_count;
    frame->stack = NULL;
    frame->stack_top = 0;
    frame->stack_capacity = 0;
}

#endif /* VORTEX_OSR_TEST_SETUP_H */
