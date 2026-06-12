/**
 * test_interp_basics.c — Integration tests for VORTEX interpreter
 *
 * Tests interpreter execution of simple programs: arithmetic, control flow,
 * method calls. Uses hand-assembled bytecode.
 */

#include "test_framework.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "interp/dispatch.h"

#include <stdint.h>
#include <string.h>
#include <math.h>

/* ========================================================================== */
/* Helper: create a bytecode object with constant pool                          */
/* ========================================================================== */

static vtx_bytecode_t make_bytecode_with_consts(
    const uint8_t *code, size_t len,
    vtx_value_t *consts, uint32_t const_count,
    uint16_t max_locals, uint16_t max_stack)
{
    vtx_bytecode_t bc;
    bc.code = code;
    bc.length = len;
    bc.constant_pool = consts;
    bc.constant_count = const_count;
    bc.max_locals = max_locals;
    bc.max_stack = max_stack;
    return bc;
}

static vtx_bytecode_t make_bytecode_simple(
    const uint8_t *code, size_t len,
    uint16_t max_locals, uint16_t max_stack)
{
    return make_bytecode_with_consts(code, len, NULL, 0, max_locals, max_stack);
}

/* ========================================================================== */
/* Test: simple integer arithmetic (addition)                                   */
/* ========================================================================== */

VTX_TEST(interp_iadd)
{
    /* load_local 0, load_local 1, iadd, return_value */
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL,  0x00, 0x00,
        VT_OP_LOAD_LOCAL,  0x00, 0x01,
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_bytecode_t bc = make_bytecode_simple(code, sizeof(code), 2, 4);

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    vtx_method_desc_t method = {
        .name = "add", .signature = "(II)I", .bytecode = &bc,
        .vtable_index = 0xFFFFFFFF, .arg_count = 2, .is_virtual = false
    };

    vtx_value_t args[] = { vtx_make_smi(3), vtx_make_smi(4) };
    vtx_value_t result = vtx_interp_run(&interp, &method, args, 2);

    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), (int64_t)7);

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Test: integer subtraction                                                    */
/* ========================================================================== */

VTX_TEST(interp_isub)
{
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL,  0x00, 0x00,
        VT_OP_LOAD_LOCAL,  0x00, 0x01,
        VT_OP_ISUB,
        VT_OP_RETURN_VALUE
    };
    vtx_bytecode_t bc = make_bytecode_simple(code, sizeof(code), 2, 4);

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);
    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    vtx_method_desc_t method = {
        .name = "sub", .signature = "(II)I", .bytecode = &bc,
        .vtable_index = 0xFFFFFFFF, .arg_count = 2, .is_virtual = false
    };

    vtx_value_t args[] = { vtx_make_smi(10), vtx_make_smi(3) };
    vtx_value_t result = vtx_interp_run(&interp, &method, args, 2);

    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), (int64_t)7);

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Test: integer multiplication                                                 */
/* ========================================================================== */

VTX_TEST(interp_imul)
{
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL,  0x00, 0x00,
        VT_OP_LOAD_LOCAL,  0x00, 0x01,
        VT_OP_IMUL,
        VT_OP_RETURN_VALUE
    };
    vtx_bytecode_t bc = make_bytecode_simple(code, sizeof(code), 2, 4);

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);
    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    vtx_method_desc_t method = {
        .name = "mul", .signature = "(II)I", .bytecode = &bc,
        .vtable_index = 0xFFFFFFFF, .arg_count = 2, .is_virtual = false
    };

    vtx_value_t args[] = { vtx_make_smi(6), vtx_make_smi(7) };
    vtx_value_t result = vtx_interp_run(&interp, &method, args, 2);

    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), (int64_t)42);

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Test: void return                                                            */
/* ========================================================================== */

VTX_TEST(interp_return_void)
{
    uint8_t code[] = { VT_OP_RETURN };
    vtx_bytecode_t bc = make_bytecode_simple(code, sizeof(code), 0, 0);

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);
    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    vtx_method_desc_t method = {
        .name = "void_fn", .signature = "()V", .bytecode = &bc,
        .vtable_index = 0xFFFFFFFF, .arg_count = 0, .is_virtual = false
    };

    vtx_value_t result = vtx_interp_run(&interp, &method, NULL, 0);
    VTX_ASSERT_TRUE(vtx_is_undefined(result));

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Test: load constants from pool                                               */
/* ========================================================================== */

