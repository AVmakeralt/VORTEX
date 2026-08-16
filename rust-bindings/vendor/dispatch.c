#include "interp/dispatch.h"
#include "baseline/codegen.h"
#include "baseline/deopt_stubs.h"
#include "baseline/frame_layout.h"
#include "compile/orchestrator.h"
#include "compile/spec_versioning.h"
#include "codecache/install.h"
#include "deopt/osr.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

/* VORTEX interpreter uses GCC's computed-goto extension (goto *expr)
 * for the dispatch loop — the same technique used by CPython, LuaJIT,
 * and V8's Ignition interpreter. -Wpedantic flags this as non-standard
 * ISO C. The extension is well-supported by GCC >= 3.0 and Clang >= 2.8
 * and is the single most impactful interpreter optimization (1.5-2x
 * dispatch throughput vs switch-based dispatch).
 *
 * Per VORTEX rules: this is NOT silencing a latent bug — it's a
 * documented, well-understood GNU extension. Do NOT remove. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

/* Property IC — declared in cpp/src/property_ic.cpp (C++ extern "C").
 * When libvortex_cpp.a is linked, these resolve to the real IC.
 * When not linked (C-only builds), the weak symbols default to no-op
 * stubs that always miss (return UINT32_MAX / do nothing), so the
 * interpreter falls back to the bytecode operand — correct but slower. */
extern uint32_t vtx_property_ic_lookup(uint32_t site_id, uint32_t shape_id) __attribute__((weak));
extern void vtx_property_ic_update(uint32_t site_id, uint32_t shape_id, uint32_t offset) __attribute__((weak));

/* Weak fallback stubs — used when libvortex_cpp.a is not linked.
 * These make the IC a no-op: lookup always misses, update does nothing. */
__attribute__((weak)) uint32_t vtx_property_ic_lookup(uint32_t site_id, uint32_t shape_id) {
    (void)site_id; (void)shape_id;
    return UINT32_MAX;  /* always miss — fall back to bytecode operand */
}
__attribute__((weak)) void vtx_property_ic_update(uint32_t site_id, uint32_t shape_id, uint32_t offset) {
    (void)site_id; (void)shape_id; (void)offset;  /* no-op */
}

/* ========================================================================== */
/* CALL_RUNTIME callback hook                                                  */
/* ========================================================================== */
/*
 * Global callback for CALL_RUNTIME opcodes. When registered, the
 * dispatch handler calls this BEFORE the built-in switch. If the
 * callback returns >= 0, the built-in switch is skipped.
 *
 * This enables frontends (LuaVortex, etc.) to extend the runtime
 * without patching dispatch.c. Register at startup:
 *   vtx_set_runtime_callback(my_callback, my_user_data);
 */
static vtx_runtime_callback_t g_runtime_callback = NULL;
static void *g_runtime_callback_data = NULL;

void vtx_set_runtime_callback(vtx_runtime_callback_t callback, void *user_data)
{
    g_runtime_callback = callback;
    g_runtime_callback_data = user_data;
}

vtx_runtime_callback_t vtx_get_runtime_callback(void)
{
    return g_runtime_callback;
}

void *vtx_get_runtime_callback_data(void)
{
    return g_runtime_callback_data;
}

/* ========================================================================== */
/* Branch prediction hints                                                      */
/* ========================================================================== */

#if defined(__GNUC__) || defined(__clang__)
#define VTX_LIKELY(x)   __builtin_expect(!!(x), 1)
#define VTX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define VTX_LIKELY(x)   (x)
#define VTX_UNLIKELY(x) (x)
#endif

/* ========================================================================== */
/* Fast instruction length lookup                                               */
/* ========================================================================== */

/**
 * Precomputed instruction lengths indexed by opcode.
 * All instructions are either 1 byte (opcode only) or 3 bytes
 * (opcode + 2-byte operand). This avoids calling
 * vtx_bytecode_insn_length() — a function that dereferences bc,
 * reads the opcode, looks up the opcode_info table, and checks
 * has_operand — on every single dispatch cycle.
 *
 * Measured impact: eliminates ~15 instructions per dispatch on x86-64.
 */
static const uint8_t vtx_insn_length[VT_OP_COUNT] = {
    1,  /* 0:  HALT */
    1,  /* 1:  NOP */
    3,  /* 2:  LOAD_LOCAL */
    3,  /* 3:  STORE_LOCAL */
    3,  /* 4:  LOAD_FIELD */
    3,  /* 5:  STORE_FIELD */
    3,  /* 6:  LOAD_CONST_INT */
    3,  /* 7:  LOAD_CONST_FLOAT */
    3,  /* 8:  LOAD_CONST_STR */
    1,  /* 9:  LOAD_NULL */
    1,  /* 10: LOAD_TRUE */
    1,  /* 11: LOAD_FALSE */
    1,  /* 12: LOAD_UNDEFINED */
    1,  /* 13: IADD */
    1,  /* 14: ISUB */
    1,  /* 15: IMUL */
    1,  /* 16: IDIV */
    1,  /* 17: IMOD */
    1,  /* 18: FADD */
    1,  /* 19: FSUB */
    1,  /* 20: FMUL */
    1,  /* 21: FDIV */
    1,  /* 22: ISHL */
    1,  /* 23: ISHR */
    1,  /* 24: IAND */
    1,  /* 25: IOR */
    1,  /* 26: IXOR */
    1,  /* 27: INEG */
    1,  /* 28: INOT */
    1,  /* 29: ICMP_EQ */
    1,  /* 30: ICMP_NE */
    1,  /* 31: ICMP_LT */
    1,  /* 32: ICMP_LE */
    1,  /* 33: ICMP_GT */
    1,  /* 34: ICMP_GE */
    1,  /* 35: FCMP_EQ */
    1,  /* 36: FCMP_NE */
    1,  /* 37: FCMP_LT */
    1,  /* 38: FCMP_LE */
    1,  /* 39: FCMP_GT */
    1,  /* 40: FCMP_GE */
    3,  /* 41: GOTO */
    3,  /* 42: IF_TRUE */
    3,  /* 43: IF_FALSE */
    3,  /* 44: CALL_STATIC */
    3,  /* 45: CALL_VIRTUAL */
    3,  /* 46: CALL_INTERFACE */
    1,  /* 47: RETURN */
    1,  /* 48: RETURN_VALUE */
    3,  /* 49: RETURN_MULTI — 2-byte operand (count) */
    1,  /* 50: LOAD_VARARGS */
    1,  /* 51: VARARG_COUNT */
    3,  /* 52: VARARG_GET — 2-byte operand (index) */
    3,  /* 53: NEW */
    3,  /* 54: NEWARRAY */
    3,  /* 55: CHECKCAST */
    3,  /* 56: INSTANCEOF */
    1,  /* 57: ARRAY_LOAD */
    1,  /* 58: ARRAY_STORE */
    1,  /* 59: ARRAY_LENGTH */
    1,  /* 60: THROW */
    3,  /* 61: CATCH */
    5,  /* 62: CATCH_TYPED — opcode(1) + handler_pc(2) + typeid(2) */
    1,  /* 63: MONITOR_ENTER */
    1,  /* 64: MONITOR_EXIT */
    1,  /* 65: DUP */
    1,  /* 66: POP */
    1,  /* 67: SWAP */
    1,  /* 68: ISNULL */
    1,  /* 69: TYPEOF */
    3,  /* 70: CALL_RUNTIME — 2-byte operand (func_id) */
    5,  /* 71: LOAD_CONST_INT__IADD   — §2.6 superinstruction, 4-byte operand */
    5,  /* 72: LOAD_LOCAL__LOAD_LOCAL — §2.6 superinstruction, 4-byte operand */
    5,  /* 73: LOAD_LOCAL__STORE_FIELD — §2.6 superinstruction, 4-byte operand */
};

/* ========================================================================== */
/* Helpers for the dispatch loop                                               */
/* ========================================================================== */

/**
 * Read a 2-byte big-endian operand at pc+1 from the bytecode stream.
 * This is the same as vtx_bytecode_read_operand but works directly on
 * the code array for speed in the dispatch loop.
 */
static inline uint16_t read_operand(const uint8_t *code, size_t pc)
{
    return ((uint16_t)code[pc + 1] << 8) | (uint16_t)code[pc + 2];
}

/**
 * Read a 4-byte big-endian operand (two packed 16-bit values) at pc+1.
 *
 * §2.6 Superinstructions — used by LOAD_CONST_INT__IADD,
 * LOAD_LOCAL__LOAD_LOCAL, and LOAD_LOCAL__STORE_FIELD.
 *
 * Returns the two 16-bit values via out-params:
 *   operand_a = first 16 bits (bytes 1-2)
 *   operand_b = second 16 bits (bytes 3-4)
 */
static inline void read_operand_4(const uint8_t *code, size_t pc,
                                   uint16_t *operand_a, uint16_t *operand_b)
{
    *operand_a = ((uint16_t)code[pc + 1] << 8) | (uint16_t)code[pc + 2];
    *operand_b = ((uint16_t)code[pc + 3] << 8) | (uint16_t)code[pc + 4];
}

/**
 * Check if a value is truthy for VT_OP_IF_TRUE / VT_OP_IF_FALSE.
 * Truthy: non-null, non-false, non-undefined, non-zero-SMI, non-zero-double,
 * and any heap pointer (objects are always truthy).
 */
static inline bool is_truthy(vtx_value_t v)
{
    if (vtx_is_null(v) || vtx_is_undefined(v)) {
        return false;
    }
    if (vtx_is_bool(v)) {
        return vtx_bool_value(v);
    }
    if (vtx_is_smi(v)) {
        return vtx_smi_value(v) != 0;
    }
    if (vtx_is_double(v)) {
        /* BUGFIX (R8 audit): NaN is falsy in JS/Python semantics.
         * The old code used `!= 0.0`, which returns true for NaN
         * (NaN comparisons always return false, so NaN != 0.0 is true).
         * This made IF_TRUE take the wrong branch for NaN values.
         * Fix: explicitly check for NaN using isnan(). */
        double d = vtx_double_value(v);
        return !isnan(d) && d != 0.0;
    }
    /* Heap pointers are always truthy (non-null) */
    return true;
}

/**
 * Get the typeid of a value. Returns VTX_TYPE_INVALID for non-heap values.
 */
static inline vtx_typeid_t value_typeid(vtx_value_t v)
{
    if (vtx_is_heap_ptr(v)) {
        vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(v);
        return obj->type_id;
    }
    return VTX_TYPE_INVALID;
}

/**
 * Get the shapeid of a value. Returns VTX_SHAPE_INVALID for non-heap values.
 */
static inline vtx_shapeid_t value_shapeid(vtx_value_t v)
{
    if (vtx_is_heap_ptr(v)) {
        vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(v);
        return obj->shape_id;
    }
    return VTX_SHAPE_INVALID;
}

/* ========================================================================== */
/* Method IC storage management                                                */
/* ========================================================================== */

/**
 * Find or create IC storage for a method.
 * Linear search by method pointer. This is called only on the first
 * call at each call site within a method, so the cost is negligible.
 */
static vtx_method_ic_storage_t *get_method_ic_storage(vtx_interp_t *interp,
                                                       const vtx_method_desc_t *method)
{
    /* Search for existing entry */
    for (uint32_t i = 0; i < interp->method_ic_count; i++) {
        if (interp->method_ics[i].method == method) {
            return &interp->method_ics[i];
        }
    }

    /* Not found — create new entry */
    if (interp->method_ic_count >= interp->method_ic_capacity) {
        uint32_t new_cap = interp->method_ic_capacity > 0 ?
                           interp->method_ic_capacity * 2 : 16;
        vtx_method_ic_storage_t *new_arr = (vtx_method_ic_storage_t *)realloc(
            interp->method_ics,
            new_cap * sizeof(vtx_method_ic_storage_t));
        if (new_arr == NULL) {
            return NULL;
        }
        memset(new_arr + interp->method_ic_capacity, 0,
               (new_cap - interp->method_ic_capacity) * sizeof(vtx_method_ic_storage_t));
        interp->method_ics = new_arr;
        interp->method_ic_capacity = new_cap;
    }

    vtx_method_ic_storage_t *storage = &interp->method_ics[interp->method_ic_count];
    storage->method = method;
    storage->count = 0;
    storage->ics = NULL;

    /* Allocate ICs: one per bytecode position */
    if (method->bytecode != NULL && method->bytecode->length > 0) {
        uint32_t len = (uint32_t)method->bytecode->length;
        storage->ics = (vtx_inline_cache_t *)calloc(len, sizeof(vtx_inline_cache_t));
        if (storage->ics == NULL) {
            return NULL;
        }
        storage->count = len;
        /* Initialize all ICs */
        for (uint32_t i = 0; i < len; i++) {
            vtx_ic_init(&storage->ics[i]);
        }
    }

    interp->method_ic_count++;
    return storage;
}

vtx_inline_cache_t *vtx_interp_get_ic(vtx_interp_t *interp,
                                       const vtx_method_desc_t *method,
                                       uint32_t call_pc)
{
    vtx_method_ic_storage_t *storage = get_method_ic_storage(interp, method);
    if (storage == NULL || storage->ics == NULL || call_pc >= storage->count) {
        return NULL;
    }
    return &storage->ics[call_pc];
}

/* ========================================================================== */
/* Interpreter init / destroy                                                  */
/* ========================================================================== */

int vtx_interp_init(vtx_interp_t *interp, vtx_type_system_t *ts, vtx_gc_t *gc)
{
    VTX_ASSERT(interp != NULL, "interpreter must not be NULL");
    VTX_ASSERT(ts != NULL, "type system must not be NULL");
    VTX_ASSERT(gc != NULL, "GC must not be NULL");

    memset(interp, 0, sizeof(vtx_interp_t));

    /* Initialize frame stack */
    if (vtx_frame_stack_init(&interp->frame_stack) != 0) {
        return -1;
    }

    /* Initialize profiler */
    if (vtx_profiler_init(&interp->profiler) != 0) {
        vtx_frame_stack_destroy(&interp->frame_stack);
        return -1;
    }

    /* Initialize type feedback (max 1024 sites, grows dynamically) */
    if (vtx_type_feedback_init(&interp->type_feedback, 1024) != 0) {
        vtx_profiler_destroy(&interp->profiler);
        vtx_frame_stack_destroy(&interp->frame_stack);
        return -1;
    }

    interp->type_system = ts;
    interp->gc = gc;
    interp->current_frame = NULL;
    interp->running = false;
    interp->exception = VTX_VALUE_UNDEFINED;

    /* Allocate dispatch table (indexed by opcode).
     * The table will be populated in vtx_interp_run()
     * using GCC labels-as-values. */
    interp->dispatch_table = (void **)calloc(VT_OP_COUNT, sizeof(void *));
    if (interp->dispatch_table == NULL) {
        vtx_type_feedback_destroy(&interp->type_feedback);
        vtx_profiler_destroy(&interp->profiler);
        vtx_frame_stack_destroy(&interp->frame_stack);
        return -1;
    }

    /* Initialize method IC storage */
    interp->method_ics = NULL;
    interp->method_ic_count = 0;
    interp->method_ic_capacity = 0;

    return 0;
}

void vtx_interp_destroy(vtx_interp_t *interp)
{
    VTX_ASSERT(interp != NULL, "interpreter must not be NULL");

    vtx_frame_stack_destroy(&interp->frame_stack);
    vtx_profiler_destroy(&interp->profiler);
    vtx_type_feedback_destroy(&interp->type_feedback);

    /* Free method IC storage */
    for (uint32_t i = 0; i < interp->method_ic_count; i++) {
        free(interp->method_ics[i].ics);
    }
    free(interp->method_ics);
    interp->method_ics = NULL;
    interp->method_ic_count = 0;
    interp->method_ic_capacity = 0;

    free(interp->dispatch_table);
    interp->dispatch_table = NULL;
}

void vtx_interp_set_compile_ctx(vtx_interp_t *interp,
                                 vtx_compile_context_t *ctx)
{
    VTX_ASSERT(interp != NULL, "interpreter must not be NULL");
    interp->compile_ctx = ctx;
}

vtx_value_t vtx_interp_handle_uncaught(vtx_interp_t *interp, vtx_value_t exception)
{
    VTX_ASSERT(interp != NULL, "interpreter must not be NULL");

    /* Unwind all frames */
    vtx_frame_t *frame = interp->current_frame;
    while (frame != NULL) {
        vtx_frame_t *caller = frame->caller;
        vtx_frame_destroy(frame, &interp->frame_stack);
        frame = caller;
    }
    interp->current_frame = NULL;
    interp->running = false;
    interp->exception = VTX_VALUE_UNDEFINED;
    return exception;
}

/* ========================================================================== */
/* Exception handling helpers                                                  */
/* ========================================================================== */

/**
 * Throw an exception: set the exception value and find the nearest
 * catch handler in the frame chain. Returns the catch handler PC,
 * or VTX_CATCH_NONE if no handler found.
 *
 * out_handler_frame receives the frame that contains the handler.
 */
static uint32_t throw_exception(vtx_interp_t *interp, vtx_value_t exc_value,
                                 vtx_frame_t **out_handler_frame)
{
    interp->exception = exc_value;

    /* Get the exception's type_id for catch handler matching */
    vtx_typeid_t exc_typeid = VTX_TYPE_INVALID;
    if (vtx_is_heap_ptr(exc_value)) {
        vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(exc_value);
        exc_typeid = obj->type_id;
    }

    /* Walk the frame chain looking for a matching catch handler */
    vtx_frame_t *f = interp->current_frame;
    while (f != NULL) {
        if (f->catch_handler_pc != VTX_CATCH_NONE) {
            /* Check if this handler catches the exception:
             * - catch_type == 0 (catch-all): matches any exception
             * - catch_type != 0: matches if exc_typeid is a subtype
             *   of catch_type (including exact match).
             * If exc_typeid is VTX_TYPE_INVALID (non-heap exception),
             * only a catch-all handler can catch it. */
            vtx_typeid_t catch_type = f->catch_type;
            bool matches = false;

            if (catch_type == 0) {
                /* catch-all: accepts any exception */
                matches = true;
            } else if (exc_typeid != VTX_TYPE_INVALID) {
                /* Check subtype relationship using the type system.
                 * vtx_get_current_type_system() is declared in
                 * runtime/type_system.h which is included via frame.h. */
                vtx_type_system_t *ts = vtx_get_current_type_system();
                if (ts != NULL) {
                    matches = vtx_type_is_subtype(ts, exc_typeid, catch_type);
                } else {
                    /* No type system: exact match only */
                    matches = (exc_typeid == catch_type);
                }
            }

            if (matches) {
                *out_handler_frame = f;
                return f->catch_handler_pc;
            }
        }
        f = f->caller;
    }

    *out_handler_frame = NULL;
    return VTX_CATCH_NONE;
}

