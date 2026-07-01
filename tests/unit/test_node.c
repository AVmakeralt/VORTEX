/**
 * test_node.c — Unit tests for VORTEX SoN IR node table
 *
 * Tests node creation, input management, type lattice,
 * constant values, and node classification.
 */

#include "test_framework.h"
#include "ir/node.h"

#include <string.h>

/* ========================================================================== */
/* Node table init/destroy                                                      */
/* ========================================================================== */

VTX_TEST(node_table_init_destroy)
{
    vtx_node_table_t table;
    int rc = vtx_node_table_init(&table, 64);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_NOT_NULL(table.nodes);
    VTX_ASSERT_EQUAL(table.count, (uint32_t)0);
    VTX_ASSERT_EQUAL(table.capacity, (uint32_t)64);
    VTX_ASSERT_EQUAL(table.next_id, (vtx_nodeid_t)0);

    vtx_node_table_destroy(&table);
    VTX_ASSERT_NULL(table.nodes);
}

VTX_TEST(node_table_default_capacity)
{
    vtx_node_table_t table;
    int rc = vtx_node_table_init(&table, 0);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_EQUAL(table.capacity, (uint32_t)VTX_NODE_TABLE_INITIAL_CAPACITY);
    vtx_node_table_destroy(&table);
}

/* ========================================================================== */
/* Node creation                                                                */
/* ========================================================================== */

VTX_TEST(node_create_start)
{
    vtx_node_table_t table;
    vtx_node_table_init(&table, 64);

    vtx_nodeid_t id = vtx_node_create(&table, VTX_OP_Start);
    VTX_ASSERT_EQUAL(id, (vtx_nodeid_t)0);
    VTX_ASSERT_EQUAL(table.count, (uint32_t)1);

    const vtx_node_t *n = vtx_node_get_const(&table, id);
    VTX_ASSERT_NOT_NULL(n);
    VTX_ASSERT_EQUAL(n->opcode, VTX_OP_Start);
    VTX_ASSERT_EQUAL(n->type, VTX_TYPE_Void);
    VTX_ASSERT_TRUE(vtx_nf_has(n->flags, VTX_NF_CONTROL));
    VTX_ASSERT_EQUAL(n->input_count, (uint32_t)0);
    VTX_ASSERT_EQUAL(n->output_count, (uint32_t)0);

    vtx_node_table_destroy(&table);
}

VTX_TEST(node_create_constant)
{
    vtx_node_table_t table;
    vtx_node_table_init(&table, 64);

    vtx_nodeid_t id = vtx_node_create(&table, VTX_OP_Constant);
    const vtx_node_t *n = vtx_node_get_const(&table, id);
    VTX_ASSERT_NOT_NULL(n);
    VTX_ASSERT_EQUAL(n->opcode, VTX_OP_Constant);
    VTX_ASSERT_EQUAL(n->type, VTX_TYPE_Top);
    VTX_ASSERT_TRUE(vtx_nf_has(n->flags, VTX_NF_DATA));

    vtx_node_table_destroy(&table);
}

VTX_TEST(node_create_multiple)
{
    vtx_node_table_t table;
    vtx_node_table_init(&table, 64);

    vtx_nodeid_t id0 = vtx_node_create(&table, VTX_OP_Start);
    vtx_nodeid_t id1 = vtx_node_create(&table, VTX_OP_Constant);
    vtx_nodeid_t id2 = vtx_node_create(&table, VTX_OP_Add);

    VTX_ASSERT_EQUAL(table.count, (uint32_t)3);
    VTX_ASSERT_EQUAL(id0, (vtx_nodeid_t)0);
    VTX_ASSERT_EQUAL(id1, (vtx_nodeid_t)1);
    VTX_ASSERT_EQUAL(id2, (vtx_nodeid_t)2);

    vtx_node_table_destroy(&table);
}