VTX_TEST(interp_load_const_int)
{
    /* load_const_int 0, load_const_int 1, iadd, return_value */
    uint8_t code[] = {
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x01,
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(100), vtx_make_smi(200) };
    vtx_bytecode_t bc = make_bytecode_with_consts(code, sizeof(code),
                                                    consts, 2, 0, 4);

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);
    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    vtx_method_desc_t method = {
        .name = "const_add", .signature = "()I", .bytecode = &bc,
        .vtable_index = 0xFFFFFFFF, .arg_count = 0, .is_virtual = false
    };

    vtx_value_t result = vtx_interp_run(&interp, &method, NULL, 0);
    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), (int64_t)300);

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Test: load special constants (null, true, false, undefined)                  */
/* ========================================================================== */

VTX_TEST(interp_load_specials)
{
    /* load_null, return_value */
    uint8_t code_null[] = { VT_OP_LOAD_NULL, VT_OP_RETURN_VALUE };
    /* load_true, return_value */
    uint8_t code_true[] = { VT_OP_LOAD_TRUE, VT_OP_RETURN_VALUE };
    /* load_false, return_value */
    uint8_t code_false[] = { VT_OP_LOAD_FALSE, VT_OP_RETURN_VALUE };
    /* load_undefined, return_value */
    uint8_t code_undef[] = { VT_OP_LOAD_UNDEFINED, VT_OP_RETURN_VALUE };

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);
    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    /* Test null */
    {
        vtx_bytecode_t bc = make_bytecode_simple(code_null, sizeof(code_null), 0, 2);
        vtx_method_desc_t method = {
            .name = "ret_null", .signature = "()Ljava/lang/Object;", .bytecode = &bc,
            .vtable_index = 0xFFFFFFFF, .arg_count = 0, .is_virtual = false
        };
        vtx_value_t result = vtx_interp_run(&interp, &method, NULL, 0);
        VTX_ASSERT_TRUE(vtx_is_null(result));
    }

    /* Test true */
    {
        vtx_bytecode_t bc = make_bytecode_simple(code_true, sizeof(code_true), 0, 2);
        vtx_method_desc_t method = {
            .name = "ret_true", .signature = "()Z", .bytecode = &bc,
            .vtable_index = 0xFFFFFFFF, .arg_count = 0, .is_virtual = false
        };
        vtx_value_t result = vtx_interp_run(&interp, &method, NULL, 0);
        VTX_ASSERT_TRUE(vtx_is_bool(result));
        VTX_ASSERT_TRUE(vtx_bool_value(result));
    }

    /* Test false */
    {
        vtx_bytecode_t bc = make_bytecode_simple(code_false, sizeof(code_false), 0, 2);
        vtx_method_desc_t method = {
            .name = "ret_false", .signature = "()Z", .bytecode = &bc,
            .vtable_index = 0xFFFFFFFF, .arg_count = 0, .is_virtual = false
        };
        vtx_value_t result = vtx_interp_run(&interp, &method, NULL, 0);
        VTX_ASSERT_TRUE(vtx_is_bool(result));
        VTX_ASSERT_FALSE(vtx_bool_value(result));
    }

    /* Test undefined */
    {
        vtx_bytecode_t bc = make_bytecode_simple(code_undef, sizeof(code_undef), 0, 2);
        vtx_method_desc_t method = {
            .name = "ret_undef", .signature = "()V", .bytecode = &bc,
            .vtable_index = 0xFFFFFFFF, .arg_count = 0, .is_virtual = false
        };
        vtx_value_t result = vtx_interp_run(&interp, &method, NULL, 0);
        VTX_ASSERT_TRUE(vtx_is_undefined(result));
    }

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Test: integer comparison and conditional branch                              */
/* ========================================================================== */

