/* Quick diagnostic for test_osr_25 crash */
#include "osr_test_setup.h"
#include "baseline/codegen.h"
#include "codecache/install.h"

int main(void)
{
    fprintf(stderr, "step1: init arena\n");
    vtx_arena_t arena; vtx_arena_init(&arena);
    fprintf(stderr, "step2: init ts\n");
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    fprintf(stderr, "step3: init gc\n");
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    fprintf(stderr, "step4: alloc code buf\n");
    uint8_t *code_buf = vtx_arena_alloc(&arena, 4);
    code_buf[0] = VT_OP_RETURN_VALUE;

    vtx_bytecode_t bc = {
        .code = code_buf, .length = 1,
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 1,
    };
    vtx_method_desc_t method = {
        .name = "osr25b", .signature = "()V",
        .bytecode = &bc, .compiled_code = NULL,
        .vtable_index = 2501, .arg_count = 0, .is_virtual = false,
    };

    fprintf(stderr, "step5: init cache\n");
    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 16);
    fprintf(stderr, "step6: init registry\n");
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);

    fprintf(stderr, "step7: calling vtx_baseline_compile...\n");
    vtx_compiled_code_t *cc = vtx_baseline_compile(
        &method, NULL, &arena, &cache, &registry);
    fprintf(stderr, "step8: compile returned %p\n", (void*)cc);

    if (cc) {
        vtx_compiled_method_t *cm = vtx_method_registry_get(&registry, method.vtable_index);
        fprintf(stderr, "step9: cm=%p\n", (void*)cm);
        if (cm) {
            fprintf(stderr, "frame_layout: total=%u locals_base=%d spill_base=%d\n",
                    cm->frame_layout.total_frame_size,
                    cm->frame_layout.locals_base,
                    cm->frame_layout.spill_base);
        }
        vtx_compiled_code_destroy(cc);
    }

    vtx_method_registry_destroy(&registry);
    vtx_code_cache_destroy(&cache);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);
    fprintf(stderr, "DONE\n");
    return 0;
}