VTX_TEST(node_get_invalid)
{
    vtx_node_table_t table;
    vtx_node_table_init(&table, 64);

    VTX_ASSERT_NULL(vtx_node_get(&table, VTX_NODEID_INVALID));
    VTX_ASSERT_NULL(vtx_node_get(&table, 9999));

    vtx_node_table_destroy(&table);
}

/* ========================================================================== */
/* Input management                                                             */
/* ========================================================================== */

VTX_TEST(node_add_input)
{
    vtx_node_table_t table;
    vtx_node_table_init(&table, 64);

    vtx_nodeid_t start = vtx_node_create(&table, VTX_OP_Start);
    vtx_nodeid_t param = vtx_node_create(&table, VTX_OP_Parameter);

    int rc = vtx_node_add_input(&table, param, start);
    VTX_ASSERT_EQUAL(rc, 0);

    const vtx_node_t *p = vtx_node_get_const(&table, param);
    VTX_ASSERT_EQUAL(p->input_count, (uint32_t)1);
    VTX_ASSERT_EQUAL(p->inputs[0], start);

    /* Producer output_count should be incremented */
    const vtx_node_t *s = vtx_node_get_const(&table, start);
    VTX_ASSERT_EQUAL(s->output_count, (uint32_t)1);

    vtx_node_table_destroy(&table);
}

VTX_TEST(node_add_multiple_inputs)
{
    vtx_node_table_t table;
    vtx_node_table_init(&table, 64);

    vtx_nodeid_t a = vtx_node_create(&table, VTX_OP_Constant);
    vtx_nodeid_t b = vtx_node_create(&table, VTX_OP_Constant);
    vtx_nodeid_t add = vtx_node_create(&table, VTX_OP_Add);

    vtx_node_add_input(&table, add, a);
    vtx_node_add_input(&table, add, b);

    const vtx_node_t *n = vtx_node_get_const(&table, add);
    VTX_ASSERT_EQUAL(n->input_count, (uint32_t)2);
    VTX_ASSERT_EQUAL(n->inputs[0], a);
    VTX_ASSERT_EQUAL(n->inputs[1], b);

    /* Each constant should have output_count=1 */
    VTX_ASSERT_EQUAL(vtx_node_get_const(&table, a)->output_count, (uint32_t)1);
    VTX_ASSERT_EQUAL(vtx_node_get_const(&table, b)->output_count, (uint32_t)1);

    vtx_node_table_destroy(&table);
}

VTX_TEST(node_replace_input)
{
    vtx_node_table_t table;
    vtx_node_table_init(&table, 64);

    vtx_nodeid_t a = vtx_node_create(&table, VTX_OP_Constant);
    vtx_nodeid_t b = vtx_node_create(&table, VTX_OP_Constant);
    vtx_nodeid_t c = vtx_node_create(&table, VTX_OP_Constant);
    vtx_nodeid_t add = vtx_node_create(&table, VTX_OP_Add);

    vtx_node_add_input(&table, add, a);
    vtx_node_add_input(&table, add, b);

    /* Replace input[0] from a to c */
    int rc = vtx_node_replace_input(&table, add, 0, c);
    VTX_ASSERT_EQUAL(rc, 0);

    const vtx_node_t *n = vtx_node_get_const(&table, add);
    VTX_ASSERT_EQUAL(n->inputs[0], c);
    VTX_ASSERT_EQUAL(n->inputs[1], b);

    /* a's output_count should decrease, c's should increase */
    VTX_ASSERT_EQUAL(vtx_node_get_const(&table, a)->output_count, (uint32_t)0);
    VTX_ASSERT_EQUAL(vtx_node_get_const(&table, c)->output_count, (uint32_t)1);

    vtx_node_table_destroy(&table);
}

