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
#include "codecache/types.h"
#include "codecache/install.h"

/* vtx_code_cache_t and vtx_method_registry_t are defined via codecache/install.h
 * (which includes codecache/cache.h). No forward declarations needed. */

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
/* Polymorphic inline cache (per call site)                                    */
/* ========================================================================== */

#define VTX_POLY_IC_SIZE 4  /* 4-entry inline cache */

/**
 * Polymorphic inline cache for virtual/interface dispatch.
 *
 * Each CALL_VIRTUAL / CALL_INTERFACE site gets its own IC.
 * The emitted machine code probes the type_ids array; on a hit,
 * it calls the corresponding target directly.  On a miss, it
 * falls through to the runtime helper which updates the IC.
 *
 * Layout (designed for cache-friendly access from emitted code):
 *   offset  0: type_ids[0..3]  — 4 × uint32_t = 16 bytes, contiguous
 *   offset 16: targets[0..3]   — 4 × void*    = 32 bytes, contiguous
 *   offset 48: count            — uint32_t
 *   offset 52: misses           — uint32_t
 */
typedef struct {
    uint32_t type_ids[VTX_POLY_IC_SIZE];   /* cached type IDs */
    void    *targets[VTX_POLY_IC_SIZE];     /* cached code targets (method desc pointers) */
    uint32_t count;                          /* number of valid entries */
    uint32_t misses;                         /* miss counter for IC training */
} vtx_poly_ic_t;

/* Offsets into vtx_poly_ic_t for emitted code */
#define VTX_POLY_IC_TYPE_IDS_OFFSET   0
#define VTX_POLY_IC_TARGETS_OFFSET    16
#define VTX_POLY_IC_COUNT_OFFSET      48
#define VTX_POLY_IC_ENTRY_SIZE_TYPE   4   /* sizeof(uint32_t) */
#define VTX_POLY_IC_ENTRY_SIZE_TARGET 8   /* sizeof(void*) */

/* ========================================================================== */
/* Compiled code result (defined in codecache/types.h)                        */
/* ========================================================================== */

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
 * If both cache and registry are non-NULL, the compiled code is installed
 * into the code cache via vtx_install_method() which handles allocation,
 * mprotect, and atomic code pointer updates. Otherwise, the code is
 * malloc'd and the caller must handle installation.
 *
 * @param method       The method to compile
 * @param profile_data Profile data for this method (may be NULL for first compilation)
 * @param arena        Arena for temporary allocations during compilation
 * @param cache        Code cache for proper code installation (may be NULL)
 * @param registry     Method registry for code installation (may be NULL)
 * @return             Compiled code struct, or NULL on failure
 */
vtx_compiled_code_t *vtx_baseline_compile(const vtx_method_desc_t *method,
                                           vtx_profile_data_t *profile_data,
                                           vtx_arena_t *arena,
                                           vtx_code_cache_t *cache,
                                           vtx_method_registry_t *registry);

#endif /* VORTEX_BASELINE_CODEGEN_H */