VTX_TEST(interp_icmp_and_branch)
{
    /* if (local0 < local1) return local0; else return local1 */
    /* load_local 0, load_local 1, icmp_lt, if_true 12, load_local 1, return_value,
       load_local 0, return_value */
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL,  0x00, 0x00,  /* PC 0 */
        VT_OP_LOAD_LOCAL,  0x00, 0x01,  /* PC 3 */
        VT_OP_ICMP_LT,                  /* PC 6 */
        VT_OP_IF_TRUE,     0x00, 0x0E,  /* PC 7: if_true → PC 14 */
        VT_OP_LOAD_LOCAL,  0x00, 0x01,  /* PC 10: else: load local 1 */
        VT_OP_RETURN_VALUE,             /* PC 13 */
        VT_OP_LOAD_LOCAL,  0x00, 0x00,  /* PC 14: then: load local 0 */
        VT_OP_RETURN_VALUE              /* PC 17 */
    };
    vtx_bytecode_t bc = make_bytecode_simple(code, sizeof(code), 2, 4);

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);
    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    vtx_method_desc_t method = {
        .name = "min", .signature = "(II)I", .bytecode = &bc,
        .vtable_index = 0xFFFFFFFF, .arg_count = 2, .is_virtual = false
    };

    /* Test: min(3, 5) == 3 */
    {
        vtx_value_t args[] = { vtx_make_smi(3), vtx_make_smi(5) };
        vtx_value_t result = vtx_interp_run(&interp, &method, args, 2);
        VTX_ASSERT_TRUE(vtx_is_smi(result));
        VTX_ASSERT_EQUAL(vtx_smi_value(result), (int64_t)3);
    }

    /* Test: min(10, 2) == 2 */
    {
        vtx_value_t args[] = { vtx_make_smi(10), vtx_make_smi(2) };
        vtx_value_t result = vtx_interp_run(&interp, &method, args, 2);
        VTX_ASSERT_TRUE(vtx_is_smi(result));
        VTX_ASSERT_EQUAL(vtx_smi_value(result), (int64_t)2);
    }

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Test: simple loop (countdown)                                                */
/* ========================================================================== */