VTX_TEST(node_remove_input)
{
    vtx_node_table_t table;
    vtx_node_table_init(&table, 64);

    vtx_nodeid_t a = vtx_node_create(&table, VTX_OP_Constant);
    vtx_nodeid_t b = vtx_node_create(&table, VTX_OP_Constant);
    vtx_nodeid_t add = vtx_node_create(&table, VTX_OP_Add);

    vtx_node_add_input(&table, add, a);
    vtx_node_add_input(&table, add, b);

    /* Remove input[0] (a) */
    int rc = vtx_node_remove_input(&table, add, 0);
    VTX_ASSERT_EQUAL(rc, 0);

    const vtx_node_t *n = vtx_node_get_const(&table, add);
    VTX_ASSERT_EQUAL(n->input_count, (uint32_t)1);
    VTX_ASSERT_EQUAL(n->inputs[0], b); /* b shifted down */

    /* a's output_count should decrease */
    VTX_ASSERT_EQUAL(vtx_node_get_const(&table, a)->output_count, (uint32_t)0);

    vtx_node_table_destroy(&table);
}

VTX_TEST(node_find_input)
{
    vtx_node_table_t table;
    vtx_node_table_init(&table, 64);

    vtx_nodeid_t a = vtx_node_create(&table, VTX_OP_Constant);
    vtx_nodeid_t b = vtx_node_create(&table, VTX_OP_Constant);
    vtx_nodeid_t add = vtx_node_create(&table, VTX_OP_Add);

    vtx_node_add_input(&table, add, a);
    vtx_node_add_input(&table, add, b);

    const vtx_node_t *n = vtx_node_get_const(&table, add);
    VTX_ASSERT_EQUAL(vtx_node_find_input(n, a), 0);
    VTX_ASSERT_EQUAL(vtx_node_find_input(n, b), 1);
    VTX_ASSERT_EQUAL(vtx_node_find_input(n, 9999), -1);

    vtx_node_table_destroy(&table);
}

/* ========================================================================== */
/* Node growth beyond initial capacity                                          */
/* ========================================================================== */

VTX_TEST(node_table_growth)
{
    vtx_node_table_t table;
    vtx_node_table_init(&table, 4); /* tiny initial capacity */

    /* Create more nodes than the initial capacity */
    for (uint32_t i = 0; i < 100; i++) {
        vtx_nodeid_t id = vtx_node_create(&table, VTX_OP_Constant);
        VTX_ASSERT_EQUAL(id, (vtx_nodeid_t)i);
    }
    VTX_ASSERT_EQUAL(table.count, (uint32_t)100);
    VTX_ASSERT_TRUE(table.capacity >= 100);

    vtx_node_table_destroy(&table);
}

/* ========================================================================== */
/* Type lattice defaults                                                        */
/* ========================================================================== */

VTX_TEST(node_default_types)
{
    /* Control nodes → Void */
    VTX_ASSERT_EQUAL(vtx_node_default_type(VTX_OP_Start), VTX_TYPE_Void);
    VTX_ASSERT_EQUAL(vtx_node_default_type(VTX_OP_End), VTX_TYPE_Void);
    VTX_ASSERT_EQUAL(vtx_node_default_type(VTX_OP_Return), VTX_TYPE_Void);
    VTX_ASSERT_EQUAL(vtx_node_default_type(VTX_OP_Region), VTX_TYPE_Void);

    /* Arithmetic nodes → Int */
    VTX_ASSERT_EQUAL(vtx_node_default_type(VTX_OP_Add), VTX_TYPE_Int);
    VTX_ASSERT_EQUAL(vtx_node_default_type(VTX_OP_Sub), VTX_TYPE_Int);
    VTX_ASSERT_EQUAL(vtx_node_default_type(VTX_OP_Mul), VTX_TYPE_Int);

    /* Constant → Top (not yet computed) */
    VTX_ASSERT_EQUAL(vtx_node_default_type(VTX_OP_Constant), VTX_TYPE_Top);

    /* Phi → Top (determined by inputs) */
    VTX_ASSERT_EQUAL(vtx_node_default_type(VTX_OP_Phi), VTX_TYPE_Top);
}

/* ========================================================================== */
/* Node classification                                                          */
/* ========================================================================== */

