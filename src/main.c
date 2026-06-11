/**
 * VORTEX JIT Compiler — Main Entry Point
 *
 * Minimal CLI: `vortex [bytecode_file]` or `vortex --test` for self-test mode.
 *
 * The self-test mode creates a type system, GC, and a simple bytecode
 * interpreter, then runs a Fibonacci program and prints the result.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "vortex_config.h"
#include "runtime/arena.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/helpers.h"

/* ========================================================================== */
/* Minimal interpreter                                                         */
/* ========================================================================== */

/**
 * A simple stack-based bytecode interpreter frame.
 * This is the T0 interpreter (simplified for the runtime demo).
 */
#define VTX_MAX_LOCALS    256
#define VTX_MAX_STACK     256
#define VTX_MAX_FRAMES     32

typedef struct {
    const vtx_bytecode_t *bytecode;
    size_t                pc;
    vtx_value_t           locals[VTX_MAX_LOCALS];
    vtx_value_t           stack[VTX_MAX_STACK];
    int                   sp;  /* stack pointer (index into stack[]) */
} vtx_frame_t;

typedef struct {
    vtx_frame_t   frames[VTX_MAX_FRAMES];
    int           frame_count;
    vtx_gc_t     *gc;
    vtx_type_system_t *ts;
} vtx_interp_t;

/* ========================================================================== */
/* Interpreter operations                                                      */
/* ========================================================================== */

static void interp_init(vtx_interp_t *interp, vtx_gc_t *gc, vtx_type_system_t *ts)
{
    memset(interp, 0, sizeof(vtx_interp_t));
    interp->gc = gc;
    interp->ts = ts;
}

static vtx_frame_t *interp_current_frame(vtx_interp_t *interp)
{
    if (interp->frame_count <= 0) {
        return NULL;
    }
    return &interp->frames[interp->frame_count - 1];
}

static vtx_frame_t *interp_push_frame(vtx_interp_t *interp, const vtx_bytecode_t *bc)
{
    VTX_ASSERT(interp->frame_count < VTX_MAX_FRAMES, "frame stack overflow");
    vtx_frame_t *frame = &interp->frames[interp->frame_count];
    frame->bytecode = bc;
    frame->pc = 0;
    frame->sp = 0;
    memset(frame->locals, 0, sizeof(frame->locals));
    memset(frame->stack, 0, sizeof(frame->stack));
    interp->frame_count++;
    return frame;
}

static inline void interp_pop_frame(vtx_interp_t *interp)
{
    VTX_ASSERT(interp->frame_count > 0, "frame stack underflow");
    interp->frame_count--;
}

static inline void frame_push(vtx_frame_t *frame, vtx_value_t value)
{
    VTX_ASSERT(frame->sp < VTX_MAX_STACK, "operand stack overflow");
    frame->stack[frame->sp++] = value;
}

static inline vtx_value_t frame_pop(vtx_frame_t *frame)
{
    VTX_ASSERT(frame->sp > 0, "operand stack underflow");
    return frame->stack[--frame->sp];
}

static inline vtx_value_t frame_peek(vtx_frame_t *frame, int offset)
{
    VTX_ASSERT(frame->sp - offset > 0, "operand stack underflow on peek");
    return frame->stack[frame->sp - 1 - offset];
}

/**
 * Truthiness test: a value is truthy if it is:
 * - a non-zero SMI
 * - a non-null, non-undefined, non-false value
 * - a non-zero double
 * - a heap pointer (always truthy)
 */
static bool is_truthy(vtx_value_t v)
{
    if (vtx_is_smi(v))      return vtx_smi_value(v) != 0;
    if (vtx_is_bool(v))     return vtx_bool_value(v);
    if (vtx_is_null(v))     return false;
    if (vtx_is_undefined(v))return false;
    if (vtx_is_double(v))   return vtx_double_value(v) != 0.0;
    if (vtx_is_heap_ptr(v)) return true; /* non-null objects are truthy */
    return false;
}

/**
 * Run the bytecode in the current frame until HALT or RETURN.
 * Returns the result value (or VTX_VALUE_UNDEFINED for void return).
 */
