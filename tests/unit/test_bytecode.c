/**
 * test_bytecode.c — Unit tests for VORTEX bytecode module
 *
 * Tests opcode table completeness, operand reading, stack effects,
 * and disassembly.
 */

#include "test_framework.h"
#include "runtime/bytecode.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/* Opcode table completeness                                                    */
/* ========================================================================== */

VTX_TEST(opcode_table_count)
{
    /* The table must have exactly VT_OP_COUNT entries */
    VTX_ASSERT_TRUE(VT_OP_COUNT > 0);
    /* Verify each entry has a non-NULL name */
    for (int i = 0; i < VT_OP_COUNT; i++) {
        VTX_ASSERT_NOT_NULL(vtx_opcode_table[i].name);
        VTX_ASSERT_TRUE(strlen(vtx_opcode_table[i].name) > 0);
    }
}

VTX_TEST(opcode_halt)
{
    VTX_ASSERT_EQUAL(vtx_opcode_table[VT_OP_HALT].stack_input_count, (uint8_t)0);
    VTX_ASSERT_EQUAL(vtx_opcode_table[VT_OP_HALT].stack_output_count, (uint8_t)0);
    VTX_ASSERT_FALSE(vtx_opcode_table[VT_OP_HALT].has_operand);
}

VTX_TEST(opcode_nop)
{
    VTX_ASSERT_EQUAL(vtx_opcode_table[VT_OP_NOP].stack_input_count, (uint8_t)0);
    VTX_ASSERT_EQUAL(vtx_opcode_table[VT_OP_NOP].stack_output_count, (uint8_t)0);
    VTX_ASSERT_FALSE(vtx_opcode_table[VT_OP_NOP].has_operand);
}

VTX_TEST(opcode_load_local)
{
    VTX_ASSERT_TRUE(vtx_opcode_table[VT_OP_LOAD_LOCAL].has_operand);
    VTX_ASSERT_EQUAL(vtx_opcode_table[VT_OP_LOAD_LOCAL].operand_size, (uint8_t)2);
    VTX_ASSERT_EQUAL(vtx_opcode_table[VT_OP_LOAD_LOCAL].stack_input_count, (uint8_t)0);
    VTX_ASSERT_EQUAL(vtx_opcode_table[VT_OP_LOAD_LOCAL].stack_output_count, (uint8_t)1);
}

VTX_TEST(opcode_store_local)
{
    VTX_ASSERT_TRUE(vtx_opcode_table[VT_OP_STORE_LOCAL].has_operand);
    VTX_ASSERT_EQUAL(vtx_opcode_table[VT_OP_STORE_LOCAL].operand_size, (uint8_t)2);
    VTX_ASSERT_EQUAL(vtx_opcode_table[VT_OP_STORE_LOCAL].stack_input_count, (uint8_t)1);
    VTX_ASSERT_EQUAL(vtx_opcode_table[VT_OP_STORE_LOCAL].stack_output_count, (uint8_t)0);
}

VTX_TEST(opcode_iadd)
{
    VTX_ASSERT_EQUAL(vtx_opcode_table[VT_OP_IADD].stack_input_count, (uint8_t)2);
    VTX_ASSERT_EQUAL(vtx_opcode_table[VT_OP_IADD].stack_output_count, (uint8_t)1);
    VTX_ASSERT_FALSE(vtx_opcode_table[VT_OP_IADD].has_operand);
}

VTX_TEST(opcode_arithmetic_no_operands)
{
    /* All pure arithmetic opcodes have no operand */
    vtx_opcode_t arith_ops[] = {
        VT_OP_IADD, VT_OP_ISUB, VT_OP_IMUL, VT_OP_IDIV, VT_OP_IMOD,
        VT_OP_FADD, VT_OP_FSUB, VT_OP_FMUL, VT_OP_FDIV,
        VT_OP_ISHL, VT_OP_ISHR, VT_OP_IAND, VT_OP_IOR, VT_OP_IXOR,
        VT_OP_INEG, VT_OP_INOT
    };
    for (size_t i = 0; i < sizeof(arith_ops) / sizeof(arith_ops[0]); i++) {
        VTX_ASSERT_FALSE(vtx_opcode_table[arith_ops[i]].has_operand);
    }
}

VTX_TEST(opcode_branch_has_operand)
{
    VTX_ASSERT_TRUE(vtx_opcode_table[VT_OP_GOTO].has_operand);
    VTX_ASSERT_TRUE(vtx_opcode_table[VT_OP_IF_TRUE].has_operand);
    VTX_ASSERT_TRUE(vtx_opcode_table[VT_OP_IF_FALSE].has_operand);
    VTX_ASSERT_EQUAL(vtx_opcode_table[VT_OP_GOTO].operand_size, (uint8_t)2);
}

