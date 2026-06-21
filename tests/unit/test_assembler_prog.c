/*
 * VORTEX Assembler Test — Program Mode (labels + pseudo-instructions)
 *
 * Audit priority #2 (Critical): "A real language frontend — at minimum, a
 * text bytecode assembler with label/jump-target support."
 *
 * Tests the new vtx_asm_program() entry point with:
 *   - Label definitions and forward references
 *   - Backward branches (loops)
 *   - Pseudo-instructions (.max_locals, .max_stack, .const, .method, .arg_count)
 *   - Comments and empty lines
 *   - Auto-computed max_stack via abstract interpretation
 *   - Round-trip: assemble → run through vtx_graph_build → verify
 *
 * Each test assembles a small VORTEX assembly program and checks the
 * resulting bytecode against expected values. The final test does
 * end-to-end: assemble → build IR → verify graph invariants.
 */

#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "ir/verify.h"
#include "assembler.h"

VTX_TEST(asm_prog_simple_return) {
    vtx_assembler_t a;
    vtx_asm_init(&a);

    const char *prog =
        "; simple program that returns void\n"
        "return\n";
    int rc = vtx_asm_program(&a, prog);
    VTX_ASSERT_TRUE(rc == 0);
    VTX_ASSERT_TRUE(a.code_len == 1);
    VTX_ASSERT_TRUE(a.code[0] == VT_OP_RETURN);
    VTX_ASSERT_TRUE(a.has_error == false);

    vtx_asm_destroy(&a);
}

VTX_TEST(asm_prog_label_forward_branch) {
    vtx_assembler_t a;
    vtx_asm_init(&a);

    /* if_true forward_branch:
     *   load_const_int 1
     *   if_true skip
     *   load_const_int 2
     * skip:
     *   return_value
     */
    const char *prog =
        "load_const_int 1\n"
        "if_true skip\n"
        "load_const_int 2\n"
        "skip:\n"
        "return_value\n";
    int rc = vtx_asm_program(&a, prog);
    VTX_ASSERT_TRUE(rc == 0);
    if (rc == 0) {
        /* Expected bytes:
         *   PC 0: 06 00 00       LOAD_CONST_INT const[0]=1   (3 bytes)
         *   PC 3: 2a 00 09       IF_TRUE target=9 (skip label) (3 bytes)
         *   PC 6: 06 00 01       LOAD_CONST_INT const[1]=2   (3 bytes)
         *   PC 9: 2f             RETURN_VALUE                (1 byte)
         * Total: 10 bytes, label 'skip' should be at PC 9. */
        VTX_ASSERT_TRUE(a.code_len == 10);
        VTX_ASSERT_TRUE(a.code[0] == VT_OP_LOAD_CONST_INT);
        VTX_ASSERT_TRUE(a.code[3] == VT_OP_IF_TRUE);
        /* Branch target should be 9 (skip label) */
        uint16_t target = ((uint16_t)a.code[4] << 8) | a.code[5];
        VTX_ASSERT_TRUE(target == 9);
        VTX_ASSERT_TRUE(a.code[6] == VT_OP_LOAD_CONST_INT);
        VTX_ASSERT_TRUE(a.code[9] == VT_OP_RETURN_VALUE);
        /* Verify label PC lookup */
        VTX_ASSERT_TRUE(vtx_asm_label_pc(&a, "skip") == 9);
    } else {
        printf("[asm] error: %s\n", a.error_msg);
    }

    vtx_asm_destroy(&a);
}

