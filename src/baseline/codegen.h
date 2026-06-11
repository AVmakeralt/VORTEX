#ifndef VORTEX_BASELINE_CODEGEN_H
#define VORTEX_BASELINE_CODEGEN_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/arena.h"
#include "runtime/gc.h"
#include "deopt/side_table.h"
#include "interp/profiler.h"
#include "baseline/frame_layout.h"
#include "baseline/guards.h"
#include "baseline/deopt_stubs.h"
#include "baseline/instrument.h"

/**
 * VORTEX Baseline JIT Code Generator
 *
 * Single-pass bytecode → x86-64 machine code translation.
 * Each bytecode maps to a sequence of x86-64 instructions.
 *
 * Register allocation (fixed mapping, no register allocator):
 *   Expression stack top (TOS) held in registers:
 *     TOS   → RAX  (accumulator, also used for return values)
 *     TOS-1 → RCX
 *     TOS-2 → RDX
 *     TOS-3 → RBX
 *   Values beyond TOS-3 are spilled to the stack frame.
 *
 *   Frame pointer:  RBP
 *   Stack pointer:  RSP
 *
 *   Argument passing (System V AMD64 ABI):
 *     1st arg → RDI
 *     2nd arg → RSI
 *     3rd arg → RDX
 *     4th arg → RCX
 *     5th arg → R8
 *     6th arg → R9
 *
 *   Callee-saved: RBX, R12-R15, RBP
 *   Caller-saved: RAX, RCX, RDX, RSI, RDI, R8-R11
 *
 * Note: Since we use RBX for the expression stack, we must save/restore
 * it in the prologue/epilogue. R12-R15 are available as scratch registers.
 */

/* ========================================================================== */
/* Code buffer                                                                 */
/* ========================================================================== */

#define VTX_CODE_BUFFER_INITIAL_CAPACITY 4096

struct vtx_code_buffer {
    uint8_t  *bytes;      /* dynamically allocated code buffer */
    uint32_t  position;   /* current write position (bytes emitted so far) */
    uint32_t  capacity;   /* allocated capacity in bytes */
};

/**
 * Initialize a code buffer with the given initial capacity.
 * Returns 0 on success, -1 on failure.
 */
int vtx_code_buffer_init(vtx_code_buffer_t *buf, uint32_t initial_capacity);

/**
 * Destroy a code buffer and free its memory.
 */
void vtx_code_buffer_destroy(vtx_code_buffer_t *buf);

/**
 * Ensure the buffer has at least `needed` bytes of free space.
 * Reallocates if necessary.
 * Returns 0 on success, -1 on failure.
 */
int vtx_code_buffer_ensure_capacity(vtx_code_buffer_t *buf, uint32_t needed);

/**
 * Get the current write position (number of bytes emitted).
 */
static inline uint32_t vtx_code_buffer_position(const vtx_code_buffer_t *buf)
{
    return buf->position;
}

/**
 * Emit a single byte into the code buffer.
 */
static inline void vtx_code_buffer_emit_byte(vtx_code_buffer_t *buf, uint8_t b)
{
    VTX_ASSERT(buf->position < buf->capacity, "code buffer overflow");
    buf->bytes[buf->position++] = b;
}

/**
 * Emit a 16-bit word (little-endian) into the code buffer.
 */
static inline void vtx_code_buffer_emit_word(vtx_code_buffer_t *buf, uint16_t w)
{
    vtx_code_buffer_emit_byte(buf, (uint8_t)(w & 0xFF));
    vtx_code_buffer_emit_byte(buf, (uint8_t)((w >> 8) & 0xFF));
}

/**
 * Emit a 32-bit dword (little-endian) into the code buffer.
 */
static inline void vtx_code_buffer_emit_dword(vtx_code_buffer_t *buf, uint32_t d)
{
    vtx_code_buffer_emit_byte(buf, (uint8_t)(d & 0xFF));
    vtx_code_buffer_emit_byte(buf, (uint8_t)((d >> 8) & 0xFF));
    vtx_code_buffer_emit_byte(buf, (uint8_t)((d >> 16) & 0xFF));
    vtx_code_buffer_emit_byte(buf, (uint8_t)((d >> 24) & 0xFF));
}

/**
 * Emit a 64-bit qword (little-endian) into the code buffer.
 */