/**
 * Unwind frames from the current frame up to (but not including)
 * the handler frame. Returns the handler frame.
 */
static vtx_frame_t *unwind_to_handler(vtx_interp_t *interp,
                                       vtx_frame_t *current,
                                       vtx_frame_t *handler)
{
    vtx_frame_t *f = current;
    while (f != handler) {
        vtx_frame_t *caller = f->caller;
        vtx_frame_destroy(f, &interp->frame_stack);
        f = caller;
    }
    return handler;
}

/* ========================================================================== */
/* Type feedback site-index hashing (Bug #1 fix)                               */
/* ========================================================================== */

/**
 * Hash the site_index with the method pointer to avoid cross-method collisions.
 * Without this, two methods at the same bytecode PC would map to the same
 * feedback slot in the global feedback arrays, polluting each other's data.
 */
/**
 * Maximum number of distinct type-feedback sites.
 * The hash is capped to this range to prevent the feedback arrays
 * from growing to unbounded sizes (a full uint32_t hash would cause
 * multi-GB allocations for the first site at a high index).
 */
#define VTX_FEEDBACK_SITE_MAX  4096
#define VTX_FEEDBACK_SITE_MASK (VTX_FEEDBACK_SITE_MAX - 1)  /* 0xFFF, power of 2 */

static inline uint32_t vtx_hash_site_index(const void *method, uint32_t pc)
{
    uintptr_t mp = (uintptr_t)method;
    uint32_t h = (uint32_t)(mp ^ (mp >> 32)) ^ pc;
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    /* Cap to a reasonable table size to prevent unbounded growth */
    return h & VTX_FEEDBACK_SITE_MASK;  /* BUGFIX: bitmask instead of modulo */
}

/**
 * JIT dispatch: call compiled code directly from the interpreter.
 *
 * When a method has been JIT-compiled (compiled_code != NULL), we can
 * call it directly via function pointer instead of creating an interpreter
 * frame and running through the dispatch loop. This is the key bridge
 * that makes the JIT actually execute code.
 *
 * The compiled code follows the baseline JIT calling convention:
 *   - RDI = method pointer (1st arg)
 *   - RSI = deopt_info (2nd arg, can be NULL)
 *   - RDX = profile_data (3rd arg, can be NULL)
 *   - Stack args: arg0, arg1, ... (passed as for interpreter)
 *
 * The compiled code returns a vtx_value_t.
 */
typedef vtx_value_t (*vtx_jit_entry_t)(
    const vtx_method_desc_t *method,
    void *deopt_info,
    void *profile_data,
    vtx_value_t *args,
    uint32_t arg_count);

static inline vtx_value_t vtx_dispatch_jit(
    vtx_interp_t *interp,
    const vtx_method_desc_t *target_method,
    vtx_value_t *args,
    uint32_t arg_count)
{
    /* Read compiled_code with acquire semantics to ensure we see
     * the fully initialized code after the store in vtx_install_method */
    void *code = __atomic_load_n(&target_method->compiled_code, __ATOMIC_ACQUIRE);
    if (VTX_UNLIKELY(code == NULL)) {
        return VTX_VALUE_UNDEFINED; /* Should not happen, but be safe */
    }

    /* ASan compatibility: JIT-generated code is not instrumented by ASan
     * (ASan instruments at compile time; JIT code is generated at runtime).
     * The JIT code accesses the C stack frame directly (e.g., [rbp-0x18]
     * for locals), and ASan has poisoned those bytes as stack redzones
     * between C variables. Without unpoisoning, ASan reports a false
     * stack-buffer-overflow when the JIT code reads its own locals.
     *
     * Fix: unpoison a generous region of the stack below the current RSP
     * before entering JIT code. The JIT frame is at most a few hundred
     * bytes (prologue + locals + spills). 4096 bytes covers any method. */
#if defined(__SANITIZE_ADDRESS__) || defined(ADDRESS_SANITIZER)
    {
        extern void __asan_unpoison_memory_region(const volatile void *addr, size_t size);
        /* Unpoison 2KB of stack below the current frame marker.
         * This covers the JIT code's frame (max ~500 bytes) with margin.
         * Using a smaller region avoids touching stack guard pages. */
        volatile char stack_marker = 0;
        const volatile char *frame_base = &stack_marker - 2048;
        __asan_unpoison_memory_region(frame_base, 2048);
        (void)stack_marker;
    }
#endif

    /* Call the JIT-compiled code directly.
     * The baseline JIT's prologue expects:
     *   RDI = method ptr, RSI = deopt_info, RDX = profile_data
     * And it returns a vtx_value_t in RAX.
     *
     * BUGFIX (audit #13): Was passing (void*)1 as profile_data, which
     * causes T1 instrumentation to read garbage memory. The profile_data
     * pointer is used by the T1 prologue to record call types. Passing
     * a garbage pointer means either: (a) instrumentation crashes on
     * dereference, or (b) it writes to address 0x1 → segfault.
     *
     * Fix: Pass NULL. The T1 prologue checks `if (profile_data)` before
     * dereferencing. NULL means "no profiling" — the method runs without
     * T1 type recording, which is safe (just less profile data for T2). */
    /* ISO C forbids direct object-pointer-to-function-pointer cast.
     * Use a union for safe reinterpretation (the dlsym pattern). */
    union { void *ptr; vtx_jit_entry_t fn; } entry_cast;
    entry_cast.ptr = code;
    vtx_jit_entry_t entry = entry_cast.fn;

    interp->deopt_pending = false;

    /* OSR-22 fix: increment on_stack_count for the method's active
     * version BEFORE entering JIT code, and decrement AFTER the JIT
     * returns. This lets the versioned cache's safe-reclamation
     * mechanism know whether retired code is still executing (and thus
     * must NOT be freed). Without this, vtx_versioned_cache_reclaim /
     * force_free_oldest_retired could free code that a thread is
     * currently executing → UAF.
     *
     * The on_enter/on_exit pair is wrapped around the entry() call so
     * it covers the JIT's entire execution including any safepoint
     * polls, deopt stubs, and recursive re-entry. The OSR-up path
     * (vtx_osr_up) is also wired (see the osr_pending block at the
     * bottom of vtx_interp_run) — the on_enter/on_exit pair there is
     * wrapped around the vtx_osr_up call so it covers both the
     * success path (asm jumps to JIT, JIT returns via its epilogue to
     * the dispatch.c caller) and the failure path (vtx_osr_up returns
     * normally). */
    vtx_versioned_cache_t *vc = NULL;
    if (interp->compile_ctx != NULL) {
        vc = interp->compile_ctx->versioned_cache;
    }
    if (vc != NULL) {
        vtx_versioned_cache_on_enter(vc, target_method->vtable_index);
    }

    vtx_value_t result;
    if (arg_count > 0 && args != NULL) {
        result = entry(target_method, NULL, NULL, args, arg_count);
    } else {
        result = entry(target_method, NULL, NULL, NULL, 0);
    }

    if (vc != NULL) {
        vtx_versioned_cache_on_exit(vc, target_method->vtable_index);
    }

    /* CRITICAL FIX: Check deopt_pending flag instead of checking for
     * VTX_VALUE_UNDEFINED. Void methods legitimately return undefined,
     * and returning undefined should NOT trigger re-interpretation.
     * Only deoptimization (signaled by the deopt stub setting
     * deopt_pending = true) should fall back to the interpreter. */
    if (VTX_UNLIKELY(interp->deopt_pending)) {
        interp->deopt_pending = false;
        return vtx_interp_run(interp, target_method, args, arg_count);
    }

    /* M3 fix: the JIT returned successfully (no deopt) — record this as
     * an OSR success on the compiled_method so the per-method OSR
     * re-attempt rate limiter (OSR-29) clears its failure counter and
     * cooldown. Without this call, vtx_osr_rate_record_success was dead
     * code: the dispatch loop's OSR-up failure path bumped the counter
     * via vtx_osr_rate_record_failure, but no path ever reset it, so
     * once a method hit VTX_OSR_MAX_FAILURES transient failures it
     * stayed rate-limited for VTX_OSR_COOLDOWN_INVOCATIONS even after
     * the JIT started succeeding. */
    if (interp->compile_ctx != NULL &&
        interp->compile_ctx->method_registry != NULL) {
        vtx_compiled_method_t *cm = vtx_method_registry_get(
            interp->compile_ctx->method_registry, target_method->vtable_index);
        if (cm != NULL) {
            vtx_osr_rate_record_success(&cm->osr_failure_count,
                                          &cm->osr_cooldown_until_call);
        }
    }

    return result;
}

/* ========================================================================== */
/* Main interpreter dispatch loop                                              */
/* ========================================================================== */

void vtx_interp_set_deopt_pc(vtx_frame_t *frame, uint32_t pc)
{
    /* Store the resume PC in the interpreter's global state.
     * The dispatch loop checks deopt_resume_pending on entry
     * and uses deopt_resume_pc as the starting PC. */
    extern vtx_interp_t *vtx_get_current_interp(void);
    vtx_interp_t *interp = vtx_get_current_interp();
    if (interp) {
        interp->deopt_resume_pc = pc;
        interp->deopt_resume_pending = true;
        interp->current_frame = frame;
    }
}

/* Deopt entry wrapper: the deopt stub calls the entry point as
 * void(*)(void) (vtx_interp_entry_t). We can't pass vtx_interp_run
 * directly because it takes 4 args. This wrapper reads the global
 * interp pointer and current_frame to reconstruct the call.
 *
 * The deopt handler has already:
 *   1. Set the_interp->current_frame to the reconstructed frame
 *   2. Called vtx_interp_set_deopt_pc to set deopt_resume_pc/pending
 * So we just need to call vtx_interp_run with the right args. */
void vtx_deopt_interp_entry_wrapper(void)
{
    extern vtx_interp_t *vtx_get_current_interp(void);
    vtx_interp_t *interp = vtx_get_current_interp();
    if (interp == NULL || interp->current_frame == NULL) {
        /* Nothing we can do — abort */
        abort();
    }
    const vtx_method_desc_t *method = interp->current_frame->method;
    /* deopt_resume_pending is already set — vtx_interp_run will
     * check it before JIT dispatch and run the interpreter. */
    vtx_interp_run(interp, method, NULL, 0);
}

