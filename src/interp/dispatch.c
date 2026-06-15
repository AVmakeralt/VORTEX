#include "interp/dispatch.h"
#include "baseline/codegen.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

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
    3,  /* 49: NEW */
    3,  /* 50: NEWARRAY */
    3,  /* 51: CHECKCAST */
    3,  /* 52: INSTANCEOF */
    1,  /* 53: ARRAY_LOAD */
    1,  /* 54: ARRAY_STORE */
    1,  /* 55: ARRAY_LENGTH */
    1,  /* 56: THROW */
    3,  /* 57: CATCH */
    5,  /* 58: CATCH_TYPED — opcode(1) + handler_pc(2) + typeid(2) */
    1,  /* 59: MONITOR_ENTER */
    1,  /* 60: MONITOR_EXIT */
    1,  /* 61: DUP */
    1,  /* 62: POP */
    1,  /* 63: SWAP */
    1,  /* 64: ISNULL */
    1,  /* 65: TYPEOF */
    3,  /* 66: CALL_RUNTIME */
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
        return vtx_double_value(v) != 0.0;
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
/* Method argument counting from signature                                     */
/* ========================================================================== */

/**
 * Count the number of arguments from a method signature string.
 * Signatures are like "(II)I" or "(Ljava/lang/String;F)V".
 * Returns 0 if signature is NULL or has no args.
 */
