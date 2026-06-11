#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "runtime/arena.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/helpers.h"
#include "interp/frame.h"
#include "interp/profiler.h"
#include "interp/lookup.h"
#include "interp/type_feedback.h"
#include "interp/dispatch.h"

int main(void) {
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);
    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts);

    /* Build constant pool */
    vtx_value_t cp[3];
    cp[0] = vtx_make_smi(0);
    cp[1] = vtx_make_smi(1);
    cp[2] = vtx_make_smi(2);

    /* Build fib(10) bytecode */
    uint8_t code[128];
    size_t pos = 0;

    #define EMIT(op) do { code[pos++] = (op); } while(0)
    #define EMIT16(v) do { code[pos++] = (uint8_t)((v) >> 8); code[pos++] = (uint8_t)((v) & 0xFF); } while(0)

    /* a=0, b=1 */
    EMIT(VT_OP_LOAD_CONST_INT); EMIT16(0);
    EMIT(VT_OP_STORE_LOCAL);    EMIT16(1);
    EMIT(VT_OP_LOAD_CONST_INT); EMIT16(1);
    EMIT(VT_OP_STORE_LOCAL);    EMIT16(2);

    /* loop: n > 0? */
    size_t loop_start = pos;
    EMIT(VT_OP_LOAD_LOCAL);     EMIT16(0);
    EMIT(VT_OP_LOAD_CONST_INT); EMIT16(0);
    EMIT(VT_OP_ICMP_GT);
    EMIT(VT_OP_IF_FALSE);
    size_t exit_patch = pos;
    EMIT16(0);

    /* temp = a + b */
    EMIT(VT_OP_LOAD_LOCAL);     EMIT16(1);
    EMIT(VT_OP_LOAD_LOCAL);     EMIT16(2);
    EMIT(VT_OP_IADD);
    EMIT(VT_OP_STORE_LOCAL);    EMIT16(3);

    /* a = b */
    EMIT(VT_OP_LOAD_LOCAL);     EMIT16(2);
    EMIT(VT_OP_STORE_LOCAL);    EMIT16(1);

    /* b = temp */
    EMIT(VT_OP_LOAD_LOCAL);     EMIT16(3);
    EMIT(VT_OP_STORE_LOCAL);    EMIT16(2);

    /* n = n - 1 */
    EMIT(VT_OP_LOAD_LOCAL);     EMIT16(0);
    EMIT(VT_OP_LOAD_CONST_INT); EMIT16(1);
    EMIT(VT_OP_ISUB);
    EMIT(VT_OP_STORE_LOCAL);    EMIT16(0);

    /* goto loop */
    EMIT(VT_OP_GOTO);
    int32_t back = (int32_t)loop_start - (int32_t)(pos + 3);
    EMIT16((uint16_t)back);

    /* exit: return a */
    size_t exit_pos = pos;
    code[exit_patch] = (uint8_t)((exit_pos - (exit_patch + 2)) >> 8);
    code[exit_patch + 1] = (uint8_t)((exit_pos - (exit_patch + 2)) & 0xFF);

    EMIT(VT_OP_LOAD_LOCAL);     EMIT16(1);
    EMIT(VT_OP_RETURN_VALUE);

    #undef EMIT
    #undef EMIT16

    printf("Bytecode: %zu bytes\n", pos);
    for (size_t i = 0; i < pos; i++) {
        printf("  [%3zu] 0x%02x", i, code[i]);
        if (code[i] < sizeof(vtx_opcode_table) / sizeof(vtx_opcode_table[0])) {
            const vtx_opcode_info_t *info = &vtx_opcode_table[code[i]];
            if (info->name) printf("  (%s)", info->name);
        }
        printf("\n");
    }

    vtx_bytecode_t bc = {
        .code = code,
        .length = (uint32_t)pos,
        .constant_pool = cp,
        .constant_count = 3,
        .max_locals = 4,
        .max_stack = 4
    };

    vtx_method_desc_t method = {
        .name = "fib", .signature = "(I)I",
        .bytecode = &bc, .vtable_index = 0, .is_virtual = false
    };

    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc); printf("Interp init OK\n"); fflush(stdout);

    vtx_value_t arg = vtx_make_smi(10);
    vtx_value_t result;
  printf("About to run interp with fib(10)...\n");
  result = vtx_interp_run(&interp, &method, &arg, 1);

    printf("\nResult type: ");
    if (vtx_is_smi(result)) printf("SMI = %lld\n", (long long)vtx_smi_value(result));
    else if (vtx_is_null(result)) printf("NULL\n");
    else if (vtx_is_double(result)) printf("DOUBLE = %f\n", vtx_double_value(result));
    else if (vtx_is_bool(result)) printf("BOOL = %d\n", vtx_bool_value(result));
    else if (vtx_is_undefined(result)) printf("UNDEFINED\n");
    else if (vtx_is_heap_ptr(result)) printf("HEAP_PTR = %p\n", vtx_heap_ptr(result));
    else printf("UNKNOWN (0x%016llx)\n", (unsigned long long)result);

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);
    return 0;
}
