#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/helpers.h"
#include "runtime/bytecode.h"
#include "interp/dispatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass_count++; } \
    else { printf("  FAIL: %s\n", msg); fail_count++; } \
} while(0)

int main(void)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);
    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    printf("=== VORTEX Interpreter T0 Test Suite ===\n\n");

    /* Test 1: Simple constant return */
    printf("Test 1: constant return\n");
    {
        uint8_t code[] = { VT_OP_LOAD_CONST_INT, 0x00, 0x00, VT_OP_RETURN_VALUE };
        vtx_value_t cp[] = { vtx_make_smi(42) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 1, 1, 4 };
        vtx_method_desc_t m = { "test1", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t result = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_smi(result) && vtx_smi_value(result) == 42, "returns 42");
    }

    /* Test 2: Integer addition */
    printf("Test 2: integer addition\n");
    {
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,
            VT_OP_IADD,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_smi(10), vtx_make_smi(20) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 2, 2, 4 };
        vtx_method_desc_t m = { "test2", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t result = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_smi(result) && vtx_smi_value(result) == 30, "10+20=30");
    }

    /* Test 3: Integer subtraction */
    printf("Test 3: integer subtraction\n");
    {
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,
            VT_OP_ISUB,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_smi(100), vtx_make_smi(37) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 2, 2, 4 };
        vtx_method_desc_t m = { "test3", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t result = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_smi(result) && vtx_smi_value(result) == 63, "100-37=63");
    }

    /* Test 4: Integer multiplication */
    printf("Test 4: integer multiplication\n");
    {
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,
            VT_OP_IMUL,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_smi(7), vtx_make_smi(6) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 2, 2, 4 };
        vtx_method_desc_t m = { "test4", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t result = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_smi(result) && vtx_smi_value(result) == 42, "7*6=42");
    }

    /* Test 5: Integer division and modulo */
    printf("Test 5: integer division/modulo\n");
    {
        uint8_t code_div[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,
            VT_OP_IDIV,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp_div[] = { vtx_make_smi(17), vtx_make_smi(5) };
        vtx_bytecode_t bc_div = { code_div, sizeof(code_div), cp_div, 2, 2, 4 };
        vtx_method_desc_t m_div = { "test5a", "()I", &bc_div, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m_div, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 3, "17/5=3");

        uint8_t code_mod[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,
            VT_OP_IMOD,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp_mod[] = { vtx_make_smi(17), vtx_make_smi(5) };
        vtx_bytecode_t bc_mod = { code_mod, sizeof(code_mod), cp_mod, 2, 2, 4 };
        vtx_method_desc_t m_mod = { "test5b", "()I", &bc_mod, 0xFFFFFFFF, false };
        r = vtx_interp_run(&interp, &m_mod, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 2, "17%%5=2");
    }

    /* Test 6: Bitwise operations */
    printf("Test 6: bitwise operations\n");
    {
        /* 0xFF & 0x0F = 0x0F = 15 */
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,
            VT_OP_IAND,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_smi(0xFF), vtx_make_smi(0x0F) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 2, 2, 4 };
        vtx_method_desc_t m = { "test6a", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 15, "0xFF & 0x0F = 15");

        /* 0xF0 | 0x0F = 0xFF = 255 */
        uint8_t code2[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,
            VT_OP_IOR,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp2[] = { vtx_make_smi(0xF0), vtx_make_smi(0x0F) };
        vtx_bytecode_t bc2 = { code2, sizeof(code2), cp2, 2, 2, 4 };
        vtx_method_desc_t m2 = { "test6b", "()I", &bc2, 0xFFFFFFFF, false };
        r = vtx_interp_run(&interp, &m2, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 255, "0xF0 | 0x0F = 255");

        /* 0xFF ^ 0x0F = 0xF0 = 240 */
        uint8_t code3[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,
            VT_OP_IXOR,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp3[] = { vtx_make_smi(0xFF), vtx_make_smi(0x0F) };
        vtx_bytecode_t bc3 = { code3, sizeof(code3), cp3, 2, 2, 4 };
        vtx_method_desc_t m3 = { "test6c", "()I", &bc3, 0xFFFFFFFF, false };
        r = vtx_interp_run(&interp, &m3, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 240, "0xFF ^ 0x0F = 240");

        /* ~0 = -1 */
        uint8_t code4[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_INOT,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp4[] = { vtx_make_smi(0) };
        vtx_bytecode_t bc4 = { code4, sizeof(code4), cp4, 1, 1, 4 };
        vtx_method_desc_t m4 = { "test6d", "()I", &bc4, 0xFFFFFFFF, false };
        r = vtx_interp_run(&interp, &m4, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == -1, "~0 = -1");

        /* -5 = neg(5) */
        uint8_t code5[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_INEG,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp5[] = { vtx_make_smi(5) };
        vtx_bytecode_t bc5 = { code5, sizeof(code5), cp5, 1, 1, 4 };
        vtx_method_desc_t m5 = { "test6e", "()I", &bc5, 0xFFFFFFFF, false };
        r = vtx_interp_run(&interp, &m5, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == -5, "neg(5) = -5");
    }

    /* Test 7: Integer comparisons */
    printf("Test 7: integer comparisons\n");
    {
        /* 3 < 5 = true */
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,
            VT_OP_ICMP_LT,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_smi(3), vtx_make_smi(5) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 2, 2, 4 };
        vtx_method_desc_t m = { "test7", "()Z", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_bool(r) && vtx_bool_value(r) == true, "3 < 5 = true");
    }

    /* Test 8: Float arithmetic */
    printf("Test 8: float arithmetic\n");
    {
        uint8_t code[] = {
            VT_OP_LOAD_CONST_FLOAT, 0x00, 0x00,
            VT_OP_LOAD_CONST_FLOAT, 0x00, 0x01,
            VT_OP_FADD,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_double(1.5), vtx_make_double(2.5) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 2, 2, 4 };
        vtx_method_desc_t m = { "test8", "()D", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_double(r) && vtx_double_value(r) == 4.0, "1.5+2.5=4.0");
    }

    /* Test 9: Conditional branch */
    printf("Test 9: conditional branch\n");
    {
        /* if 10 > 5 then return 1 else return 0 */
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,   /* 0: push 10 */
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,   /* 3: push 5 */
            VT_OP_ICMP_GT,                        /* 6: 10 > 5 = true */
            VT_OP_IF_TRUE, 0x00, 0x0B,          /* 7: goto PC 11 */
            VT_OP_LOAD_CONST_INT, 0x00, 0x02,   /* 10: push 0 */
            VT_OP_RETURN_VALUE,                   /* 13: return 0 */
            VT_OP_LOAD_CONST_INT, 0x00, 0x03,   /* 14: push 1 (PC=11? No...) */
            VT_OP_RETURN_VALUE
        };
        /* Let me recalculate PC positions:
         * PC 0: LOAD_CONST_INT (3 bytes) → PC 3
         * PC 3: LOAD_CONST_INT (3 bytes) → PC 6
         * PC 6: ICMP_GT (1 byte) → PC 7
         * PC 7: IF_TRUE (3 bytes, target=?) → PC 10
         * PC 10: LOAD_CONST_INT (3 bytes) → PC 13
         * PC 13: RETURN_VALUE (1 byte) → done
         * PC 14: LOAD_CONST_INT (3 bytes) → PC 17
         * PC 17: RETURN_VALUE (1 byte)
         * So IF_TRUE should target PC 14 for the "then" branch.
         */
        (void)code; /* suppress unused */
        uint8_t code2[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,   /* 0: push 10 */
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,   /* 3: push 5 */
            VT_OP_ICMP_GT,                        /* 6: 10 > 5 = true */
            VT_OP_IF_TRUE, 0x00, 0x0E,          /* 7: goto PC 14 */
            VT_OP_LOAD_CONST_INT, 0x00, 0x02,   /* 10: push 0 */
            VT_OP_RETURN_VALUE,                   /* 13: return 0 */
            VT_OP_LOAD_CONST_INT, 0x00, 0x03,   /* 14: push 1 */
            VT_OP_RETURN_VALUE                    /* 17: return 1 */
        };
        vtx_value_t cp[] = { vtx_make_smi(10), vtx_make_smi(5), vtx_make_smi(0), vtx_make_smi(1) };
        vtx_bytecode_t bc = { code2, sizeof(code2), cp, 4, 2, 8 };
        vtx_method_desc_t m = { "test9", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 1, "10>5 → return 1");

        /* Now test the else branch: 3 > 5 = false */
        uint8_t code3[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,
            VT_OP_ICMP_GT,
            VT_OP_IF_TRUE, 0x00, 0x0E,
            VT_OP_LOAD_CONST_INT, 0x00, 0x02,
            VT_OP_RETURN_VALUE,
            VT_OP_LOAD_CONST_INT, 0x00, 0x03,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp3[] = { vtx_make_smi(3), vtx_make_smi(5), vtx_make_smi(0), vtx_make_smi(1) };
        vtx_bytecode_t bc3 = { code3, sizeof(code3), cp3, 4, 2, 8 };
        vtx_method_desc_t m3 = { "test9b", "()I", &bc3, 0xFFFFFFFF, false };
        r = vtx_interp_run(&interp, &m3, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 0, "3>5 → return 0");
    }

    /* Test 10: Loop (sum 5+4+3+2+1=15) */
    printf("Test 10: loop\n");
    {
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00, VT_OP_STORE_LOCAL, 0x00, 0x00, /* sum=0 */
            VT_OP_LOAD_CONST_INT, 0x00, 0x01, VT_OP_STORE_LOCAL, 0x00, 0x01, /* i=5 */
            VT_OP_LOAD_LOCAL, 0x00, 0x00,   /* push sum */
            VT_OP_LOAD_LOCAL, 0x00, 0x01,   /* push i */
            VT_OP_IADD,                       /* sum+i */
            VT_OP_STORE_LOCAL, 0x00, 0x00,   /* sum=sum+i */
            VT_OP_LOAD_LOCAL, 0x00, 0x01,   /* push i */
            VT_OP_LOAD_CONST_INT, 0x00, 0x02, /* push 1 */
            VT_OP_ISUB,                       /* i-1 */
            VT_OP_STORE_LOCAL, 0x00, 0x01,   /* i=i-1 */
            VT_OP_LOAD_LOCAL, 0x00, 0x01,   /* push i */
            VT_OP_LOAD_CONST_INT, 0x00, 0x03, /* push 0 */
            VT_OP_ICMP_GT,                    /* i>0? */
            VT_OP_IF_TRUE, 0x00, 0x0C,      /* goto loop header (PC 12) */
            VT_OP_LOAD_LOCAL, 0x00, 0x00,   /* push sum */
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_smi(0), vtx_make_smi(5), vtx_make_smi(1), vtx_make_smi(0) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 4, 4, 4 };
        vtx_method_desc_t m = { "test10", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 15, "sum(1..5)=15");
    }

    /* Test 11: GOTO */
    printf("Test 11: goto\n");
    {
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,   /* 0: push 99 */
            VT_OP_GOTO, 0x00, 0x07,             /* 3: goto PC 7 */
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,   /* 6: push 0 (skipped) */
            VT_OP_RETURN_VALUE                    /* 9: return 99 */
        };
        vtx_value_t cp[] = { vtx_make_smi(99), vtx_make_smi(0) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 2, 1, 4 };
        vtx_method_desc_t m = { "test11", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 99, "goto skips dead code, returns 99");
    }

    /* Test 12: ISNULL, LOAD_NULL, LOAD_TRUE, LOAD_FALSE, LOAD_UNDEFINED */
    printf("Test 12: special values\n");
    {
        uint8_t code[] = { VT_OP_LOAD_NULL, VT_OP_ISNULL, VT_OP_RETURN_VALUE };
        vtx_bytecode_t bc = { code, sizeof(code), NULL, 0, 1, 4 };
        vtx_method_desc_t m = { "test12a", "()Z", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_bool(r) && vtx_bool_value(r) == true, "null is null");

        uint8_t code2[] = { VT_OP_LOAD_TRUE, VT_OP_RETURN_VALUE };
        vtx_bytecode_t bc2 = { code2, sizeof(code2), NULL, 0, 1, 4 };
        vtx_method_desc_t m2 = { "test12b", "()Z", &bc2, 0xFFFFFFFF, false };
        r = vtx_interp_run(&interp, &m2, NULL, 0);
        CHECK(vtx_is_bool(r) && vtx_bool_value(r) == true, "load_true returns true");

        uint8_t code3[] = { VT_OP_LOAD_FALSE, VT_OP_RETURN_VALUE };
        vtx_bytecode_t bc3 = { code3, sizeof(code3), NULL, 0, 1, 4 };
        vtx_method_desc_t m3 = { "test12c", "()Z", &bc3, 0xFFFFFFFF, false };
        r = vtx_interp_run(&interp, &m3, NULL, 0);
        CHECK(vtx_is_bool(r) && vtx_bool_value(r) == false, "load_false returns false");

        uint8_t code4[] = { VT_OP_LOAD_UNDEFINED, VT_OP_RETURN_VALUE };
        vtx_bytecode_t bc4 = { code4, sizeof(code4), NULL, 0, 1, 4 };
        vtx_method_desc_t m4 = { "test12d", "()V", &bc4, 0xFFFFFFFF, false };
        r = vtx_interp_run(&interp, &m4, NULL, 0);
        CHECK(vtx_is_undefined(r), "load_undefined returns undefined");
    }

    /* Test 13: Stack manipulation (DUP, POP, SWAP) */
    printf("Test 13: stack manipulation\n");
    {
        /* Push 7, dup, add → 14 */
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_DUP,
            VT_OP_IADD,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_smi(7) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 1, 1, 8 };
        vtx_method_desc_t m = { "test13a", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 14, "dup then add: 7+7=14");

        /* Push 3, push 5, swap, return top → 3 */
        uint8_t code2[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,
            VT_OP_SWAP,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp2[] = { vtx_make_smi(3), vtx_make_smi(5) };
        vtx_bytecode_t bc2 = { code2, sizeof(code2), cp2, 2, 2, 8 };
        vtx_method_desc_t m2 = { "test13b", "()I", &bc2, 0xFFFFFFFF, false };
        r = vtx_interp_run(&interp, &m2, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 3, "swap: push 3,5 swap → top is 3");

        /* Push 42, pop, push 7, return → 7 */
        uint8_t code3[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_POP,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp3[] = { vtx_make_smi(42), vtx_make_smi(7) };
        vtx_bytecode_t bc3 = { code3, sizeof(code3), cp3, 2, 1, 8 };
        vtx_method_desc_t m3 = { "test13c", "()I", &bc3, 0xFFFFFFFF, false };
        r = vtx_interp_run(&interp, &m3, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 7, "pop then push 7");
    }

    /* Test 14: NOP and HALT */
    printf("Test 14: nop and halt\n");
    {
        uint8_t code[] = {
            VT_OP_NOP,
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_smi(1) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 1, 1, 4 };
        vtx_method_desc_t m = { "test14", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 1, "nop is no-op");
    }

    /* Test 15: TYPEOF */
    printf("Test 15: typeof\n");
    {
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_TYPEOF,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_smi(42) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 1, 1, 4 };
        vtx_method_desc_t m = { "test15", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        /* SMI should have typeid VTX_TYPE_INVALID (0) since it's not a heap object */
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == VTX_TYPE_INVALID, "typeof(smi) = INVALID");
    }

    /* Test 16: Frame operations (get/set local, stack depth) */
    printf("Test 16: frame operations\n");
    {
        /* Store 42 in local 0, load it, return */
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_STORE_LOCAL, 0x00, 0x00,
            VT_OP_LOAD_LOCAL, 0x00, 0x00,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_smi(42) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 1, 2, 4 };
        vtx_method_desc_t m = { "test16", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 42, "store/load local works");
    }

    /* Test 17: Object creation and field access */
    printf("Test 17: object + fields\n");
    {
        vtx_field_desc_t *fields = (vtx_field_desc_t *)malloc(2 * sizeof(vtx_field_desc_t));
        fields[0].name = "x"; fields[0].type = VTX_TYPE_OBJECT; fields[0].offset = 0;
        fields[1].name = "y"; fields[1].type = VTX_TYPE_OBJECT; fields[1].offset = 0;
        vtx_typeid_t pt = vtx_type_register(&ts, "Point2D", VTX_TYPE_OBJECT, 2, fields, 0, NULL);

        /* new Point2D; dup; push 10; store_field 0; dup; push 20; store_field 1; load_field 0; return */
        uint8_t code[] = {
            VT_OP_NEW, (uint8_t)(pt >> 8), (uint8_t)(pt & 0xFF),
            VT_OP_DUP,
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_STORE_FIELD, 0x00, 0x00,
            VT_OP_DUP,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,
            VT_OP_STORE_FIELD, 0x00, 0x01,
            VT_OP_LOAD_FIELD, 0x00, 0x00,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_smi(10), vtx_make_smi(20) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 2, 2, 8 };
        vtx_method_desc_t m = { "test17", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 10, "object field 0 = 10");
    }

    /* Test 18: Array operations */
    printf("Test 18: array operations\n");
    {
        /* newarray 3; dup; push 99; store_field 0 (index); load_field 0; return */
        /* Actually, array ops use ARRAY_LOAD/ARRAY_STORE. Let's test those. */
        /* newarray(3), dup, push 0 (index), push 42 (value), array_store,
         * dup, push 0 (index), array_load, return */
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,   /* push 3 (size) */
            VT_OP_NEWARRAY, 0x00, 0x01,          /* newarray, typeid=1 (Object) */
            VT_OP_DUP,                             /* dup array ref */
            VT_OP_LOAD_CONST_INT, 0x00, 0x02,    /* push index 0 */
            VT_OP_LOAD_CONST_INT, 0x00, 0x03,    /* push value 42 */
            VT_OP_ARRAY_STORE,                     /* arr[0] = 42 */
            VT_OP_DUP,                             /* dup array ref */
            VT_OP_LOAD_CONST_INT, 0x00, 0x02,    /* push index 0 */
            VT_OP_ARRAY_LOAD,                      /* load arr[0] */
            VT_OP_RETURN_VALUE                     /* return arr[0] */
        };
        vtx_value_t cp[] = { vtx_make_smi(3), vtx_make_smi(1), vtx_make_smi(0), vtx_make_smi(42) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 4, 2, 16 };
        vtx_method_desc_t m = { "test18", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 42, "array[0] = 42");

        /* Array length */
        uint8_t code2[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_NEWARRAY, 0x00, 0x01,
            VT_OP_ARRAY_LENGTH,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp2[] = { vtx_make_smi(5), vtx_make_smi(1) };
        vtx_bytecode_t bc2 = { code2, sizeof(code2), cp2, 2, 1, 8 };
        vtx_method_desc_t m2 = { "test18b", "()I", &bc2, 0xFFFFFFFF, false };
        r = vtx_interp_run(&interp, &m2, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 5, "array length = 5");
    }

    /* Test 19: IF_FALSE */
    printf("Test 19: if_false\n");
    {
        /* Push false, if_false goto return 1, else return 0 */
        uint8_t code[] = {
            VT_OP_LOAD_FALSE,                      /* 0: push false */
            VT_OP_IF_FALSE, 0x00, 0x07,          /* 1: if false goto PC 7 */
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,    /* 4: push 0 */
            VT_OP_RETURN_VALUE,                    /* 7: return 0 ... wait PC math */
        };
        /* Let me recalculate:
         * PC 0: LOAD_FALSE (1 byte) → PC 1
         * PC 1: IF_FALSE (3 bytes, target=?) → PC 4
         * PC 4: LOAD_CONST_INT (3 bytes) → PC 7
         * PC 7: RETURN_VALUE (1 byte)
         * PC 8: LOAD_CONST_INT (3 bytes) → PC 11
         * PC 11: RETURN_VALUE
         * So IF_FALSE should target PC 8 for the "taken" branch.
         */
        uint8_t code2[] = {
            VT_OP_LOAD_FALSE,                      /* 0: push false */
            VT_OP_IF_FALSE, 0x00, 0x08,          /* 1: if false goto PC 8 */
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,    /* 4: push 0 (not taken) */
            VT_OP_RETURN_VALUE,                    /* 7: return 0 */
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,    /* 8: push 1 (taken) */
            VT_OP_RETURN_VALUE                     /* 11: return 1 */
        };
        vtx_value_t cp[] = { vtx_make_smi(0), vtx_make_smi(1) };
        vtx_bytecode_t bc = { code2, sizeof(code2), cp, 2, 1, 8 };
        vtx_method_desc_t m = { "test19", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 1, "if_false with false → taken");
    }

    /* Test 20: Profiler */
    printf("Test 20: profiler\n");
    {
        /* Run a simple method several times and check profiler state */
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00, VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_smi(1) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 1, 1, 4 };
        vtx_method_desc_t m = { "test20", "()I", &bc, 0xFFFFFFFF, false };
        for (int i = 0; i < 10; i++) {
            vtx_interp_run(&interp, &m, NULL, 0);
        }
        vtx_profile_data_t *pd = vtx_profiler_get_method_data(&interp.profiler, &m);
        CHECK(pd != NULL && pd->invocation_count >= 10, "invocation count >= 10");
    }

    /* Test 21: Type feedback */
    printf("Test 21: type feedback\n");
    {
        vtx_type_feedback_record_call(&interp.type_feedback, 0, 1, 1);
        vtx_type_feedback_record_call(&interp.type_feedback, 0, 1, 1);
        vtx_type_feedback_record_call(&interp.type_feedback, 0, 2, 2);
        vtx_typeid_t dom = vtx_type_feedback_get_dominant_call_type(&interp.type_feedback, 0);
        CHECK(dom == 1, "dominant call type = 1");

        vtx_type_feedback_record_branch(&interp.type_feedback, 0, true);
        vtx_type_feedback_record_branch(&interp.type_feedback, 0, true);
        vtx_type_feedback_record_branch(&interp.type_feedback, 0, false);
        double prob = vtx_type_feedback_get_branch_probability(&interp.type_feedback, 0);
        CHECK(prob > 0.5 && prob < 1.0, "branch probability > 0.5");
    }

    /* Test 22: Interpreter IC storage */
    printf("Test 22: IC storage\n");
    {
        uint8_t code[] = { VT_OP_LOAD_CONST_INT, 0x00, 0x00, VT_OP_RETURN_VALUE };
        vtx_value_t cp[] = { vtx_make_smi(1) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 1, 1, 4 };
        vtx_method_desc_t m = { "test22", "()I", &bc, 0xFFFFFFFF, false };
        vtx_inline_cache_t *ic = vtx_interp_get_ic(&interp, &m, 0);
        CHECK(ic != NULL, "IC allocated for method");
        /* Run the method to exercise IC creation */
        vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(true, "method with IC runs without crash");
    }

    /* Test 23: Left shift */
    printf("Test 23: shift operations\n");
    {
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,
            VT_OP_ISHL,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_smi(1), vtx_make_smi(4) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 2, 2, 4 };
        vtx_method_desc_t m = { "test23a", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 16, "1 << 4 = 16");

        uint8_t code2[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,
            VT_OP_ISHR,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp2[] = { vtx_make_smi(16), vtx_make_smi(2) };
        vtx_bytecode_t bc2 = { code2, sizeof(code2), cp2, 2, 2, 4 };
        vtx_method_desc_t m2 = { "test23b", "()I", &bc2, 0xFFFFFFFF, false };
        r = vtx_interp_run(&interp, &m2, NULL, 0);
        CHECK(vtx_is_smi(r) && vtx_smi_value(r) == 4, "16 >> 2 = 4");
    }

    /* Test 24: Return void */
    printf("Test 24: void return\n");
    {
        uint8_t code[] = { VT_OP_LOAD_CONST_INT, 0x00, 0x00, VT_OP_RETURN };
        vtx_value_t cp[] = { vtx_make_smi(42) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 1, 1, 4 };
        vtx_method_desc_t m = { "test24", "()V", &bc, 0xFFFFFFFF, false };
        vtx_value_t r = vtx_interp_run(&interp, &m, NULL, 0);
        CHECK(vtx_is_undefined(r), "void return gives undefined");
    }

    /* Test 25: Method lookup with IC */
    printf("Test 25: method lookup\n");
    {
        vtx_inline_cache_t ic;
        vtx_ic_init(&ic);
        /* Lookup on non-heap value should return NULL */
        const vtx_method_desc_t *result = vtx_lookup_method(&ts, &ic, vtx_make_smi(42), "toString");
        CHECK(result == NULL, "lookup on SMI returns NULL");
    }

    /* Cleanup */
    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);

    printf("\n=== Results: %d passed, %d failed ===\n", pass_count, fail_count);
    return fail_count > 0 ? 1 : 0;
}