static inline void vtx_code_buffer_emit_qword(vtx_code_buffer_t *buf, uint64_t q)
{
    vtx_code_buffer_emit_dword(buf, (uint32_t)(q & 0xFFFFFFFF));
    vtx_code_buffer_emit_dword(buf, (uint32_t)((q >> 32) & 0xFFFFFFFF));
}

/**
 * Emit an array of bytes into the code buffer.
 */
static inline void vtx_code_buffer_emit_bytes(vtx_code_buffer_t *buf,
                                               const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        vtx_code_buffer_emit_byte(buf, data[i]);
    }
}

/**
 * Patch a 32-bit value at the given position in the code buffer.
 * Used for fixing up branch targets and call displacements.
 */
static inline void vtx_code_buffer_patch_dword(vtx_code_buffer_t *buf,
                                                 uint32_t pos, uint32_t value)
{
    VTX_ASSERT(pos + 4 <= buf->position, "patch position out of bounds");
    buf->bytes[pos]     = (uint8_t)(value & 0xFF);
    buf->bytes[pos + 1] = (uint8_t)((value >> 8) & 0xFF);
    buf->bytes[pos + 2] = (uint8_t)((value >> 16) & 0xFF);
    buf->bytes[pos + 3] = (uint8_t)((value >> 24) & 0xFF);
}

/* ========================================================================== */
/* Branch fixup record                                                         */
/* ========================================================================== */

/**
 * Records a forward branch that needs to be patched once the target
 * position is known. Used for GOTO, IF_TRUE, IF_FALSE targets.
 */
typedef struct {
    uint32_t patch_position;  /* position of the 32-bit displacement to patch */
    uint32_t source_offset;   /* native code offset of the branch instruction */
    uint16_t target_bc_pc;    /* bytecode PC of the branch target */
    bool     is_32bit;        /* true if 32-bit displacement, false if 8-bit */
} vtx_branch_fixup_t;

/* ========================================================================== */
/* Bytecode-to-native PC mapping entry                                         */
/* ========================================================================== */

typedef struct {
    uint32_t bytecode_pc;    /* bytecode PC */
    uint32_t native_offset;  /* corresponding native code offset */
} vtx_bc_pc_map_entry_t;

/* ========================================================================== */
/* Compiled code result                                                        */
/* ========================================================================== */

/**
 * The result of baseline compilation: contains the generated native code
 * and all associated metadata.
 */
typedef struct {
    uint8_t              *code;           /* executable native code (malloc'd) */
    uint32_t              code_size;      /* size of native code in bytes */

    vtx_jit_frame_layout_t frame_layout;  /* frame layout for this method */
    vtx_guard_array_t     guards;         /* emitted guards */
    vtx_deopt_stub_array_t deopt_stubs;   /* generated deopt stubs */
    vtx_side_table_t      *side_table;    /* deopt side table */
    vtx_deopt_info_t      *deopt_info;    /* deopt info for the interpreter */

    /* Bytecode PC → native offset mapping for debugging and deopt */
    vtx_bc_pc_map_entry_t *bc_pc_map;     /* sorted by bytecode_pc */
    uint32_t               bc_pc_map_count;

    /* Native offset → bytecode PC mapping (for deopt) */
    uint32_t              *native_to_bc_pc; /* indexed by native offset / 8 */
    uint32_t               native_to_bc_pc_count;

    /* Method identity */
    const vtx_method_desc_t *method;      /* the compiled method */
} vtx_compiled_code_t;

/**
 * Destroy a compiled code struct and free all associated memory.
 * Does NOT free the code buffer itself (that's managed by the code cache).
 */
void vtx_compiled_code_destroy(vtx_compiled_code_t *code);

/* ========================================================================== */
/* Compilation entry point                                                     */
/* ========================================================================== */

/**
 * Compile a method using the baseline JIT.
 *
 * Performs a single pass over the bytecode, emitting x86-64 machine code
 * for each instruction. Guards are emitted for potentially speculative
 * operations. Profiling instrumentation is inserted at key points.
 * Deopt stubs are generated after the main code.
 *
 * @param method       The method to compile
 * @param profile_data Profile data for this method (may be NULL for first compilation)
 * @param arena        Arena for temporary allocations during compilation
 * @return             Compiled code struct, or NULL on failure
 */
vtx_compiled_code_t *vtx_baseline_compile(const vtx_method_desc_t *method,
                                           vtx_profile_data_t *profile_data,
                                           vtx_arena_t *arena);

#endif /* VORTEX_BASELINE_CODEGEN_H */