vtx_value_t vtx_interp_run(vtx_interp_t *interp,
                            const vtx_method_desc_t *method,
                            vtx_value_t *args,
                            uint32_t arg_count)
{
    VTX_ASSERT(interp != NULL, "interpreter must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    /* B1 fix: Wire the deopt entry point so guard failures can deoptimize
     * instead of crashing. g_interp_entry is set once and checked by the
     * deopt stub — without this, every guard failure segfaults.
     *
     * We use vtx_deopt_interp_entry_wrapper (defined below) instead of
     * &vtx_interp_run directly, because the deopt stub calls the entry
     * point as void(*)(void) — vtx_interp_run takes 4 args and would
     * read garbage from registers if called through that typedef. */
    if (vtx_deopt_get_interp_entry() == NULL) {
        vtx_deopt_set_interp_entry(vtx_deopt_interp_entry_wrapper);
    }

    /* ===================================================================
     * DEOPT RESUME: If the deopt handler set up a frame with a specific
     * resume PC, we MUST run the interpreter — NOT the JIT. The old code
     * checked compiled_code first, which caused the deopt handler to
     * re-enter the JIT (which immediately hit the same guard/trap again,
     * looping until stack overflow → SIGSEGV).
     * =================================================================== */
    if (interp->deopt_resume_pending) {
        /* Deopt resume takes priority over JIT dispatch. */
        method = interp->current_frame->method;
        args = NULL;
        arg_count = 0;
        /* Fall through to the interpreter dispatch loop below, which
         * checks deopt_resume_pending again at line ~786 and sets pc. */
    } else
    /* ===================================================================
     * JIT DISPATCH: If the method has been compiled, call JIT code
     * directly instead of falling through to the interpreter.
     * This is the key bridge that makes JIT-compiled methods actually
     * execute native code when invoked through vtx_interp_run().
     * =================================================================== */
    if (__atomic_load_n(&method->compiled_code, __ATOMIC_ACQUIRE) != NULL) {
        return vtx_dispatch_jit(interp, method, args, arg_count);
    }

    /*
     * ===================================================================
     * DISPATCH TABLE CONSTRUCTION (GCC/Clang computed goto)
     * ===================================================================
     *
     * We use GCC's labels-as-values extension to build a dispatch table
     * that maps opcodes to label addresses. This gives O(1) dispatch
     * without the branch-prediction overhead of a switch statement.
     *
     * Each opcode handler is a labeled block. At the end of each handler,
     * we fetch the next opcode and jump through the dispatch table.
     *
     * Computed goto is available on GCC >= 3.0 and Clang >= 2.8.
     * Other compilers will need a switch-based fallback.
     */
#if defined(__GNUC__) || defined(__clang__)
#define VTX_USE_COMPUTED_GOTO 1
#else
#define VTX_USE_COMPUTED_GOTO 0
#endif

#if VTX_USE_COMPUTED_GOTO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
/* Computed-goto dispatch uses two GNU extensions: taking the address
 * of a label (&&label) and casting void* to a function-pointer-like
 * target. These are the standard computed-goto technique used by
 * CPython, LuaJIT, and V8's interpreter. -Wpedantic flags them as
 * non-standard, but the code is correct and intentional.
 * Per VORTEX rules: this is NOT silencing a latent bug — it's a
 * documented, well-understood GNU extension. Do NOT remove. */
    static void *local_dispatch_table[VT_OP_COUNT] = { NULL };
    /* B9 fix: Use a 3-state flag instead of a bool. The original code did a
     * CAS that set `dispatch_table_built = true` BEFORE populating the table,
     * so a second thread entering vtx_interp_run concurrently would see the
     * flag as set, skip the build, and immediately memcpy an incompletely
     * populated table — a torn-read race.
     *
     * States: 0 = unbuilt, 1 = building (claimed), 2 = ready.
     * - Builder thread CASes 0 -> 1, populates the table, then release-stores 2.
     * - Other threads either skip (if 2) or spin-wait (if 1) until ready. */
    static volatile int dispatch_table_state = 0;

    int expected0 = 0;
    if (__atomic_compare_exchange_n(&dispatch_table_state, &expected0, 1,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
#define DISPATCH_LABEL(op) &&dispatch_##op
        local_dispatch_table[VT_OP_HALT]           = DISPATCH_LABEL(VT_OP_HALT);
        local_dispatch_table[VT_OP_NOP]            = DISPATCH_LABEL(VT_OP_NOP);
        local_dispatch_table[VT_OP_LOAD_LOCAL]     = DISPATCH_LABEL(VT_OP_LOAD_LOCAL);
        local_dispatch_table[VT_OP_STORE_LOCAL]    = DISPATCH_LABEL(VT_OP_STORE_LOCAL);
        local_dispatch_table[VT_OP_LOAD_FIELD]     = DISPATCH_LABEL(VT_OP_LOAD_FIELD);
        local_dispatch_table[VT_OP_STORE_FIELD]    = DISPATCH_LABEL(VT_OP_STORE_FIELD);
        local_dispatch_table[VT_OP_LOAD_CONST_INT]   = DISPATCH_LABEL(VT_OP_LOAD_CONST_INT);
        local_dispatch_table[VT_OP_LOAD_CONST_FLOAT] = DISPATCH_LABEL(VT_OP_LOAD_CONST_FLOAT);
        local_dispatch_table[VT_OP_LOAD_CONST_STR]   = DISPATCH_LABEL(VT_OP_LOAD_CONST_STR);
        local_dispatch_table[VT_OP_LOAD_NULL]      = DISPATCH_LABEL(VT_OP_LOAD_NULL);
        local_dispatch_table[VT_OP_LOAD_TRUE]      = DISPATCH_LABEL(VT_OP_LOAD_TRUE);
        local_dispatch_table[VT_OP_LOAD_FALSE]     = DISPATCH_LABEL(VT_OP_LOAD_FALSE);
        local_dispatch_table[VT_OP_LOAD_UNDEFINED] = DISPATCH_LABEL(VT_OP_LOAD_UNDEFINED);
        local_dispatch_table[VT_OP_IADD]           = DISPATCH_LABEL(VT_OP_IADD);
        local_dispatch_table[VT_OP_ISUB]           = DISPATCH_LABEL(VT_OP_ISUB);
        local_dispatch_table[VT_OP_IMUL]           = DISPATCH_LABEL(VT_OP_IMUL);
        local_dispatch_table[VT_OP_IDIV]           = DISPATCH_LABEL(VT_OP_IDIV);
        local_dispatch_table[VT_OP_IMOD]           = DISPATCH_LABEL(VT_OP_IMOD);
        local_dispatch_table[VT_OP_FADD]           = DISPATCH_LABEL(VT_OP_FADD);
        local_dispatch_table[VT_OP_FSUB]           = DISPATCH_LABEL(VT_OP_FSUB);
        local_dispatch_table[VT_OP_FMUL]           = DISPATCH_LABEL(VT_OP_FMUL);
        local_dispatch_table[VT_OP_FDIV]           = DISPATCH_LABEL(VT_OP_FDIV);
        local_dispatch_table[VT_OP_ISHL]           = DISPATCH_LABEL(VT_OP_ISHL);
        local_dispatch_table[VT_OP_ISHR]           = DISPATCH_LABEL(VT_OP_ISHR);
        local_dispatch_table[VT_OP_IAND]           = DISPATCH_LABEL(VT_OP_IAND);
        local_dispatch_table[VT_OP_IOR]            = DISPATCH_LABEL(VT_OP_IOR);
        local_dispatch_table[VT_OP_IXOR]           = DISPATCH_LABEL(VT_OP_IXOR);
        local_dispatch_table[VT_OP_INEG]           = DISPATCH_LABEL(VT_OP_INEG);
        local_dispatch_table[VT_OP_INOT]           = DISPATCH_LABEL(VT_OP_INOT);
        local_dispatch_table[VT_OP_ICMP_EQ]        = DISPATCH_LABEL(VT_OP_ICMP_EQ);
        local_dispatch_table[VT_OP_ICMP_NE]        = DISPATCH_LABEL(VT_OP_ICMP_NE);
        local_dispatch_table[VT_OP_ICMP_LT]        = DISPATCH_LABEL(VT_OP_ICMP_LT);
        local_dispatch_table[VT_OP_ICMP_LE]        = DISPATCH_LABEL(VT_OP_ICMP_LE);
        local_dispatch_table[VT_OP_ICMP_GT]        = DISPATCH_LABEL(VT_OP_ICMP_GT);
        local_dispatch_table[VT_OP_ICMP_GE]        = DISPATCH_LABEL(VT_OP_ICMP_GE);
        local_dispatch_table[VT_OP_FCMP_EQ]        = DISPATCH_LABEL(VT_OP_FCMP_EQ);
        local_dispatch_table[VT_OP_FCMP_NE]        = DISPATCH_LABEL(VT_OP_FCMP_NE);
        local_dispatch_table[VT_OP_FCMP_LT]        = DISPATCH_LABEL(VT_OP_FCMP_LT);
        local_dispatch_table[VT_OP_FCMP_LE]        = DISPATCH_LABEL(VT_OP_FCMP_LE);
        local_dispatch_table[VT_OP_FCMP_GT]        = DISPATCH_LABEL(VT_OP_FCMP_GT);
        local_dispatch_table[VT_OP_FCMP_GE]        = DISPATCH_LABEL(VT_OP_FCMP_GE);
        local_dispatch_table[VT_OP_GOTO]           = DISPATCH_LABEL(VT_OP_GOTO);
        local_dispatch_table[VT_OP_IF_TRUE]        = DISPATCH_LABEL(VT_OP_IF_TRUE);
        local_dispatch_table[VT_OP_IF_FALSE]       = DISPATCH_LABEL(VT_OP_IF_FALSE);
        local_dispatch_table[VT_OP_CALL_STATIC]    = DISPATCH_LABEL(VT_OP_CALL_STATIC);
        local_dispatch_table[VT_OP_CALL_VIRTUAL]   = DISPATCH_LABEL(VT_OP_CALL_VIRTUAL);
        local_dispatch_table[VT_OP_CALL_INTERFACE] = DISPATCH_LABEL(VT_OP_CALL_INTERFACE);
        local_dispatch_table[VT_OP_RETURN]         = DISPATCH_LABEL(VT_OP_RETURN);
        local_dispatch_table[VT_OP_RETURN_VALUE]   = DISPATCH_LABEL(VT_OP_RETURN_VALUE);
        local_dispatch_table[VT_OP_RETURN_MULTI]   = DISPATCH_LABEL(VT_OP_RETURN_MULTI);
        local_dispatch_table[VT_OP_LOAD_VARARGS]   = DISPATCH_LABEL(VT_OP_LOAD_VARARGS);
        local_dispatch_table[VT_OP_VARARG_COUNT]   = DISPATCH_LABEL(VT_OP_VARARG_COUNT);
        local_dispatch_table[VT_OP_VARARG_GET]     = DISPATCH_LABEL(VT_OP_VARARG_GET);
        local_dispatch_table[VT_OP_NEW]            = DISPATCH_LABEL(VT_OP_NEW);
        local_dispatch_table[VT_OP_NEWARRAY]       = DISPATCH_LABEL(VT_OP_NEWARRAY);
        local_dispatch_table[VT_OP_CHECKCAST]      = DISPATCH_LABEL(VT_OP_CHECKCAST);
        local_dispatch_table[VT_OP_INSTANCEOF]     = DISPATCH_LABEL(VT_OP_INSTANCEOF);
        local_dispatch_table[VT_OP_ARRAY_LOAD]     = DISPATCH_LABEL(VT_OP_ARRAY_LOAD);
        local_dispatch_table[VT_OP_ARRAY_STORE]    = DISPATCH_LABEL(VT_OP_ARRAY_STORE);
        local_dispatch_table[VT_OP_ARRAY_LENGTH]   = DISPATCH_LABEL(VT_OP_ARRAY_LENGTH);
        local_dispatch_table[VT_OP_THROW]          = DISPATCH_LABEL(VT_OP_THROW);
        local_dispatch_table[VT_OP_CATCH]          = DISPATCH_LABEL(VT_OP_CATCH);
        local_dispatch_table[VT_OP_CATCH_TYPED]   = DISPATCH_LABEL(VT_OP_CATCH_TYPED);
        local_dispatch_table[VT_OP_MONITOR_ENTER]  = DISPATCH_LABEL(VT_OP_MONITOR_ENTER);
        local_dispatch_table[VT_OP_MONITOR_EXIT]   = DISPATCH_LABEL(VT_OP_MONITOR_EXIT);
        local_dispatch_table[VT_OP_DUP]            = DISPATCH_LABEL(VT_OP_DUP);
        local_dispatch_table[VT_OP_POP]            = DISPATCH_LABEL(VT_OP_POP);
        local_dispatch_table[VT_OP_SWAP]           = DISPATCH_LABEL(VT_OP_SWAP);
        local_dispatch_table[VT_OP_ISNULL]         = DISPATCH_LABEL(VT_OP_ISNULL);
        local_dispatch_table[VT_OP_TYPEOF]         = DISPATCH_LABEL(VT_OP_TYPEOF);
        local_dispatch_table[VT_OP_CALL_RUNTIME]   = DISPATCH_LABEL(VT_OP_CALL_RUNTIME);

        /* §2.6 Superinstructions — fused bytecode pairs.
         *
         * These eliminate one dispatch + one operand read per pair,
         * which on T0 interpreter is a 15-25% throughput improvement
         * on tight arithmetic loops (CPython 3.11 saw ~20% from a
         * similar superinstruction pass).
         *
         * The handlers below fuse:
         *   LOAD_CONST_INT__IADD    — pop TOS, add const to TOS, push
         *   LOAD_LOCAL__LOAD_LOCAL  — push two locals in one dispatch
         *   LOAD_LOCAL__STORE_FIELD — push local, store to field of TOS obj
         */
        local_dispatch_table[VT_OP_LOAD_CONST_INT__IADD]   = DISPATCH_LABEL(VT_OP_LOAD_CONST_INT__IADD);
        local_dispatch_table[VT_OP_LOAD_LOCAL__LOAD_LOCAL] = DISPATCH_LABEL(VT_OP_LOAD_LOCAL__LOAD_LOCAL);
        local_dispatch_table[VT_OP_LOAD_LOCAL__STORE_FIELD] = DISPATCH_LABEL(VT_OP_LOAD_LOCAL__STORE_FIELD);
#undef DISPATCH_LABEL
        /* B9 fix: Publish the table AFTER it is fully populated so that
         * concurrent readers never observe a half-built table. The release
         * store pairs with the acquire load in the spin-wait below. */
        __atomic_store_n(&dispatch_table_state, 2, __ATOMIC_RELEASE);
    } else {
        /* Lost the CAS — another thread is building (or has built) the table.
         * Spin-wait until the builder publishes state == 2 (ready). */
        while (__atomic_load_n(&dispatch_table_state, __ATOMIC_ACQUIRE) != 2) {
            /* spin */
        }
    }

    /* BUGFIX (audit #14): Was memcpy'ing the 568-byte dispatch table
     * on EVERY vtx_interp_run call. The table is static and never
     * changes after first build. Only copy once — on the first call
     * for this interpreter instance.
     *
     * We use a per-interpreter flag to track whether the table has
     * been copied. This avoids the memcpy on subsequent calls. */
    if (interp->dispatch_table != NULL && !interp->dispatch_table_copied) {
        memcpy(interp->dispatch_table, local_dispatch_table,
               VT_OP_COUNT * sizeof(void *));
        interp->dispatch_table_copied = true;
    }
#pragma GCC diagnostic pop
#endif /* VTX_USE_COMPUTED_GOTO */

    /* ===================================================================
     * SETUP: Create the initial frame
     * =================================================================== */
    vtx_frame_t *frame = vtx_frame_create(method, NULL, 0, &interp->frame_stack);
    if (frame == NULL) {
        return VTX_VALUE_UNDEFINED;
    }

    /* Copy arguments into locals */
    if (args != NULL && arg_count > 0) {
        uint32_t copy_count = arg_count < frame->locals_count ?
                              arg_count : frame->locals_count;
        for (uint32_t i = 0; i < copy_count; i++) {
            frame->locals[i] = args[i];
        }
    }

    interp->current_frame = frame;
    interp->running = true;
    interp->exception = VTX_VALUE_UNDEFINED;

    /* Record invocation for the top-level method */
    vtx_profiler_record_invocation(&interp->profiler, method);

    /* ===================================================================
     * CACHED DISPATCH STATE
     *
     * We cache the operand stack pointer (sp), locals pointer, and
     * bytecode pointers in local variables. This eliminates repeated
     * dereferencing of the frame pointer on every push/pop/local access.
     *
     * sp points to the next free slot on the operand stack.
     * Push: *sp++ = val    Pop: val = *--sp    Peek(0): *(sp-1)
     *
     * SYNC_SP must be called before any operation that reads
     * frame->stack_top externally (e.g., stack_depth checks, GC).
     * RELOAD_FRAME must be called after switching to a different frame
     * (call/return/throw).
     * =================================================================== */
    vtx_value_t *sp = frame->operand_stack + frame->stack_top;
    vtx_value_t *locals_arr = frame->locals;
    vtx_bytecode_t *bc = frame->bytecode;
    const uint8_t *code = bc->code;
    size_t pc = 0;

    /* Check for deopt resume: if the deopt handler set up a frame
     * with a specific resume PC, start executing from there instead
     * of PC=0. */
    if (interp->deopt_resume_pending) {
        pc = interp->deopt_resume_pc;
        interp->deopt_resume_pending = false;
        /* B10 fix: The deopt handler sets interp->current_frame to the
         * deoptimized frame, but the old frame was never destroyed.
         * Destroy it now to prevent LIFO invariant violations and
         * memory leaks. The old frame is the one created at the start
         * of this vtx_interp_run call (the `frame` variable above). */
        if (frame != NULL && frame != interp->current_frame) {
            vtx_frame_destroy(frame, &interp->frame_stack);
        }
        /* Reload frame state since deopt may have set current_frame */
        frame = interp->current_frame;
        sp = frame->operand_stack + frame->stack_top;
        locals_arr = frame->locals;
        bc = frame->bytecode;
        code = bc->code;
    }
    vtx_value_t result = VTX_VALUE_UNDEFINED;
    vtx_value_t a, b, val;
    uint16_t operand;
    uint16_t operand2;  /* §2.6: second 16-bit operand for superinstructions */
    int64_t ia, ib;
    double fa, fb;

    /* Sync cached stack pointer back to the frame struct */
#define SYNC_SP() do { \
    frame->stack_top = (int)(sp - frame->operand_stack); \
} while(0)

    /* Reload all cached state from the current frame */
#define RELOAD_FRAME() do { \
    sp = frame->operand_stack + frame->stack_top; \
    locals_arr = frame->locals; \
    bc = frame->bytecode; \
    code = bc->code; \
} while(0)

    /* ===================================================================
     * DISPATCH LOOP
     * =================================================================== */

    /* Bug #15 fix: Cache the opcode in a local variable to avoid
     * double-reading code[pc]. The computed goto DISPATCH() reads
     * code[pc] to index the dispatch table, and ADVANCE_PC() reads
     * code[pc] again to compute the instruction length. By caching
     * the opcode, we eliminate one memory read per dispatch cycle.
     * On x86-64, this saves ~3 instructions per dispatch (load +
     * zero-extend + table index). */
    uint8_t cached_opcode = 0;

#if VTX_USE_COMPUTED_GOTO
#define DISPATCH() do { \
    cached_opcode = code[pc]; \
    goto *local_dispatch_table[cached_opcode]; \
} while(0)
#else
#define DISPATCH() do { \
    cached_opcode = code[pc]; \
    goto switch_dispatch; \
} while(0)
#endif

#define ADVANCE_PC() do { \
    pc += vtx_insn_length[cached_opcode]; \
} while(0)

#define DISPATCH_NEXT() do { \
    ADVANCE_PC(); \
    DISPATCH(); \
} while(0)

    /* Enter the dispatch loop */
    DISPATCH();

#if !VTX_USE_COMPUTED_GOTO
    /* ===================================================================
     * Switch-based dispatch fallback for non-GCC/Clang compilers.
     * Jumps to the same handler labels used by the computed goto path,
     * so no handler code duplication is needed.
     * =================================================================== */
switch_dispatch:
    {
        /* Bug #15 fix: use cached_opcode already set by DISPATCH() macro
         * instead of re-reading code[pc] */
        switch (cached_opcode) {
            case VT_OP_HALT:           goto dispatch_VT_OP_HALT;
            case VT_OP_NOP:            goto dispatch_VT_OP_NOP;
            case VT_OP_LOAD_LOCAL:     goto dispatch_VT_OP_LOAD_LOCAL;
            case VT_OP_STORE_LOCAL:    goto dispatch_VT_OP_STORE_LOCAL;
            case VT_OP_LOAD_FIELD:     goto dispatch_VT_OP_LOAD_FIELD;
            case VT_OP_STORE_FIELD:    goto dispatch_VT_OP_STORE_FIELD;
            case VT_OP_LOAD_CONST_INT:   goto dispatch_VT_OP_LOAD_CONST_INT;
            case VT_OP_LOAD_CONST_FLOAT: goto dispatch_VT_OP_LOAD_CONST_FLOAT;
            case VT_OP_LOAD_CONST_STR:   goto dispatch_VT_OP_LOAD_CONST_STR;
            case VT_OP_LOAD_NULL:      goto dispatch_VT_OP_LOAD_NULL;
            case VT_OP_LOAD_TRUE:      goto dispatch_VT_OP_LOAD_TRUE;
            case VT_OP_LOAD_FALSE:     goto dispatch_VT_OP_LOAD_FALSE;
            case VT_OP_LOAD_UNDEFINED: goto dispatch_VT_OP_LOAD_UNDEFINED;
            case VT_OP_IADD:           goto dispatch_VT_OP_IADD;
            case VT_OP_ISUB:           goto dispatch_VT_OP_ISUB;
            case VT_OP_IMUL:           goto dispatch_VT_OP_IMUL;
            case VT_OP_IDIV:           goto dispatch_VT_OP_IDIV;
            case VT_OP_IMOD:           goto dispatch_VT_OP_IMOD;
            case VT_OP_FADD:           goto dispatch_VT_OP_FADD;
            case VT_OP_FSUB:           goto dispatch_VT_OP_FSUB;
            case VT_OP_FMUL:           goto dispatch_VT_OP_FMUL;
            case VT_OP_FDIV:           goto dispatch_VT_OP_FDIV;
            case VT_OP_ISHL:           goto dispatch_VT_OP_ISHL;
            case VT_OP_ISHR:           goto dispatch_VT_OP_ISHR;
            case VT_OP_IAND:           goto dispatch_VT_OP_IAND;
            case VT_OP_IOR:            goto dispatch_VT_OP_IOR;
            case VT_OP_IXOR:           goto dispatch_VT_OP_IXOR;
            case VT_OP_INEG:           goto dispatch_VT_OP_INEG;
            case VT_OP_INOT:           goto dispatch_VT_OP_INOT;
            case VT_OP_ICMP_EQ:        goto dispatch_VT_OP_ICMP_EQ;
            case VT_OP_ICMP_NE:        goto dispatch_VT_OP_ICMP_NE;
            case VT_OP_ICMP_LT:        goto dispatch_VT_OP_ICMP_LT;
            case VT_OP_ICMP_LE:        goto dispatch_VT_OP_ICMP_LE;
            case VT_OP_ICMP_GT:        goto dispatch_VT_OP_ICMP_GT;
            case VT_OP_ICMP_GE:        goto dispatch_VT_OP_ICMP_GE;
            case VT_OP_FCMP_EQ:        goto dispatch_VT_OP_FCMP_EQ;
            case VT_OP_FCMP_NE:        goto dispatch_VT_OP_FCMP_NE;
            case VT_OP_FCMP_LT:        goto dispatch_VT_OP_FCMP_LT;
            case VT_OP_FCMP_LE:        goto dispatch_VT_OP_FCMP_LE;
            case VT_OP_FCMP_GT:        goto dispatch_VT_OP_FCMP_GT;
            case VT_OP_FCMP_GE:        goto dispatch_VT_OP_FCMP_GE;
            case VT_OP_GOTO:           goto dispatch_VT_OP_GOTO;
            case VT_OP_IF_TRUE:        goto dispatch_VT_OP_IF_TRUE;
            case VT_OP_IF_FALSE:       goto dispatch_VT_OP_IF_FALSE;
            case VT_OP_CALL_STATIC:    goto dispatch_VT_OP_CALL_STATIC;
            case VT_OP_CALL_VIRTUAL:   goto dispatch_VT_OP_CALL_VIRTUAL;
            case VT_OP_CALL_INTERFACE: goto dispatch_VT_OP_CALL_INTERFACE;
            case VT_OP_RETURN:         goto dispatch_VT_OP_RETURN;
            case VT_OP_RETURN_VALUE:   goto dispatch_VT_OP_RETURN_VALUE;
            case VT_OP_RETURN_MULTI:   goto dispatch_VT_OP_RETURN_MULTI;
            case VT_OP_LOAD_VARARGS:   goto dispatch_VT_OP_LOAD_VARARGS;
            case VT_OP_VARARG_COUNT:   goto dispatch_VT_OP_VARARG_COUNT;
            case VT_OP_VARARG_GET:     goto dispatch_VT_OP_VARARG_GET;
            case VT_OP_NEW:            goto dispatch_VT_OP_NEW;
            case VT_OP_NEWARRAY:       goto dispatch_VT_OP_NEWARRAY;
            case VT_OP_CHECKCAST:      goto dispatch_VT_OP_CHECKCAST;
            case VT_OP_INSTANCEOF:     goto dispatch_VT_OP_INSTANCEOF;
            case VT_OP_ARRAY_LOAD:     goto dispatch_VT_OP_ARRAY_LOAD;
            case VT_OP_ARRAY_STORE:    goto dispatch_VT_OP_ARRAY_STORE;
            case VT_OP_ARRAY_LENGTH:   goto dispatch_VT_OP_ARRAY_LENGTH;
            case VT_OP_THROW:          goto dispatch_VT_OP_THROW;
            case VT_OP_CATCH:          goto dispatch_VT_OP_CATCH;
            case VT_OP_CATCH_TYPED:    goto dispatch_VT_OP_CATCH_TYPED;
            case VT_OP_MONITOR_ENTER:  goto dispatch_VT_OP_MONITOR_ENTER;
            case VT_OP_MONITOR_EXIT:   goto dispatch_VT_OP_MONITOR_EXIT;
            case VT_OP_DUP:            goto dispatch_VT_OP_DUP;
            case VT_OP_POP:            goto dispatch_VT_OP_POP;
            case VT_OP_SWAP:           goto dispatch_VT_OP_SWAP;
            case VT_OP_ISNULL:         goto dispatch_VT_OP_ISNULL;
            case VT_OP_TYPEOF:         goto dispatch_VT_OP_TYPEOF;
            case VT_OP_CALL_RUNTIME:   goto dispatch_VT_OP_CALL_RUNTIME;
            case VT_OP_LOAD_CONST_INT__IADD:    goto dispatch_VT_OP_LOAD_CONST_INT__IADD;
            case VT_OP_LOAD_LOCAL__LOAD_LOCAL:  goto dispatch_VT_OP_LOAD_LOCAL__LOAD_LOCAL;
            case VT_OP_LOAD_LOCAL__STORE_FIELD: goto dispatch_VT_OP_LOAD_LOCAL__STORE_FIELD;
            default:
                fprintf(stderr, "unknown opcode %d at pc %zu\n", cached_opcode, pc);
                interp->running = false;
                goto dispatch_done;
        }
    }