static vtx_value_t interp_run(vtx_interp_t *interp)
{
    vtx_frame_t *frame = interp_current_frame(interp);
    VTX_ASSERT(frame != NULL, "no frame to execute");
    VTX_ASSERT(frame->bytecode != NULL, "frame has no bytecode");

    const vtx_bytecode_t *bc = frame->bytecode;

    while (frame->pc < bc->length) {
        vtx_opcode_t opcode = vtx_bytecode_opcode_at(bc, frame->pc);

        switch (opcode) {
        case VT_OP_HALT:
            return frame->sp > 0 ? frame_pop(frame) : vtx_make_undefined();

        case VT_OP_NOP:
            frame->pc += 1;
            break;

        case VT_OP_LOAD_LOCAL: {
            uint16_t idx = vtx_bytecode_read_operand(bc, frame->pc);
            frame_push(frame, frame->locals[idx]);
            frame->pc += 3;
            break;
        }

        case VT_OP_STORE_LOCAL: {
            uint16_t idx = vtx_bytecode_read_operand(bc, frame->pc);
            frame->locals[idx] = frame_pop(frame);
            frame->pc += 3;
            break;
        }

        case VT_OP_LOAD_FIELD: {
            /* pop object, push field */
            uint16_t field_off = vtx_bytecode_read_operand(bc, frame->pc);
            vtx_value_t obj_val = frame_pop(frame);
            if (vtx_is_heap_ptr(obj_val)) {
                vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(obj_val);
                vtx_value_t field_val = vtx_object_get_field(obj, field_off);
                frame_push(frame, field_val);
            } else {
                frame_push(frame, vtx_make_undefined());
            }
            frame->pc += 3;
            break;
        }

        case VT_OP_STORE_FIELD: {
            /* pop object and value, store value into object field */
            uint16_t field_off = vtx_bytecode_read_operand(bc, frame->pc);
            vtx_value_t value = frame_pop(frame);
            vtx_value_t obj_val = frame_pop(frame);
            if (vtx_is_heap_ptr(obj_val)) {
                vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(obj_val);
                vtx_object_set_field(obj, field_off, value);
                vtx_gc_write_barrier(interp->gc, obj, field_off, value);
            }
            frame->pc += 3;
            break;
        }

        case VT_OP_LOAD_CONST_INT: {
            uint16_t idx = vtx_bytecode_read_operand(bc, frame->pc);
            VTX_ASSERT(idx < bc->constant_count, "constant index out of bounds");
            frame_push(frame, bc->constant_pool[idx]);
            frame->pc += 3;
            break;
        }

        case VT_OP_LOAD_CONST_FLOAT: {
            uint16_t idx = vtx_bytecode_read_operand(bc, frame->pc);
            VTX_ASSERT(idx < bc->constant_count, "constant index out of bounds");
            frame_push(frame, bc->constant_pool[idx]);
            frame->pc += 3;
            break;
        }

        case VT_OP_LOAD_CONST_STR: {
            uint16_t idx = vtx_bytecode_read_operand(bc, frame->pc);
            VTX_ASSERT(idx < bc->constant_count, "constant index out of bounds");
            frame_push(frame, bc->constant_pool[idx]);
            frame->pc += 3;
            break;
        }

        case VT_OP_LOAD_NULL:
            frame_push(frame, vtx_make_null());
            frame->pc += 1;
            break;

        case VT_OP_LOAD_TRUE:
            frame_push(frame, vtx_make_bool(true));
            frame->pc += 1;
            break;

        case VT_OP_LOAD_FALSE:
            frame_push(frame, vtx_make_bool(false));
            frame->pc += 1;
            break;

        case VT_OP_LOAD_UNDEFINED:
            frame_push(frame, vtx_make_undefined());
            frame->pc += 1;
            break;

        case VT_OP_IADD: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_smi(a + b));
            frame->pc += 1;
            break;
        }

        case VT_OP_ISUB: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_smi(a - b));
            frame->pc += 1;
            break;
        }

        case VT_OP_IMUL: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_smi(a * b));
            frame->pc += 1;
            break;
        }

        case VT_OP_IDIV: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            VTX_ASSERT(b != 0, "division by zero");
            frame_push(frame, vtx_make_smi(a / b));
            frame->pc += 1;
            break;
        }

        case VT_OP_IMOD: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            VTX_ASSERT(b != 0, "modulo by zero");
            frame_push(frame, vtx_make_smi(a % b));
            frame->pc += 1;
            break;
        }

        case VT_OP_FADD: {
            double b = vtx_double_value(frame_pop(frame));
            double a = vtx_double_value(frame_pop(frame));
            frame_push(frame, vtx_make_double(a + b));
            frame->pc += 1;
            break;
        }

        case VT_OP_FSUB: {
            double b = vtx_double_value(frame_pop(frame));
            double a = vtx_double_value(frame_pop(frame));
            frame_push(frame, vtx_make_double(a - b));
            frame->pc += 1;
            break;
        }

        case VT_OP_FMUL: {
            double b = vtx_double_value(frame_pop(frame));
            double a = vtx_double_value(frame_pop(frame));
            frame_push(frame, vtx_make_double(a * b));
            frame->pc += 1;
            break;
        }

        case VT_OP_FDIV: {
            double b = vtx_double_value(frame_pop(frame));
            double a = vtx_double_value(frame_pop(frame));
            frame_push(frame, vtx_make_double(a / b));
            frame->pc += 1;
            break;
        }

        case VT_OP_ISHL: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_smi(a << (b & 63)));
            frame->pc += 1;
            break;
        }

        case VT_OP_ISHR: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_smi(a >> (b & 63)));
            frame->pc += 1;
            break;
        }

        case VT_OP_IAND: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_smi(a & b));
            frame->pc += 1;
            break;
        }

        case VT_OP_IOR: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_smi(a | b));
            frame->pc += 1;
            break;
        }

        case VT_OP_IXOR: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_smi(a ^ b));
            frame->pc += 1;
            break;
        }

        case VT_OP_INEG: {
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_smi(-a));
            frame->pc += 1;
            break;
        }

        case VT_OP_INOT: {
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_smi(~a));
            frame->pc += 1;
            break;
        }

        case VT_OP_ICMP_EQ: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_bool(a == b));
            frame->pc += 1;
            break;
        }

        case VT_OP_ICMP_NE: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_bool(a != b));
            frame->pc += 1;
            break;
        }

        case VT_OP_ICMP_LT: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_bool(a < b));
            frame->pc += 1;
            break;
        }

        case VT_OP_ICMP_LE: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_bool(a <= b));
            frame->pc += 1;
            break;
        }

        case VT_OP_ICMP_GT: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_bool(a > b));
            frame->pc += 1;
            break;
        }

        case VT_OP_ICMP_GE: {
            int64_t b = vtx_smi_value(frame_pop(frame));
            int64_t a = vtx_smi_value(frame_pop(frame));
            frame_push(frame, vtx_make_bool(a >= b));
            frame->pc += 1;
            break;
        }

        case VT_OP_FCMP_EQ: {
            double b = vtx_double_value(frame_pop(frame));
            double a = vtx_double_value(frame_pop(frame));
            frame_push(frame, vtx_make_bool(a == b));
            frame->pc += 1;
            break;
        }

        case VT_OP_FCMP_NE: {
            double b = vtx_double_value(frame_pop(frame));
            double a = vtx_double_value(frame_pop(frame));
            frame_push(frame, vtx_make_bool(a != b));
            frame->pc += 1;
            break;
        }

        case VT_OP_FCMP_LT: {
            double b = vtx_double_value(frame_pop(frame));
            double a = vtx_double_value(frame_pop(frame));
            frame_push(frame, vtx_make_bool(a < b));
            frame->pc += 1;
            break;
        }

        case VT_OP_FCMP_LE: {
            double b = vtx_double_value(frame_pop(frame));
            double a = vtx_double_value(frame_pop(frame));
            frame_push(frame, vtx_make_bool(a <= b));
            frame->pc += 1;
            break;
        }

        case VT_OP_FCMP_GT: {
            double b = vtx_double_value(frame_pop(frame));
            double a = vtx_double_value(frame_pop(frame));
            frame_push(frame, vtx_make_bool(a > b));
            frame->pc += 1;
            break;
        }

        case VT_OP_FCMP_GE: {
            double b = vtx_double_value(frame_pop(frame));
            double a = vtx_double_value(frame_pop(frame));
            frame_push(frame, vtx_make_bool(a >= b));
            frame->pc += 1;
            break;
        }

        case VT_OP_GOTO: {
            uint16_t target = vtx_bytecode_read_operand(bc, frame->pc);
            frame->pc = target;
            /* Check safepoint on backward branches */
            if (target < frame->pc) {
                vtx_gc_safepoint(interp->gc);
            }
            break;
        }

        case VT_OP_IF_TRUE: {
            uint16_t target = vtx_bytecode_read_operand(bc, frame->pc);
            vtx_value_t cond = frame_pop(frame);
            if (is_truthy(cond)) {
                frame->pc = target;
            } else {
                frame->pc += 3;
            }
            break;
        }

        case VT_OP_IF_FALSE: {
            uint16_t target = vtx_bytecode_read_operand(bc, frame->pc);
            vtx_value_t cond = frame_pop(frame);
            if (!is_truthy(cond)) {
                frame->pc = target;
            } else {
                frame->pc += 3;
            }
            break;
        }

        case VT_OP_CALL_STATIC: {
            /* Simplified: just advance PC (no actual method call in demo) */
            frame->pc += 3;
            break;
        }

        case VT_OP_CALL_VIRTUAL:
        case VT_OP_CALL_INTERFACE: {
            frame->pc += 3;
            break;
        }

        case VT_OP_RETURN:
            return vtx_make_undefined();

        case VT_OP_RETURN_VALUE: {
            vtx_value_t val = frame_pop(frame);
            return val;
        }

        case VT_OP_NEW: {
            uint16_t typeid_ = vtx_bytecode_read_operand(bc, frame->pc);
            const vtx_type_desc_t *td = vtx_type_get(interp->ts, typeid_);
            size_t alloc_size = td != NULL ? td->instance_size :
                                vtx_heap_object_alloc_size(0);
            vtx_heap_object_t *obj = vtx_gc_alloc(interp->gc, alloc_size, typeid_);
            if (obj == NULL) {
                frame_push(frame, vtx_make_null());
            } else {
                frame_push(frame, vtx_make_heap_ptr(obj));
                vtx_gc_root_push(interp->gc, vtx_make_heap_ptr(obj));
            }
            frame->pc += 3;
            break;
        }

        case VT_OP_NEWARRAY: {
            /* Simplified: create an object with array-like storage */
            uint16_t elem_type = vtx_bytecode_read_operand(bc, frame->pc);
            (void)elem_type;
            vtx_value_t size_val = frame_pop(frame);
            int64_t arr_size = vtx_is_smi(size_val) ? vtx_smi_value(size_val) : 0;
            if (arr_size < 0) arr_size = 0;

            /* Allocate: header + length field + element fields */
            uint32_t total_fields = (uint32_t)arr_size + 1; /* +1 for length */
            size_t alloc_size = vtx_heap_object_alloc_size(total_fields);
            vtx_heap_object_t *arr = vtx_gc_alloc(interp->gc, alloc_size, VTX_TYPE_OBJECT);
            if (arr == NULL) {
                frame_push(frame, vtx_make_null());
            } else {
                arr->field_count = total_fields;
                arr->fields[0] = vtx_make_smi(arr_size);
                frame_push(frame, vtx_make_heap_ptr(arr));
                vtx_gc_root_push(interp->gc, vtx_make_heap_ptr(arr));
            }
            frame->pc += 3;
            break;
        }

        case VT_OP_CHECKCAST: {
            /* Simplified: just pass through */
            frame->pc += 3;
            break;
        }

        case VT_OP_INSTANCEOF: {
            uint16_t typeid_ = vtx_bytecode_read_operand(bc, frame->pc);
            vtx_value_t obj_val = frame_pop(frame);
            bool result = vtx_helpers_type_check(interp->ts, obj_val, typeid_);
            frame_push(frame, vtx_make_bool(result));
            frame->pc += 3;
            break;
        }

        case VT_OP_ARRAY_LOAD: {
            vtx_value_t idx_val = frame_pop(frame);
            vtx_value_t arr_val = frame_pop(frame);
            if (vtx_is_heap_ptr(arr_val) && vtx_is_smi(idx_val)) {
                vtx_heap_object_t *arr = (vtx_heap_object_t *)vtx_heap_ptr(arr_val);
                int64_t idx = vtx_smi_value(idx_val);
                /* Field 0 = length, fields 1..N = elements */
                if (idx >= 0 && (uint64_t)idx + 1 < arr->field_count) {
                    frame_push(frame, arr->fields[idx + 1]);
                } else {
                    frame_push(frame, vtx_make_undefined());
                }
            } else {
                frame_push(frame, vtx_make_undefined());
            }
            frame->pc += 1;
            break;
        }

        case VT_OP_ARRAY_STORE: {
            vtx_value_t val = frame_pop(frame);
            vtx_value_t idx_val = frame_pop(frame);
            vtx_value_t arr_val = frame_pop(frame);
            if (vtx_is_heap_ptr(arr_val) && vtx_is_smi(idx_val)) {
                vtx_heap_object_t *arr = (vtx_heap_object_t *)vtx_heap_ptr(arr_val);
                int64_t idx = vtx_smi_value(idx_val);
                if (idx >= 0 && (uint64_t)idx + 1 < arr->field_count) {
                    arr->fields[idx + 1] = val;
                    vtx_gc_write_barrier(interp->gc, arr, (uint32_t)(idx + 1), val);
                }
            }
            frame->pc += 1;
            break;
        }

        case VT_OP_ARRAY_LENGTH: {
            vtx_value_t arr_val = frame_pop(frame);
            if (vtx_is_heap_ptr(arr_val)) {
                vtx_heap_object_t *arr = (vtx_heap_object_t *)vtx_heap_ptr(arr_val);
                if (arr->field_count > 0 && vtx_is_smi(arr->fields[0])) {
                    frame_push(frame, arr->fields[0]);
                } else {
                    frame_push(frame, vtx_make_smi(0));
                }
            } else {
                frame_push(frame, vtx_make_smi(0));
            }
            frame->pc += 1;
            break;
        }

        case VT_OP_THROW: {
            /* Simplified: just halt */
            fprintf(stderr, "VORTEX: unhandled exception\n");
            return vtx_make_undefined();
        }

        case VT_OP_CATCH:
            frame->pc += 3;
            break;

        case VT_OP_MONITOR_ENTER:
            frame_pop(frame); /* consume the object */
            frame->pc += 1;
            break;

        case VT_OP_MONITOR_EXIT:
            frame_pop(frame);
            frame->pc += 1;
            break;

        case VT_OP_DUP: {
            vtx_value_t val = frame_peek(frame, 0);
            frame_push(frame, val);
            frame->pc += 1;
            break;
        }

        case VT_OP_POP:
            frame_pop(frame);
            frame->pc += 1;
            break;

        case VT_OP_SWAP: {
            vtx_value_t a = frame_pop(frame);
            vtx_value_t b = frame_pop(frame);
            frame_push(frame, a);
            frame_push(frame, b);
            frame->pc += 1;
            break;
        }

        case VT_OP_ISNULL: {
            vtx_value_t val = frame_pop(frame);
            frame_push(frame, vtx_make_bool(vtx_is_null(val)));
            frame->pc += 1;
            break;
        }

        case VT_OP_TYPEOF: {
            (void)frame_pop(frame);
            /* Simplified: push the type name as undefined for now */
            frame_push(frame, vtx_make_undefined());
            frame->pc += 1;
            break;
        }

        default:
            fprintf(stderr, "VORTEX: unknown opcode %d at pc %zu\n", opcode, frame->pc);
            return vtx_make_undefined();
        }
    }

    return frame->sp > 0 ? frame_pop(frame) : vtx_make_undefined();
}

