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

    fprintf(stderr, "Interpreter initialized successfully!\n");

    /* Test 1: Simple constant return */
    fprintf(stderr, "Test 1: constant return...\n");
    {
        uint8_t code[] = { VT_OP_LOAD_CONST_INT, 0x00, 0x00, VT_OP_RETURN_VALUE };
        vtx_value_t cp[] = { vtx_make_smi(42) };
        vtx_bytecode_t bc = { code, sizeof(code), cp, 1, 1, 4 };
        vtx_method_desc_t m = { "test1", "()I", &bc, 0xFFFFFFFF, false };
        fprintf(stderr, "  About to call vtx_interp_run...\n");
        vtx_value_t result = vtx_interp_run(&interp, &m, NULL, 0);
        fprintf(stderr, "  vtx_interp_run returned\n");
        if (vtx_is_smi(result)) {
            fprintf(stderr, "  Result is SMI: %lld\n", (long long)vtx_smi_value(result));
        } else {
            fprintf(stderr, "  Result is NOT SMI\n");
        }
    }

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);

    fprintf(stderr, "Done.\n");
    return 0;
}
