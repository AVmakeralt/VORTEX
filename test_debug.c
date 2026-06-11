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

int main(void)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);
    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts);
    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    fprintf(stderr, "Test: constant return\n");
    {
        uint8_t code[] = { VT_OP_LOAD_CONST_INT, 0x00, 0x00, VT_OP_RETURN_VALUE };
        vtx_value_t cp[] = { vtx_make_smi(42) };
        fprintf(stderr, "  cp[0] raw = 0x%016" PRIx64 "\n", cp[0]);
        fprintf(stderr, "  vtx_is_smi(cp[0]) = %d\n", vtx_is_smi(cp[0]));
        fprintf(stderr, "  vtx_smi_value(cp[0]) = %" PRId64 "\n", vtx_smi_value(cp[0]));

        vtx_bytecode_t bc = { code, sizeof(code), cp, 1, 1, 4 };
        vtx_method_desc_t m = { "test1", "()I", &bc, 0xFFFFFFFF, false };

        vtx_value_t result = vtx_interp_run(&interp, &m, NULL, 0);
        fprintf(stderr, "  result raw = 0x%016" PRIx64 "\n", result);
        fprintf(stderr, "  vtx_is_nan_boxed = %d\n", vtx_is_nan_boxed(result));
        if (vtx_is_smi(result)) {
            fprintf(stderr, "  Is SMI, value = %" PRId64 "\n", vtx_smi_value(result));
        } else if (vtx_is_double(result)) {
            fprintf(stderr, "  Is double, value = %f\n", vtx_double_value(result));
        } else if (vtx_is_bool(result)) {
            fprintf(stderr, "  Is bool, value = %d\n", (int)vtx_bool_value(result));
        } else if (vtx_is_null(result)) {
            fprintf(stderr, "  Is null\n");
        } else if (vtx_is_undefined(result)) {
            fprintf(stderr, "  Is undefined\n");
        } else if (vtx_is_heap_ptr(result)) {
            fprintf(stderr, "  Is heap ptr, ptr = %p\n", vtx_heap_ptr(result));
        } else {
            fprintf(stderr, "  Unknown type, tag = %llu\n", (unsigned long long)(result & VTX_TAG_MASK));
        }
    }

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    return 0;
}
