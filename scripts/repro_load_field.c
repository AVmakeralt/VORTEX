/* Minimal reproducer for T1 LOAD_FIELD crash */
#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "baseline/codegen.h"
#include "codecache/install.h"
#include "interp/dispatch.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    /* Register Point type with x, y fields */
    vtx_field_desc_t fields[2];
    fields[0].name = "x"; fields[0].offset = 0; fields[0].type = 0;
    fields[1].name = "y"; fields[1].offset = 1; fields[1].type = 0;
    vtx_typeid_t point_type = vtx_type_register(&ts, "Point", VTX_TYPE_OBJECT,
                                                  2, fields, 0, NULL);

    /* distance(p) = p.x + p.y */
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL,   0x00, 0x00,  /* load_local 0 (p) */
        VT_OP_LOAD_FIELD,   0x00, 0x00,  /* p.x */
        VT_OP_LOAD_LOCAL,   0x00, 0x00,  /* load_local 0 (p) */
        VT_OP_LOAD_FIELD,   0x00, 0x01,  /* p.y */
        VT_OP_IADD,
        VT_OP_RETURN_VALUE,
    };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 1, .max_stack = 4,
    };
    vtx_method_desc_t method = {
        .name = "distance", .signature = "(LPoint;)I",
        .bytecode = &bc, .compiled_code = NULL,
        .vtable_index = 102, .arg_count = 1, .is_virtual = false,
    };

    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 20);
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);
    vtx_compiled_code_t *compiled = vtx_baseline_compile(
        &method, NULL, &arena, &cache, &registry);
    if (!compiled) { fprintf(stderr, "compile failed\n"); return 1; }
    fprintf(stderr, "compiled: entry=%p code=%p code_size=%u\n",
            (void*)compiled->entry_point, (void*)compiled->code,
            compiled->code_size);
    /* Dump first 64 bytes of generated code from the buffer (ctx.buf.bytes).
     * After install, compiled->code is NULL but the code is in the cache
     * at method->compiled_code (== compiled->entry_point). */
    /* Dump first 200 bytes of generated code to a file for analysis. */
    FILE *df = fopen("/tmp/jit_code.bin", "wb");
    if (df) {
        fwrite(compiled->entry_point, 1, compiled->code_size, df);
        fclose(df);
        fprintf(stderr, "code dumped to /tmp/jit_code.bin (%u bytes)\n",
                compiled->code_size);
    }
    vtx_compiled_code_destroy(compiled);

    /* Create a Point object with x=30, y=12 */
    size_t alloc_size = vtx_heap_object_alloc_size(2);
    vtx_heap_object_t *pt = vtx_gc_alloc(&gc, alloc_size, point_type);
    pt->field_count = 2;
    pt->fields[0] = vtx_make_smi(30);
    pt->fields[1] = vtx_make_smi(12);
    vtx_value_t pt_val = vtx_make_heap_ptr(pt);

    fprintf(stderr, "pt=%p field_count=%u x=%lld y=%lld\n",
            (void*)pt, pt->field_count,
            (long long)vtx_smi_value(pt->fields[0]),
            (long long)vtx_smi_value(pt->fields[1]));
    fprintf(stderr, "pt_val raw=0x%llx\n", (unsigned long long)pt_val);
    fprintf(stderr, "untag: (val>>3)&mask << 3 = 0x%llx\n",
            (unsigned long long)((((pt_val >> 3) & VTX_NAN_DATA_MASK) << 3));

    /* Call with JIT */
    vtx_interp_t interp; vtx_interp_init(&interp, &ts, &gc);
    fprintf(stderr, "calling vtx_interp_run...\n");
    vtx_value_t r = vtx_interp_run(&interp, &method, &pt_val, 1);
    fprintf(stderr, "result: ");
    if (vtx_is_smi(r))
        fprintf(stderr, "SMI(%lld)\n", (long long)vtx_smi_value(r));
    else
        fprintf(stderr, "non-SMI (raw=%llx)\n", (unsigned long long)r);
    vtx_interp_destroy(&interp);

    vtx_method_registry_destroy(&registry);
    vtx_code_cache_destroy(&cache);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);
    return 0;
}