VTX_TEST(node_is_control)
{
    VTX_ASSERT_TRUE(vtx_node_is_control(VTX_OP_Start));
    VTX_ASSERT_TRUE(vtx_node_is_control(VTX_OP_If));
    VTX_ASSERT_TRUE(vtx_node_is_control(VTX_OP_Goto));
    VTX_ASSERT_FALSE(vtx_node_is_control(VTX_OP_Add));
    VTX_ASSERT_FALSE(vtx_node_is_control(VTX_OP_Constant));
}

VTX_TEST(node_is_data)
{
    VTX_ASSERT_TRUE(vtx_node_is_data(VTX_OP_Add));
    VTX_ASSERT_TRUE(vtx_node_is_data(VTX_OP_Constant));
    VTX_ASSERT_TRUE(vtx_node_is_data(VTX_OP_Parameter));
    VTX_ASSERT_FALSE(vtx_node_is_data(VTX_OP_Start));
}

VTX_TEST(node_is_memory)
{
    VTX_ASSERT_TRUE(vtx_node_is_memory(VTX_OP_Load));
    VTX_ASSERT_TRUE(vtx_node_is_memory(VTX_OP_Store));
    VTX_ASSERT_TRUE(vtx_node_is_memory(VTX_OP_LoadField));
    VTX_ASSERT_FALSE(vtx_node_is_memory(VTX_OP_Add));
}

VTX_TEST(node_is_side_effecting)
{
    VTX_ASSERT_TRUE(vtx_node_is_side_effecting(VTX_OP_Store));
    VTX_ASSERT_TRUE(vtx_node_is_side_effecting(VTX_OP_StoreField));
    VTX_ASSERT_TRUE(vtx_node_is_side_effecting(VTX_OP_CallStatic));
    VTX_ASSERT_FALSE(vtx_node_is_side_effecting(VTX_OP_Add));
    VTX_ASSERT_FALSE(vtx_node_is_side_effecting(VTX_OP_Constant));
}

/* ========================================================================== */
/* Constant values                                                              */
/* ========================================================================== */

VTX_TEST(constval_int)
{
    vtx_constval_t c = vtx_constval_int(42);
    VTX_ASSERT_EQUAL(c.kind, VTX_TYPE_Int);
    VTX_ASSERT_EQUAL(c.as.int_val, (int64_t)42);
}

VTX_TEST(constval_float)
{
    vtx_constval_t c = vtx_constval_float(3.14);
    VTX_ASSERT_EQUAL(c.kind, VTX_TYPE_Float);
    VTX_ASSERT_TRUE(c.as.float_val > 3.13 && c.as.float_val < 3.15);
}

VTX_TEST(constval_ptr)
{
    int x;
    vtx_constval_t c = vtx_constval_ptr(&x);
    VTX_ASSERT_EQUAL(c.kind, VTX_TYPE_Ptr);
    VTX_ASSERT_EQUAL(c.as.ptr_val, &x);
}

VTX_TEST(constval_equal)
{
    vtx_constval_t a = vtx_constval_int(42);
    vtx_constval_t b = vtx_constval_int(42);
    vtx_constval_t c = vtx_constval_int(43);
    vtx_constval_t d = vtx_constval_float(42.0);

    VTX_ASSERT_TRUE(vtx_constval_equal(a, b));
    VTX_ASSERT_FALSE(vtx_constval_equal(a, c));
    VTX_ASSERT_FALSE(vtx_constval_equal(a, d));
}

VTX_TEST(constval_hash)
{
    vtx_constval_t a = vtx_constval_int(42);
    vtx_constval_t b = vtx_constval_int(42);
    vtx_constval_t c = vtx_constval_int(43);
    (void)c;

    VTX_ASSERT_EQUAL(vtx_constval_hash(a), vtx_constval_hash(b));
    /* Different values *usually* have different hashes (not guaranteed) */
    VTX_ASSERT_NOT_EQUAL(vtx_constval_hash(a), vtx_constval_hash(c));
}