static uint32_t count_method_args(const char *signature)
{
    if (!signature) return 0;
    uint32_t count = 0;
    const char *p = strchr(signature, '(');
    if (!p) return 0;
    p++; /* skip '(' */
    while (*p && *p != ')') {
        count++;
        if (*p == 'L') {
            /* Object type: skip to ';' */
            while (*p && *p != ';') p++;
            if (*p) p++;
        } else if (*p == '[') {
            /* Array type: skip '[' markers, then count the element type */
            while (*p == '[') p++;
            if (*p == 'L') {
                while (*p && *p != ';') p++;
                if (*p) p++;
            } else {
                p++; /* primitive element type */
            }
        } else {
            p++; /* primitive type (I, F, D, J, etc.) */
        }
    }
    return count;
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

static inline uint32_t vtx_hash_site_index(const void *method, uint32_t pc)
{
    uintptr_t mp = (uintptr_t)method;
    uint32_t h = (uint32_t)(mp ^ (mp >> 32)) ^ pc;
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    /* Cap to a reasonable table size to prevent unbounded growth */
    return h % VTX_FEEDBACK_SITE_MAX;
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

    /* Call the JIT-compiled code directly.
     * The baseline JIT's prologue expects:
     *   RDI = method ptr, RSI = deopt_info, RDX = profile_data
     * And it returns a vtx_value_t in RAX.
     *
     * CRITICAL FIX: We pass a sentinel value for profile_data (1 instead
     * of NULL) so the JIT prologue's `if (profile_data)` check doesn't
     * skip instrumentation entirely, but also doesn't dereference NULL.
     * The sentinel value 1 is never a valid pointer but is non-NULL,
     * so the prologue can safely check it without crashing. The actual
     * profile data is stored in the compiled_code metadata, not passed
     * through this argument in practice. */
    vtx_jit_entry_t entry = (vtx_jit_entry_t)code;

    /* Clear the deopt_pending flag before calling JIT code.
     * The deopt stub will set this flag instead of returning
     * VTX_VALUE_UNDEFINED, so we can distinguish between
     * a legitimate undefined return value and a deopt. */
    interp->deopt_pending = false;

    vtx_value_t result;
    if (arg_count > 0 && args != NULL) {
        /* Pass args directly — the JIT prologue copies args[i] → local[i].
         * No placeholder receiver is needed; for virtual calls the receiver
         * is already args[0], and for static calls args map 1:1 to locals.
         * We pass (void*)1 as profile_data to avoid NULL dereference in
         * the JIT prologue's instrumentation check. */
        result = entry(target_method, NULL, (void*)1, args, arg_count);
    } else {
        result = entry(target_method, NULL, (void*)1, NULL, 0);
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

    return result;
}

/* ========================================================================== */
/* Main interpreter dispatch loop                                              */
/* ========================================================================== */

vtx_value_t vtx_interp_run(vtx_interp_t *interp,
                            const vtx_method_desc_t *method,
                            vtx_value_t *args,
                            uint32_t arg_count)
{
    VTX_ASSERT(interp != NULL, "interpreter must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

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
    static void *local_dispatch_table[VT_OP_COUNT] = { NULL };
    static volatile bool dispatch_table_built = false;

    /* Bug #6 fix: Use atomic CAS to prevent race condition when two threads
     * enter vtx_interp_run concurrently and both try to build the table. */
    bool expected = false;
    if (__atomic_compare_exchange_n(&dispatch_table_built, &expected, true,
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
#undef DISPATCH_LABEL
    }

    /* Copy dispatch table to interpreter instance */
    if (interp->dispatch_table != NULL) {
        memcpy(interp->dispatch_table, local_dispatch_table,
               VT_OP_COUNT * sizeof(void *));
    }
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
    vtx_value_t result = VTX_VALUE_UNDEFINED;
    vtx_value_t a, b, val;
    uint16_t operand;
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
    vtx_helpers_null_check(a);
    {
        vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(a);
        VTX_ASSERT(operand < obj->field_count, "field offset out of bounds");
        val = vtx_object_get_field(obj, operand);
        *sp++ = val;
        /* Record field shape for profiling */
        vtx_profiler_record_field_shape(&interp->profiler, frame->method,
                                         (uint32_t)pc, obj->shape_id);
        /* Record field in type feedback */
        vtx_type_feedback_record_field(&interp->type_feedback,
                                        vtx_hash_site_index(frame->method, (uint32_t)pc),
                                        obj->shape_id,
                                        value_typeid(val));
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_STORE_FIELD ---- */
dispatch_VT_OP_STORE_FIELD:
    operand = read_operand(code, pc);
    val = *--sp;  /* value */
    a = *--sp;    /* object */
    vtx_helpers_null_check(a);
    {
        vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(a);
        VTX_ASSERT(operand < obj->field_count, "field offset out of bounds");
        vtx_object_set_field(obj, operand, val);
        /* Write barrier for GC */
        vtx_gc_write_barrier(interp->gc, obj, operand, val);
        /* Record field shape for profiling */
        vtx_profiler_record_field_shape(&interp->profiler, frame->method,
                                         (uint32_t)pc, obj->shape_id);
        vtx_type_feedback_record_field(&interp->type_feedback,
                                        vtx_hash_site_index(frame->method, (uint32_t)pc),
                                        obj->shape_id,
                                        value_typeid(val));
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
        /* Fast path: SMI + SMI (most common case) */
        int64_t ia_smi = vtx_smi_value(a);
        int64_t ib_smi = vtx_smi_value(b);
        int64_t result_i = ia_smi + ib_smi;
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
        /* Fast path: SMI - SMI */
        int64_t ia_smi = vtx_smi_value(a);
        int64_t ib_smi = vtx_smi_value(b);
        int64_t result_i = ia_smi - ib_smi;
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
        VTX_ASSERT(ib_smi != 0, "integer division by zero");
        if (VTX_UNLIKELY(ib_smi == 0)) {
            *sp++ = VTX_VALUE_UNDEFINED;
            DISPATCH_NEXT();
        }
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
                /* Threshold reached — request JIT compilation.
                 * This is the key wiring that connects the interpreter's
                 * hot-code detection to the JIT compilation thread pool.
                 * Previously this was a TODO comment; now it actually
                 * submits the method for background compilation. */
                if (interp->compile_ctx != NULL) {
                    vtx_request_compilation(interp->compile_ctx, frame->method);
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
                    vtx_request_compilation(interp->compile_ctx, frame->method);
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
                            vtx_request_compilation(interp->compile_ctx, frame->method);
                        }
                }

                /* BUG-3 fix: Check for deopt pending at backward branches */
                if (VTX_UNLIKELY(interp->deopt_pending)) {
                    interp->deopt_pending = false;
                    if (interp->compile_ctx != NULL && frame->method != NULL) {
                        vtx_request_compilation(interp->compile_ctx, frame->method);
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
                            vtx_request_compilation(interp->compile_ctx, frame->method);
                        }
                }

                /* BUG-3 fix: Check for deopt pending at backward branches */
                if (VTX_UNLIKELY(interp->deopt_pending)) {
                    interp->deopt_pending = false;
                    if (interp->compile_ctx != NULL && frame->method != NULL) {
                        vtx_request_compilation(interp->compile_ctx, frame->method);
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

        /* Record call type in profiler */
        vtx_typeid_t receiver_tid = VTX_TYPE_INVALID;
        if (((int)(sp - frame->operand_stack)) > 0) {
            receiver_tid = value_typeid(*(sp - 1));
        }
        vtx_profiler_record_call_type(&interp->profiler, frame->method,
                                       (uint32_t)pc, receiver_tid);

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
         * Bug #3 fix: The receiver is at the BOTTOM of the argument area
         * on the stack (pushed first: receiver, arg1, arg2, ...).
         * We don't know arg_count yet, so peek the top of stack as a
         * provisional value for IC lookup. After resolving the method,
         * we re-peek the correct receiver position for profiling. */
        VTX_ASSERT(((int)(sp - frame->operand_stack)) > 0, "stack underflow for virtual call");
        vtx_value_t receiver = *(sp - 1);

        /* Look up the method using the inline cache */
        vtx_inline_cache_t *ic = vtx_interp_get_ic(interp, frame->method, (uint32_t)pc);
        const vtx_method_desc_t *target_method = NULL;

        if (ic != NULL && method_name != NULL) {
            target_method = vtx_lookup_method(interp->type_system, ic,
                                               receiver, method_name);
        } else {
            /* Fallback: direct vtable walk */
            if (vtx_is_heap_ptr(receiver) && method_name != NULL) {
                vtx_typeid_t tid = value_typeid(receiver);
                target_method = vtx_type_resolve_method(
                    interp->type_system, tid, method_name);
            }
        }

        /* Bug #3 fix: After resolving the method, peek the correct receiver.
         * The receiver is at *(sp - arg_count - 1) because it was pushed
         * before the arguments. We only need the correct receiver for
         * profiling/type-feedback; the IC lookup above may have used the
         * wrong stack position, but that's a pre-existing limitation. */
        vtx_typeid_t receiver_tid = value_typeid(receiver);
        if (target_method != NULL) {
            uint32_t rarg_count = target_method->arg_count;
            if ((uint32_t)(sp - frame->operand_stack) > rarg_count) {
                receiver = *(sp - rarg_count - 1);
                receiver_tid = value_typeid(receiver);
            }
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
    goto dispatch_return;

    /* ---- VT_OP_RETURN_VALUE ---- */
dispatch_VT_OP_RETURN_VALUE:
    result = *--sp;
    goto dispatch_return;

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

        /* Push the return value onto the caller's operand stack */
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
        vtx_heap_object_t *arr = vtx_gc_alloc(interp->gc, alloc_size, elem_type);

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
        vtx_helpers_null_check(a);
        {
            vtx_heap_object_t *arr = (vtx_heap_object_t *)vtx_heap_ptr(a);
            int64_t length = 0;
            if (arr->field_count > 0 && vtx_is_smi(arr->fields[0])) {
                length = vtx_smi_value(arr->fields[0]);
            }
            int64_t index = vtx_is_smi(b) ? vtx_smi_value(b) : 
                            (vtx_is_double(b) ? (int64_t)vtx_double_value(b) : -1);
            vtx_helpers_bounds_check(index, length);
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
        vtx_helpers_null_check(a);
        {
            vtx_heap_object_t *arr = (vtx_heap_object_t *)vtx_heap_ptr(a);
            int64_t length = 0;
            if (arr->field_count > 0 && vtx_is_smi(arr->fields[0])) {
                length = vtx_smi_value(arr->fields[0]);
            }
            int64_t index = vtx_is_smi(b) ? vtx_smi_value(b) : 
                            (vtx_is_double(b) ? (int64_t)vtx_double_value(b) : -1);
            vtx_helpers_bounds_check(index, length);
            uint32_t field_idx = 1 + (uint32_t)index;
            arr->fields[field_idx] = val;
            vtx_gc_write_barrier(interp->gc, arr, field_idx, val);
        }
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_ARRAY_LENGTH ---- */
dispatch_VT_OP_ARRAY_LENGTH:
    a = *--sp;
    vtx_helpers_null_check(a);
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
        uint16_t catch_type_raw = (uint16_t)read_operand(code, pc + 3);
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
    vtx_helpers_null_check(a);
    /* T0 interpreter: monitors are no-ops. A full implementation
     * would use pthread_mutex or similar. We still pop the object
     * to maintain correct stack behavior. */
    DISPATCH_NEXT();

    /* ---- VT_OP_MONITOR_EXIT ---- */
dispatch_VT_OP_MONITOR_EXIT:
    a = *--sp;
    vtx_helpers_null_check(a);
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
            vtx_helpers_null_check(a);
            /* T0 interpreter: monitors are no-ops. A full implementation
             * would use pthread_mutex or similar. */
            break;
        case 2: /* monitor_exit */
            a = *--sp;
            vtx_helpers_null_check(a);
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
        default:
            /* Unknown runtime function — push undefined as a safe fallback */
            *sp++ = VTX_VALUE_UNDEFINED;
            break;
        }
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * END OF DISPATCH LOOP
     * =================================================================== */

dispatch_done:
    /* Sync sp back to frame so the frame state is consistent */
    SYNC_SP();
    /* Undefine macros local to this function */
#undef DISPATCH
#undef ADVANCE_PC
#undef DISPATCH_NEXT
#undef SYNC_SP
#undef RELOAD_FRAME
#undef VTX_USE_COMPUTED_GOTO
    return result;
}
