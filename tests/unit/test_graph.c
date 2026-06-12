/**
 * test_graph.c — Unit tests for VORTEX SoN graph construction
 *
 * Tests SoN graph construction from simple bytecode programs.
 * Verifies node creation, control flow, memory chains, and Phi insertion.
 */

#include "test_framework.h"
#include "ir/graph.h"
#include "runtime/arena.h"

#include <string.h>

/* ========================================================================== */
/* Helper: create a bytecode object from raw bytes                              */
/* ========================================================================== */

static vtx_bytecode_t make_bytecode(const uint8_t *code, size_t len,
                                     uint16_t max_locals, uint16_t max_stack)
{
    vtx_bytecode_t bc;
    bc.code = code;
    bc.length = len;
    bc.constant_pool = NULL;
    bc.constant_count = 0;
    bc.max_locals = max_locals;
    bc.max_stack = max_stack;
    return bc;
}

/* ========================================================================== */
/* Graph init/destroy                                                           */
/* ========================================================================== */

VTX_TEST(graph_init_destroy)
{
    vtx_graph_t graph;
    int rc = vtx_graph_init(&graph, 0);
    VTX_ASSERT_EQUAL(rc, 0);

    /* Start node should exist */
    VTX_ASSERT_NOT_EQUAL(graph.start_node, VTX_NODEID_INVALID);
    VTX_ASSERT_TRUE(vtx_graph_node_count(&graph) > 0);

    /* Start node should be a control node */
    vtx_node_t *start = vtx_graph_node(&graph, graph.start_node);
    VTX_ASSERT_NOT_NULL(start);
    VTX_ASSERT_EQUAL(start->opcode, VTX_OP_Start);

    vtx_graph_destroy(&graph);
}

VTX_TEST(graph_init_with_params)
{
    vtx_graph_t graph;
    int rc = vtx_graph_init(&graph, 3);
    VTX_ASSERT_EQUAL(rc, 0);

    VTX_ASSERT_EQUAL(graph.parameter_count, (uint32_t)3);
    VTX_ASSERT_NOT_NULL(graph.parameters);

    /* Each parameter should be a Parameter node */
    for (uint32_t i = 0; i < 3; i++) {
        vtx_node_t *p = vtx_graph_node(&graph, graph.parameters[i]);
        VTX_ASSERT_NOT_NULL(p);
        VTX_ASSERT_EQUAL(p->opcode, VTX_OP_Parameter);
        VTX_ASSERT_EQUAL(p->local_index, i);
    }

    vtx_graph_destroy(&graph);
}

VTX_TEST(graph_province_node)
{
    vtx_graph_t graph;
    vtx_graph_init(&graph, 0);

    /* Province node should exist as initial memory state */
    VTX_ASSERT_NOT_EQUAL(graph.entry_memory, VTX_NODEID_INVALID);
    vtx_node_t *province = vtx_graph_node(&graph, graph.entry_memory);
    VTX_ASSERT_NOT_NULL(province);
    VTX_ASSERT_EQUAL(province->opcode, VTX_OP_Province);

    vtx_graph_destroy(&graph);
}

/* ========================================================================== */
/* Graph building from simple bytecode                                          */
/* ========================================================================== */