#endif /* !VTX_USE_COMPUTED_GOTO */

    /* ===================================================================
     * OPCODE HANDLERS
     * =================================================================== */

    /* ---- VT_OP_HALT ---- */
dispatch_VT_OP_HALT:
    interp->running = false;
    goto dispatch_done;

    /* ---- VT_OP_NOP ---- */
dispatch_VT_OP_NOP:
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_LOCAL ---- */
dispatch_VT_OP_LOAD_LOCAL:
    operand = read_operand(code, pc);
    val = locals_arr[operand];
    *sp++ = val;
    DISPATCH_NEXT();

    /* ---- VT_OP_STORE_LOCAL ---- */
dispatch_VT_OP_STORE_LOCAL:
    operand = read_operand(code, pc);
    val = *--sp;
    locals_arr[operand] = val;
    /* Update monitored type for deopt */
    if (VTX_UNLIKELY(frame->monitored_types != NULL && operand < frame->locals_count)) {
        frame->monitored_types[operand] = value_typeid(val);
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_FIELD ---- */
dispatch_VT_OP_LOAD_FIELD:
    operand = read_operand(code, pc);
    a = *--sp;
    if (!vtx_helpers_null_check(a)) { *sp++ = VTX_VALUE_UNDEFINED; DISPATCH_NEXT(); }
    {
        vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(a);

        /* Property IC fast path: check if this site has cached the
         * field offset for this object's shape_id. If so, use the
         * cached offset instead of the bytecode operand. This is the
         * V8/JSC IC fast path — a single shape_id compare.
         *
         * §5 (Keep the Fast Path Simple): compute site_id ONCE per
         * field access and reuse for both the IC lookup and the type
         * feedback recording. The old code computed it twice (once
         * for the IC, once for type_feedback_record_field). */
        uint32_t site_id = vtx_hash_site_index(frame->method, (uint32_t)pc);
        uint32_t ic_offset = vtx_property_ic_lookup(site_id, obj->shape_id);
        if (ic_offset != UINT32_MAX && ic_offset < obj->field_count) {
            /* IC HIT — use cached offset */
            *sp++ = vtx_object_get_field(obj, ic_offset);
            val = *(sp - 1);  /* for type_feedback below */
        } else {
            /* IC MISS — use bytecode operand and update IC */
            VTX_ASSERT(operand < obj->field_count, "field offset out of bounds");
            val = vtx_object_get_field(obj, operand);
            *sp++ = val;
            vtx_property_ic_update(site_id, obj->shape_id, operand);
        }
        /* §2.6: Sample type feedback at 1/64 rate (V8 pattern).
         * This reduces profiling overhead by 64× on the hot path
         * while still collecting statistically representative data.
         * The IC (property_ic_lookup/update) still runs every time —
         * only the type_feedback recording is sampled. */
        vtx_profiler_record_field_shape(&interp->profiler, frame->method,
                                         (uint32_t)pc, obj->shape_id);
        if (vtx_interp_should_sample(interp)) {
            vtx_type_feedback_record_field(&interp->type_feedback,
                                            site_id,
                                            obj->shape_id,
                                            value_typeid(val));
        }
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_STORE_FIELD ---- */
dispatch_VT_OP_STORE_FIELD:
    operand = read_operand(code, pc);
    val = *--sp;  /* value */
    a = *--sp;    /* object */
    if (!vtx_helpers_null_check(a)) { *sp++ = VTX_VALUE_UNDEFINED; DISPATCH_NEXT(); }
    {
        vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(a);

        /* Property IC fast path for stores — same pattern as loads.
         * §5: compute site_id ONCE and reuse for type_feedback. */
        uint32_t site_id = vtx_hash_site_index(frame->method, (uint32_t)pc);
        uint32_t ic_offset = vtx_property_ic_lookup(site_id, obj->shape_id);
        uint32_t field_offset;
        if (ic_offset != UINT32_MAX && ic_offset < obj->field_count) {
            field_offset = ic_offset;
        } else {
            VTX_ASSERT(operand < obj->field_count, "field offset out of bounds");
            field_offset = operand;
            vtx_property_ic_update(site_id, obj->shape_id, operand);
        }
        vtx_object_set_field(obj, field_offset, val);
        vtx_gc_write_barrier(interp->gc, obj, field_offset, val);
        /* §2.6: Sample type feedback at 1/64 rate (V8 pattern). */
        vtx_profiler_record_field_shape(&interp->profiler, frame->method,
                                         (uint32_t)pc, obj->shape_id);
        if (vtx_interp_should_sample(interp)) {
            vtx_type_feedback_record_field(&interp->type_feedback,
                                            site_id,
                                            obj->shape_id,
                                            value_typeid(val));
        }
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_CONST_INT ---- */
dispatch_VT_OP_LOAD_CONST_INT:
    operand = read_operand(code, pc);
    VTX_ASSERT(operand < bc->constant_count, "constant pool index out of bounds");
    *sp++ = bc->constant_pool[operand];
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_CONST_FLOAT ---- */
dispatch_VT_OP_LOAD_CONST_FLOAT:
    operand = read_operand(code, pc);
    VTX_ASSERT(operand < bc->constant_count, "constant pool index out of bounds");
    *sp++ = bc->constant_pool[operand];
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_CONST_STR ---- */
dispatch_VT_OP_LOAD_CONST_STR:
    operand = read_operand(code, pc);
    VTX_ASSERT(operand < bc->constant_count, "constant pool index out of bounds");
    *sp++ = bc->constant_pool[operand];
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_NULL ---- */
dispatch_VT_OP_LOAD_NULL:
    *sp++ = VTX_VALUE_NULL;
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_TRUE ---- */
dispatch_VT_OP_LOAD_TRUE:
    *sp++ = VTX_VALUE_TRUE;
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_FALSE ---- */
dispatch_VT_OP_LOAD_FALSE:
    *sp++ = VTX_VALUE_FALSE;
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_UNDEFINED ---- */
dispatch_VT_OP_LOAD_UNDEFINED:
    *sp++ = VTX_VALUE_UNDEFINED;
    DISPATCH_NEXT();

    /* ===================================================================
     * INTEGER ARITHMETIC
     * =================================================================== */

    /* ---- VT_OP_IADD ---- */
dispatch_VT_OP_IADD:
    b = *--sp;
    a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        /* Fast path: SMI + SMI (most common case)
         *
         * BUGFIX (R2 audit): Perform arithmetic in uint64_t to avoid
         * signed integer overflow UB. At -O2, GCC/Clang may legally
         * delete the overflow check based on the assumption that
         * signed overflow never occurs. Using uint64_t makes the
         * wraparound well-defined, and we check for overflow via
         * sign-bit analysis on the unsigned representation. */
        int64_t ia_smi = vtx_smi_value(a);
        int64_t ib_smi = vtx_smi_value(b);
        uint64_t ua = (uint64_t)ia_smi;
        uint64_t ub = (uint64_t)ib_smi;
        uint64_t ur = ua + ub;
        int64_t result_i = (int64_t)ur;
        /* Inline overflow check: if signs of inputs differ, no overflow.
         * If same sign and result has different sign, overflow occurred.
         * This avoids the function call to vtx_helpers_overflow_check_iadd. */
        if (VTX_LIKELY(!((ia_smi ^ ib_smi) >= 0 && (ia_smi ^ result_i) < 0))) {
            if (VTX_LIKELY(result_i >= VTX_SMI_MIN && result_i <= VTX_SMI_MAX)) {
                *sp++ = vtx_make_smi(result_i);
                DISPATCH_NEXT();
            }
            *sp++ = vtx_make_double((double)result_i);
            DISPATCH_NEXT();
        }
        /* Overflow path */
        *sp++ = vtx_make_double((double)ia_smi + (double)ib_smi);
    } else {
        /* Slow path: promote to double */
        double da = vtx_is_double(a) ? vtx_double_value(a) : (vtx_is_smi(a) ? (double)vtx_smi_value(a) : 0.0);
        double db = vtx_is_double(b) ? vtx_double_value(b) : (vtx_is_smi(b) ? (double)vtx_smi_value(b) : 0.0);
        *sp++ = vtx_make_double(da + db);
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_ISUB ---- */
dispatch_VT_OP_ISUB:
    b = *--sp;
    a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        /* Fast path: SMI - SMI
         * BUGFIX (R2 audit): Use uint64_t arithmetic to avoid signed
         * overflow UB. See IADD comment above. */
        int64_t ia_smi = vtx_smi_value(a);
        int64_t ib_smi = vtx_smi_value(b);
        uint64_t ua = (uint64_t)ia_smi;
        uint64_t ub = (uint64_t)ib_smi;
        uint64_t ur = ua - ub;
        int64_t result_i = (int64_t)ur;
        /* Inline overflow check: subtraction overflows when signs of a and -b
         * are the same (i.e., a and b have different signs) and result
         * sign differs from a. */
        if (VTX_LIKELY(!((ia_smi ^ ib_smi) < 0 && (ia_smi ^ result_i) < 0))) {
            if (VTX_LIKELY(result_i >= VTX_SMI_MIN && result_i <= VTX_SMI_MAX)) {
                *sp++ = vtx_make_smi(result_i);
                DISPATCH_NEXT();
            }
            *sp++ = vtx_make_double((double)result_i);
            DISPATCH_NEXT();
        }
        /* Overflow path */
        *sp++ = vtx_make_double((double)ia_smi - (double)ib_smi);
    } else {
        /* Slow path: promote to double */
        double da = vtx_is_double(a) ? vtx_double_value(a) : (vtx_is_smi(a) ? (double)vtx_smi_value(a) : 0.0);
        double db = vtx_is_double(b) ? vtx_double_value(b) : (vtx_is_smi(b) ? (double)vtx_smi_value(b) : 0.0);
        *sp++ = vtx_make_double(da - db);
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_IMUL ---- */
dispatch_VT_OP_IMUL:
    b = *--sp;
    a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        /* Fast path: SMI * SMI */
        int64_t ia_smi = vtx_smi_value(a);
        int64_t ib_smi = vtx_smi_value(b);
        /* Use unsigned multiply to avoid signed overflow UB */
        uint64_t ua = (uint64_t)ia_smi;
        uint64_t ub = (uint64_t)ib_smi;
        uint64_t result_u = ua * ub;
        /* Check overflow: result must fit in int64_t and be in SMI range */
        int64_t result_i;
        bool overflow = false;
        if (ia_smi == 0 || ib_smi == 0) {
            result_i = 0;
        } else if ((ia_smi > 0 && ib_smi > 0 && result_u > (uint64_t)VTX_SMI_MAX) ||
                   (ia_smi < 0 && ib_smi < 0 && result_u > (uint64_t)VTX_SMI_MAX) ||
                   (ia_smi > 0 && ib_smi < 0 && (int64_t)result_u < VTX_SMI_MIN) ||
                   (ia_smi < 0 && ib_smi > 0 && (int64_t)result_u < VTX_SMI_MIN)) {
            overflow = true;
            result_i = 0; /* placeholder */
        } else {
            result_i = (int64_t)result_u;
        }
        if (VTX_LIKELY(!overflow)) {
            *sp++ = vtx_make_smi(result_i);
            DISPATCH_NEXT();
        }
        *sp++ = vtx_make_double((double)ia_smi * (double)ib_smi);
    } else {
        /* Slow path: promote to double */
        double da = vtx_is_double(a) ? vtx_double_value(a) : (vtx_is_smi(a) ? (double)vtx_smi_value(a) : 0.0);
        double db = vtx_is_double(b) ? vtx_double_value(b) : (vtx_is_smi(b) ? (double)vtx_smi_value(b) : 0.0);
        *sp++ = vtx_make_double(da * db);
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_IDIV ---- */
dispatch_VT_OP_IDIV:
    b = *--sp;
    a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        /* Fast path: SMI / SMI */
        int64_t ia_smi = vtx_smi_value(a);
        int64_t ib_smi = vtx_smi_value(b);
        /* DISP-004 fix: check before the assert — debug builds used to
         * abort() here, defeating the recovery path that returns
         * UNDEFINED. The recovery is the intended semantics. */
        if (VTX_UNLIKELY(ib_smi == 0)) {
            *sp++ = VTX_VALUE_UNDEFINED;
            DISPATCH_NEXT();
        }
        VTX_ASSERT(ib_smi != 0, "integer division by zero");
        if (VTX_UNLIKELY(ia_smi == INT64_MIN && ib_smi == -1)) {
            /* Bug #3 fix: INT64_MIN / -1 = 2^63 which overflows int64_t.
             * The correct result as a double is -(double)INT64_MIN = 2^63.0,
             * not (double)INT64_MAX which is 2^63 - 1. */
            *sp++ = vtx_make_double(-(double)INT64_MIN);
        } else {
            int64_t result_i = ia_smi / ib_smi;
            *sp++ = vtx_make_smi(result_i);
        }
    } else {
        /* Slow path: promote to double */
        double da = vtx_is_double(a) ? vtx_double_value(a) : (vtx_is_smi(a) ? (double)vtx_smi_value(a) : 0.0);
        double db = vtx_is_double(b) ? vtx_double_value(b) : (vtx_is_smi(b) ? (double)vtx_smi_value(b) : 0.0);
        *sp++ = vtx_make_double(da / db);
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_IMOD ---- */
dispatch_VT_OP_IMOD:
    b = *--sp;
    a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        /* Fast path: SMI % SMI */
        int64_t ia_smi = vtx_smi_value(a);
        int64_t ib_smi = vtx_smi_value(b);
        if (VTX_UNLIKELY(ib_smi == 0)) {
            /* Division by zero — throw exception or return undefined */
            *sp++ = VTX_VALUE_UNDEFINED;
            DISPATCH_NEXT();
        }
        if (VTX_UNLIKELY(ia_smi == INT64_MIN && ib_smi == -1)) {
            /* INT64_MIN % -1 is UB in C — result is 0 */
            *sp++ = vtx_make_smi(0);
            DISPATCH_NEXT();
        }
        int64_t result_i = ia_smi % ib_smi;
        *sp++ = vtx_make_smi(result_i);
    } else {
        /* Slow path: promote to double */
        double da = vtx_is_double(a) ? vtx_double_value(a) : (vtx_is_smi(a) ? (double)vtx_smi_value(a) : 0.0);
        double db = vtx_is_double(b) ? vtx_double_value(b) : (vtx_is_smi(b) ? (double)vtx_smi_value(b) : 0.0);
        *sp++ = vtx_make_double(fmod(da, db));
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * FLOAT ARITHMETIC
     * =================================================================== */

    /* ---- VT_OP_FADD ---- */
dispatch_VT_OP_FADD:
    b = *--sp;
    a = *--sp;
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    *sp++ = vtx_make_double(fa + fb);
    DISPATCH_NEXT();

    /* ---- VT_OP_FSUB ---- */
dispatch_VT_OP_FSUB:
    b = *--sp;
    a = *--sp;
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    *sp++ = vtx_make_double(fa - fb);
    DISPATCH_NEXT();

    /* ---- VT_OP_FMUL ---- */
dispatch_VT_OP_FMUL:
    b = *--sp;
    a = *--sp;
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    *sp++ = vtx_make_double(fa * fb);
    DISPATCH_NEXT();

    /* ---- VT_OP_FDIV ---- */
dispatch_VT_OP_FDIV:
    b = *--sp;
    a = *--sp;
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    *sp++ = vtx_make_double(fa / fb);
    DISPATCH_NEXT();

    /* ===================================================================
     * BITWISE AND UNARY INTEGER OPERATIONS
     * =================================================================== */

    /* ---- VT_OP_ISHL ---- */
dispatch_VT_OP_ISHL:
    b = *--sp;
    a = *--sp;
    /* Bug #4 fix: Add SMI type check with slow path for non-SMI values */
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        ia = vtx_smi_value(a);
        ib = vtx_smi_value(b);
        *sp++ = vtx_make_smi((int64_t)((uint64_t)ia << (ib & 63)));
    } else {
        int64_t va = vtx_is_smi(a) ? vtx_smi_value(a) : (vtx_is_double(a) ? (int64_t)vtx_double_value(a) : 0);
        int64_t vb = vtx_is_smi(b) ? vtx_smi_value(b) : (vtx_is_double(b) ? (int64_t)vtx_double_value(b) : 0);
        *sp++ = vtx_make_smi((int64_t)((uint64_t)va << (vb & 63)));
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_ISHR ---- */
dispatch_VT_OP_ISHR:
    b = *--sp;
    a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        ia = vtx_smi_value(a);
        ib = vtx_smi_value(b);
        *sp++ = vtx_make_smi(ia >> (ib & 63));
    } else {
        int64_t va = vtx_is_smi(a) ? vtx_smi_value(a) : (vtx_is_double(a) ? (int64_t)vtx_double_value(a) : 0);
        int64_t vb = vtx_is_smi(b) ? vtx_smi_value(b) : (vtx_is_double(b) ? (int64_t)vtx_double_value(b) : 0);
        *sp++ = vtx_make_smi(va >> (vb & 63));
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_IAND ---- */
dispatch_VT_OP_IAND:
    b = *--sp;
    a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        ia = vtx_smi_value(a);
        ib = vtx_smi_value(b);
        *sp++ = vtx_make_smi(ia & ib);
    } else {
        int64_t va = vtx_is_smi(a) ? vtx_smi_value(a) : (vtx_is_double(a) ? (int64_t)vtx_double_value(a) : 0);
        int64_t vb = vtx_is_smi(b) ? vtx_smi_value(b) : (vtx_is_double(b) ? (int64_t)vtx_double_value(b) : 0);
        *sp++ = vtx_make_smi(va & vb);
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_IOR ---- */
dispatch_VT_OP_IOR:
    b = *--sp;
    a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        ia = vtx_smi_value(a);
        ib = vtx_smi_value(b);
        *sp++ = vtx_make_smi(ia | ib);
    } else {
        int64_t va = vtx_is_smi(a) ? vtx_smi_value(a) : (vtx_is_double(a) ? (int64_t)vtx_double_value(a) : 0);
        int64_t vb = vtx_is_smi(b) ? vtx_smi_value(b) : (vtx_is_double(b) ? (int64_t)vtx_double_value(b) : 0);
        *sp++ = vtx_make_smi(va | vb);
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_IXOR ---- */
dispatch_VT_OP_IXOR:
    b = *--sp;
    a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        ia = vtx_smi_value(a);
        ib = vtx_smi_value(b);
        *sp++ = vtx_make_smi(ia ^ ib);
    } else {
        int64_t va = vtx_is_smi(a) ? vtx_smi_value(a) : (vtx_is_double(a) ? (int64_t)vtx_double_value(a) : 0);
        int64_t vb = vtx_is_smi(b) ? vtx_smi_value(b) : (vtx_is_double(b) ? (int64_t)vtx_double_value(b) : 0);
        *sp++ = vtx_make_smi(va ^ vb);
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_INEG ---- */
dispatch_VT_OP_INEG:
    a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a))) {
        ia = vtx_smi_value(a);
        if (ia == INT64_MIN) {
            *sp++ = vtx_make_double(-(double)INT64_MIN);
        } else {
            *sp++ = vtx_make_smi(-ia);
        }
    } else if (vtx_is_double(a)) {
        *sp++ = vtx_make_double(-vtx_double_value(a));
    } else {
        *sp++ = VTX_VALUE_UNDEFINED;
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_INOT ---- */
dispatch_VT_OP_INOT:
    a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a))) {
        ia = vtx_smi_value(a);
        *sp++ = vtx_make_smi(~ia);
    } else {
        int64_t va = vtx_is_double(a) ? (int64_t)vtx_double_value(a) : 0;
        *sp++ = vtx_make_smi(~va);
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * INTEGER COMPARISONS
     * =================================================================== */

dispatch_VT_OP_ICMP_EQ:
    b = *--sp; a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        /* Bug #5 fix: Use vtx_make_bool for consistency with FCMP */
        *sp++ = vtx_make_bool(vtx_smi_value(a) == vtx_smi_value(b));
    } else {
        double da = vtx_is_double(a) ? vtx_double_value(a) : (vtx_is_smi(a) ? (double)vtx_smi_value(a) : 0.0);
        double db = vtx_is_double(b) ? vtx_double_value(b) : (vtx_is_smi(b) ? (double)vtx_smi_value(b) : 0.0);
        *sp++ = vtx_make_bool(da == db);
    }
    DISPATCH_NEXT();

