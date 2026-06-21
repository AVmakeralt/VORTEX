/*
 * Diagnostic: dump T2 IR for is_zero(0)
 * to understand why T2 returns 0 instead of SMI(1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "ir/verify.h"
#include "ir/schedule.h"
#include "assembler.h"

int main(void) {
    vtx_assembler_t a;
    vtx_asm_init(&a);

    const char *prog =
        ".method is_zero (I)I\n"
        ".arg_count 1\n"
        ".max_locals 1\n"
        ".max_stack 4\n"
        "load_local 0\n"
        "if_false non_zero\n"
        "load_const_int 0\n"
        "return_value\n"
        "non_zero:\n"
        "load_const_int 1\n"
        "return_value\n";
    int rc = vtx_asm_program(&a, prog);
    if (rc != 0) {
        printf("assemble error: %s\n", a.error_msg);
        return 1;
    }

    printf("=== Bytecode (%zu bytes) ===\n", a.code_len);
    for (size_t i = 0; i < a.code_len; i++) {
        printf(" %02x", a.code[i]);
    }
    printf("\n");

    /* Build IR and dump */
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_graph_t graph; vtx_graph_init(&graph, 1);
    vtx_bytecode_t bc = vtx_asm_emit(&a);
    vtx_method_desc_t method = {
        .name = "is_zero", .signature = "(I)I", .bytecode = &bc,
        .compiled_code = NULL, .vtable_index = 0, .arg_count = 1, .is_virtual = false,
    };
    int brc = vtx_graph_build(&graph, &bc, &method, &arena);
    printf("graph_build rc=%d\n", brc);
    if (brc == 0) {
        vtx_graph_print(&graph);
        printf("\nverify: %d\n", vtx_verify_graph(&graph));

        /* Schedule and dump */
        vtx_schedule_t schedule;
        memset(&schedule, 0, sizeof(schedule));
        int src = vtx_schedule_run(&graph, &arena, &schedule);
        printf("\nschedule_run rc=%d, blocks=%u\n", src, schedule.count);
        if (src == 0) {
            for (uint32_t b = 0; b < schedule.count; b++) {
                vtx_schedule_block_t *blk = &schedule.blocks[b];
                printf("Block %u: region=N%u, succ_count=%u, pred_count=%u, is_loop_header=%d\n",
                       b, blk->region_node, blk->succ_count, blk->pred_count, blk->is_loop_header);
                for (uint32_t s = 0; s < blk->succ_count; s++) {
                    printf("  succ[%u] = block %u\n", s, blk->succ_blocks[s]);
                }
            }
        }
    }
    vtx_arena_destroy(&arena);

    vtx_asm_destroy(&a);
    return 0;
}
