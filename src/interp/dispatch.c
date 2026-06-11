#include "interp/dispatch.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

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

    /* Walk the frame chain looking for a catch handler */
    vtx_frame_t *f = interp->current_frame;
    while (f != NULL) {
        if (f->catch_handler_pc != VTX_CATCH_NONE) {
            *out_handler_frame = f;
            return f->catch_handler_pc;
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
/* Main interpreter dispatch loop                                              */
/* ========================================================================== */

vtx_value_t vtx_interp_run(vtx_interp_t *interp,
                            const vtx_method_desc_t *method,
                            vtx_value_t *args,
                            uint32_t arg_count)
{
    VTX_ASSERT(interp != NULL, "interpreter must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    /*
     * ===================================================================
     * DISPATCH TABLE CONSTRUCTION (GCC computed goto)
     * ===================================================================
     *
     * We use GCC's labels-as-values extension to build a dispatch table
     * that maps opcodes to label addresses. This gives O(1) dispatch
     * without the branch-prediction overhead of a switch statement.
     *
     * Each opcode handler is a labeled block. At the end of each handler,
     * we fetch the next opcode and jump through the dispatch table.
     */
    static void *local_dispatch_table[VT_OP_COUNT] = { NULL };
    static bool dispatch_table_built = false;

    if (!dispatch_table_built) {
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
        local_dispatch_table[VT_OP_MONITOR_ENTER]  = DISPATCH_LABEL(VT_OP_MONITOR_ENTER);
        local_dispatch_table[VT_OP_MONITOR_EXIT]   = DISPATCH_LABEL(VT_OP_MONITOR_EXIT);
        local_dispatch_table[VT_OP_DUP]            = DISPATCH_LABEL(VT_OP_DUP);
        local_dispatch_table[VT_OP_POP]            = DISPATCH_LABEL(VT_OP_POP);
        local_dispatch_table[VT_OP_SWAP]           = DISPATCH_LABEL(VT_OP_SWAP);
        local_dispatch_table[VT_OP_ISNULL]         = DISPATCH_LABEL(VT_OP_ISNULL);
        local_dispatch_table[VT_OP_TYPEOF]         = DISPATCH_LABEL(VT_OP_TYPEOF);
#undef DISPATCH_LABEL
        dispatch_table_built = true;
    }

    /* Copy dispatch table to interpreter instance */
    if (interp->dispatch_table != NULL) {
        memcpy(interp->dispatch_table, local_dispatch_table,
               VT_OP_COUNT * sizeof(void *));
    }

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

    /* Local variables for the dispatch loop */
    vtx_bytecode_t *bc = frame->bytecode;
    const uint8_t *code = bc->code;
    size_t pc = 0;
    vtx_value_t result = VTX_VALUE_UNDEFINED;
    vtx_value_t a, b, val;
    uint16_t operand;
    int64_t ia, ib;
    double fa, fb;

    /* ===================================================================
     * DISPATCH LOOP
     * =================================================================== */

#define DISPATCH() do { \
    goto *local_dispatch_table[code[pc]]; \
} while(0)

#define ADVANCE_PC() do { \
    pc += vtx_bytecode_insn_length(bc, pc); \
} while(0)

#define DISPATCH_NEXT() do { \
    ADVANCE_PC(); \
    DISPATCH(); \
} while(0)

    /* Enter the dispatch loop */
    DISPATCH();

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
    val = vtx_frame_get_local(frame, operand);
    vtx_frame_push(frame, val);
    DISPATCH_NEXT();

    /* ---- VT_OP_STORE_LOCAL ---- */
dispatch_VT_OP_STORE_LOCAL:
    operand = read_operand(code, pc);
    val = vtx_frame_pop(frame);
    vtx_frame_set_local(frame, operand, val);
    /* Update monitored type for deopt */
    if (frame->monitored_types != NULL && operand < frame->locals_count) {
        frame->monitored_types[operand] = value_typeid(val);
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_FIELD ---- */
dispatch_VT_OP_LOAD_FIELD:
    operand = read_operand(code, pc);
    a = vtx_frame_pop(frame);
    vtx_helpers_null_check(a);
    {
        vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(a);
        VTX_ASSERT(operand < obj->field_count, "field offset out of bounds");
        val = vtx_object_get_field(obj, operand);
        vtx_frame_push(frame, val);
        /* Record field shape for profiling */
        vtx_profiler_record_field_shape(&interp->profiler, frame->method,
                                         (uint32_t)pc, obj->shape_id);
        /* Record field in type feedback */
        vtx_type_feedback_record_field(&interp->type_feedback,
                                        (uint32_t)pc, obj->shape_id,
                                        value_typeid(val));
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_STORE_FIELD ---- */
dispatch_VT_OP_STORE_FIELD:
    operand = read_operand(code, pc);
    val = vtx_frame_pop(frame);  /* value */
    a = vtx_frame_pop(frame);    /* object */
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
                                        (uint32_t)pc, obj->shape_id,
                                        value_typeid(val));
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_CONST_INT ---- */
dispatch_VT_OP_LOAD_CONST_INT:
    operand = read_operand(code, pc);
    VTX_ASSERT(operand < bc->constant_count, "constant pool index out of bounds");
    vtx_frame_push(frame, bc->constant_pool[operand]);
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_CONST_FLOAT ---- */
dispatch_VT_OP_LOAD_CONST_FLOAT:
    operand = read_operand(code, pc);
    VTX_ASSERT(operand < bc->constant_count, "constant pool index out of bounds");
    vtx_frame_push(frame, bc->constant_pool[operand]);
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_CONST_STR ---- */
dispatch_VT_OP_LOAD_CONST_STR:
    operand = read_operand(code, pc);
    VTX_ASSERT(operand < bc->constant_count, "constant pool index out of bounds");
    vtx_frame_push(frame, bc->constant_pool[operand]);
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_NULL ---- */
dispatch_VT_OP_LOAD_NULL:
    vtx_frame_push(frame, VTX_VALUE_NULL);
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_TRUE ---- */
dispatch_VT_OP_LOAD_TRUE:
    vtx_frame_push(frame, VTX_VALUE_TRUE);
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_FALSE ---- */
dispatch_VT_OP_LOAD_FALSE:
    vtx_frame_push(frame, VTX_VALUE_FALSE);
    DISPATCH_NEXT();

    /* ---- VT_OP_LOAD_UNDEFINED ---- */
dispatch_VT_OP_LOAD_UNDEFINED:
    vtx_frame_push(frame, VTX_VALUE_UNDEFINED);
    DISPATCH_NEXT();

    /* ===================================================================
     * INTEGER ARITHMETIC
     * =================================================================== */

    /* ---- VT_OP_IADD ---- */
dispatch_VT_OP_IADD:
    b = vtx_frame_pop(frame);
    a = vtx_frame_pop(frame);
    ia = vtx_smi_value(a);
    ib = vtx_smi_value(b);
    {
        if (!vtx_helpers_overflow_check_iadd(ia, ib)) {
            /* Overflow: promote to double */
            vtx_frame_push(frame, vtx_make_double((double)ia + (double)ib));
        } else {
            int64_t result_i = ia + ib;
            if (result_i >= VTX_SMI_MIN && result_i <= VTX_SMI_MAX) {
                vtx_frame_push(frame, vtx_make_smi(result_i));
            } else {
                vtx_frame_push(frame, vtx_make_double((double)result_i));
            }
        }
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_ISUB ---- */
dispatch_VT_OP_ISUB:
    b = vtx_frame_pop(frame);
    a = vtx_frame_pop(frame);
    ia = vtx_smi_value(a);
    ib = vtx_smi_value(b);
    {
        bool overflow = false;
        if (ib < 0 && ia > INT64_MAX + ib) overflow = true;
        if (ib > 0 && ia < INT64_MIN + ib) overflow = true;
        if (overflow) {
            vtx_frame_push(frame, vtx_make_double((double)ia - (double)ib));
        } else {
            int64_t result_i = ia - ib;
            if (result_i >= VTX_SMI_MIN && result_i <= VTX_SMI_MAX) {
                vtx_frame_push(frame, vtx_make_smi(result_i));
            } else {
                vtx_frame_push(frame, vtx_make_double((double)result_i));
            }
        }
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_IMUL ---- */
dispatch_VT_OP_IMUL:
    b = vtx_frame_pop(frame);
    a = vtx_frame_pop(frame);
    ia = vtx_smi_value(a);
    ib = vtx_smi_value(b);
    {
        if (!vtx_helpers_overflow_check_imul(ia, ib)) {
            vtx_frame_push(frame, vtx_make_double((double)ia * (double)ib));
        } else {
            int64_t result_i = ia * ib;
            if (result_i >= VTX_SMI_MIN && result_i <= VTX_SMI_MAX) {
                vtx_frame_push(frame, vtx_make_smi(result_i));
            } else {
                vtx_frame_push(frame, vtx_make_double((double)result_i));
            }
        }
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_IDIV ---- */
dispatch_VT_OP_IDIV:
    b = vtx_frame_pop(frame);
    a = vtx_frame_pop(frame);
    ia = vtx_smi_value(a);
    ib = vtx_smi_value(b);
    {
        VTX_ASSERT(ib != 0, "integer division by zero");
        if (ia == INT64_MIN && ib == -1) {
            vtx_frame_push(frame, vtx_make_double((double)INT64_MAX));
        } else {
            int64_t result_i = ia / ib;
            vtx_frame_push(frame, vtx_make_smi(result_i));
        }
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_IMOD ---- */
dispatch_VT_OP_IMOD:
    b = vtx_frame_pop(frame);
    a = vtx_frame_pop(frame);
    ia = vtx_smi_value(a);
    ib = vtx_smi_value(b);
    {
        VTX_ASSERT(ib != 0, "integer modulo by zero");
        int64_t result_i = ia % ib;
        vtx_frame_push(frame, vtx_make_smi(result_i));
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * FLOAT ARITHMETIC
     * =================================================================== */

    /* ---- VT_OP_FADD ---- */
dispatch_VT_OP_FADD:
    b = vtx_frame_pop(frame);
    a = vtx_frame_pop(frame);
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    vtx_frame_push(frame, vtx_make_double(fa + fb));
    DISPATCH_NEXT();

    /* ---- VT_OP_FSUB ---- */
dispatch_VT_OP_FSUB:
    b = vtx_frame_pop(frame);
    a = vtx_frame_pop(frame);
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    vtx_frame_push(frame, vtx_make_double(fa - fb));
    DISPATCH_NEXT();

    /* ---- VT_OP_FMUL ---- */
dispatch_VT_OP_FMUL:
    b = vtx_frame_pop(frame);
    a = vtx_frame_pop(frame);
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    vtx_frame_push(frame, vtx_make_double(fa * fb));
    DISPATCH_NEXT();

    /* ---- VT_OP_FDIV ---- */
dispatch_VT_OP_FDIV:
    b = vtx_frame_pop(frame);
    a = vtx_frame_pop(frame);
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    vtx_frame_push(frame, vtx_make_double(fa / fb));
    DISPATCH_NEXT();

    /* ===================================================================
     * BITWISE AND UNARY INTEGER OPERATIONS
     * =================================================================== */

    /* ---- VT_OP_ISHL ---- */
dispatch_VT_OP_ISHL:
    b = vtx_frame_pop(frame);
    a = vtx_frame_pop(frame);
    ia = vtx_smi_value(a);
    ib = vtx_smi_value(b);
    vtx_frame_push(frame, vtx_make_smi(ia << (ib & 63)));
    DISPATCH_NEXT();

    /* ---- VT_OP_ISHR ---- */
dispatch_VT_OP_ISHR:
    b = vtx_frame_pop(frame);
    a = vtx_frame_pop(frame);
    ia = vtx_smi_value(a);
    ib = vtx_smi_value(b);
    vtx_frame_push(frame, vtx_make_smi(ia >> (ib & 63)));
    DISPATCH_NEXT();

    /* ---- VT_OP_IAND ---- */
dispatch_VT_OP_IAND:
    b = vtx_frame_pop(frame);
    a = vtx_frame_pop(frame);
    ia = vtx_smi_value(a);
    ib = vtx_smi_value(b);
    vtx_frame_push(frame, vtx_make_smi(ia & ib));
    DISPATCH_NEXT();

    /* ---- VT_OP_IOR ---- */
dispatch_VT_OP_IOR:
    b = vtx_frame_pop(frame);
    a = vtx_frame_pop(frame);
    ia = vtx_smi_value(a);
    ib = vtx_smi_value(b);
    vtx_frame_push(frame, vtx_make_smi(ia | ib));
    DISPATCH_NEXT();

    /* ---- VT_OP_IXOR ---- */
dispatch_VT_OP_IXOR:
    b = vtx_frame_pop(frame);
    a = vtx_frame_pop(frame);
    ia = vtx_smi_value(a);
    ib = vtx_smi_value(b);
    vtx_frame_push(frame, vtx_make_smi(ia ^ ib));
    DISPATCH_NEXT();

    /* ---- VT_OP_INEG ---- */
dispatch_VT_OP_INEG:
    a = vtx_frame_pop(frame);
    ia = vtx_smi_value(a);
    {
        if (ia == INT64_MIN) {
            vtx_frame_push(frame, vtx_make_double(-(double)INT64_MIN));
        } else {
            vtx_frame_push(frame, vtx_make_smi(-ia));
        }
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_INOT ---- */
dispatch_VT_OP_INOT:
    a = vtx_frame_pop(frame);
    ia = vtx_smi_value(a);
    vtx_frame_push(frame, vtx_make_smi(~ia));
    DISPATCH_NEXT();

    /* ===================================================================
     * INTEGER COMPARISONS
     * =================================================================== */

dispatch_VT_OP_ICMP_EQ:
    b = vtx_frame_pop(frame); a = vtx_frame_pop(frame);
    vtx_frame_push(frame, vtx_make_bool(vtx_smi_value(a) == vtx_smi_value(b)));
    DISPATCH_NEXT();

dispatch_VT_OP_ICMP_NE:
    b = vtx_frame_pop(frame); a = vtx_frame_pop(frame);
    vtx_frame_push(frame, vtx_make_bool(vtx_smi_value(a) != vtx_smi_value(b)));
    DISPATCH_NEXT();

dispatch_VT_OP_ICMP_LT:
    b = vtx_frame_pop(frame); a = vtx_frame_pop(frame);
    vtx_frame_push(frame, vtx_make_bool(vtx_smi_value(a) < vtx_smi_value(b)));
    DISPATCH_NEXT();

dispatch_VT_OP_ICMP_LE:
    b = vtx_frame_pop(frame); a = vtx_frame_pop(frame);
    vtx_frame_push(frame, vtx_make_bool(vtx_smi_value(a) <= vtx_smi_value(b)));
    DISPATCH_NEXT();

dispatch_VT_OP_ICMP_GT:
    b = vtx_frame_pop(frame); a = vtx_frame_pop(frame);
    vtx_frame_push(frame, vtx_make_bool(vtx_smi_value(a) > vtx_smi_value(b)));
    DISPATCH_NEXT();

dispatch_VT_OP_ICMP_GE:
    b = vtx_frame_pop(frame); a = vtx_frame_pop(frame);
    vtx_frame_push(frame, vtx_make_bool(vtx_smi_value(a) >= vtx_smi_value(b)));
    DISPATCH_NEXT();

    /* ===================================================================
     * FLOAT COMPARISONS
     * =================================================================== */

dispatch_VT_OP_FCMP_EQ:
    b = vtx_frame_pop(frame); a = vtx_frame_pop(frame);
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    vtx_frame_push(frame, vtx_make_bool(fa == fb));
    DISPATCH_NEXT();

dispatch_VT_OP_FCMP_NE:
    b = vtx_frame_pop(frame); a = vtx_frame_pop(frame);
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    vtx_frame_push(frame, vtx_make_bool(fa != fb));
    DISPATCH_NEXT();

dispatch_VT_OP_FCMP_LT:
    b = vtx_frame_pop(frame); a = vtx_frame_pop(frame);
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    vtx_frame_push(frame, vtx_make_bool(fa < fb));
    DISPATCH_NEXT();

dispatch_VT_OP_FCMP_LE:
    b = vtx_frame_pop(frame); a = vtx_frame_pop(frame);
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    vtx_frame_push(frame, vtx_make_bool(fa <= fb));
    DISPATCH_NEXT();

dispatch_VT_OP_FCMP_GT:
    b = vtx_frame_pop(frame); a = vtx_frame_pop(frame);
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    vtx_frame_push(frame, vtx_make_bool(fa > fb));
    DISPATCH_NEXT();

dispatch_VT_OP_FCMP_GE:
    b = vtx_frame_pop(frame); a = vtx_frame_pop(frame);
    fa = vtx_is_double(a) ? vtx_double_value(a) : (double)vtx_smi_value(a);
    fb = vtx_is_double(b) ? vtx_double_value(b) : (double)vtx_smi_value(b);
    vtx_frame_push(frame, vtx_make_bool(fa >= fb));
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
        if (target_pc <= (uint32_t)pc) {
            vtx_profiler_record_backward_branch(&interp->profiler, frame->method);
            vtx_gc_safepoint(interp->gc);
        }
        pc = target_pc;
        DISPATCH();
    }

    /* ---- VT_OP_IF_TRUE ---- */
dispatch_VT_OP_IF_TRUE:
    operand = read_operand(code, pc);
    a = vtx_frame_pop(frame);
    {
        bool taken = is_truthy(a);
        vtx_profiler_record_branch(&interp->profiler, frame->method,
                                    (uint32_t)pc, taken);
        vtx_type_feedback_record_branch(&interp->type_feedback,
                                         (uint32_t)pc, taken);
        if (taken) {
            uint32_t target_pc = (uint32_t)operand;
            if (target_pc <= (uint32_t)pc) {
                vtx_profiler_record_backward_branch(&interp->profiler, frame->method);
                vtx_gc_safepoint(interp->gc);
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
    a = vtx_frame_pop(frame);
    {
        bool taken = !is_truthy(a);
        vtx_profiler_record_branch(&interp->profiler, frame->method,
                                    (uint32_t)pc, taken);
        vtx_type_feedback_record_branch(&interp->type_feedback,
                                         (uint32_t)pc, taken);
        if (taken) {
            uint32_t target_pc = (uint32_t)operand;
            if (target_pc <= (uint32_t)pc) {
                vtx_profiler_record_backward_branch(&interp->profiler, frame->method);
                vtx_gc_safepoint(interp->gc);
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
            vtx_frame_push(frame, VTX_VALUE_UNDEFINED);
            DISPATCH_NEXT();
        }

        /* Record invocation in profiler */
        vtx_profiler_record_invocation(&interp->profiler, target_method);

        /* Record call type in profiler */
        vtx_typeid_t receiver_tid = VTX_TYPE_INVALID;
        if (vtx_frame_stack_depth(frame) > 0) {
            receiver_tid = value_typeid(vtx_frame_peek(frame, 0));
        }
        vtx_profiler_record_call_type(&interp->profiler, frame->method,
                                       (uint32_t)pc, receiver_tid);

        /* Create new frame for the callee */
        vtx_frame_t *callee_frame = vtx_frame_create(
            target_method, frame, (uint32_t)(pc + vtx_bytecode_insn_length(bc, pc)),
            &interp->frame_stack);
        if (callee_frame == NULL) {
            vtx_frame_push(frame, VTX_VALUE_UNDEFINED);
            DISPATCH_NEXT();
        }

        /* Switch to the callee frame */
        frame = callee_frame;
        interp->current_frame = frame;
        bc = frame->bytecode;
        code = bc->code;
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

        /* Get the receiver (top of stack) */
        VTX_ASSERT(vtx_frame_stack_depth(frame) > 0, "stack underflow for virtual call");
        vtx_value_t receiver = vtx_frame_peek(frame, 0);

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

        /* Record call type in profiler and type feedback */
        vtx_typeid_t receiver_tid = value_typeid(receiver);
        vtx_profiler_record_call_type(&interp->profiler, frame->method,
                                       (uint32_t)pc, receiver_tid);
        vtx_type_feedback_record_call(&interp->type_feedback,
                                       (uint32_t)pc, receiver_tid,
                                       VTX_TYPE_INVALID);

        if (target_method == NULL || target_method->bytecode == NULL) {
            vtx_frame_push(frame, VTX_VALUE_UNDEFINED);
            DISPATCH_NEXT();
        }

        /* Record invocation */
        vtx_profiler_record_invocation(&interp->profiler, target_method);

        /* Create new frame for the callee */
        vtx_frame_t *callee_frame = vtx_frame_create(
            target_method, frame, (uint32_t)(pc + vtx_bytecode_insn_length(bc, pc)),
            &interp->frame_stack);
        if (callee_frame == NULL) {
            vtx_frame_push(frame, VTX_VALUE_UNDEFINED);
            DISPATCH_NEXT();
        }

        /* Switch to the callee frame */
        frame = callee_frame;
        interp->current_frame = frame;
        bc = frame->bytecode;
        code = bc->code;
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

        /* Get the receiver */
        VTX_ASSERT(vtx_frame_stack_depth(frame) > 0, "stack underflow for interface call");
        vtx_value_t receiver = vtx_frame_peek(frame, 0);

        /* Look up using interface IC */
        vtx_inline_cache_t *ic = vtx_interp_get_ic(interp, frame->method, (uint32_t)pc);
        const vtx_method_desc_t *target_method = NULL;

        if (ic != NULL && method_name != NULL) {
            target_method = vtx_lookup_interface_method(
                interp->type_system, ic, receiver, interface_typeid, method_name);
        }

        /* Record call type */
        vtx_typeid_t receiver_tid = value_typeid(receiver);
        vtx_profiler_record_call_type(&interp->profiler, frame->method,
                                       (uint32_t)pc, receiver_tid);
        vtx_type_feedback_record_call(&interp->type_feedback,
                                       (uint32_t)pc, receiver_tid,
                                       VTX_TYPE_INVALID);

        if (target_method == NULL || target_method->bytecode == NULL) {
            vtx_frame_push(frame, VTX_VALUE_UNDEFINED);
            DISPATCH_NEXT();
        }

        vtx_profiler_record_invocation(&interp->profiler, target_method);

        vtx_frame_t *callee_frame = vtx_frame_create(
            target_method, frame, (uint32_t)(pc + vtx_bytecode_insn_length(bc, pc)),
            &interp->frame_stack);
        if (callee_frame == NULL) {
            vtx_frame_push(frame, VTX_VALUE_UNDEFINED);
            DISPATCH_NEXT();
        }

        frame = callee_frame;
        interp->current_frame = frame;
        bc = frame->bytecode;
        code = bc->code;
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
    result = vtx_frame_pop(frame);
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
        frame = caller;
        interp->current_frame = frame;
        bc = frame->bytecode;
        code = bc->code;
        pc = ret_pc;

        /* Push the return value onto the caller's operand stack */
        vtx_frame_push(frame, result);

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
            vtx_frame_push(frame, VTX_VALUE_NULL);
            DISPATCH_NEXT();
        }

        /* Allocate the object using the GC */
        size_t alloc_size = vtx_heap_object_alloc_size(td->field_count);
        vtx_heap_object_t *obj = vtx_gc_alloc(interp->gc, alloc_size, typeid_);

        if (obj == NULL) {
            vtx_frame_push(frame, VTX_VALUE_NULL);
            DISPATCH_NEXT();
        }

        /* Initialize fields to undefined */
        for (uint32_t i = 0; i < td->field_count; i++) {
            obj->fields[i] = VTX_VALUE_UNDEFINED;
        }

        vtx_frame_push(frame, vtx_make_heap_ptr(obj));
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_NEWARRAY ---- */
dispatch_VT_OP_NEWARRAY:
    operand = read_operand(code, pc);
    {
        /* Pop the size from the stack */
        a = vtx_frame_pop(frame);
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
        uint32_t total_fields = 1 + length; /* field[0]=length, field[1..N]=elements */

        vtx_typeid_t elem_type = (vtx_typeid_t)operand;
        size_t alloc_size = vtx_heap_object_alloc_size(total_fields);
        vtx_heap_object_t *arr = vtx_gc_alloc(interp->gc, alloc_size, elem_type);

        if (arr == NULL) {
            vtx_frame_push(frame, VTX_VALUE_NULL);
            DISPATCH_NEXT();
        }

        /* Initialize: field[0] = length */
        arr->fields[0] = vtx_make_smi((int64_t)length);
        /* Initialize elements to undefined */
        for (uint32_t i = 1; i <= length && i < arr->field_count; i++) {
            arr->fields[i] = VTX_VALUE_UNDEFINED;
        }

        vtx_frame_push(frame, vtx_make_heap_ptr(arr));
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * TYPE CHECKS
     * =================================================================== */

    /* ---- VT_OP_CHECKCAST ---- */
dispatch_VT_OP_CHECKCAST:
    operand = read_operand(code, pc);
    a = vtx_frame_pop(frame);
    {
        vtx_typeid_t target_typeid = (vtx_typeid_t)operand;
        if (!vtx_helpers_type_check(interp->type_system, a, target_typeid)) {
            /* Cast failed — throw ClassCastException */
            vtx_frame_t *handler_frame = NULL;
            uint32_t handler_pc = throw_exception(interp, a, &handler_frame);
            if (handler_pc != VTX_CATCH_NONE && handler_frame != NULL) {
                frame = unwind_to_handler(interp, frame, handler_frame);
                interp->current_frame = frame;
                bc = frame->bytecode;
                code = bc->code;
                pc = handler_pc;
                vtx_frame_push(frame, interp->exception);
                interp->exception = VTX_VALUE_UNDEFINED;
                DISPATCH();
            } else {
                result = vtx_interp_handle_uncaught(interp, interp->exception);
                goto dispatch_done;
            }
        }
        /* Cast succeeded — push the value back */
        vtx_frame_push(frame, a);
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_INSTANCEOF ---- */
dispatch_VT_OP_INSTANCEOF:
    operand = read_operand(code, pc);
    a = vtx_frame_pop(frame);
    {
        vtx_typeid_t target_typeid = (vtx_typeid_t)operand;
        bool is_instance = vtx_helpers_type_check(interp->type_system, a, target_typeid);
        vtx_frame_push(frame, vtx_make_bool(is_instance));
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * ARRAY OPERATIONS
     * =================================================================== */

    /* ---- VT_OP_ARRAY_LOAD ---- */
dispatch_VT_OP_ARRAY_LOAD:
    {
        b = vtx_frame_pop(frame);  /* index */
        a = vtx_frame_pop(frame);  /* array */
        vtx_helpers_null_check(a);
        {
            vtx_heap_object_t *arr = (vtx_heap_object_t *)vtx_heap_ptr(a);
            int64_t length = 0;
            if (arr->field_count > 0 && vtx_is_smi(arr->fields[0])) {
                length = vtx_smi_value(arr->fields[0]);
            }
            int64_t index = vtx_smi_value(b);
            vtx_helpers_bounds_check(index, length);
            /* Elements start at field[1] */
            vtx_frame_push(frame, arr->fields[1 + (uint32_t)index]);
        }
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_ARRAY_STORE ---- */
dispatch_VT_OP_ARRAY_STORE:
    {
        val = vtx_frame_pop(frame);  /* value */
        b = vtx_frame_pop(frame);    /* index */
        a = vtx_frame_pop(frame);    /* array */
        vtx_helpers_null_check(a);
        {
            vtx_heap_object_t *arr = (vtx_heap_object_t *)vtx_heap_ptr(a);
            int64_t length = 0;
            if (arr->field_count > 0 && vtx_is_smi(arr->fields[0])) {
                length = vtx_smi_value(arr->fields[0]);
            }
            int64_t index = vtx_smi_value(b);
            vtx_helpers_bounds_check(index, length);
            uint32_t field_idx = 1 + (uint32_t)index;
            arr->fields[field_idx] = val;
            vtx_gc_write_barrier(interp->gc, arr, field_idx, val);
        }
    }
    DISPATCH_NEXT();

    /* ---- VT_OP_ARRAY_LENGTH ---- */
dispatch_VT_OP_ARRAY_LENGTH:
    a = vtx_frame_pop(frame);
    vtx_helpers_null_check(a);
    {
        vtx_heap_object_t *arr = (vtx_heap_object_t *)vtx_heap_ptr(a);
        int64_t length = 0;
        if (arr->field_count > 0 && vtx_is_smi(arr->fields[0])) {
            length = vtx_smi_value(arr->fields[0]);
        }
        vtx_frame_push(frame, vtx_make_smi(length));
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * EXCEPTION HANDLING
     * =================================================================== */

    /* ---- VT_OP_THROW ---- */
dispatch_VT_OP_THROW:
    a = vtx_frame_pop(frame);
    {
        vtx_frame_t *handler_frame = NULL;
        uint32_t handler_pc = throw_exception(interp, a, &handler_frame);

        if (handler_pc != VTX_CATCH_NONE && handler_frame != NULL) {
            frame = unwind_to_handler(interp, frame, handler_frame);
            interp->current_frame = frame;
            bc = frame->bytecode;
            code = bc->code;
            pc = handler_pc;
            vtx_frame_push(frame, interp->exception);
            interp->exception = VTX_VALUE_UNDEFINED;
            DISPATCH();
        } else {
            result = vtx_interp_handle_uncaught(interp, interp->exception);
            goto dispatch_done;
        }
    }

    /* ---- VT_OP_CATCH ---- */
dispatch_VT_OP_CATCH:
    operand = read_operand(code, pc);
    {
        /* Set the catch handler PC for the current frame */
        frame->catch_handler_pc = (uint32_t)operand;
        /* Push undefined as a placeholder for the exception variable */
        vtx_frame_push(frame, VTX_VALUE_UNDEFINED);
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * MONITORS (synchronization)
     * =================================================================== */

    /* ---- VT_OP_MONITOR_ENTER ---- */
dispatch_VT_OP_MONITOR_ENTER:
    a = vtx_frame_pop(frame);
    vtx_helpers_null_check(a);
    /* T0 interpreter: monitors are no-ops. A full implementation
     * would use pthread_mutex or similar. We still pop the object
     * to maintain correct stack behavior. */
    DISPATCH_NEXT();

    /* ---- VT_OP_MONITOR_EXIT ---- */
dispatch_VT_OP_MONITOR_EXIT:
    a = vtx_frame_pop(frame);
    vtx_helpers_null_check(a);
    /* T0 interpreter: monitors are no-ops. */
    DISPATCH_NEXT();

    /* ===================================================================
     * STACK MANIPULATION
     * =================================================================== */

    /* ---- VT_OP_DUP ---- */
dispatch_VT_OP_DUP:
    a = vtx_frame_peek(frame, 0);
    vtx_frame_push(frame, a);
    DISPATCH_NEXT();

    /* ---- VT_OP_POP ---- */
dispatch_VT_OP_POP:
    vtx_frame_pop(frame);
    DISPATCH_NEXT();

    /* ---- VT_OP_SWAP ---- */
dispatch_VT_OP_SWAP:
    b = vtx_frame_pop(frame);
    a = vtx_frame_pop(frame);
    vtx_frame_push(frame, b);
    vtx_frame_push(frame, a);
    DISPATCH_NEXT();

    /* ===================================================================
     * TYPE QUERIES
     * =================================================================== */

    /* ---- VT_OP_ISNULL ---- */
dispatch_VT_OP_ISNULL:
    a = vtx_frame_pop(frame);
    vtx_frame_push(frame, vtx_make_bool(vtx_is_null(a)));
    DISPATCH_NEXT();

    /* ---- VT_OP_TYPEOF ---- */
dispatch_VT_OP_TYPEOF:
    a = vtx_frame_pop(frame);
    {
        vtx_typeid_t tid = value_typeid(a);
        vtx_frame_push(frame, vtx_make_smi((int64_t)tid));
    }
    DISPATCH_NEXT();

    /* ===================================================================
     * END OF DISPATCH LOOP
     * =================================================================== */

dispatch_done:
    return result;
}