VTX_TEST(opcode_calls_have_operands)
{
    VTX_ASSERT_TRUE(vtx_opcode_table[VT_OP_CALL_STATIC].has_operand);
    VTX_ASSERT_TRUE(vtx_opcode_table[VT_OP_CALL_VIRTUAL].has_operand);
    VTX_ASSERT_TRUE(vtx_opcode_table[VT_OP_CALL_INTERFACE].has_operand);
    VTX_ASSERT_EQUAL(vtx_opcode_table[VT_OP_CALL_STATIC].operand_size, (uint8_t)2);
}

VTX_TEST(opcode_const_loads)
{
    VTX_ASSERT_TRUE(vtx_opcode_table[VT_OP_LOAD_CONST_INT].has_operand);
    VTX_ASSERT_TRUE(vtx_opcode_table[VT_OP_LOAD_CONST_FLOAT].has_operand);
    VTX_ASSERT_TRUE(vtx_opcode_table[VT_OP_LOAD_CONST_STR].has_operand);
    VTX_ASSERT_FALSE(vtx_opcode_table[VT_OP_LOAD_NULL].has_operand);
    VTX_ASSERT_FALSE(vtx_opcode_table[VT_OP_LOAD_TRUE].has_operand);
    VTX_ASSERT_FALSE(vtx_opcode_table[VT_OP_LOAD_FALSE].has_operand);
    VTX_ASSERT_FALSE(vtx_opcode_table[VT_OP_LOAD_UNDEFINED].has_operand);
}

/* ========================================================================== */
/* Operand reading                                                             */
/* ========================================================================== */

VTX_TEST(bytecode_read_operand_big_endian)
{
    /* Construct a small bytecode stream: [opcode, 0x12, 0x34] */
    uint8_t code[] = { VT_OP_LOAD_LOCAL, 0x12, 0x34 };
    vtx_bytecode_t bc = {
        .code = code,
        .length = sizeof(code),
        .constant_pool = NULL,
        .constant_count = 0,
        .max_locals = 1,
        .max_stack = 4
    };

    uint16_t operand = vtx_bytecode_read_operand(&bc, 0);
    VTX_ASSERT_EQUAL(operand, (uint16_t)0x1234);
}

VTX_TEST(bytecode_read_operand_zero)
{
    uint8_t code[] = { VT_OP_LOAD_LOCAL, 0x00, 0x00 };
    vtx_bytecode_t bc = {
        .code = code,
        .length = sizeof(code),
        .constant_pool = NULL,
        .constant_count = 0,
        .max_locals = 1,
        .max_stack = 4
    };

    uint16_t operand = vtx_bytecode_read_operand(&bc, 0);
    VTX_ASSERT_EQUAL(operand, (uint16_t)0);
}

VTX_TEST(bytecode_read_operand_max)
{
    uint8_t code[] = { VT_OP_LOAD_LOCAL, 0xFF, 0xFF };
    vtx_bytecode_t bc = {
        .code = code,
        .length = sizeof(code),
        .constant_pool = NULL,
        .constant_count = 0,
        .max_locals = 1,
        .max_stack = 4
    };

    uint16_t operand = vtx_bytecode_read_operand(&bc, 0);
    VTX_ASSERT_EQUAL(operand, (uint16_t)0xFFFF);
}

/* ========================================================================== */
/* Stack effects                                                                */
/* ========================================================================== */

VTX_TEST(stack_effect_iadd)
{
    /* iadd: pops 2, pushes 1 → net -1 */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_IADD), -1);
}

VTX_TEST(stack_effect_load_local)
{
    /* load_local: pops 0, pushes 1 → net +1 */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_LOAD_LOCAL), 1);
}

VTX_TEST(stack_effect_store_local)
{
    /* store_local: pops 1, pushes 0 → net -1 */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_STORE_LOCAL), -1);
}

VTX_TEST(stack_effect_goto)
{
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_GOTO), 0);
}

VTX_TEST(stack_effect_dup)
{
    /* dup: pops 1, pushes 2 → net +1 */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_DUP), 1);
}

VTX_TEST(stack_effect_pop)
{
    /* pop: pops 1, pushes 0 → net -1 */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_POP), -1);
}

VTX_TEST(stack_effect_swap)
{
    /* swap: pops 2, pushes 2 → net 0 */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_SWAP), 0);
}

/* ========================================================================== */
/* Opcode at PC                                                                 */
/* ========================================================================== */