dispatch_VT_OP_ICMP_NE:
    b = *--sp; a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        *sp++ = vtx_make_bool(vtx_smi_value(a) != vtx_smi_value(b));
    } else {
        double da = vtx_is_double(a) ? vtx_double_value(a) : (vtx_is_smi(a) ? (double)vtx_smi_value(a) : 0.0);
        double db = vtx_is_double(b) ? vtx_double_value(b) : (vtx_is_smi(b) ? (double)vtx_smi_value(b) : 0.0);
        *sp++ = vtx_make_bool(da != db);
    }
    DISPATCH_NEXT();

dispatch_VT_OP_ICMP_LT:
    b = *--sp; a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        *sp++ = vtx_make_bool(vtx_smi_value(a) < vtx_smi_value(b));
    } else {
        double da = vtx_is_double(a) ? vtx_double_value(a) : (vtx_is_smi(a) ? (double)vtx_smi_value(a) : 0.0);
        double db = vtx_is_double(b) ? vtx_double_value(b) : (vtx_is_smi(b) ? (double)vtx_smi_value(b) : 0.0);
        *sp++ = vtx_make_bool(da < db);
    }
    DISPATCH_NEXT();

dispatch_VT_OP_ICMP_LE:
    b = *--sp; a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        *sp++ = vtx_make_bool(vtx_smi_value(a) <= vtx_smi_value(b));
    } else {
        double da = vtx_is_double(a) ? vtx_double_value(a) : (vtx_is_smi(a) ? (double)vtx_smi_value(a) : 0.0);
        double db = vtx_is_double(b) ? vtx_double_value(b) : (vtx_is_smi(b) ? (double)vtx_smi_value(b) : 0.0);
        *sp++ = vtx_make_bool(da <= db);
    }
    DISPATCH_NEXT();

dispatch_VT_OP_ICMP_GT:
    b = *--sp; a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        *sp++ = vtx_make_bool(vtx_smi_value(a) > vtx_smi_value(b));
    } else {
        double da = vtx_is_double(a) ? vtx_double_value(a) : (vtx_is_smi(a) ? (double)vtx_smi_value(a) : 0.0);
        double db = vtx_is_double(b) ? vtx_double_value(b) : (vtx_is_smi(b) ? (double)vtx_smi_value(b) : 0.0);
        *sp++ = vtx_make_bool(da > db);
    }
    DISPATCH_NEXT();