/* ========================================================================== */
/* Self-test: Fibonacci program                                                */
/* ========================================================================== */

/**
 * Build a Fibonacci bytecode program:
 *
 * function fib(n):
 *   if n <= 1: return n
 *   return fib(n-1) + fib(n-2)
 *
 * Since our interpreter doesn't support recursive calls yet (simplified),
 * we implement an iterative Fibonacci instead:
 *
 * function fib(n):
 *   local[0] = n          // argument
 *   local[1] = 0          // a = fib(0)
 *   local[2] = 1          // b = fib(1)
 *   if local[0] <= 1 goto done
 * loop:
 *   local[3] = local[1] + local[2]  // tmp = a + b
 *   local[1] = local[2]             // a = b
 *   local[2] = local[3]             // b = tmp
 *   local[0] = local[0] - 1         // n = n - 1
 *   if local[0] > 1 goto loop       // if n > 1, continue
 * done:
 *   return local[2]       // return b (or a if n <= 1)
 *
 * Bytecode layout:
 *   000: LOAD_CONST_INT 0    // load n from constant pool
 *   003: STORE_LOCAL 0       // local[0] = n
 *   006: LOAD_CONST_INT 1    // load 0
 *   009: STORE_LOCAL 1       // local[1] = 0
 *   012: LOAD_CONST_INT 2    // load 1
 *   015: STORE_LOCAL 2       // local[2] = 1
 *   018: LOAD_LOCAL 0        // n
 *   021: LOAD_CONST_INT 1    // 1
 *   024: ICMP_LE             // n <= 1
 *   025: IF_TRUE <done>      // if n <= 1, go to done
 *   028: LOAD_LOCAL 1        // a
 *   031: LOAD_LOCAL 2        // b
 *   034: IADD                // a + b
 *   035: STORE_LOCAL 3       // local[3] = a + b
 *   038: LOAD_LOCAL 2        // b
 *   041: STORE_LOCAL 1       // a = b
 *   044: LOAD_LOCAL 3        // tmp
 *   047: STORE_LOCAL 2       // b = tmp
 *   050: LOAD_LOCAL 0        // n
 *   053: LOAD_CONST_INT 2    // 1
 *   056: ISUB                // n - 1
 *   057: STORE_LOCAL 0       // n = n - 1
 *   060: LOAD_LOCAL 0        // n
 *   063: LOAD_CONST_INT 2    // 1
 *   066: ICMP_GT             // n > 1
 *   067: IF_TRUE <loop=28>   // if n > 1, go to loop
 *   070: LOAD_LOCAL 1        // if n was 0, return a (0); if n was 1, return b
 *                             // Actually we need to return local[1] when n==0, local[2] when n>=1
 *   done: LOAD_LOCAL 2       // return b
 *   073: RETURN_VALUE
 *   074: HALT
 *
 * Wait, let me fix the logic. For n=0, we want to return 0 (local[1]).
 * For n=1 and above, we want to return local[2] after the loop.
 * But after the loop, if n started as 0, we'd go directly to done and
 * return local[2]=1, which is wrong. Let me fix:
 *
 *   if n == 0: return 0
 *   if n == 1: return 1
 *   then loop from n down to 2.
 *
 * Actually, let me simplify: for n<=1, return local[0] (= n).
 * For n>1, iterate.
 *
 * At the "done" label, we return local[2].
 * But for n=0: local[2] = 1, which is wrong.
 * Let me return local[1] when we arrive at done from the n<=1 check,
 * and local[2] when we arrive from the loop.
 *
 * Simpler: just handle n=0 and n=1 as special cases at the beginning,
 * then for n>1, the loop runs until n becomes 1, and we return local[2].
 *
 * For n=0: return local[1] (=0)
 * For n=1: return local[2] (=1)
 * But we need to distinguish. Let me use a different approach:
 *
 *   if n == 0: return 0
 *   if n == 1: return 1
 *   a = 0, b = 1
 *   loop: tmp = a+b; a = b; b = tmp; n--; if n > 1 goto loop
 *   return b
 *
 * Let me encode this properly:
 */