VTX_TEST(asm_prog_loop_backward_branch) {
    vtx_assembler_t a;
    vtx_asm_init(&a);

    /* Loop that counts down from 5 to 0:
     *   .max_locals 1
     *   load_const_int 5
     *   store_local 0          ; local[0] = 5
     * loop:
     *   load_local 0
     *   if_false done          ; if local[0] == 0, exit
     *   load_local 0
     *   load_const_int 1
     *   isub
     *   store_local 0          ; local[0] -= 1
     *   goto loop
     * done:
     *   load_local 0
     *   return_value
     */
    const char *prog =
        ".max_locals 1\n"
        "load_const_int 5\n"
        "store_local 0\n"
        "loop:\n"
        "load_local 0\n"
        "if_false done\n"
        "load_local 0\n"
        "load_const_int 1\n"
        "isub\n"
        "store_local 0\n"
        "goto loop\n"
        "done:\n"
        "load_local 0\n"
        "return_value\n";
    int rc = vtx_asm_program(&a, prog);
    VTX_ASSERT_TRUE(rc == 0);
    if (rc == 0) {
        /* The 'loop' label is at PC 7 (after: 3+3+3 bytes for the first 3
         * instructions: load_const_int(3) + store_local(3) = 6, plus the
         * loop: label itself adds nothing). Wait, let me recount:
         *   PC 0: 06 00 00  (load_const_int 5) — 3 bytes
         *   PC 3: 03 00 00  (store_local 0) — 3 bytes
         *   PC 6: 02 00 00  (load_local 0) — 3 bytes  [LOOP LABEL = PC 6]
         *   PC 9: 2b ?? ??  (if_false done) — 3 bytes
         *   PC 12: 02 00 00 (load_local 0) — 3 bytes
         *   PC 15: 06 00 01 (load_const_int 1) — 3 bytes
         *   PC 18: 0e       (isub) — 1 byte
         *   PC 19: 03 00 00 (store_local 0) — 3 bytes
         *   PC 22: 29 ?? ?? (goto loop) — 3 bytes
         *   PC 25: 02 00 00 (load_local 0) — 3 bytes  [DONE LABEL = PC 25]
         *   PC 28: 2f       (return_value) — 1 byte
         * Total: 29 bytes */
        VTX_ASSERT_TRUE(a.code_len == 29);
        VTX_ASSERT_TRUE(vtx_asm_label_pc(&a, "loop") == 6);
        VTX_ASSERT_TRUE(vtx_asm_label_pc(&a, "done") == 25);

        /* goto loop should target PC 6 */
        uint16_t goto_target = ((uint16_t)a.code[23] << 8) | a.code[24];
        VTX_ASSERT_TRUE(goto_target == 6);

        /* if_false done should target PC 25 */
        uint16_t if_target = ((uint16_t)a.code[10] << 8) | a.code[11];
        VTX_ASSERT_TRUE(if_target == 25);

        /* max_locals should be 1 (set explicitly) */
        VTX_ASSERT_TRUE(a.max_locals == 1);
    } else {
        printf("[asm] error: %s\n", a.error_msg);
    }

    vtx_asm_destroy(&a);
}

VTX_TEST(asm_prog_pseudo_instructions) {
    vtx_assembler_t a;
    vtx_asm_init(&a);

    const char *prog =
        ".method fib (I)I\n"
        ".arg_count 1\n"
        ".max_locals 4\n"
        ".max_stack 8\n"
        ".const int 0\n"
        ".const int 1\n"
        "load_const_int 0\n"
        "return_value\n";
    int rc = vtx_asm_program(&a, prog);
    VTX_ASSERT_TRUE(rc == 0);
    if (rc == 0) {
        VTX_ASSERT_TRUE(a.max_locals == 4);
        VTX_ASSERT_TRUE(a.max_stack == 8);
        VTX_ASSERT_TRUE(a.arg_count == 1);
        VTX_ASSERT_TRUE(strcmp(a.method_name, "fib") == 0);
        VTX_ASSERT_TRUE(strcmp(a.method_sig, "(I)I") == 0);
        VTX_ASSERT_TRUE(a.const_count == 2);
        VTX_ASSERT_TRUE(vtx_is_smi(a.constant_pool[0]));
        VTX_ASSERT_TRUE(vtx_smi_value(a.constant_pool[0]) == 0);
        VTX_ASSERT_TRUE(vtx_smi_value(a.constant_pool[1]) == 1);
    } else {
        printf("[asm] error: %s\n", a.error_msg);
    }

    vtx_asm_destroy(&a);
}

VTX_TEST(asm_prog_undefined_label) {
    vtx_assembler_t a;
    vtx_asm_init(&a);

    /* goto nowhere — 'nowhere' is never defined */
    const char *prog = "goto nowhere\nreturn\n";
    int rc = vtx_asm_program(&a, prog);
    VTX_ASSERT_TRUE(rc == -1);
    VTX_ASSERT_TRUE(a.has_error == true);

    vtx_asm_destroy(&a);
}

VTX_TEST(asm_prog_duplicate_label) {
    vtx_assembler_t a;
    vtx_asm_init(&a);

    const char *prog =
        "foo:\n"
        "return\n"
        "foo:\n"
        "return\n";
    int rc = vtx_asm_program(&a, prog);
    VTX_ASSERT_TRUE(rc == -1);
    VTX_ASSERT_TRUE(a.has_error == true);

    vtx_asm_destroy(&a);
}