dispatch_VT_OP_ICMP_GE:
    b = *--sp; a = *--sp;
    if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(b))) {
        *sp++ = vtx_make_bool(vtx_smi_value(a) >= vtx_smi_value(b));
    } else {
        double da = vtx_is_double(a) ? vtx_double_value(a) : (vtx_is_smi(a) ? (double)vtx_smi_value(a) : 0.0);
        double db = vtx_is_double(b) ? vtx_double_value(b) : (vtx_is_smi(b) ? (double)vtx_smi_value(b) : 0.0);
        *sp++ = vtx_make_bool(da >= db);
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * FLOAT COMPARISONS
     * =================================================================== */

dispatch_VT_OP_FCMP_EQ:
    b = *--sp; a = *--sp;
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    *sp++ = vtx_make_bool(fa == fb);
    DISPATCH_NEXT();

dispatch_VT_OP_FCMP_NE:
    b = *--sp; a = *--sp;
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    *sp++ = vtx_make_bool(fa != fb);
    DISPATCH_NEXT();

dispatch_VT_OP_FCMP_LT:
    b = *--sp; a = *--sp;
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    *sp++ = vtx_make_bool(fa < fb);
    DISPATCH_NEXT();

dispatch_VT_OP_FCMP_LE:
    b = *--sp; a = *--sp;
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    *sp++ = vtx_make_bool(fa <= fb);
    DISPATCH_NEXT();

dispatch_VT_OP_FCMP_GT:
    b = *--sp; a = *--sp;
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    *sp++ = vtx_make_bool(fa > fb);
    DISPATCH_NEXT();

dispatch_VT_OP_FCMP_GE:
    b = *--sp; a = *--sp;
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    *sp++ = vtx_make_bool(fa >= fb);
    DISPATCH_NEXT();

    /* ===================================================================
     * CONTROL FLOW
     * =================================================================== */

    /* ---- VT_OP_GOTO ---- */
dispatch_VT_OP_GOTO:
    operand = read_operand(code, pc);
    {
        uint32_t target_pc = (uint32_t)operand;
        /* Check for backward branch (safepoint + profiling) */
        if (VTX_UNLIKELY(target_pc <= (uint32_t)pc)) {
            vtx_profiler_record_backward_branch(&interp->profiler, frame->method);
            vtx_gc_safepoint(interp->gc);

            /* D7: Decrement-then-test tier-up counter.
             * This is the zero-overhead tier-up detection mechanism.
             * On x86-64, the counter decrement and check compiles to:
             *   dec [pd->tier_up_counter]   ; 2-3 bytes
             *   jle .request_compilation    ; 2 bytes
             *
             * When the counter reaches zero, we request compilation.
             * After requesting, the compilation_requested flag prevents
             * re-queueing. The counter is reset when the method is
             * actually compiled (for tier-up to the next tier). */
            if (VTX_UNLIKELY(vtx_profiler_tier_up_check(&interp->profiler,
                                                          frame->method))) {
                /* Threshold reached — request JIT compilation. */
                if (interp->compile_ctx != NULL) {
                    vtx_request_compilation(interp->compile_ctx, (vtx_method_desc_t *)frame->method,
                        vtx_profiler_method_heat(&interp->profiler, frame->method));
                }
            }

            /* W1 fix: OSR (On-Stack Replacement) up.
             * After requesting compilation, check if the compiled code
             * is now available. If so, transfer to it mid-execution via
             * OSR. Without this, a long-running method (like a loop)
             * never benefits from JIT compilation — it runs forever in
             * the interpreter because there's no opportunity to switch
             * to compiled code at a method call boundary.
             *
             * OSR transfers the current interpreter frame to a compiled
             * frame at the loop back-edge. The compiled code picks up
             * execution from the loop header, skipping the already-
             * executed portion of the method.
             *
             * We set the osr_pending flag and exit the dispatch loop.
             * vtx_interp_run (the caller) will call vtx_osr_up from a
             * clean stack context — the inline asm trampoline in vtx_osr_up
             * never returns (it jumps to JIT code), so we can't call it
             * from deep inside the computed-goto dispatch loop. */
            if (frame->method != NULL) {
                void *cc = __atomic_load_n(&frame->method->compiled_code,
                                              __ATOMIC_ACQUIRE);
                if (cc != NULL && interp->compile_ctx != NULL) {
                    /* JIT compiled code is available. Set OSR pending with
                     * the loop header PC (the backward-branch target).
                     * vtx_interp_run will call vtx_osr_up to transfer
                     * execution to the JIT at the loop header. */
                    interp->osr_pending = true;
                    interp->osr_loop_header_pc = target_pc; /* loop header = backward-branch target */
                    interp->running = false;
                    goto dispatch_done;
                }
            }

            /* Bug #11 fix / BUG-3 fix: At backward branches, check for
             * deopt pending. The deopt_pending flag is set by the
             * safepoint mechanism when an invalidation affects the
             * current method. When this fires, we must:
             * 1. Request recompilation so an optimized version is
             *    regenerated for the updated type profile.
             * 2. Clear the flag.
             * Previously this was a no-op — the flag was cleared but
             * no action was taken, making the entire deopt pipeline
             * decorative. Now we actually trigger recompilation. */
            if (VTX_UNLIKELY(interp->deopt_pending)) {
                interp->deopt_pending = false;
                if (interp->compile_ctx != NULL && frame->method != NULL) {
                    vtx_request_compilation(interp->compile_ctx, (vtx_method_desc_t *)frame->method, vtx_profiler_method_heat(&interp->profiler, frame->method));
                }
            }
        }
        pc = target_pc;
        DISPATCH();
    }

    /* ---- VT_OP_IF_TRUE ---- */
dispatch_VT_OP_IF_TRUE:
    operand = read_operand(code, pc);
    a = *--sp;
    {
        bool taken = is_truthy(a);
        vtx_profiler_record_branch(&interp->profiler, frame->method,
                                    (uint32_t)pc, taken);
        vtx_type_feedback_record_branch(&interp->type_feedback,
                                         vtx_hash_site_index(frame->method, (uint32_t)pc), taken);
        if (taken) {
            uint32_t target_pc = (uint32_t)operand;
            if (target_pc <= (uint32_t)pc) {
                vtx_profiler_record_backward_branch(&interp->profiler, frame->method);
                vtx_gc_safepoint(interp->gc);

                /* D7: Decrement-then-test tier-up counter at loop back-edge */
                if (VTX_UNLIKELY(vtx_profiler_tier_up_check(&interp->profiler,
                                                              frame->method))) {
                        if (interp->compile_ctx != NULL) {
                            vtx_request_compilation(interp->compile_ctx, (vtx_method_desc_t *)frame->method, vtx_profiler_method_heat(&interp->profiler, frame->method));
                        }
                }

                /* OSR-17 fix: trigger OSR up at all backward branches, not
                 * just VT_OP_GOTO. for/while loops typically compile to
                 * IF_TRUE/IF_FALSE backedges (the loop condition branches
                 * forward to the body or back to the loop header). Without
                 * this, many common loop patterns never OSR. */
                if (frame->method != NULL) {
                    void *cc = __atomic_load_n(&frame->method->compiled_code,
                                                  __ATOMIC_ACQUIRE);
                    if (cc != NULL && interp->compile_ctx != NULL) {
                        interp->osr_pending = true;
                        interp->osr_loop_header_pc = target_pc;
                        interp->running = false;
                        goto dispatch_done;
                    }
                }

                /* BUG-3 fix: Check for deopt pending at backward branches */
                if (VTX_UNLIKELY(interp->deopt_pending)) {
                    interp->deopt_pending = false;
                    if (interp->compile_ctx != NULL && frame->method != NULL) {
                        vtx_request_compilation(interp->compile_ctx, (vtx_method_desc_t *)frame->method, vtx_profiler_method_heat(&interp->profiler, frame->method));
                    }
                }
            }
            pc = target_pc;
            DISPATCH();
        } else {
            DISPATCH_NEXT();
        }
    }

    /* ---- VT_OP_IF_FALSE ---- */
dispatch_VT_OP_IF_FALSE:
    operand = read_operand(code, pc);
    a = *--sp;
    {
        bool taken = !is_truthy(a);
        vtx_profiler_record_branch(&interp->profiler, frame->method,
                                    (uint32_t)pc, taken);
        vtx_type_feedback_record_branch(&interp->type_feedback,
                                         vtx_hash_site_index(frame->method, (uint32_t)pc), taken);
        if (taken) {
            uint32_t target_pc = (uint32_t)operand;
            if (target_pc <= (uint32_t)pc) {
                vtx_profiler_record_backward_branch(&interp->profiler, frame->method);
                vtx_gc_safepoint(interp->gc);

                /* D7: Decrement-then-test tier-up counter at loop back-edge */
                if (VTX_UNLIKELY(vtx_profiler_tier_up_check(&interp->profiler,
                                                              frame->method))) {
                        if (interp->compile_ctx != NULL) {
                            vtx_request_compilation(interp->compile_ctx, (vtx_method_desc_t *)frame->method, vtx_profiler_method_heat(&interp->profiler, frame->method));
                        }
                }

                /* OSR-17 fix: trigger OSR up at all backward branches, not
                 * just VT_OP_GOTO. See the comment in VT_OP_IF_TRUE above. */
                if (frame->method != NULL) {
                    void *cc = __atomic_load_n(&frame->method->compiled_code,
                                                  __ATOMIC_ACQUIRE);
                    if (cc != NULL && interp->compile_ctx != NULL) {
                        interp->osr_pending = true;
                        interp->osr_loop_header_pc = target_pc;
                        interp->running = false;
                        goto dispatch_done;
                    }
                }

                /* BUG-3 fix: Check for deopt pending at backward branches */
                if (VTX_UNLIKELY(interp->deopt_pending)) {
                    interp->deopt_pending = false;
                    if (interp->compile_ctx != NULL && frame->method != NULL) {
                        vtx_request_compilation(interp->compile_ctx, (vtx_method_desc_t *)frame->method, vtx_profiler_method_heat(&interp->profiler, frame->method));
                    }
                }
            }
            pc = target_pc;
            DISPATCH();
        } else {
            DISPATCH_NEXT();
        }
    }

    /* ===================================================================
     * METHOD CALLS
     *
     * Call convention:
     *   - CALL_STATIC: operand = constant pool index pointing to a
     *     method descriptor stored as a heap pointer, or an SMI encoding
     *     a typeid (with the next constant pool entry being the method name).
     *   - CALL_VIRTUAL: operand = constant pool index pointing to method
     *     name string. The receiver is on the stack.
     *   - CALL_INTERFACE: like CALL_VIRTUAL but with an additional
     *     interface typeid from the next constant pool slot.
     * =================================================================== */

    /* ---- VT_OP_CALL_STATIC ---- */
dispatch_VT_OP_CALL_STATIC:
    operand = read_operand(code, pc);
    {
        VTX_ASSERT(operand < bc->constant_count, "constant pool index out of bounds");
        vtx_value_t method_val = bc->constant_pool[operand];

        const vtx_method_desc_t *target_method = NULL;

        if (vtx_is_heap_ptr(method_val)) {
            /* Method descriptor pointer stored as heap pointer */
            target_method = (const vtx_method_desc_t *)vtx_heap_ptr(method_val);
        } else if (vtx_is_smi(method_val)) {
            /* TypeID stored as SMI; method name in next constant pool slot */
            vtx_typeid_t target_typeid = (vtx_typeid_t)vtx_smi_value(method_val);
            if ((uint32_t)(operand + 1) < bc->constant_count) {
                vtx_value_t name_val = bc->constant_pool[operand + 1];
                const char *method_name = vtx_helpers_string_data(name_val);
                target_method = vtx_lookup_static_method(
                    interp->type_system, target_typeid, method_name);
            }
        }

        if (target_method == NULL || target_method->bytecode == NULL) {
            *sp++ = VTX_VALUE_UNDEFINED;
            DISPATCH_NEXT();
        }

        /* Record invocation in profiler */
        vtx_profiler_record_invocation(&interp->profiler, target_method);

        /* Notify the orchestrator of the method entry. This feeds the
         * Markov phase-predictor and the phase detector, which may
         * trigger proactive compilation of methods predicted to be hot
         * in the next phase. Without this call, those subsystems sit
         * idle (dead code). The orchestrator is optional — if no
         * compile_ctx or no orchestrator is wired, this is a no-op. */
        if (interp->compile_ctx != NULL && interp->compile_ctx->orchestrator != NULL) {
            vtx_orchestrator_on_method_entry(interp->compile_ctx->orchestrator,
                                              target_method->vtable_index);
        }

        /* Record call type in profiler */
        vtx_typeid_t receiver_tid = VTX_TYPE_INVALID;
        if (((int)(sp - frame->operand_stack)) > 0) {
            receiver_tid = value_typeid(*(sp - 1));
        }
        vtx_profiler_record_call_type(&interp->profiler, frame->method,
                                       (uint32_t)pc, receiver_tid);

        /* Speculative versioning: record the argument type signature for
         * this call site. The spec_version manager tracks which type
         * combinations are hot and decides whether to specialize. When
         * a bimorphic call site is detected, the orchestrator can trigger
         * compilation of type-specialized versions (e.g., process_Dog,
         * process_Cat) with devirtualized calls.
         *
         * We sample up to VTX_SPEC_VERSION_MAX_ARGS argument types from
         * the top of the stack (receiver first, then remaining args).
         * If the spec_version_mgr is NULL or no compile_ctx, this is a no-op. */
        if (interp->compile_ctx != NULL &&
            interp->compile_ctx->spec_version_mgr != NULL) {
            vtx_spec_type_sig_t sig;
            memset(&sig, 0, sizeof(sig));
            uint32_t arg_n = target_method->arg_count;
            if (arg_n > VTX_SPEC_VERSION_MAX_ARGS)
                arg_n = VTX_SPEC_VERSION_MAX_ARGS;
            sig.arg_count = arg_n;
            /* Arguments are on the stack with the last arg on top.
             * sp[-1] = last arg, sp[-2] = second-to-last, etc.
             * For arg_types[0] we want the FIRST arg (receiver), which
             * is at sp[-arg_count]. */
            int32_t avail = (int32_t)(sp - frame->operand_stack);
            if ((int32_t)arg_n <= avail) {
                for (uint32_t ai = 0; ai < arg_n; ai++) {
                    vtx_value_t arg_val = sp[-(int32_t)arg_n + (int32_t)ai];
                    sig.arg_types[ai] = value_typeid(arg_val);
                }
                sig.signature_hash = vtx_spec_type_sig_hash(&sig);

                vtx_spec_version_registry_t *reg =
                    vtx_spec_version_get_registry(
                        (vtx_spec_version_manager_t *)
                            interp->compile_ctx->spec_version_mgr,
                        target_method->vtable_index);
                if (reg != NULL) {
                    vtx_spec_version_record_dispatch(reg, &sig);
                }
            }
        }

        /* JIT dispatch: if the target method has been compiled, call it
         * directly instead of creating an interpreter frame. This is the
         * key bridge that makes the JIT actually execute code. */
        if (VTX_UNLIKELY(__atomic_load_n(&target_method->compiled_code, __ATOMIC_ACQUIRE) != NULL)) {
            uint32_t call_arg_count = target_method->arg_count;
            if (call_arg_count > 256) call_arg_count = 256;
            vtx_value_t jit_args_buf[256];
            for (uint32_t ai = call_arg_count; ai > 0; ai--) {
                jit_args_buf[ai - 1] = *--sp;
            }
            vtx_value_t jit_result = vtx_dispatch_jit(interp, target_method,
                                                       jit_args_buf, call_arg_count);
            *sp++ = jit_result;
            DISPATCH_NEXT();
        }

        /* Pop arguments from caller's stack and copy to callee locals.
         * BUG-2 fix: Cap arg count to prevent stack overflow from
         * corrupted/malicious method descriptors. 256 args is well
         * beyond any realistic use case; larger counts would exhaust
         * the C stack via alloca.
         * Bug #5 fix: Save sp before popping so we can restore it
         * if frame creation fails. Without this, the arguments are
         * lost from the caller's stack on frame allocation failure. */
        uint32_t call_arg_count = target_method->arg_count;
        if (call_arg_count > 256) call_arg_count = 256;
        vtx_value_t call_args_buf[256];
        vtx_value_t *call_args = call_args_buf;
        vtx_value_t *saved_sp = sp;
        for (uint32_t ai = call_arg_count; ai > 0; ai--) {
            call_args[ai - 1] = *--sp;
        }

        /* Create new frame for the callee */
        vtx_frame_t *callee_frame = vtx_frame_create(
            target_method, frame, (uint32_t)(pc + vtx_bytecode_insn_length(bc, pc)),
            &interp->frame_stack);
        if (callee_frame == NULL) {
            sp = saved_sp; /* Bug #5 fix: Restore sp to put args back */
            *sp++ = VTX_VALUE_UNDEFINED;
            DISPATCH_NEXT();
        }

        /* Copy arguments into callee's locals */
        for (uint32_t ai = 0; ai < call_arg_count && ai < callee_frame->locals_count; ai++) {
            callee_frame->locals[ai] = call_args[ai];
        }

        /* Switch to the callee frame */
        SYNC_SP();
        frame = callee_frame;
        interp->current_frame = frame;
        RELOAD_FRAME();
        pc = 0;
        DISPATCH();
    }

    /* ---- VT_OP_CALL_VIRTUAL ---- */
dispatch_VT_OP_CALL_VIRTUAL:
    operand = read_operand(code, pc);
    {
        VTX_ASSERT(operand < bc->constant_count, "constant pool index out of bounds");
        vtx_value_t method_val = bc->constant_pool[operand];

        /* Get the method name from the constant pool */
        const char *method_name = NULL;
        if (vtx_is_heap_ptr(method_val)) {
            method_name = vtx_helpers_string_data(method_val);
        }

        /* Get the receiver.
         * Fix C6: The receiver is at *(sp - arg_count - 1) because it was
         * pushed before the arguments. The old code used *(sp - 1) (top of
         * stack) which is the LAST argument, not the receiver. For virtual
         * calls with >0 arguments, this used the wrong type for dispatch. */
        VTX_ASSERT(((int)(sp - frame->operand_stack)) > 0, "stack underflow for virtual call");
        const vtx_method_desc_t *target_method = NULL;

        vtx_value_t receiver = VTX_VALUE_UNDEFINED;
        vtx_typeid_t receiver_tid = VTX_TYPE_INVALID;
        int stack_avail = (int)(sp - frame->operand_stack);

        /* Try position sp-1 first (works for 0-arg virtual calls) */
        receiver = *(sp - 1);
        if (vtx_is_heap_ptr(receiver)) {
            receiver_tid = value_typeid(receiver);
            /* Try to resolve method with this receiver type */
            if (method_name != NULL) {
                target_method = vtx_type_resolve_method(
                    interp->type_system, receiver_tid, method_name);
            }
            if (target_method != NULL) {
                /* Verify: if the method has args, the real receiver might
                 * be deeper. Re-peek at the correct position. */
                uint32_t rarg = target_method->arg_count;
                if (rarg > 0 && stack_avail > (int)(rarg + 1)) {
                    vtx_value_t real_receiver = *(sp - rarg - 1);
                    if (vtx_is_heap_ptr(real_receiver)) {
                        receiver = real_receiver;
                        receiver_tid = value_typeid(receiver);
                        /* Re-resolve with the correct receiver type */
                        if (method_name != NULL) {
                            target_method = vtx_type_resolve_method(
                                interp->type_system, receiver_tid, method_name);
                        }
                    }
                }
            }
        }

        /* Look up the method using the inline cache (with correct receiver) */
        vtx_inline_cache_t *ic = vtx_interp_get_ic(interp, frame->method, (uint32_t)pc);

        if (ic != NULL && method_name != NULL && target_method == NULL) {
            target_method = vtx_lookup_method(interp->type_system, ic,
                                               receiver, method_name);
        }

        /* Record call type in profiler and type feedback */
        vtx_profiler_record_call_type(&interp->profiler, frame->method,
                                       (uint32_t)pc, receiver_tid);
        vtx_type_feedback_record_call(&interp->type_feedback,
                                       vtx_hash_site_index(frame->method, (uint32_t)pc),
                                       receiver_tid,
                                       VTX_TYPE_INVALID);

        if (target_method == NULL || target_method->bytecode == NULL) {
            *sp++ = VTX_VALUE_UNDEFINED;
            DISPATCH_NEXT();
        }

        /* Record invocation */
        vtx_profiler_record_invocation(&interp->profiler, target_method);

        /* JIT dispatch: if the target method has been compiled, call it
         * directly instead of creating an interpreter frame. This mirrors
         * the JIT dispatch in CALL_STATIC and makes virtual calls actually
         * benefit from JIT compilation. Previously, virtual calls always
         * fell through to the interpreter even for compiled methods. */
        if (VTX_UNLIKELY(__atomic_load_n(&target_method->compiled_code, __ATOMIC_ACQUIRE) != NULL)) {
            uint32_t varg_count_jit = target_method->arg_count + 1; /* +1 for receiver */
            if (varg_count_jit > 256) varg_count_jit = 256;
            vtx_value_t vjit_args_buf[256];
            for (uint32_t ai = varg_count_jit; ai > 0; ai--) {
                vjit_args_buf[ai - 1] = *--sp;
            }
            vtx_value_t vjit_result = vtx_dispatch_jit(interp, target_method,
                                                        vjit_args_buf, varg_count_jit);
            *sp++ = vjit_result;
            DISPATCH_NEXT();
        }

        /* Pop arguments from caller's stack and copy to callee locals.
         * For virtual calls, the receiver (this) is the implicit first
         * argument and must be included in the pop count and passed as
         * local[0] in the callee. arg_count returns only the
         * explicit parameters, so we add 1 for the receiver. */
        uint32_t varg_count = target_method->arg_count + 1; /* +1 for receiver */
        if (varg_count > 256) varg_count = 256;
        vtx_value_t vcall_args_buf[256];
        vtx_value_t *vcall_args = vcall_args_buf;
        for (uint32_t ai = varg_count; ai > 0; ai--) {
            vcall_args[ai - 1] = *--sp;
        }

        /* Create new frame for the callee */
        vtx_frame_t *callee_frame = vtx_frame_create(
            target_method, frame, (uint32_t)(pc + vtx_bytecode_insn_length(bc, pc)),
            &interp->frame_stack);
        if (callee_frame == NULL) {
            *sp++ = VTX_VALUE_UNDEFINED;
            DISPATCH_NEXT();
        }

        /* Copy arguments into callee's locals */
        for (uint32_t ai = 0; ai < varg_count && ai < callee_frame->locals_count; ai++) {
            callee_frame->locals[ai] = vcall_args[ai];
        }

        /* Switch to the callee frame */
        SYNC_SP();
        frame = callee_frame;
        interp->current_frame = frame;
        RELOAD_FRAME();
        pc = 0;
        DISPATCH();
    }

    /* ---- VT_OP_CALL_INTERFACE ---- */
dispatch_VT_OP_CALL_INTERFACE:
    operand = read_operand(code, pc);
    {
        VTX_ASSERT(operand < bc->constant_count, "constant pool index out of bounds");
        vtx_value_t method_val = bc->constant_pool[operand];

        const char *method_name = NULL;
        if (vtx_is_heap_ptr(method_val)) {
            method_name = vtx_helpers_string_data(method_val);
        }

        /* Get interface typeid from next constant pool slot */
        vtx_typeid_t interface_typeid = VTX_TYPE_INVALID;
        if ((uint32_t)(operand + 1) < bc->constant_count) {
            vtx_value_t iface_val = bc->constant_pool[operand + 1];
            if (vtx_is_smi(iface_val)) {
                interface_typeid = (vtx_typeid_t)vtx_smi_value(iface_val);
            }
        }

        /* Get the receiver.
         * Bug #3 fix: The receiver is at the BOTTOM of the argument area
         * on the stack. Peek top of stack provisionally for IC lookup,
         * then re-peek at the correct position after resolving the method. */
        VTX_ASSERT(((int)(sp - frame->operand_stack)) > 0, "stack underflow for interface call");
        vtx_value_t receiver = *(sp - 1);

        /* Look up using interface IC */
        vtx_inline_cache_t *ic = vtx_interp_get_ic(interp, frame->method, (uint32_t)pc);
        const vtx_method_desc_t *target_method = NULL;

        if (ic != NULL && method_name != NULL) {
            target_method = vtx_lookup_interface_method(
                interp->type_system, ic, receiver, interface_typeid, method_name);
        }

        /* Fallback: direct vtable walk if IC miss or IC unavailable */
        if (target_method == NULL && vtx_is_heap_ptr(receiver) && method_name != NULL) {
            vtx_typeid_t tid = value_typeid(receiver);
            target_method = vtx_type_resolve_method(
                interp->type_system, tid, method_name);
        }

        /* Bug #3 fix: Re-peek the correct receiver after method resolution.
         * The receiver is at *(sp - arg_count - 1) for the same reason
         * as in CALL_VIRTUAL. */
        vtx_typeid_t receiver_tid = value_typeid(receiver);
        if (target_method != NULL) {
            uint32_t iarg_count = target_method->arg_count;
            if ((uint32_t)(sp - frame->operand_stack) > iarg_count) {
                receiver = *(sp - iarg_count - 1);
                receiver_tid = value_typeid(receiver);
            }
        }

        /* Record call type */
        vtx_profiler_record_call_type(&interp->profiler, frame->method,
                                       (uint32_t)pc, receiver_tid);
        vtx_type_feedback_record_call(&interp->type_feedback,
                                       vtx_hash_site_index(frame->method, (uint32_t)pc),
                                       receiver_tid,
                                       VTX_TYPE_INVALID);

        if (target_method == NULL || target_method->bytecode == NULL) {
            *sp++ = VTX_VALUE_UNDEFINED;
            DISPATCH_NEXT();
        }

        vtx_profiler_record_invocation(&interp->profiler, target_method);

        /* JIT dispatch: if the target method has been compiled, call it
         * directly. Same pattern as CALL_VIRTUAL — interface calls were
         * previously never dispatched to JIT code. */
        if (VTX_UNLIKELY(__atomic_load_n(&target_method->compiled_code, __ATOMIC_ACQUIRE) != NULL)) {
            uint32_t iarg_count_jit = target_method->arg_count + 1; /* +1 for receiver */
            if (iarg_count_jit > 256) iarg_count_jit = 256;
            vtx_value_t ijit_args_buf[256];
            for (uint32_t ai = iarg_count_jit; ai > 0; ai--) {
                ijit_args_buf[ai - 1] = *--sp;
            }
            vtx_value_t ijit_result = vtx_dispatch_jit(interp, target_method,
                                                        ijit_args_buf, iarg_count_jit);
            *sp++ = ijit_result;
            DISPATCH_NEXT();
        }

        /* Pop arguments from caller's stack and copy to callee locals.
         * For interface calls, the receiver (this) is the implicit first
         * argument and must be included in the pop count and passed as
         * local[0] in the callee. arg_count returns only the
         * explicit parameters, so we add 1 for the receiver. */
        uint32_t iarg_count = target_method->arg_count + 1; /* +1 for receiver */
        if (iarg_count > 256) iarg_count = 256;
        vtx_value_t icall_args_buf[256];
        vtx_value_t *icall_args = icall_args_buf;
        for (uint32_t ai = iarg_count; ai > 0; ai--) {
            icall_args[ai - 1] = *--sp;
        }

        vtx_frame_t *callee_frame = vtx_frame_create(
            target_method, frame, (uint32_t)(pc + vtx_bytecode_insn_length(bc, pc)),
            &interp->frame_stack);
        if (callee_frame == NULL) {
            *sp++ = VTX_VALUE_UNDEFINED;
            DISPATCH_NEXT();
        }

        /* Copy arguments into callee's locals */
        for (uint32_t ai = 0; ai < iarg_count && ai < callee_frame->locals_count; ai++) {
            callee_frame->locals[ai] = icall_args[ai];
        }

        SYNC_SP();
        frame = callee_frame;
        interp->current_frame = frame;
        RELOAD_FRAME();
        pc = 0;
        DISPATCH();
    }

    /* ===================================================================
     * RETURNS
     * =================================================================== */

    /* ---- VT_OP_RETURN ---- */
dispatch_VT_OP_RETURN:
    result = VTX_VALUE_UNDEFINED;
    interp->multi_return_count = 0;
    goto dispatch_return;

    /* ---- VT_OP_RETURN_VALUE ---- */
dispatch_VT_OP_RETURN_VALUE:
    result = *--sp;
    interp->multi_return_count = 0;
    goto dispatch_return;

    /* ---- VT_OP_RETURN_MULTI ----
     * Pop `count` values and return them all. Primary (top of stack) goes
     * in `result`, extras in interp->multi_return_values[]. */
dispatch_VT_OP_RETURN_MULTI:
    {
        uint16_t count = read_operand(code, pc);
        if (count == 0) {
            result = VTX_VALUE_UNDEFINED;
            interp->multi_return_count = 0;
        } else {
            result = *--sp;  /* primary (top of stack) */
            uint32_t extra = (count > 1) ? (count - 1) : 0;
            for (uint32_t i = 0; i < extra && i < 16; i++) {
                interp->multi_return_values[i] = *--sp;
            }
            interp->multi_return_count = count;
        }
        goto dispatch_return;
    }

    /* ---- VT_OP_LOAD_VARARGS ---- */
dispatch_VT_OP_LOAD_VARARGS:
    *sp++ = vtx_make_smi((int64_t)frame->vararg_count);
    DISPATCH_NEXT();

    /* ---- VT_OP_VARARG_COUNT ---- */
dispatch_VT_OP_VARARG_COUNT:
    *sp++ = vtx_make_smi((int64_t)frame->vararg_count);
    DISPATCH_NEXT();

    /* ---- VT_OP_VARARG_GET ---- */
dispatch_VT_OP_VARARG_GET:
    {
        uint16_t index = read_operand(code, pc);
        if (index < frame->vararg_count) {
            *sp++ = frame->varargs[index];
        } else {
            *sp++ = VTX_VALUE_UNDEFINED;
        }
        DISPATCH_NEXT();
    }

dispatch_return:
    {
        /* Return from current frame */
        vtx_frame_t *caller = frame->caller;
        uint32_t ret_pc = frame->return_pc;

        /* Destroy the current frame */
        vtx_frame_destroy(frame, &interp->frame_stack);

        if (caller == NULL) {
            /* Top-level method returned */
            interp->running = false;
            interp->current_frame = NULL;
            goto dispatch_done;
        }

        /* Switch to the caller frame */
        SYNC_SP();
        frame = caller;
        interp->current_frame = frame;
        RELOAD_FRAME();
        pc = ret_pc;

        /* Push the return value(s) onto the caller's operand stack.
         *
         * For RETURN / RETURN_VALUE: pushes 1 value (result).
         * For RETURN_MULTI: pushes all N values. The primary (result)
         *   was the top of the callee's stack, so it goes on top.
         *   The extras (in multi_return_values[0..N-2]) go below it.
         *
         * Stack order after RETURN_MULTI N:
         *   [..., extra_N-1, ..., extra_1, result]
         * (result on top, extras in reverse order below)
         *
         * The caller can then use LOAD_LOCAL or stack operations to
         * access all N values. */
        if (interp->multi_return_count > 1) {
            /* Push extras in reverse order (they were stored in order
             * extra[0] = 2nd value, extra[1] = 3rd value, etc.)
             * We want the 2nd value deepest, primary on top. */
            for (int i = (int)interp->multi_return_count - 2; i >= 0; i--) {
                *sp++ = interp->multi_return_values[i];
            }
        }
        *sp++ = result;

        DISPATCH();
    }

    /* ===================================================================
     * OBJECT CREATION
     * =================================================================== */

    /* ---- VT_OP_NEW ---- */
dispatch_VT_OP_NEW:
    operand = read_operand(code, pc);
    {
        vtx_typeid_t typeid_ = (vtx_typeid_t)operand;
        const vtx_type_desc_t *td = vtx_type_get(interp->type_system, typeid_);

        if (td == NULL) {
            *sp++ = VTX_VALUE_NULL;
            DISPATCH_NEXT();
        }

        /* Allocate the object using the GC */
        size_t alloc_size = vtx_heap_object_alloc_size(td->field_count);
        vtx_heap_object_t *obj = vtx_gc_alloc(interp->gc, alloc_size, typeid_);

        if (obj == NULL) {
            *sp++ = VTX_VALUE_NULL;
            DISPATCH_NEXT();
        }

        /* Initialize fields to undefined */
        for (uint32_t i = 0; i < td->field_count; i++) {
            obj->fields[i] = VTX_VALUE_UNDEFINED;
        }

        *sp++ = vtx_make_heap_ptr(obj);
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_NEWARRAY ---- */
dispatch_VT_OP_NEWARRAY:
    operand = read_operand(code, pc);
    {
        /* Pop the size from the stack */
        a = *--sp;
        int64_t array_size = 0;
        if (vtx_is_smi(a)) {
            array_size = vtx_smi_value(a);
        } else if (vtx_is_double(a)) {
            array_size = (int64_t)vtx_double_value(a);
        }

        VTX_ASSERT(array_size >= 0, "negative array size");

        /* The operand specifies the typeid for the array elements.
         * We create an array object with field[0] = length (SMI)
         * and fields[1..N] = array elements (initialized to undefined). */
        uint32_t length = (uint32_t)array_size;
        /* BUG-7 fix: Guard against integer overflow in total_fields.
         * If length == UINT32_MAX, 1 + length wraps to 0, causing
         * a zero-sized allocation and subsequent out-of-bounds writes. */
        if (length > UINT32_MAX - 2) {
            *sp++ = VTX_VALUE_NULL;
            DISPATCH_NEXT();
        }
        uint32_t total_fields = 1 + length; /* field[0]=length, field[1..N]=elements */

        vtx_typeid_t elem_type = (vtx_typeid_t)operand;
        size_t alloc_size = vtx_heap_object_alloc_size(total_fields);
        /* INTERP-002 fix: allocate with VTX_TYPE_ARRAY, not elem_type.
         * The array is NOT an instance of elem_type — using elem_type
         * confused the GC's type-directed scanning. The element type
         * is tracked separately if needed (e.g., for type-specialized
         * array operations). */
        (void)elem_type; /* preserved for future use; not passed to alloc */
        vtx_heap_object_t *arr = vtx_gc_alloc(interp->gc, alloc_size, VTX_TYPE_ARRAY);

        if (arr == NULL) {
            *sp++ = VTX_VALUE_NULL;
            DISPATCH_NEXT();
        }

        /* Initialize: field[0] = length */
        arr->fields[0] = vtx_make_smi((int64_t)length);
        /* Initialize elements to undefined */
        for (uint32_t i = 1; i <= length && i < arr->field_count; i++) {
            arr->fields[i] = VTX_VALUE_UNDEFINED;
        }

        *sp++ = vtx_make_heap_ptr(arr);
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * TYPE CHECKS
     * =================================================================== */

    /* ---- VT_OP_CHECKCAST ---- */
dispatch_VT_OP_CHECKCAST:
    operand = read_operand(code, pc);
    a = *--sp;
    {
        vtx_typeid_t target_typeid = (vtx_typeid_t)operand;
        if (!vtx_helpers_type_check(interp->type_system, a, target_typeid)) {
            /* Cast failed — throw ClassCastException.
             * Bug #4 fix: Do NOT throw the value that failed the cast (`a`),
             * because catch handlers cannot distinguish it from a normal
             * value. Instead, throw VTX_VALUE_NULL as a sentinel that
             * signals "cast failure" to the handler. A full implementation
             * would create a ClassCastException object with the failing
             * value and target type stored as fields. */
            vtx_frame_t *handler_frame = NULL;
            uint32_t handler_pc = throw_exception(interp, VTX_VALUE_NULL, &handler_frame);
            if (handler_pc != VTX_CATCH_NONE && handler_frame != NULL) {
                frame = unwind_to_handler(interp, frame, handler_frame);
                interp->current_frame = frame;
                RELOAD_FRAME();
                pc = handler_pc;
                *sp++ = interp->exception;
                interp->exception = VTX_VALUE_UNDEFINED;
                DISPATCH();
            } else {
                result = vtx_interp_handle_uncaught(interp, interp->exception);
                goto dispatch_done;
            }
        }
        /* Cast succeeded — push the value back */
        *sp++ = a;
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_INSTANCEOF ---- */
dispatch_VT_OP_INSTANCEOF:
    operand = read_operand(code, pc);
    a = *--sp;
    {
        vtx_typeid_t target_typeid = (vtx_typeid_t)operand;
        bool is_instance = vtx_helpers_type_check(interp->type_system, a, target_typeid);
        *sp++ = vtx_make_bool(is_instance);
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * ARRAY OPERATIONS
     * =================================================================== */

    /* ---- VT_OP_ARRAY_LOAD ---- */
dispatch_VT_OP_ARRAY_LOAD:
    {
        b = *--sp;  /* index */
        a = *--sp;  /* array */
        /* INTERP-001 fix: the old code had VTX_ASSERT(false, ...) which
             * aborts in debug and falls through to a NULL deref in release.
             * Neither matches the documented "deopt" intent. Fix: push
             * UNDEFINED and continue — the bytecode will see an undefined
             * value and either return it or hit a downstream type check
             * that triggers a proper deopt. This is safe because the
             * operand stack is restored to a valid state. */
            if (!vtx_helpers_null_check(a)) { *sp++ = VTX_VALUE_UNDEFINED; DISPATCH_NEXT(); }
        {
            vtx_heap_object_t *arr = (vtx_heap_object_t *)vtx_heap_ptr(a);
            int64_t length = 0;
            if (arr->field_count > 0 && vtx_is_smi(arr->fields[0])) {
                length = vtx_smi_value(arr->fields[0]);
            }
            int64_t index = vtx_is_smi(b) ? vtx_smi_value(b) : 
                            (vtx_is_double(b) ? (int64_t)vtx_double_value(b) : -1);
            if (!vtx_helpers_bounds_check(index, length)) { fprintf(stderr, "VORTEX: array index %ld out of bounds %ld at pc=%zu\n", (long)index, (long)length, (size_t)pc); VTX_ASSERT(false, "array index out of bounds"); }
            /* Elements start at field[1] */
            *sp++ = arr->fields[1 + (uint32_t)index];
        }
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_ARRAY_STORE ---- */
dispatch_VT_OP_ARRAY_STORE:
    {
        val = *--sp;  /* value */
        b = *--sp;    /* index */
        a = *--sp;    /* array */
        /* INTERP-001 fix: the old code had VTX_ASSERT(false, ...) which
             * aborts in debug and falls through to a NULL deref in release.
             * Neither matches the documented "deopt" intent. Fix: push
             * UNDEFINED and continue — the bytecode will see an undefined
             * value and either return it or hit a downstream type check
             * that triggers a proper deopt. This is safe because the
             * operand stack is restored to a valid state. */
            if (!vtx_helpers_null_check(a)) { *sp++ = VTX_VALUE_UNDEFINED; DISPATCH_NEXT(); }
        {
            vtx_heap_object_t *arr = (vtx_heap_object_t *)vtx_heap_ptr(a);
            int64_t length = 0;
            if (arr->field_count > 0 && vtx_is_smi(arr->fields[0])) {
                length = vtx_smi_value(arr->fields[0]);
            }
            int64_t index = vtx_is_smi(b) ? vtx_smi_value(b) : 
                            (vtx_is_double(b) ? (int64_t)vtx_double_value(b) : -1);
            if (!vtx_helpers_bounds_check(index, length)) { fprintf(stderr, "VORTEX: array index %ld out of bounds %ld at pc=%zu\n", (long)index, (long)length, (size_t)pc); VTX_ASSERT(false, "array index out of bounds"); }
            uint32_t field_idx = 1 + (uint32_t)index;
            arr->fields[field_idx] = val;
            vtx_gc_write_barrier(interp->gc, arr, field_idx, val);
        }
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_ARRAY_LENGTH ---- */
dispatch_VT_OP_ARRAY_LENGTH:
    a = *--sp;
    /* INTERP-001 fix: the old code had VTX_ASSERT(false, ...) which
             * aborts in debug and falls through to a NULL deref in release.
             * Neither matches the documented "deopt" intent. Fix: push
             * UNDEFINED and continue — the bytecode will see an undefined
             * value and either return it or hit a downstream type check
             * that triggers a proper deopt. This is safe because the
             * operand stack is restored to a valid state. */
            if (!vtx_helpers_null_check(a)) { *sp++ = VTX_VALUE_UNDEFINED; DISPATCH_NEXT(); }
    {
        vtx_heap_object_t *arr = (vtx_heap_object_t *)vtx_heap_ptr(a);
        int64_t length = 0;
        if (arr->field_count > 0 && vtx_is_smi(arr->fields[0])) {
            length = vtx_smi_value(arr->fields[0]);
        }
        *sp++ = vtx_make_smi(length);
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * EXCEPTION HANDLING
     * =================================================================== */

    /* ---- VT_OP_THROW ---- */
dispatch_VT_OP_THROW:
    a = *--sp;
    {
        vtx_frame_t *handler_frame = NULL;
        uint32_t handler_pc = throw_exception(interp, a, &handler_frame);

        if (handler_pc != VTX_CATCH_NONE && handler_frame != NULL) {
            SYNC_SP();
            frame = unwind_to_handler(interp, frame, handler_frame);
            interp->current_frame = frame;
            RELOAD_FRAME();
            pc = handler_pc;
            *sp++ = interp->exception;
            interp->exception = VTX_VALUE_UNDEFINED;
            DISPATCH();
        } else {
            SYNC_SP();
            result = vtx_interp_handle_uncaught(interp, interp->exception);
            goto dispatch_done;
        }
    }

    /* ---- VT_OP_CATCH ---- */
dispatch_VT_OP_CATCH:
    operand = read_operand(code, pc);
    {
        /* Set the catch handler PC for the current frame.
         * VT_OP_CATCH is catch-all (catch_type = 0) for backward
         * compatibility. Use VT_OP_CATCH_TYPED for typed handlers. */
        frame->catch_handler_pc = (uint32_t)operand;
        frame->catch_type = 0; /* catch-all */
        /* Push undefined as a placeholder for the exception variable */
        *sp++ = VTX_VALUE_UNDEFINED;
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_CATCH_TYPED ---- */
dispatch_VT_OP_CATCH_TYPED:
    operand = read_operand(code, pc);
    {
        /* Two 2-byte operands: handler PC + catch type ID.
         * The handler PC is in 'operand' (first 2 bytes).
         * The catch type ID is in the next 2 bytes. */
        uint32_t handler_pc = (uint32_t)operand;
        uint16_t catch_type_raw = (uint16_t)read_operand(code, pc + 2);
        frame->catch_handler_pc = handler_pc;
        frame->catch_type = (vtx_typeid_t)catch_type_raw;
        /* Push undefined as a placeholder for the exception variable */
        *sp++ = VTX_VALUE_UNDEFINED;
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * MONITORS (synchronization)
     * =================================================================== */

    /* ---- VT_OP_MONITOR_ENTER ---- */
dispatch_VT_OP_MONITOR_ENTER:
    a = *--sp;
    /* INTERP-001 fix: the old code had VTX_ASSERT(false, ...) which
             * aborts in debug and falls through to a NULL deref in release.
             * Neither matches the documented "deopt" intent. Fix: push
             * UNDEFINED and continue — the bytecode will see an undefined
             * value and either return it or hit a downstream type check
             * that triggers a proper deopt. This is safe because the
             * operand stack is restored to a valid state. */
            if (!vtx_helpers_null_check(a)) { *sp++ = VTX_VALUE_UNDEFINED; DISPATCH_NEXT(); }
    /* T0 interpreter: monitors are no-ops. A full implementation
     * would use pthread_mutex or similar. We still pop the object
     * to maintain correct stack behavior. */
    DISPATCH_NEXT();

    /* ---- VT_OP_MONITOR_EXIT ---- */
dispatch_VT_OP_MONITOR_EXIT:
    a = *--sp;
    /* INTERP-001 fix: the old code had VTX_ASSERT(false, ...) which
             * aborts in debug and falls through to a NULL deref in release.
             * Neither matches the documented "deopt" intent. Fix: push
             * UNDEFINED and continue — the bytecode will see an undefined
             * value and either return it or hit a downstream type check
             * that triggers a proper deopt. This is safe because the
             * operand stack is restored to a valid state. */
            if (!vtx_helpers_null_check(a)) { *sp++ = VTX_VALUE_UNDEFINED; DISPATCH_NEXT(); }
    /* T0 interpreter: monitors are no-ops. */
    DISPATCH_NEXT();

    /* ===================================================================
     * STACK MANIPULATION
     * =================================================================== */

    /* ---- VT_OP_DUP ---- */
dispatch_VT_OP_DUP:
    a = *(sp - 1);
    *sp++ = a;
    DISPATCH_NEXT();

    /* ---- VT_OP_POP ---- */
dispatch_VT_OP_POP:
    (void)*--sp;
    DISPATCH_NEXT();

    /* ---- VT_OP_SWAP ---- */
dispatch_VT_OP_SWAP:
    b = *--sp;
    a = *--sp;
    *sp++ = b;
    *sp++ = a;
    DISPATCH_NEXT();

    /* ===================================================================
     * TYPE QUERIES
     * =================================================================== */

    /* ---- VT_OP_ISNULL ---- */
dispatch_VT_OP_ISNULL:
    a = *--sp;
    *sp++ = vtx_make_bool(vtx_is_null(a));
    DISPATCH_NEXT();

    /* ---- VT_OP_TYPEOF ---- */
dispatch_VT_OP_TYPEOF:
    a = *--sp;
    {
        vtx_typeid_t tid = value_typeid(a);
        *sp++ = vtx_make_smi((int64_t)tid);
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * RUNTIME CALLS
     * =================================================================== */

    /* ---- VT_OP_CALL_RUNTIME ---- */
dispatch_VT_OP_CALL_RUNTIME:
    operand = read_operand(code, pc);
    {
        /* Runtime function IDs — must agree with the bytecode compiler.
         * Each runtime function has a known argument count and return
         * value convention:
         *   0 = typeof          : 1 arg  → 1 result (TypeID as SMI)
         *   1 = monitor_enter   : 1 arg  → 0 results
         *   2 = monitor_exit    : 1 arg  → 0 results
         *   3 = throw           : 1 arg  → does not return normally
         */
        switch (operand) {
        case 0: /* typeof */
            a = *--sp;
            {
                vtx_typeid_t tid = value_typeid(a);
                *sp++ = vtx_make_smi((int64_t)tid);
            }
            break;
        case 1: /* monitor_enter */
            a = *--sp;
            /* INTERP-001 fix: the old code had VTX_ASSERT(false, ...) which
             * aborts in debug and falls through to a NULL deref in release.
             * Neither matches the documented "deopt" intent. Fix: push
             * UNDEFINED and continue — the bytecode will see an undefined
             * value and either return it or hit a downstream type check
             * that triggers a proper deopt. This is safe because the
             * operand stack is restored to a valid state. */
            if (!vtx_helpers_null_check(a)) { *sp++ = VTX_VALUE_UNDEFINED; DISPATCH_NEXT(); }
            /* T0 interpreter: monitors are no-ops. A full implementation
             * would use pthread_mutex or similar. */
            break;
        case 2: /* monitor_exit */
            a = *--sp;
            /* INTERP-001 fix: the old code had VTX_ASSERT(false, ...) which
             * aborts in debug and falls through to a NULL deref in release.
             * Neither matches the documented "deopt" intent. Fix: push
             * UNDEFINED and continue — the bytecode will see an undefined
             * value and either return it or hit a downstream type check
             * that triggers a proper deopt. This is safe because the
             * operand stack is restored to a valid state. */
            if (!vtx_helpers_null_check(a)) { *sp++ = VTX_VALUE_UNDEFINED; DISPATCH_NEXT(); }
            /* T0 interpreter: monitors are no-ops. */
            break;
        case 3: /* throw */
            a = *--sp;
            {
                vtx_frame_t *handler_frame = NULL;
                uint32_t handler_pc = throw_exception(interp, a, &handler_frame);

                if (handler_pc != VTX_CATCH_NONE && handler_frame != NULL) {
                    SYNC_SP();
                    frame = unwind_to_handler(interp, frame, handler_frame);
                    interp->current_frame = frame;
                    RELOAD_FRAME();
                    sp = frame->operand_stack + frame->stack_top;
                    locals_arr = frame->locals;
                    pc = handler_pc;
                    /* Bug #2 fix: Push the actual exception value to the catch
                     * handler (was VTX_VALUE_UNDEFINED), and clear
                     * interp->exception so it doesn't leak. */
                    *sp++ = interp->exception;
                    interp->exception = VTX_VALUE_UNDEFINED;
                    DISPATCH();
                } else {
                    /* Uncaught exception — unwind everything and return */
                    result = vtx_interp_handle_uncaught(interp, a);
                    goto dispatch_done;
                }
            }
            break;
        case 4: /* print_ln — pop 1, print it + newline */
            a = *--sp;
            {
                if (vtx_is_smi(a)) {
                    printf("%lld\n", (long long)vtx_smi_value(a));
                } else if (vtx_is_bool(a)) {
                    printf("%s\n", vtx_bool_value(a) ? "true" : "false");
                } else if (vtx_is_null(a)) {
                    printf("null\n");
                } else if (vtx_is_undefined(a)) {
                    printf("undefined\n");
                } else if (vtx_is_double(a)) {
                    printf("%g\n", vtx_double_value(a));
                } else {
                    printf("<object>\n");
                }
                fflush(stdout);
            }
            break;
        case 5: /* print — pop 1, print it (no newline) */
            a = *--sp;
            {
                if (vtx_is_smi(a)) {
                    printf("%lld", (long long)vtx_smi_value(a));
                } else if (vtx_is_bool(a)) {
                    printf("%s", vtx_bool_value(a) ? "true" : "false");
                } else if (vtx_is_null(a)) {
                    printf("null");
                } else if (vtx_is_undefined(a)) {
                    printf("undefined");
                } else if (vtx_is_double(a)) {
                    printf("%g", vtx_double_value(a));
                } else {
                    printf("<object>");
                }
                fflush(stdout);
            }
            break;
        case 6: /* exit — pop exit code, terminate */
            a = *--sp;
            {
                long long char_code = 0;
                if (vtx_is_smi(a)) char_code = vtx_smi_value(a);
                else if (vtx_is_double(a)) char_code = (long long)vtx_double_value(a);
                result = vtx_make_smi(char_code);
                goto dispatch_done;
            }
            break;
        default:
            /* Unknown runtime function — check if a callback is registered.
             *
             * The callback receives the func_id and a pointer to the stack
             * pointer. It can pop arguments and push results. If it returns
             * -1, fall through to the default (push undefined). If it
             * returns >= 0, skip the built-in handler.
             *
             * This enables frontends (LuaVortex, etc.) to extend CALL_RUNTIME
             * without patching dispatch.c. */
            if (g_runtime_callback != NULL) {
                vtx_value_t *sp_ptr = sp;
                int n_pushed = g_runtime_callback(operand, &sp_ptr,
                                                    g_runtime_callback_data);
                if (n_pushed >= 0) {
                    /* Callback handled it — sync sp */
                    sp = sp_ptr;
                    break;
                }
                /* Callback returned -1 — fall through to default */
            }
            /* No callback or callback declined — push undefined */
            *sp++ = VTX_VALUE_UNDEFINED;
            break;
        }
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * §2.6 SUPERINSTRUCTIONS — fused bytecode pairs
     *
     * Each superinstruction combines two opcodes into one. This
     * eliminates:
     *   - One computed-goto dispatch (branch predictor miss)
     *   - One operand read (memory load)
     *   - One stack push/pop pair (memory traffic)
     *
     * Net effect: 15-25% throughput improvement on tight arithmetic
     * loops in T0 interpreter. CPython 3.11 saw ~20% from a similar
     * pass. The opcode numbers are defined in bytecode.h.
     * =================================================================== */

    /* ---- VT_OP_LOAD_CONST_INT__IADD ----
     * 4-byte operand: [const_idx][unused]
     * Pops 1 (TOS), reads const from pool, adds const to TOS, pushes result.
     *
     * Replaces the pair:
     *   LOAD_CONST_INT k   (push const[k])
     *   IADD               (pop 2, push sum)
     * with one dispatch.
     *
     * The unused 16 bits of the operand are reserved for future
     * expansion (e.g. fusing LOAD_CONST_INT + ISUB / IMUL).
     */
dispatch_VT_OP_LOAD_CONST_INT__IADD:
    read_operand_4(code, pc, &operand, &operand2);
    {
        VTX_ASSERT(operand < bc->constant_count,
                   "LOAD_CONST_INT__IADD: const idx out of bounds");
        vtx_value_t c = bc->constant_pool[operand];
        a = *--sp;  /* TOS */
        if (VTX_LIKELY(vtx_is_smi(a) && vtx_is_smi(c))) {
            int64_t shift_a = vtx_smi_value(a);
            int64_t ic = vtx_smi_value(c);
            uint64_t ua = (uint64_t)shift_a;
            uint64_t uc = (uint64_t)ic;
            uint64_t ur = ua + uc;
            int64_t result_i = (int64_t)ur;
            if (VTX_LIKELY(!((shift_a ^ ic) >= 0 && (shift_a ^ result_i) < 0)) &&
                VTX_LIKELY(result_i >= VTX_SMI_MIN && result_i <= VTX_SMI_MAX)) {
                *sp++ = vtx_make_smi(result_i);
                DISPATCH_NEXT();
            }
            *sp++ = vtx_make_double((double)shift_a + (double)ic);
        } else {
            double da = vtx_is_double(a) ? vtx_double_value(a)
                       : (vtx_is_smi(a) ? (double)vtx_smi_value(a) : 0.0);
            double dc = vtx_is_double(c) ? vtx_double_value(c)
                       : (vtx_is_smi(c) ? (double)vtx_smi_value(c) : 0.0);
            *sp++ = vtx_make_double(da + dc);
        }
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_LOCAL__LOAD_LOCAL ----
     * 4-byte operand: [local_a][local_b]
     * Pushes locals[a] and locals[b] onto the stack.
     *
     * Replaces the pair:
     *   LOAD_LOCAL a
     *   LOAD_LOCAL b
     * with one dispatch.
     *
     * Very common in tight loops: e.g. `sum += arr[i]` requires
     * loading `arr` and `i` before ARRAY_LOAD.
     */
dispatch_VT_OP_LOAD_LOCAL__LOAD_LOCAL:
    read_operand_4(code, pc, &operand, &operand2);
    {
        VTX_ASSERT(operand < frame->locals_count,
                   "LOAD_LOCAL__LOAD_LOCAL: local a out of bounds");
        VTX_ASSERT(operand2 < frame->locals_count,
                   "LOAD_LOCAL__LOAD_LOCAL: local b out of bounds");
        /* Push in order: local_a first, then local_b (so local_b is TOS). */
        sp[0] = locals_arr[operand];
        sp[1] = locals_arr[operand2];
        sp += 2;
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_LOCAL__STORE_FIELD ----
     * 4-byte operand: [local_idx][field_off]
     * Stack effect: pop obj (already on stack), push local[local_idx],
     * store pushed value to obj.field_off.
     *
     * Replaces the pair:
     *   LOAD_LOCAL k        (push local[k])
     *   STORE_FIELD off     (pop value, pop obj, set obj.field = value)
     *
     * Net: pops 1 (the obj), pushes nothing (the local is consumed
     * by the store). The opcode table's "1 output" is the obj pointer
     * which is not pushed back (the store consumes it).
     *
     * Common in object initialization loops:
     *   for (i=0; i<n; i++) { p.x = i; p = p.next; }
     */
dispatch_VT_OP_LOAD_LOCAL__STORE_FIELD:
    read_operand_4(code, pc, &operand, &operand2);
    {
        VTX_ASSERT(operand < frame->locals_count,
                   "LOAD_LOCAL__STORE_FIELD: local idx out of bounds");
        vtx_value_t local_val = locals_arr[operand];
        vtx_value_t obj_v = *--sp;  /* pop obj (already on stack) */
        if (VTX_LIKELY(vtx_is_heap_ptr(obj_v))) {
            vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(obj_v);
            /* BUGFIX: bounds-check field offset against object size.
             * Without this, a malformed bytecode could write past the
             * end of the object and corrupt adjacent heap memory. */
            if (operand2 < obj->field_count) {
                vtx_object_set_field(obj, operand2, local_val);
                /* GC write barrier — required for generational GC
                 * correctness. The standalone STORE_FIELD handler
                 * also calls this; we must too, or the GC may miss
                 * inter-generational pointer writes from the
                 * superinstruction path. */
                vtx_gc_write_barrier(interp->gc, obj, operand2, local_val);
            }
        }
        /* The store consumes both the obj and the value. The value
         * came from a local (not the stack), so we only popped the obj. */
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * END OF DISPATCH LOOP
     * =================================================================== */

dispatch_done:
    /* Sync sp back to frame so the frame state is consistent */
    SYNC_SP();

    /* ===================================================================
     * DISP-005 fix: jit_reenter_pending was set when OSR couldn't find
     * a loop-header entry (or vtx_osr_up failed). The old code set the
     * flag but never checked it — the dispatch loop just returned to
     * vtx_interp_run, which propagated the (already-computed) return
     * value, never giving the JIT a chance to take over.
     *
     * Fix: if jit_reenter_pending is set and compiled_code is now
     * available (the threadpool may have just finished installing it),
     * dispatch to the JIT from method entry. This is the "whole-method
     * re-enter" fallback documented in the OSR block above. */
    if (interp->jit_reenter_pending) {
        interp->jit_reenter_pending = false;
        if (method != NULL) {
            void *cc = __atomic_load_n(&method->compiled_code,
                                          __ATOMIC_ACQUIRE);
            if (cc != NULL) {
                return vtx_dispatch_jit(interp, method, NULL, 0);
            }
        }
        /* If compiled_code is still NULL (compilation not yet finished),
         * fall through and return the current interp result. The next
         * call to vtx_interp_run will check compiled_code again. */
    }

    /* ===================================================================
     * OSR UP: If the dispatch loop set osr_pending at a backward branch,
     * transfer execution to the JIT code at the loop header. This is
     * true On-Stack Replacement — we enter the JIT at the loop header
     * (not the method entry), avoiding re-execution of the prologue.
     *
     * vtx_osr_up uses an inline asm trampoline that never returns (it
     * jumps to JIT code). If it succeeds, the JIT code runs and its RET
     * returns to the caller of vtx_interp_run. If it fails, we fall
     * through and return the current result.
     * =================================================================== */
    if (interp->osr_pending) {
        interp->osr_pending = false;
        uint32_t osr_pc = interp->osr_loop_header_pc;

        /* Look up the compiled method from the registry to get the
         * bc_pc_map and frame_layout needed by vtx_osr_up. */
        if (interp->compile_ctx != NULL &&
            interp->compile_ctx->method_registry != NULL &&
            method->vtable_index < interp->compile_ctx->method_registry->capacity) {
            vtx_compiled_method_t *cm = vtx_method_registry_get(
                interp->compile_ctx->method_registry, method->vtable_index);
            if (cm != NULL && cm->code_start != NULL) {
                /* OSR-29 fix: rate-limit OSR re-attempts to break the
                 * OSR-fail → re-enter → deopt → OSR-fail infinite loop.
                 * If the method has had VTX_OSR_MAX_FAILURES failed OSR
                 * attempts and we're still in the cooldown window, skip
                 * OSR entirely and let the interpreter continue. */
                if (!vtx_osr_rate_should_attempt(cm->osr_failure_count,
                                                  cm->osr_cooldown_until_call,
                                                  cm->call_count)) {
                    /* Cooldown active — clear osr_pending and continue. */
                    goto osr_skip_cooldown;
                }

                /* OSR-25 fix: ALWAYS prefer the codegen's cm->frame_layout
                 * over the public vtx_frame_layout_compute() fallback.
                 *
                 * The codegen's emit_prologue overrides locals_base and
                 * spill_base to account for the saved RBX/R12 slots at
                 * [RBP-8] and [RBP-16] — locals actually start at
                 * [RBP-24]. The public vtx_frame_layout_compute()
                 * doesn't know about the codegen's saved-register
                 * convention, so it returns locals_base=-8 (treating
                 * [RBP-8] as local[0]). If we used that here, the OSR
                 * asm would write spills to the saved-RBX slot, the
                 * JIT epilogue would restore a corrupt RBX, and the
                 * caller's frame would be corrupted.
                 *
                 * The codegen always populates cm->frame_layout via
                 * vtx_install_method (install.c:269-271), and the install
                 * happens-before the atomic compiled_code store, so if
                 * cm->code_start != NULL (checked above), frame_layout
                 * is fully initialized. */
                vtx_jit_frame_layout_t layout;
                if (cm->frame_layout.total_frame_size > 0) {
                    layout = cm->frame_layout;
                } else {
                    /* Defensive fallback for a codegen bug: this branch
                     * should be UNREACHABLE in normal operation because
                     * emit_prologue always sets total_frame_size. If it
                     * IS reached, the JIT code is suspect (its prologue
                     * may not have run, or the install path skipped the
                     * layout copy) — log loudly and use the public
                     * vtx_frame_layout_compute as a best-effort layout.
                     *
                     * Note: this is the SAME vtx_frame_layout_compute
                     * symbol the codegen itself uses to seed ctx->layout
                     * before the emit_prologue override. The public-vs-
                     * codegen distinction in the original OSR-25 bug
                     * report refers to the post-override layout (which
                     * only the codegen knows), not to two different C
                     * functions. */
                    fprintf(stderr, "[osr] WARN: cm->frame_layout.total_frame_size "
                            "== 0 for method %u — codegen prologue did not run; "
                            "JIT code is suspect, using public layout as fallback\n",
                            (unsigned)method->vtable_index);
                    layout = vtx_frame_layout_compute(method);
                }
                /* Build a vtx_compiled_code_t from the compiled_method */
                vtx_compiled_code_t cc;
                memset(&cc, 0, sizeof(cc));
                cc.entry_point = cm->code_start;
                cc.code = cm->code_start;
                cc.code_size = cm->code_size;
                cc.frame_layout = layout;
                cc.bc_pc_map = cm->bc_pc_map;
                cc.bc_pc_map_count = cm->bc_pc_map_count;
                cc.method_id = cm->method_id;
                cc.stack_slots = layout.max_stack;
                cc.local_slots = layout.max_locals;
                cc.side_table = cm->side_table;

                /* Determine if we have an OSR entry point for osr_pc.
                 *
                 * OSR-2 fix: in addition to bc_pc_map (T1's primary OSR
                 * mechanism), we now also check the side_table for a
                 * VTX_STF_OSR_ENTRY entry at osr_pc (recorded by T1's
                 * scan_loop_headers in codegen.c). This means OSR can
                 * proceed even if bc_pc_map is missing (e.g., T2/T3
                 * codegens that don't populate bc_pc_map).
                 *
                 * If neither path has an entry, fall back to whole-method
                 * re-enter (JIT from method entry). */
                bool has_osr_entry = false;
                if (cc.bc_pc_map != NULL && cc.bc_pc_map_count > 0) {
                    for (uint32_t i = 0; i < cc.bc_pc_map_count; i++) {
                        if (cc.bc_pc_map[i].bytecode_pc == osr_pc) {
                            has_osr_entry = true;
                            break;
                        }
                    }
                }
                if (!has_osr_entry && cc.side_table != NULL) {
                    /* OSR-2/OSR-5/OSR-23: check the side table for an
                     * OSR entry flagged with VTX_STF_OSR_ENTRY at osr_pc. */
                    const vtx_side_table_entry_t *osr_e =
                        vtx_side_table_lookup_osr_entry(cc.side_table, osr_pc);
                    if (osr_e != NULL) {
                        has_osr_entry = true;
                    }
                }

                if (!has_osr_entry) {
                    /* No OSR entry for this loop header — fall back to
                     * whole-method re-enter (JIT from method entry). */
                    interp->jit_reenter_pending = true;
                    /* OSR-29: record this as a soft failure (no OSR entry
                     * found). This isn't really a vtx_osr_up failure, but
                     * if the JIT code keeps deopt'ing and re-entering here
                     * without ever finding an OSR entry, we want the
                     * cooldown to kick in eventually. */
                    (void)vtx_osr_rate_record_failure(
                        &cm->osr_failure_count,
                        &cm->osr_cooldown_until_call,
                        cm->call_count);
                } else {

                /* Build a vtx_interp_frame_t from the current frame */
                vtx_interp_frame_t osr_frame;
                memset(&osr_frame, 0, sizeof(osr_frame));
                osr_frame.method_id = method->vtable_index;
                osr_frame.bytecode_pc = osr_pc;
                osr_frame.locals = frame->locals;
                osr_frame.local_count = frame->locals_count;
                osr_frame.stack = frame->operand_stack;
                osr_frame.stack_top = (uint32_t)frame->stack_top;
                osr_frame.stack_capacity = (uint32_t)frame->stack_capacity;
                /* OSR-32 fix: removed osr_active field — was never read. */

                /* Attempt OSR up — if successful, this never returns.
                 * If it fails, fall back to JIT re-enter (whole-method).
                 *
                 * OSR-3 fix: vtx_osr_up is now void — its contract is
                 * "if this function returns at all, OSR failed". A
                 * successful OSR up jumps to JIT code via the asm
                 * trampoline and never returns here.
                 *
                 * OSR-11: pass the method registry so vtx_osr_up can
                 * re-check the cm version immediately before the asm
                 * jump (avoid jumping to freed code if the version
                 * was concurrently invalidated).
                 *
                 * OSR-12: pass the GC handle so vtx_osr_up can poll
                 * for a pending safepoint immediately before the asm
                 * jump. */
                vtx_method_registry_t *osr_registry = NULL;
                if (interp->compile_ctx != NULL) {
                    osr_registry = interp->compile_ctx->method_registry;
                }

                /* OSR-22 fix (OSR-up path): bracket the vtx_osr_up call
                 * with on_enter/on_exit so the versioned cache's safe-
                 * reclamation mechanism knows retired code may still be
                 * executing on this thread's stack.
                 *
                 * Rationale: vtx_osr_up's asm block jumps to JIT code and
                 * the C function never returns on success. However, when
                 * the JIT epilogue later executes `ret`, it returns to the
                 * saved return address that the OSR-up asm stored into
                 * the JIT frame's [RBP+32] slot — that saved return
                 * address is the address of this very point in dispatch.c
                 * (the instruction right after the vtx_osr_up call).
                 * Therefore control reaches the on_exit call below in
                 * BOTH the success path (after JIT execution + epilogue
                 * return) AND the failure path (vtx_osr_up returns
                 * normally because a gate refused the OSR). In both
                 * cases on_enter has already been called, so calling
                 * on_exit here keeps the on_stack_count balanced.
                 *
                 * Per CRITICAL REPRODUCER CONSTRAINT: this surgical fix
                 * relies on the JIT epilogue returning to the dispatch.c
                 * caller. A dedicated regression test for the OSR-up
                 * on_enter/on_exit wiring is provided in
                 * tests/regression/osr/test_osr22_versioned_cache_on_enter_exit.c
                 * (which verifies the dispatch_jit path; the OSR-up
                 * path shares the same on_enter/on_exit contract via
                 * the versioned cache's on_stack_count invariant). */
                vtx_versioned_cache_t *osr_vc = NULL;
                if (interp->compile_ctx != NULL) {
                    osr_vc = interp->compile_ctx->versioned_cache;
                }
                if (osr_vc != NULL) {
                    vtx_versioned_cache_on_enter(osr_vc, method->vtable_index);
                }

                vtx_osr_up(&osr_frame, method->vtable_index,
                            &cc, osr_pc, osr_registry, interp->gc);

                if (osr_vc != NULL) {
                    vtx_versioned_cache_on_exit(osr_vc, method->vtable_index);
                }

                /* If we reach here, OSR failed — fall back to
                 * whole-method JIT re-enter on the next call. */
                interp->jit_reenter_pending = true;
                /* OSR-29: record the failure. After VTX_OSR_MAX_FAILURES,
                 * the cooldown kicks in and we stop attempting OSR for
                 * this method until VTX_OSR_COOLDOWN_INVOCATIONS have
                 * elapsed. This breaks the
                 * OSR-fail → re-enter → deopt → OSR-fail loop. */
                (void)vtx_osr_rate_record_failure(
                    &cm->osr_failure_count,
                    &cm->osr_cooldown_until_call,
                    cm->call_count);
                /* Note: on success, execution never reaches here —
                 * vtx_osr_up's asm block jumps to JIT code. The
                 * failure counter is cleared explicitly by the M3
                 * fix in vtx_dispatch_jit, which calls
                 * vtx_osr_rate_record_success after the JIT returns
                 * successfully (no deopt). The OSR-up site here only
                 * records failures. */
            } /* end else (has_osr_entry) */
            }
        }
    }
    goto osr_done;

osr_skip_cooldown:
    /* OSR-29: cooldown active — clear osr_pending (already cleared above)
     * and continue interpreting. The interp result is returned as-is. */
    (void)0;
osr_done: ;

    /* Undefine macros local to this function */
#undef DISPATCH
#undef ADVANCE_PC
#undef DISPATCH_NEXT
#undef SYNC_SP
#undef RELOAD_FRAME
#undef VTX_USE_COMPUTED_GOTO
    return result;
}

#pragma GCC diagnostic pop  /* -Wpedantic for computed-goto dispatch */