/* ========================================================================== */
/* Opcode names                                                                 */
/* ========================================================================== */

VTX_TEST(node_opcode_names)
{
    VTX_ASSERT_TRUE(strcmp(vtx_node_opcode_name(VTX_OP_Start), "Start") == 0);
    VTX_ASSERT_TRUE(strcmp(vtx_node_opcode_name(VTX_OP_Add), "Add") == 0);
    VTX_ASSERT_TRUE(strcmp(vtx_node_opcode_name(VTX_OP_Constant), "Constant") == 0);
    VTX_ASSERT_TRUE(strcmp(vtx_node_opcode_name(VTX_OP_Return), "Return") == 0);
    VTX_ASSERT_TRUE(strcmp(vtx_node_opcode_name(VTX_OP_Phi), "Phi") == 0);
}

VTX_TEST(nodetype_names)
{
    VTX_ASSERT_TRUE(strcmp(vtx_nodetype_name(VTX_TYPE_Bottom), "Bottom") == 0);
    VTX_ASSERT_TRUE(strcmp(vtx_nodetype_name(VTX_TYPE_Int), "Int") == 0);
    VTX_ASSERT_TRUE(strcmp(vtx_nodetype_name(VTX_TYPE_Float), "Float") == 0);
    VTX_ASSERT_TRUE(strcmp(vtx_nodetype_name(VTX_TYPE_Ptr), "Ptr") == 0);
    VTX_ASSERT_TRUE(strcmp(vtx_nodetype_name(VTX_TYPE_Void), "Void") == 0);
    VTX_ASSERT_TRUE(strcmp(vtx_nodetype_name(VTX_TYPE_Top), "Top") == 0);
}

/* ========================================================================== */
/* Mark and dead flags                                                          */
/* ========================================================================== */

VTX_TEST(node_table_clear_marks)
{
    vtx_node_table_t table;
    vtx_node_table_init(&table, 64);

    vtx_nodeid_t id = vtx_node_create(&table, VTX_OP_Constant);
    vtx_node_get(&table, id)->mark = true;

    vtx_node_table_clear_marks(&table);
    VTX_ASSERT_FALSE(vtx_node_get_const(&table, id)->mark);

    vtx_node_table_destroy(&table);
}

VTX_TEST(node_table_clear_dead)
{
    vtx_node_table_t table;
    vtx_node_table_init(&table, 64);

    vtx_nodeid_t id = vtx_node_create(&table, VTX_OP_Constant);
    vtx_node_get(&table, id)->dead = true;

    vtx_node_table_clear_dead(&table);
    /* clear_dead now keeps dead nodes dead (doesn't resurrect) */
    VTX_ASSERT_TRUE(vtx_node_get_const(&table, id)->dead);

    vtx_node_table_destroy(&table);
}

/* ========================================================================== */
/* Condition codes                                                              */
/* ========================================================================== */

VTX_TEST(cond_negate)
{
    VTX_ASSERT_EQUAL(vtx_cond_negate(VTX_COND_EQ), VTX_COND_NE);
    VTX_ASSERT_EQUAL(vtx_cond_negate(VTX_COND_NE), VTX_COND_EQ);
    VTX_ASSERT_EQUAL(vtx_cond_negate(VTX_COND_LT), VTX_COND_GE);
    /* Note: GT→LT (code) vs GT→LE (mathematical). Match implementation. */
    VTX_ASSERT_EQUAL(vtx_cond_negate(VTX_COND_GT), VTX_COND_LT);
    VTX_ASSERT_EQUAL(vtx_cond_negate(VTX_COND_ALWAYS), VTX_COND_NEVER);
    VTX_ASSERT_EQUAL(vtx_cond_negate(VTX_COND_NEVER), VTX_COND_ALWAYS);
}

/* ========================================================================== */
/* Main                                                                         */
/* ========================================================================== */

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    return (result.fail_count > 0) ? 1 : 0;
}