VTX_TEST(asm_prog_auto_max_stack) {
    vtx_assembler_t a;
    vtx_asm_init(&a);

    /* Program that pushes 3 values without popping — auto max_stack
     * should detect 3. */
    const char *prog =
        "load_const_int 1\n"
        "load_const_int 2\n"
        "load_const_int 3\n"
        "pop\n"
        "pop\n"
        "pop\n"
        "return\n";
    int rc = vtx_asm_program(&a, prog);
    VTX_ASSERT_TRUE(rc == 0);
    if (rc == 0) {
        /* max_stack should be auto-computed as >= 3 */
        VTX_ASSERT_TRUE(a.max_stack >= 3);
        printf("[asm] auto max_stack = %u\n", a.max_stack);
    }

    vtx_asm_destroy(&a);
}

VTX_TEST(asm_prog_end_to_end_ir_build) {
    /* Assemble a real program with a loop, then build IR from it and
     * verify the IR passes all invariant checks. This is the
     * end-to-end test that the audit demands. */
    vtx_assembler_t a;
    vtx_asm_init(&a);

    const char *prog =
        ".max_locals 2\n"
        ".max_stack 4\n"
        "load_const_int 10\n"
        "store_local 0\n"           /* n = 10 */
        "load_const_int 0\n"
        "store_local 1\n"           /* sum = 0 */
        "loop:\n"
        "load_local 0\n"
        "if_false done\n"           /* if n == 0, exit */
        "load_local 1\n"
        "load_local 0\n"
        "iadd\n"
        "store_local 1\n"           /* sum += n */
        "load_local 0\n"
        "load_const_int 1\n"
        "isub\n"
        "store_local 0\n"           /* n -= 1 */
        "goto loop\n"
        "done:\n"
        "load_local 1\n"
        "return_value\n";
    int rc = vtx_asm_program(&a, prog);
    VTX_ASSERT_TRUE(rc == 0);
    if (rc != 0) {
        printf("[asm] error: %s\n", a.error_msg);
        vtx_asm_destroy(&a);
        return;
    }

    /* Build the IR */
    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_graph_t graph;
    vtx_graph_init(&graph, 1);

    vtx_bytecode_t bc = vtx_asm_emit(&a);
    vtx_method_desc_t method = {
        .name = "sum_loop", .signature = "()I", .bytecode = &bc,
        .compiled_code = NULL, .vtable_index = 0, .arg_count = 0, .is_virtual = false,
    };

    int brc = vtx_graph_build(&graph, &bc, &method, &arena);
    VTX_ASSERT_TRUE(brc == 0);
    if (brc == 0) {
        bool ok = vtx_verify_graph(&graph);
        VTX_ASSERT_TRUE(ok);
        /* The graph should have Start, End, Return, and a LoopBegin (because
         * of the backward branch). */
        uint32_t node_count = vtx_graph_node_count(&graph);
        VTX_ASSERT_TRUE(node_count >= 10);  /* at least 10 nodes for this loop */
        printf("[asm] end-to-end: %u nodes, verify=%d\n", node_count, (int)ok);
    }

    vtx_arena_destroy(&arena);
    vtx_asm_destroy(&a);
}

VTX_TEST(asm_prog_const_pool_dedup) {
    /* The same int constant added twice should reuse the same pool slot. */
    vtx_assembler_t a;
    vtx_asm_init(&a);

    const char *prog =
        "load_const_int 42\n"
        "load_const_int 42\n"
        "iadd\n"
        "return_value\n";
    int rc = vtx_asm_program(&a, prog);
    VTX_ASSERT_TRUE(rc == 0);
    if (rc == 0) {
        VTX_ASSERT_TRUE(a.const_count == 1);  /* deduplicated */
        VTX_ASSERT_TRUE(vtx_smi_value(a.constant_pool[0]) == 42);
    }

    vtx_asm_destroy(&a);
}

VTX_TEST(asm_prog_inline_comment_same_line_as_label) {
    /* Label and instruction on the same line. */
    vtx_assembler_t a;
    vtx_asm_init(&a);

    const char *prog =
        "start: return\n";
    int rc = vtx_asm_program(&a, prog);
    VTX_ASSERT_TRUE(rc == 0);
    if (rc == 0) {
        VTX_ASSERT_TRUE(vtx_asm_label_pc(&a, "start") == 0);
        VTX_ASSERT_TRUE(a.code_len == 1);
        VTX_ASSERT_TRUE(a.code[0] == VT_OP_RETURN);
    }

    vtx_asm_destroy(&a);
}

int main(void) {
    printf("=== VORTEX Assembler Program-Mode Tests ===\n\n");
    vtx_test_run_all();
    return 0;
}