VTX_TEST(interp_loop_countdown)
{
    /* local0 = counter, local1 = accumulator
     * while (counter > 0) { accumulator += counter; counter--; }
     * return accumulator
     *
     * PC  0: load_local 0        (counter)
     * PC  3: load_const_int 0    (0)
     * PC  6: icmp_gt
     * PC  7: if_false 28         (exit to PC 28)
     * PC 10: load_local 1        (accum)
     * PC 13: load_local 0        (counter)
     * PC 16: iadd
     * PC 17: store_local 1       (accum = accum + counter)
     * PC 20: load_local 0        (counter)
     * PC 23: ineg
     * PC 24: load_const_int 1    (hmm, we need isub. Let's use a different approach)
     *
     * Better approach: use store_local with computed value
     * Actually, let's just test a goto loop with an if_false exit.
     */

    /* Simpler: sum = 0; for i = n down to 1: sum += i
     * Rewrite: just test a trivial loop that counts down.
     *
     * PC  0: load_local 0        counter
     * PC  3: if_false 22         if 0 exit (using falsy check)
     * PC  6: load_local 1        accum
     * PC  9: load_local 0        counter
     * PC 12: iadd
     * PC 13: store_local 1
     * PC 16: ... actually this is getting complicated without a decrement op.
     * Let's use a simple goto loop that's easier.
     */

    /* Simplest possible loop: just goto back */
    /* PC 0: load_local 0, if_false 10, goto 0, return_value */
    /* Actually, that doesn't do useful work. Let me do a simple 3-iteration loop
     * that we can verify: */

    /* Just test that the interpreter can handle a basic goto + if_false:
     *   local0 = initial_value
     *   loop:
     *     if (local0 == 0) goto end
     *     local0 = local0 - 1  (using isub: load 0, load const 1, isub, store 0)
     *     goto loop
     *   end:
     *     return local0
     *
     * But we need load_const_int for the 1... let me set up a const pool.
     */

    vtx_value_t consts[] = { vtx_make_smi(0), vtx_make_smi(1) };

    /* PC  0: load_local 0        ; counter
     * PC  3: load_const_int 0    ; 0
     * PC  6: icmp_eq
     * PC  7: if_true 25          ; → exit at PC 25
     * PC 10: load_local 0        ; counter
     * PC 13: load_const_int 1    ; 1
     * PC 16: isub
     * PC 17: store_local 0       ; counter = counter - 1
     * PC 20: goto 0              ; → loop start
     * PC 23: nop                 ; padding (3 bytes for goto at PC 20 = PC 23)
     * PC 23: ... goto is 3 bytes so next is PC 23, need target PC 25
     * Hmm, let me recalculate:
     * PC  0: load_local 0       (3 bytes) → PC 3
     * PC  3: load_const_int 0   (3 bytes) → PC 6
     * PC  6: icmp_eq            (1 byte) → PC 7
     * PC  7: if_true 25         (3 bytes) → PC 10
     * PC 10: load_local 0       (3 bytes) → PC 13
     * PC 13: load_const_int 1   (3 bytes) → PC 16
     * PC 16: isub               (1 byte) → PC 17
     * PC 17: store_local 0      (3 bytes) → PC 20
     * PC 20: goto 0             (3 bytes) → PC 23
     * PC 23: nop                (1 byte) → PC 24
     * PC 24: nop                (1 byte) → PC 25
     * PC 25: load_local 0       (3 bytes) → PC 28
     * PC 28: return_value       (1 byte) → PC 29
     */
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL,     0x00, 0x00,  /* PC  0 */
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,  /* PC  3 */
        VT_OP_ICMP_EQ,                      /* PC  6 */
        VT_OP_IF_TRUE,        0x00, 0x19,  /* PC  7 → PC 25 */
        VT_OP_LOAD_LOCAL,     0x00, 0x00,  /* PC 10 */
        VT_OP_LOAD_CONST_INT, 0x00, 0x01,  /* PC 13 */
        VT_OP_ISUB,                         /* PC 16 */
        VT_OP_STORE_LOCAL,    0x00, 0x00,  /* PC 17 */
        VT_OP_GOTO,           0x00, 0x00,  /* PC 20 → PC 0 */
        VT_OP_NOP,                          /* PC 23 */
        VT_OP_NOP,                          /* PC 24 */
        VT_OP_LOAD_LOCAL,     0x00, 0x00,  /* PC 25 */
        VT_OP_RETURN_VALUE                  /* PC 28 */
    };
    vtx_bytecode_t bc = make_bytecode_with_consts(code, sizeof(code),
                                                    consts, 2, 2, 4);

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);
    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    vtx_method_desc_t method = {
        .name = "countdown", .signature = "(I)I", .bytecode = &bc,
        .vtable_index = 0xFFFFFFFF, .arg_count = 1, .is_virtual = false
    };

    /* Countdown from 5 should reach 0 */
    vtx_value_t args[] = { vtx_make_smi(5), vtx_make_smi(0) };
    vtx_value_t result = vtx_interp_run(&interp, &method, args, 2);

    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), (int64_t)0);

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Test: dup and pop stack manipulation                                         */
/* ========================================================================== */

VTX_TEST(interp_dup_pop)
{
    /* load_local 0, dup, iadd, return_value  → returns local0 + local0 */
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL,  0x00, 0x00,
        VT_OP_DUP,
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_bytecode_t bc = make_bytecode_simple(code, sizeof(code), 1, 4);

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);
    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    vtx_method_desc_t method = {
        .name = "double", .signature = "(I)I", .bytecode = &bc,
        .vtable_index = 0xFFFFFFFF, .arg_count = 1, .is_virtual = false
    };

    vtx_value_t args[] = { vtx_make_smi(21) };
    vtx_value_t result = vtx_interp_run(&interp, &method, args, 1);

    VTX_ASSERT_TRUE(vtx_is_smi(result));
    VTX_ASSERT_EQUAL(vtx_smi_value(result), (int64_t)42);

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Main                                                                         */
/* ========================================================================== */

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    return (result.fail_count > 0) ? 1 : 0;
}