static int build_fibonacci_bytecode(uint8_t *code, size_t code_size,
                                    vtx_value_t *const_pool, uint16_t *const_count,
                                    int64_t n_value)
{
    int ci = 0;  /* constant pool index */
    int pc = 0;  /* bytecode PC */

    /* Constants */
    const_pool[ci] = vtx_make_smi(n_value);   /* const 0: n */
    uint16_t const_n = ci; ci++;
    const_pool[ci] = vtx_make_smi(0);         /* const 1: 0 */
    uint16_t const_0 = ci; ci++;
    const_pool[ci] = vtx_make_smi(1);         /* const 2: 1 */
    uint16_t const_1 = ci; ci++;

    *const_count = (uint16_t)ci;

    /* Helper macro to emit bytes */
    #define EMIT1(b) do { if (pc < (int)code_size) code[pc] = (b); pc++; } while(0)
    #define EMIT_OP(op) EMIT1((uint8_t)(op))
    #define EMIT_U16(val) do { \
        if (pc + 1 < (int)code_size) { \
            code[pc] = (uint8_t)(((val) >> 8) & 0xFF); \
            code[pc+1] = (uint8_t)((val) & 0xFF); \
        } \
        pc += 2; \
    } while(0)

    /* 000: LOAD_CONST_INT const_n  */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(const_n);
    /* 003: STORE_LOCAL 0  (n) */
    EMIT_OP(VT_OP_STORE_LOCAL); EMIT_U16(0);

    /* 006: LOAD_CONST_INT const_0  */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(const_0);
    /* 009: STORE_LOCAL 1  (a = 0) */
    EMIT_OP(VT_OP_STORE_LOCAL); EMIT_U16(1);

    /* 012: LOAD_CONST_INT const_1  */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(const_1);
    /* 015: STORE_LOCAL 2  (b = 1) */
    EMIT_OP(VT_OP_STORE_LOCAL); EMIT_U16(2);

    /* 018: LOAD_LOCAL 0 */
    EMIT_OP(VT_OP_LOAD_LOCAL); EMIT_U16(0);
    /* 021: LOAD_CONST_INT const_1 */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(const_1);
    /* 024: ICMP_LE */
    EMIT_OP(VT_OP_ICMP_LE);
    /* 025: IF_TRUE <done> — we'll patch this later */
    int if_true_pc = pc;
    EMIT_OP(VT_OP_IF_TRUE); EMIT_U16(0); /* placeholder */

    /* 028: loop start */
    int loop_start = pc;

    /* LOAD_LOCAL 1, LOAD_LOCAL 2, IADD, STORE_LOCAL 3 */
    EMIT_OP(VT_OP_LOAD_LOCAL); EMIT_U16(1);
    EMIT_OP(VT_OP_LOAD_LOCAL); EMIT_U16(2);
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL); EMIT_U16(3);

    /* LOAD_LOCAL 2, STORE_LOCAL 1 */
    EMIT_OP(VT_OP_LOAD_LOCAL); EMIT_U16(2);
    EMIT_OP(VT_OP_STORE_LOCAL); EMIT_U16(1);

    /* LOAD_LOCAL 3, STORE_LOCAL 2 */
    EMIT_OP(VT_OP_LOAD_LOCAL); EMIT_U16(3);
    EMIT_OP(VT_OP_STORE_LOCAL); EMIT_U16(2);

    /* LOAD_LOCAL 0, LOAD_CONST_INT const_1, ISUB, STORE_LOCAL 0 */
    EMIT_OP(VT_OP_LOAD_LOCAL); EMIT_U16(0);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(const_1);
    EMIT_OP(VT_OP_ISUB);
    EMIT_OP(VT_OP_STORE_LOCAL); EMIT_U16(0);

    /* LOAD_LOCAL 0, LOAD_CONST_INT const_1, ICMP_GT */
    EMIT_OP(VT_OP_LOAD_LOCAL); EMIT_U16(0);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(const_1);
    EMIT_OP(VT_OP_ICMP_GT);

    /* IF_TRUE <loop_start> */
    EMIT_OP(VT_OP_IF_TRUE); EMIT_U16((uint16_t)loop_start);

    /* done: */
    int done_pc = pc;

    /* For n<=1: we want to return n (local[0])
     * For n>1: we want to return b (local[2])
     * We can use: if n <= 1 return local[1] (which is a=0 when n=0, a=0 when n=1
     *   -- wait that's wrong)
     *
     * Simpler: at done, we check if n was originally <= 1.
     * But we've been modifying n. Let me store n in another local first.
     *
     * Actually, let me just handle it differently:
     *   At done, if the original n was 0, local[1]=0, local[2]=1.
     *   We want to return local[1] for n=0 and local[2] for n>=1.
     *
     * Let me use another local: local[4] = original n.
     * If local[4] == 0, return local[1]; else return local[2].
     *
     * Actually, the simplest fix: just initialize a=n, b=0 if n==0.
     * But we've already emitted the code. Let me just patch the done label
     * to always return local[2] and for n=0, local[2]=1 which is wrong.
     *
     * The easiest fix: save original n in local[4], then at done:
     *   if local[4] == 0, return local[1], else return local[2].
     *
     * But I've already emitted the code up to the IF_TRUE patch point.
     * Let me just use a different strategy: at the done label, check if
     * local[0] (current n) is the original value. If n<=1 and we came
     * from the initial check, local[0] is still the original n.
     * If n>1 and we came from the loop, local[0] has been decremented.
     *
     * Wait: in the n<=1 case, we jump to done immediately, so local[0]
     * is still the original n. In the n>1 case, after the loop, local[0]
     * has been decremented. So at done:
     *   if local[0] == 0: return local[1] (= 0 for n=0)
     *   else: return local[2]
     *
     * For n=0: local[0]=0, local[1]=0, local[2]=1 → return local[1]=0 ✓
     * For n=1: local[0]=1, local[1]=0, local[2]=1 → return local[2]=1 ✓
     * For n=10: after loop local[0]=1, local[2]=55 → return local[2]=55 ✓
     *
     * Great! Let me add the conditional at done.
     */

    /* done: LOAD_LOCAL 0, LOAD_CONST_INT const_0, ICMP_EQ, IF_TRUE <ret_a> */
    EMIT_OP(VT_OP_LOAD_LOCAL); EMIT_U16(0);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(const_0);
    EMIT_OP(VT_OP_ICMP_EQ);
    int if_zero_pc = pc;
    EMIT_OP(VT_OP_IF_TRUE); EMIT_U16(0); /* placeholder for ret_a */

    /* ret_b: LOAD_LOCAL 2, RETURN_VALUE */
    EMIT_OP(VT_OP_LOAD_LOCAL); EMIT_U16(2);
    EMIT_OP(VT_OP_RETURN_VALUE);

    /* ret_a: LOAD_LOCAL 1, RETURN_VALUE */
    int ret_a_pc = pc;
    EMIT_OP(VT_OP_LOAD_LOCAL); EMIT_U16(1);
    EMIT_OP(VT_OP_RETURN_VALUE);

    /* Patch the IF_TRUE at if_zero_pc to jump to ret_a_pc */
    code[if_true_pc + 1] = (uint8_t)((done_pc >> 8) & 0xFF);
    code[if_true_pc + 2] = (uint8_t)(done_pc & 0xFF);

    /* Patch the IF_TRUE at if_zero_pc to jump to ret_a_pc */
    code[if_zero_pc + 1] = (uint8_t)((ret_a_pc >> 8) & 0xFF);
    code[if_zero_pc + 2] = (uint8_t)(ret_a_pc & 0xFF);

    #undef EMIT1
    #undef EMIT_OP
    #undef EMIT_U16

    return pc; /* total bytecode length */
}

