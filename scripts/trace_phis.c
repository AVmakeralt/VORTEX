#include <stdio.h>
#include <string.h>
#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "ir/schedule.h"
#include "lower/isel.h"
#include "lower/regalloc.h"
#include "assembler.h"

int main(void) {
    vtx_assembler_t a; vtx_asm_init(&a);
    const char *prog =
        ".method popcount (I)I\n.arg_count 1\n.max_locals 3\n.max_stack 4\n"
        "load_const_int 0\nstore_local 1\n"
        "loop:\nload_local 0\nif_false done\n"
        "load_local 0\nload_const_int 2\nimod\n"
        "load_const_int 1\nicmp_eq\nif_false skip\n"
        "load_local 1\nload_const_int 1\niadd\nstore_local 1\n"
        "skip:\nload_local 0\nload_const_int 2\nidiv\nstore_local 0\n"
        "goto loop\ndone:\nload_local 1\nreturn_value\n";
    vtx_asm_program(&a, prog);
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    vtx_graph_t graph; vtx_graph_init(&graph, 1);
    vtx_bytecode_t bc = vtx_asm_emit(&a);
    vtx_method_desc_t method = {.name="f",.signature="(I)I",.bytecode=&bc,
        .compiled_code=NULL,.vtable_index=0,.arg_count=1,.is_virtual=false};
    vtx_graph_build(&graph, &bc, &method, &arena);

    /* Print IR with full details for Phis */
    printf("=== Phi nodes ===\n");
    for (uint32_t i = 0; i < graph.node_table.count; i++) {
        vtx_node_t *n = &graph.node_table.nodes[i];
        if (n->dead || n->opcode != VTX_OP_Phi) continue;
        printf("  N%u: Phi -> [", i);
        for (uint32_t j = 0; j < n->input_count; j++) {
            vtx_nodeid_t inp = n->inputs[j];
            const vtx_node_t *in = vtx_node_get_const(&graph.node_table, inp);
            printf("N%u(%s)", inp, in ? vtx_node_opcode_name(in->opcode) : "?");
            if (j+1 < n->input_count) printf(",");
        }
        printf("]\n");
    }

    /* Print schedule */
    vtx_schedule_t schedule;
    memset(&schedule, 0, sizeof(schedule));
    vtx_schedule_run(&graph, &arena, &schedule);
    printf("\n=== Schedule ===\n");
    for (uint32_t b = 0; b < schedule.count; b++) {
        printf("Block %u (preds=%u):", b, schedule.blocks[b].pred_count);
        for (uint32_t p = 0; p < schedule.blocks[b].pred_count; p++)
            printf(" pred%u=Block%u", p, schedule.blocks[b].pred_blocks[p]);
        printf(" nodes:");
        for (uint32_t n = 0; n < schedule.blocks[b].node_count; n++)
            printf(" N%u", schedule.blocks[b].nodes[n]);
        printf("\n");
    }

    /* Run isel and print the Phi copy instructions */
    vtx_inst_stream_t *stream = vtx_isel_select(&schedule, &graph, &arena);
    if (!stream) { printf("ISEL FAILED\n"); return 1; }

    printf("\n=== PHI_COPY instructions ===\n");
    for (uint32_t b = 0; b < stream->block_count; b++) {
        vtx_inst_block_t *blk = &stream->blocks[b];
        for (uint32_t i = 0; i < blk->inst_count; i++) {
            vtx_inst_t *inst = &blk->insts[i];
            if (inst->flags & 0x10000) { /* PHI_COPY */
                printf("  Block %u [%u]: MOV vreg%u <- vreg%u (node=N%u)\n",
                       b, i, inst->operands[0], inst->operands[1], inst->source_node);
            }
        }
    }

    /* Print vreg->node mapping for Phi-related vregs */
    printf("\n=== Vreg mapping (Phis and their inputs) ===\n");
    for (uint32_t i = 0; i < graph.node_table.count; i++) {
        vtx_node_t *n = &graph.node_table.nodes[i];
        if (n->dead) continue;
        if (n->opcode == VTX_OP_Phi || n->opcode == VTX_OP_Add ||
            n->opcode == VTX_OP_Div || n->opcode == VTX_OP_Mod) {
            uint32_t vreg = vtx_isel_node_vreg(stream, i);
            printf("  N%u (%s) -> vreg %u\n", i,
                   vtx_node_opcode_name(n->opcode), vreg);
        }
    }

    /* Run regalloc and print mapping for key vregs */
    vtx_regalloc_result_t *ra = vtx_regalloc_run(stream, &arena);
    if (ra) {
        printf("\n=== Regalloc for key vregs ===\n");
        static const char *reg_names[] = {
            "RAX","RCX","RDX","RBX","RSP","RBP","RSI","RDI",
            "R8","R9","R10","R11","R12","R13","R14","R15"
        };
        int key_vregs[] = {12, 13, 14, 25, 33, -1};
        for (int k = 0; key_vregs[k] >= 0; k++) {
            uint32_t v = key_vregs[k];
            if (v < ra->vreg_to_phys_count) {
                uint8_t phys = ra->vreg_to_phys[v];
                uint32_t spill = ra->vreg_to_spill[v];
                printf("  vreg %u -> %s", v,
                       phys < 16 ? reg_names[phys] : "NONE");
                if (spill != 0xFFFFFFFF)
                    printf(" spill[%u]", spill);
                printf("\n");
            }
        }
    }

    vtx_arena_destroy(&arena);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_asm_destroy(&a);
    return 0;
}

// Additional: print regalloc result