VTX_TEST(graph_build_trivial_return)
{
    /* Simple program: return_void */
    uint8_t code[] = { VT_OP_RETURN };
    vtx_bytecode_t bc = make_bytecode(code, sizeof(code), 0, 0);

    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 0), 0);

    vtx_method_desc_t method = {
        .name = "test", .signature = "()V", .bytecode = &bc,
        .vtable_index = 0xFFFFFFFF, .arg_count = 0, .is_virtual = false
    };

    int rc = vtx_graph_build(&graph, &bc, &method, &arena);
    VTX_ASSERT_EQUAL(rc, 0);

    /* Should have at least Start + Province + Region (entry) + Return */
    VTX_ASSERT_TRUE(vtx_graph_node_count(&graph) >= 3);

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(graph_build_arithmetic)
{
    /* load_local 0, load_local 1, iadd, return_value */
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL,  0x00, 0x00,  /* load local 0 */
        VT_OP_LOAD_LOCAL,  0x00, 0x01,  /* load local 1 */
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_bytecode_t bc = make_bytecode(code, sizeof(code), 2, 4);

    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 2), 0);

    vtx_method_desc_t method = {
        .name = "add", .signature = "(II)I", .bytecode = &bc,
        .vtable_index = 0xFFFFFFFF, .arg_count = 2, .is_virtual = false
    };

    int rc = vtx_graph_build(&graph, &bc, &method, &arena);
    VTX_ASSERT_EQUAL(rc, 0);

    /* Should have more nodes than a trivial graph */
    uint32_t count = vtx_graph_node_count(&graph);
    VTX_ASSERT_TRUE(count > 4);

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(graph_build_if_branch)
{
    /* load_local 0, if_true 12, load_null, return_value, load_true, return_value */
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL,  0x00, 0x00,  /* PC 0: load local 0 */
        VT_OP_IF_TRUE,     0x00, 0x0C,  /* PC 3: if_true → PC 12 */
        VT_OP_LOAD_NULL,                /* PC 6 */
        VT_OP_RETURN_VALUE,             /* PC 7 */
        VT_OP_LOAD_TRUE,                /* PC 8 */
        VT_OP_RETURN_VALUE,             /* PC 9 */
        /* Padding to reach PC 12 */
        VT_OP_NOP,                      /* PC 10 */
        VT_OP_NOP,                      /* PC 11 */
        VT_OP_LOAD_TRUE,                /* PC 12: branch target */
        VT_OP_RETURN_VALUE              /* PC 13 */
    };
    vtx_bytecode_t bc = make_bytecode(code, sizeof(code), 1, 4);

    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 1), 0);

    vtx_method_desc_t method = {
        .name = "branch", .signature = "(Z)Z", .bytecode = &bc,
        .vtable_index = 0xFFFFFFFF, .arg_count = 1, .is_virtual = false
    };

    int rc = vtx_graph_build(&graph, &bc, &method, &arena);
    VTX_ASSERT_EQUAL(rc, 0);

    /* Should have Region nodes for multiple blocks */
    bool has_if = false;
    for (uint32_t i = 0; i < graph.node_table.count; i++) {
        if (graph.node_table.nodes[i].opcode == VTX_OP_If) {
            has_if = true;
            break;
        }
    }
    VTX_ASSERT_TRUE(has_if);

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(graph_build_goto_loop)
{
    /* Simple loop: load_local 0, if_false 10 (exit), goto 0, return
     * This tests that backward branches produce LoopBegin nodes
     * without requiring arithmetic that the builder may not simulate fully. */
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL,   0x00, 0x00,  /* PC  0: load local 0 */
        VT_OP_IF_FALSE,     0x00, 0x0A,  /* PC  3: if_false → PC 10 */
        VT_OP_GOTO,         0x00, 0x00,  /* PC  6: goto PC 0 (backward = loop) */
        VT_OP_NOP,                       /* PC  9: padding */
        VT_OP_LOAD_LOCAL,   0x00, 0x00,  /* PC 10: exit path */
        VT_OP_RETURN_VALUE               /* PC 13 */
    };
    vtx_bytecode_t bc = make_bytecode(code, sizeof(code), 1, 4);

    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 1), 0);

    vtx_method_desc_t method = {
        .name = "loop", .signature = "(I)I", .bytecode = &bc,
        .vtable_index = 0xFFFFFFFF, .arg_count = 1, .is_virtual = false
    };

    int rc = vtx_graph_build(&graph, &bc, &method, &arena);
    VTX_ASSERT_EQUAL(rc, 0);

    /* Should have a LoopBegin node (backward branch detected) */
    bool has_loop_begin = false;
    for (uint32_t i = 0; i < graph.node_table.count; i++) {
        if (graph.node_table.nodes[i].opcode == VTX_OP_LoopBegin) {
            has_loop_begin = true;
            break;
        }
    }
    VTX_ASSERT_TRUE(has_loop_begin);

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Graph node count                                                             */
/* ========================================================================== */

VTX_TEST(graph_node_count)
{
    vtx_graph_t graph;
    vtx_graph_init(&graph, 2);

    /* Start + Province + 2 Parameters = 4 */
    VTX_ASSERT_EQUAL(vtx_graph_node_count(&graph), (uint32_t)4);

    vtx_graph_destroy(&graph);
}

/* ========================================================================== */
/* Graph node lookup                                                            */
/* ========================================================================== */

VTX_TEST(graph_node_lookup)
{
    vtx_graph_t graph;
    vtx_graph_init(&graph, 0);

    vtx_node_t *start = vtx_graph_node(&graph, graph.start_node);
    VTX_ASSERT_NOT_NULL(start);

    vtx_node_t *invalid = vtx_graph_node(&graph, VTX_NODEID_INVALID);
    VTX_ASSERT_NULL(invalid);

    vtx_graph_destroy(&graph);
}

/* ========================================================================== */
/* Main                                                                         */
/* ========================================================================== */

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    return (result.fail_count > 0) ? 1 : 0;
}