/* ========================================================================== */
/* Self-test runner                                                            */
/* ========================================================================== */

static int run_self_test(void)
{
    int passed = 0;
    int failed = 0;

    printf("=== VORTEX Runtime Self-Test ===\n\n");

    /* ---- Test 1: Arena allocator ---- */
    {
        printf("[Test] Arena allocator... ");
        vtx_arena_t arena;
        if (vtx_arena_init(&arena) != 0) {
            printf("FAIL (init)\n");
            failed++;
        } else {
            void *p1 = vtx_arena_alloc(&arena, 64);
            void *p2 = vtx_arena_alloc(&arena, 128);
            void *p3 = vtx_arena_alloc(&arena, 256);

            if (p1 == NULL || p2 == NULL || p3 == NULL) {
                printf("FAIL (alloc)\n");
                failed++;
            } else if (((uintptr_t)p1 & 0xF) != 0 || ((uintptr_t)p2 & 0xF) != 0 ||
                       ((uintptr_t)p3 & 0xF) != 0) {
                printf("FAIL (alignment)\n");
                failed++;
            } else {
                /* Test reset */
                vtx_arena_reset(&arena);
                void *p4 = vtx_arena_alloc(&arena, 64);
                if (p4 == NULL) {
                    printf("FAIL (alloc after reset)\n");
                    failed++;
                } else {
                    printf("PASS\n");
                    passed++;
                }
            }
            vtx_arena_destroy(&arena);
        }
    }

    /* ---- Test 2: Tagged values ---- */
    {
        printf("[Test] Tagged values... ");
        bool ok = true;

        /* SMI */
        vtx_value_t smi_v = vtx_make_smi(42);
        if (!vtx_is_smi(smi_v) || vtx_smi_value(smi_v) != 42) ok = false;

        vtx_value_t smi_neg = vtx_make_smi(-100);
        if (!vtx_is_smi(smi_neg) || vtx_smi_value(smi_neg) != -100) ok = false;

        /* Boolean */
        vtx_value_t bv_t = vtx_make_bool(true);
        vtx_value_t bv_f = vtx_make_bool(false);
        if (!vtx_is_bool(bv_t) || !vtx_bool_value(bv_t) ||
            !vtx_is_bool(bv_f) || vtx_bool_value(bv_f)) ok = false;

        /* Null and undefined */
        vtx_value_t nv = vtx_make_null();
        vtx_value_t uv = vtx_make_undefined();
        if (!vtx_is_null(nv) || vtx_is_undefined(nv) ||
            !vtx_is_undefined(uv) || vtx_is_null(uv)) ok = false;

        /* Double */
        vtx_value_t dv = vtx_make_double(3.14159);
        if (!vtx_is_double(dv)) ok = false;
        double recovered = vtx_double_value(dv);
        if (recovered != 3.14159) ok = false;

        /* Negative double */
        vtx_value_t dv2 = vtx_make_double(-2.71828);
        if (!vtx_is_double(dv2) || vtx_double_value(dv2) != -2.71828) ok = false;

        /* Double NaN */
        vtx_value_t dnan = vtx_make_double(NAN);
        if (!vtx_is_double(dnan)) ok = false;

        /* Double zero */
        vtx_value_t dz = vtx_make_double(0.0);
        if (!vtx_is_double(dz) || vtx_double_value(dz) != 0.0) ok = false;

        /* Heap pointer */
        uint64_t fake_obj = 0;
        void *aligned_ptr = (void *)((uintptr_t)&fake_obj & ~(uintptr_t)0x7);
        if ((uintptr_t)aligned_ptr % 8 != 0) {
            /* Find an aligned address */
            aligned_ptr = (void *)((((uintptr_t)&fake_obj) + 7) & ~(uintptr_t)7);
        }
        vtx_value_t hp = vtx_make_heap_ptr(aligned_ptr);
        if (!vtx_is_heap_ptr(hp) || vtx_heap_ptr(hp) != aligned_ptr) ok = false;

        /* Cross-type checks: SMI is not double, double is not bool, etc. */
        if (vtx_is_double(smi_v) || vtx_is_smi(dv) || vtx_is_bool(nv) ||
            vtx_is_null(uv) || vtx_is_undefined(nv)) ok = false;

        if (ok) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL\n");
            failed++;
        }
    }

    /* ---- Test 3: Type system ---- */
    {
        printf("[Test] Type system... ");
        vtx_type_system_t ts;
        bool ok = true;

        if (vtx_type_system_init(&ts) != 0) {
            printf("FAIL (init)\n");
            failed++;
        } else {
            /* Object type should exist */
            const vtx_type_desc_t *obj_td = vtx_type_get(&ts, VTX_TYPE_OBJECT);
            if (obj_td == NULL || strcmp(obj_td->name, "Object") != 0) ok = false;

            /* Register a subclass "Integer" */
            vtx_field_desc_t *int_fields = (vtx_field_desc_t *)calloc(1, sizeof(vtx_field_desc_t));
            int_fields[0].name = "value";
            int_fields[0].type = VTX_TYPE_OBJECT;

            vtx_typeid_t int_type = vtx_type_register(&ts, "Integer", VTX_TYPE_OBJECT,
                                                       1, int_fields, 0, NULL);
            if (int_type == VTX_TYPE_INVALID) ok = false;

            /* Check subtype */
            if (!vtx_type_is_subtype(&ts, int_type, VTX_TYPE_OBJECT)) ok = false;
            if (vtx_type_is_subtype(&ts, VTX_TYPE_OBJECT, int_type)) ok = false;

            /* Register a sub-subclass "SmallInteger" */
            vtx_field_desc_t *si_fields = (vtx_field_desc_t *)calloc(1, sizeof(vtx_field_desc_t));
            si_fields[0].name = "range";
            si_fields[0].type = VTX_TYPE_OBJECT;

            vtx_typeid_t si_type = vtx_type_register(&ts, "SmallInteger", int_type,
                                                      1, si_fields, 0, NULL);

            if (!vtx_type_is_subtype(&ts, si_type, int_type)) ok = false;
            if (!vtx_type_is_subtype(&ts, si_type, VTX_TYPE_OBJECT)) ok = false;

            /* Check instance size */
            uint32_t isz = vtx_type_instance_size(&ts, int_type);
            if (isz < VTX_HEAP_OBJECT_HEADER_SIZE + sizeof(vtx_value_t)) ok = false;

            if (ok) {
                printf("PASS\n");
                passed++;
            } else {
                printf("FAIL\n");
                failed++;
            }
            vtx_type_system_destroy(&ts);
        }
    }

    /* ---- Test 4: Inline cache ---- */
    {
        printf("[Test] Inline cache... ");
        vtx_type_system_t ts;
        vtx_type_system_init(&ts);

        bool ok = true;
        vtx_inline_cache_t ic;
        vtx_ic_init(&ic);

        if (ic.state != VT_IC_MONOMORPHIC || ic.count != 0) ok = false;

        /* Monomorphic insertion */
        vtx_method_desc_t m1 = {.name = "foo", .is_virtual = true};
        vtx_ic_update(&ic, 2, &m1);
        if (ic.state != VT_IC_MONOMORPHIC || ic.count != 1) ok = false;

        const vtx_method_desc_t *found = vtx_ic_lookup(&ic, 2);
        if (found != &m1) ok = false;

        /* Polymorphic insertion */
        vtx_method_desc_t m2 = {.name = "bar", .is_virtual = true};
        vtx_ic_update(&ic, 3, &m2);
        if (ic.state != VT_IC_POLYMORPHIC || ic.count != 2) ok = false;

        found = vtx_ic_lookup(&ic, 3);
        if (found != &m2) ok = false;

        /* Lookup miss */
        found = vtx_ic_lookup(&ic, 99);
        if (found != NULL) ok = false;

        /* Fill up to megamorphic */
        vtx_method_desc_t m3 = {.name = "baz", .is_virtual = true};
        vtx_method_desc_t m4 = {.name = "qux", .is_virtual = true};
        vtx_method_desc_t m5 = {.name = "quux", .is_virtual = true};
        vtx_ic_update(&ic, 4, &m3);
        vtx_ic_update(&ic, 5, &m4);
        if (ic.state != VT_IC_POLYMORPHIC) ok = false;
        vtx_ic_update(&ic, 6, &m5); /* VTX_POLY_LIMIT=4, so 5th different type → megamorphic */
        if (ic.state != VT_IC_MEGAMORPHIC) ok = false;

        if (ok) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL (state=%d, count=%u)\n", ic.state, ic.count);
            failed++;
        }

        vtx_type_system_destroy(&ts);
    }

    /* ---- Test 5: Bytecode ---- */
    {
        printf("[Test] Bytecode... ");
        bool ok = true;

        /* Check opcode table */
        if (strcmp(vtx_opcode_table[VT_OP_HALT].name, "VT_OP_HALT") != 0) ok = false;
        if (vtx_opcode_table[VT_OP_IADD].stack_input_count != 2) ok = false;
        if (vtx_opcode_table[VT_OP_IADD].stack_output_count != 1) ok = false;
        if (vtx_opcode_table[VT_OP_LOAD_LOCAL].has_operand != true) ok = false;
        if (vtx_opcode_table[VT_OP_LOAD_LOCAL].operand_size != 2) ok = false;

        /* Check stack effect */
        if (vtx_bytecode_stack_effect(VT_OP_IADD) != -1) ok = false;  /* 1 - 2 = -1 */
        if (vtx_bytecode_stack_effect(VT_OP_LOAD_LOCAL) != 1) ok = false;  /* 1 - 0 = 1 */
        if (vtx_bytecode_stack_effect(VT_OP_DUP) != 1) ok = false;  /* 2 - 1 = 1 */

        /* Check disassembly */
        uint8_t code[] = { VT_OP_IADD };
        vtx_bytecode_t bc = {.code = code, .length = 1, .constant_pool = NULL, .constant_count = 0,
                             .max_locals = 0, .max_stack = 0};
        char buf[128];
        size_t next_pc = vtx_bytecode_disassemble_op(&bc, 0, buf, sizeof(buf));
        if (next_pc != 1 || strstr(buf, "IADD") == NULL) ok = false;

        if (ok) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL\n");
            failed++;
        }
    }

    /* ---- Test 6: GC ---- */
    {
        printf("[Test] GC allocation... ");
        vtx_type_system_t ts;
        vtx_type_system_init(&ts);

        /* Register a simple type with one field */
        vtx_field_desc_t *fields = (vtx_field_desc_t *)calloc(1, sizeof(vtx_field_desc_t));
        fields[0].name = "val";
        fields[0].type = VTX_TYPE_OBJECT;
        vtx_typeid_t point_type = vtx_type_register(&ts, "Point", VTX_TYPE_OBJECT,
                                                     1, fields, 0, NULL);

        vtx_gc_t gc;
        bool ok = true;

        if (vtx_gc_init(&gc, &ts) != 0) {
            printf("FAIL (init)\n");
            failed++;
        } else {
            /* Allocate some objects */
            vtx_heap_object_t *obj1 = vtx_gc_alloc(&gc,
                vtx_heap_object_alloc_size(1), point_type);
            vtx_heap_object_t *obj2 = vtx_gc_alloc(&gc,
                vtx_heap_object_alloc_size(1), point_type);

            if (obj1 == NULL || obj2 == NULL) {
                ok = false;
            } else {
                /* Set fields */
                vtx_object_set_field(obj1, 0, vtx_make_smi(10));
                vtx_object_set_field(obj2, 0, vtx_make_smi(20));

                /* Read back */
                if (vtx_smi_value(vtx_object_get_field(obj1, 0)) != 10 ||
                    vtx_smi_value(vtx_object_get_field(obj2, 0)) != 20) {
                    ok = false;
                }

                /* Root the objects */
                vtx_gc_root_push(&gc, vtx_make_heap_ptr(obj1));
                vtx_gc_root_push(&gc, vtx_make_heap_ptr(obj2));

                /* Young gen should have objects */
                if (vtx_gc_young_used(&gc) == 0) ok = false;

                /* Trigger a young-gen collection */
                vtx_gc_collect_young(&gc);

                /* Objects should still be accessible (they are rooted) */
                /* Note: after collection, the objects may have moved.
                 * We need to use the root stack values to find them. */
                vtx_value_t v1 = vtx_gc_root_pop(&gc);
                vtx_value_t v2 = vtx_gc_root_pop(&gc);

                if (!vtx_is_heap_ptr(v1) || !vtx_is_heap_ptr(v2)) {
                    ok = false;
                } else {
                    vtx_heap_object_t *new_obj1 = (vtx_heap_object_t *)vtx_heap_ptr(v1);
                    vtx_heap_object_t *new_obj2 = (vtx_heap_object_t *)vtx_heap_ptr(v2);

                    if (vtx_smi_value(vtx_object_get_field(new_obj1, 0)) != 10 ||
                        vtx_smi_value(vtx_object_get_field(new_obj2, 0)) != 20) {
                        ok = false;
                    }
                }
            }

            if (ok) {
                printf("PASS\n");
                passed++;
            } else {
                printf("FAIL\n");
                failed++;
            }
            vtx_gc_destroy(&gc);
        }
        vtx_type_system_destroy(&ts);
    }

    /* ---- Test 7: Helpers ---- */
    {
        printf("[Test] Helper functions... ");
        bool ok = true;

        /* Overflow checks */
        if (!vtx_helpers_overflow_check_iadd(100, 200)) ok = false;
        if (vtx_helpers_overflow_check_iadd(INT64_MAX, 1)) ok = false;
        if (vtx_helpers_overflow_check_iadd(INT64_MIN, -1)) ok = false;
        if (!vtx_helpers_overflow_check_iadd(0, 0)) ok = false;

        if (!vtx_helpers_overflow_check_imul(100, 200)) ok = false;
        if (vtx_helpers_overflow_check_imul(INT64_MAX / 2, 2)) ok = false; /* just at the edge */
        if (vtx_helpers_overflow_check_imul(INT64_MAX, 2)) ok = false;
        if (!vtx_helpers_overflow_check_imul(0, INT64_MAX)) ok = false;

        /* Bounds check */
        if (!vtx_helpers_bounds_check(0, 10)) ok = false;
        if (!vtx_helpers_bounds_check(9, 10)) ok = false;

        /* These should trap, so we can't test them here without catching signals.
         * Just test the passing cases. */

        if (ok) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL\n");
            failed++;
        }
    }

    /* ---- Test 8: Fibonacci bytecode interpreter ---- */
    {
        printf("[Test] Fibonacci interpreter... ");
        bool ok = true;

        vtx_type_system_t ts;
        vtx_type_system_init(&ts);

        vtx_gc_t gc;
        vtx_gc_init(&gc, &ts);

        /* Test fibonacci for several values */
        struct { int64_t n; int64_t expected; } fib_tests[] = {
            {0, 0},
            {1, 1},
            {2, 1},
            {3, 2},
            {5, 5},
            {10, 55},
            {20, 6765},
        };

        int num_tests = sizeof(fib_tests) / sizeof(fib_tests[0]);

        for (int i = 0; i < num_tests; i++) {
            uint8_t code[256];
            vtx_value_t const_pool[4];
            uint16_t const_count = 0;

            int code_len = build_fibonacci_bytecode(code, sizeof(code),
                                                     const_pool, &const_count,
                                                     fib_tests[i].n);

            vtx_bytecode_t bc = {
                .code = code,
                .length = (size_t)code_len,
                .constant_pool = const_pool,
                .constant_count = const_count,
                .max_locals = 8,
                .max_stack = 16
            };

            vtx_interp_t interp;
            interp_init(&interp, &gc, &ts);
            (void)interp_push_frame(&interp, &bc);

            /* Copy constants to locals for the initial setup */
            /* The bytecode handles this itself */

            vtx_value_t result = interp_run(&interp);

            if (!vtx_is_smi(result)) {
                printf("FAIL (fib(%lld) returned non-SMI)\n",
                       (long long)fib_tests[i].n);
                ok = false;
                break;
            }

            int64_t got = vtx_smi_value(result);
            if (got != fib_tests[i].expected) {
                printf("FAIL (fib(%lld) = %lld, expected %lld)\n",
                       (long long)fib_tests[i].n,
                       (long long)got,
                       (long long)fib_tests[i].expected);
                ok = false;
                break;
            }
        }

        if (ok) {
            printf("PASS\n");
            passed++;
        } else {
            failed++;
        }

        vtx_gc_destroy(&gc);
        vtx_type_system_destroy(&ts);
    }

    /* ---- Summary ---- */
    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);

    return failed > 0 ? 1 : 0;
}

/* ========================================================================== */
/* Main entry point                                                            */
/* ========================================================================== */

int main(int argc, char *argv[])
{
    if (argc > 1) {
        if (strcmp(argv[1], "--test") == 0) {
            return run_self_test();
        }
        /* Otherwise, treat argv[1] as a bytecode file (not yet implemented) */
        fprintf(stderr, "VORTEX: bytecode file loading not yet implemented\n");
        fprintf(stderr, "Usage: vortex [--test]\n");
        return 1;
    }

    /* Default: run self-test */
    printf("VORTEX JIT Compiler v0.1.0\n");
    printf("Running self-test (use --test for explicit test mode)...\n\n");
    return run_self_test();
}
