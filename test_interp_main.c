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

int main(void)
{
    vtx_type_system_t ts;
    if (vtx_type_system_init(&ts) != 0) { fprintf(stderr, "Failed to init type system\n"); return 1; }

    vtx_gc_t gc;
    if (vtx_gc_init(&gc, &ts) != 0) { fprintf(stderr, "Failed to init GC\n"); return 1; }

    vtx_interp_t interp;
    if (vtx_interp_init(&interp, &ts, &gc) != 0) { fprintf(stderr, "Failed to init interpreter\n"); return 1; }

    printf("Interpreter initialized successfully!\n");

    /* Test 1: Simple constant return */
    printf("Test 1: constant return... ");
    {
        uint8_t code[] = { VT_OP_LOAD_CONST_INT, 0x00, 0x00, VT_OP_RETURN_VALUE };
        vtx_value_t cp[] = { vtx_make_smi(42) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 1, 1, 4 };
        vtx_method_desc_t m = { "test1", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t result = vtx_interp_run(&interp, &m, NULL, 0);
        if (vtx_is_smi(result) && vtx_smi_value(result) == 42) printf("PASS (42)\n");
        else printf("FAIL\n");
    }

    /* Test 2: Integer addition */
    printf("Test 2: addition... ");
    {
        uint8_t code[] = { VT_OP_LOAD_CONST_INT, 0x00, 0x00, VT_OP_LOAD_CONST_INT, 0x00, 0x01, VT_OP_IADD, VT_OP_RETURN_VALUE };
        vtx_value_t cp[] = { vtx_make_smi(10), vtx_make_smi(20) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 2, 2, 4 };
        vtx_method_desc_t m = { "test2", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t result = vtx_interp_run(&interp, &m, NULL, 0);
        if (vtx_is_smi(result) && vtx_smi_value(result) == 30) printf("PASS (30)\n");
        else if (vtx_is_smi(result)) printf("FAIL (got %lld)\n", (long long)vtx_smi_value(result));
        else printf("FAIL (not SMI)\n");
    }

    /* Test 3: Loop sum(1..5)=15 */
    printf("Test 3: loop... ");
    {
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00, VT_OP_STORE_LOCAL, 0x00, 0x00,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01, VT_OP_STORE_LOCAL, 0x00, 0x01,
            VT_OP_LOAD_LOCAL, 0x00, 0x00, VT_OP_LOAD_LOCAL, 0x00, 0x01,
            VT_OP_IADD, VT_OP_STORE_LOCAL, 0x00, 0x00,
            VT_OP_LOAD_LOCAL, 0x00, 0x01, VT_OP_LOAD_CONST_INT, 0x00, 0x02,
            VT_OP_ISUB, VT_OP_STORE_LOCAL, 0x00, 0x01,
            VT_OP_LOAD_LOCAL, 0x00, 0x01, VT_OP_LOAD_CONST_INT, 0x00, 0x03,
            VT_OP_ICMP_GT, VT_OP_IF_TRUE, 0x00, 0x0C,
            VT_OP_LOAD_LOCAL, 0x00, 0x00, VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_smi(0), vtx_make_smi(5), vtx_make_smi(1), vtx_make_smi(0) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 4, 4, 4 };
        vtx_method_desc_t m = { "test3", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t result = vtx_interp_run(&interp, &m, NULL, 0);
        if (vtx_is_smi(result) && vtx_smi_value(result) == 15) printf("PASS (15)\n");
        else if (vtx_is_smi(result)) printf("FAIL (got %lld)\n", (long long)vtx_smi_value(result));
        else printf("FAIL (not SMI)\n");
    }

    /* Test 4: Stack ops */
    printf("Test 4: stack ops... ");
    {
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00, VT_OP_DUP,
            VT_OP_LOAD_CONST_INT, 0x00, 0x01, VT_OP_SWAP, VT_OP_POP,
            VT_OP_RETURN_VALUE
        };
        vtx_value_t cp[] = { vtx_make_smi(7), vtx_make_smi(3) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 2, 2, 8 };
        vtx_method_desc_t m = { "test4", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t result = vtx_interp_run(&interp, &m, NULL, 0);
        if (vtx_is_smi(result) && vtx_smi_value(result) == 3) printf("PASS (3)\n");
        else if (vtx_is_smi(result)) printf("FAIL (got %lld)\n", (long long)vtx_smi_value(result));
        else printf("FAIL (not SMI)\n");
    }

    /* Test 5: ISNULL */
    printf("Test 5: null check... ");
    {
        uint8_t code[] = { VT_OP_LOAD_NULL, VT_OP_ISNULL, VT_OP_RETURN_VALUE };
        vtx_bytecode_t bc = { code, sizeof(code), NULL, 0, 1, 4 };
        vtx_method_desc_t m = { "test5", "()Z", &bc, 0xFFFFFFFF, false };
        vtx_value_t result = vtx_interp_run(&interp, &m, NULL, 0);
        if (vtx_is_bool(result) && vtx_bool_value(result)) printf("PASS (true)\n");
        else printf("FAIL\n");
    }

    /* Test 6: Conditional branch */
    printf("Test 6: conditional... ");
    {
        /* if 3 > 5 then return 1 else return 0 */
        uint8_t code[] = {
            VT_OP_LOAD_CONST_INT, 0x00, 0x00,   /* push 3 */
            VT_OP_LOAD_CONST_INT, 0x00, 0x01,   /* push 5 */
            VT_OP_ICMP_GT,                        /* 3 > 5? → false */
            VT_OP_IF_TRUE, 0x00, 0x0B,          /* goto PC 11 */
            VT_OP_LOAD_CONST_INT, 0x00, 0x02,   /* push 0 */
            VT_OP_RETURN_VALUE,                   /* return 0 (PC 10) */
            VT_OP_LOAD_CONST_INT, 0x00, 0x03,   /* push 1 (PC 11) */
            VT_OP_RETURN_VALUE                    /* return 1 (PC 14) */
        };
        vtx_value_t cp[] = { vtx_make_smi(3), vtx_make_smi(5), vtx_make_smi(0), vtx_make_smi(1) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 4, 2, 8 };
        vtx_method_desc_t m = { "test6", "()I", &bc, 0xFFFFFFFF, false };
        vtx_value_t result = vtx_interp_run(&interp, &m, NULL, 0);
        if (vtx_is_smi(result) && vtx_smi_value(result) == 0) printf("PASS (0)\n");
        else if (vtx_is_smi(result)) printf("FAIL (got %lld)\n", (long long)vtx_smi_value(result));
        else printf("FAIL\n");
    }

    /* Test 7: Profiler */
    printf("Test 7: profiler... ");
    {
        uint64_t heat = vtx_profiler_method_heat(&interp.profiler, NULL);
        printf("PASS (heat for NULL method: %lu)\n", (unsigned long)heat);
    }

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);

    printf("\nAll tests completed.\n");
    return 0;
}