VTX_TEST(bytecode_opcode_at)
{
    uint8_t code[] = { VT_OP_IADD, VT_OP_ISUB, VT_OP_HALT };
    vtx_bytecode_t bc = {
        .code = code,
        .length = sizeof(code),
        .constant_pool = NULL,
        .constant_count = 0,
        .max_locals = 0,
        .max_stack = 4
    };

    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 0) == VT_OP_IADD);
    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 1) == VT_OP_ISUB);
    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 2) == VT_OP_HALT);
}

/* ========================================================================== */
/* Instruction length                                                           */
/* ========================================================================== */

VTX_TEST(insn_length_no_operand)
{
    uint8_t code[] = { VT_OP_IADD };
    vtx_bytecode_t bc = {
        .code = code,
        .length = sizeof(code),
        .constant_pool = NULL,
        .constant_count = 0,
        .max_locals = 0,
        .max_stack = 4
    };

    VTX_ASSERT_EQUAL(vtx_bytecode_insn_length(&bc, 0), (size_t)1);
}

VTX_TEST(insn_length_with_2byte_operand)
{
    uint8_t code[] = { VT_OP_LOAD_LOCAL, 0x00, 0x05 };
    vtx_bytecode_t bc = {
        .code = code,
        .length = sizeof(code),
        .constant_pool = NULL,
        .constant_count = 0,
        .max_locals = 1,
        .max_stack = 4
    };

    VTX_ASSERT_EQUAL(vtx_bytecode_insn_length(&bc, 0), (size_t)3);
}

/* ========================================================================== */
/* Disassembly                                                                  */
/* ========================================================================== */

VTX_TEST(disassemble_no_operand)
{
    uint8_t code[] = { VT_OP_IADD };
    vtx_bytecode_t bc = {
        .code = code,
        .length = sizeof(code),
        .constant_pool = NULL,
        .constant_count = 0,
        .max_locals = 0,
        .max_stack = 4
    };

    char buf[128];
    size_t next_pc = vtx_bytecode_disassemble_op(&bc, 0, buf, sizeof(buf));
    VTX_ASSERT_EQUAL(next_pc, (size_t)1);
    /* Should contain "IADD" */
    VTX_ASSERT_NOT_NULL(strstr(buf, "IADD"));
}

VTX_TEST(disassemble_with_operand)
{
    uint8_t code[] = { VT_OP_LOAD_LOCAL, 0x00, 0x05 };
    vtx_bytecode_t bc = {
        .code = code,
        .length = sizeof(code),
        .constant_pool = NULL,
        .constant_count = 0,
        .max_locals = 1,
        .max_stack = 4
    };

    char buf[128];
    size_t next_pc = vtx_bytecode_disassemble_op(&bc, 0, buf, sizeof(buf));
    VTX_ASSERT_EQUAL(next_pc, (size_t)3);
    VTX_ASSERT_NOT_NULL(strstr(buf, "LOAD_LOCAL"));
    VTX_ASSERT_NOT_NULL(strstr(buf, "5"));
}

VTX_TEST(disassemble_multi_instruction)
{
    /* Construct: load_const_int 0, load_const_int 1, iadd, return_value */
    uint8_t code[] = {
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,  /* load const pool[0] */
        VT_OP_LOAD_CONST_INT, 0x00, 0x01,  /* load const pool[1] */
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(10), vtx_make_smi(20) };
    vtx_bytecode_t bc = {
        .code = code,
        .length = sizeof(code),
        .constant_pool = consts,
        .constant_count = 2,
        .max_locals = 0,
        .max_stack = 4
    };

    char buf[128];

    /* First instruction */
    size_t pc = vtx_bytecode_disassemble_op(&bc, 0, buf, sizeof(buf));
    VTX_ASSERT_NOT_NULL(strstr(buf, "LOAD_CONST_INT"));

    /* Second instruction */
    pc = vtx_bytecode_disassemble_op(&bc, pc, buf, sizeof(buf));
    VTX_ASSERT_NOT_NULL(strstr(buf, "LOAD_CONST_INT"));

    /* Third instruction */
    pc = vtx_bytecode_disassemble_op(&bc, pc, buf, sizeof(buf));
    VTX_ASSERT_NOT_NULL(strstr(buf, "IADD"));

    /* Fourth instruction */
    pc = vtx_bytecode_disassemble_op(&bc, pc, buf, sizeof(buf));
    VTX_ASSERT_NOT_NULL(strstr(buf, "RETURN_VALUE"));
}

/* ========================================================================== */
/* Main                                                                         */
/* ========================================================================== */

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    return (result.fail_count > 0) ? 1 : 0;
}
