#include "baseline/guards.h"
#include "baseline/codegen.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Helper: emit REX prefix for 64-bit operations                              */
/* ========================================================================== */

/**
 * Emit a REX.W prefix (0x48) for 64-bit operand size.
 * Also emits REX.B or REX.R if extended registers are used.
 */
static void emit_rex_w(vtx_code_buffer_t *buf, vtx_reg_t reg, vtx_reg_t rm)
{
    uint8_t rex = 0x48; /* REX.W */
    if (reg >= VTX_REG_R8)  rex |= 0x04; /* REX.R */
    if (rm >= VTX_REG_R8)   rex |= 0x01; /* REX.B */
    vtx_code_buffer_emit_byte(buf, rex);
}

/**
 * Emit a REX prefix with W bit only if needed for extended registers,
 * without forcing 64-bit operand size (for 32-bit operations).
 */
static void emit_rex_if_needed(vtx_code_buffer_t *buf, vtx_reg_t reg, vtx_reg_t rm)
{
    uint8_t rex = 0x40;
    bool needed = false;
    if (reg >= VTX_REG_R8)  { rex |= 0x04; needed = true; }
    if (rm >= VTX_REG_R8)   { rex |= 0x01; needed = true; }
    if (needed) vtx_code_buffer_emit_byte(buf, rex);
}

/* ========================================================================== */
/* Helper: ModR/M byte                                                        */
/* ========================================================================== */

static inline uint8_t make_modrm(uint8_t mod, uint8_t reg, uint8_t rm)
{
    return (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

/* ========================================================================== */
/* Helper: emit cmp [reg+offset], imm32                                       */
/* ========================================================================== */

/**
 * Emit: cmp dword ptr [reg + offset], imm32
 * Compares a 32-bit memory value against an immediate.
 */
static void emit_cmp_mem_imm32(vtx_code_buffer_t *buf, vtx_reg_t base,
                                int32_t offset, uint32_t imm32)
{
    /* REX.W prefix not needed for 32-bit compare, but we may need REX for
     * extended base register. However, we use 32-bit compare (no REX.W)
     * to avoid sign-extension issues with 64-bit values.
     *
     * Actually, the type_id field is uint32_t. We want a 32-bit compare.
     * Opcode: 81 /7 id  — CMP r/m32, imm32
     */
    emit_rex_if_needed(buf, (vtx_reg_t)7, base);

    if (offset == 0 && (base & 7) != VTX_REG_RBP && (base & 7) != VTX_REG_R13) {
        /* No displacement needed (but RBP/R13 always need disp8) */
        vtx_code_buffer_emit_byte(buf, 0x81); /* CMP r/m32, imm32 */
        vtx_code_buffer_emit_byte(buf, make_modrm(0, 7, base));
    } else if (offset >= -128 && offset <= 127) {
        vtx_code_buffer_emit_byte(buf, 0x81);
        vtx_code_buffer_emit_byte(buf, make_modrm(1, 7, base)); /* disp8 */
        vtx_code_buffer_emit_byte(buf, (uint8_t)(offset & 0xFF));
    } else {
        vtx_code_buffer_emit_byte(buf, 0x81);
        vtx_code_buffer_emit_byte(buf, make_modrm(2, 7, base)); /* disp32 */
        vtx_code_buffer_emit_dword(buf, (uint32_t)offset);
    }
    vtx_code_buffer_emit_dword(buf, imm32);
}

/* ========================================================================== */
/* Helper: emit test reg, reg (for null check)                                */
/* ========================================================================== */

/**
 * Emit: test reg, reg  (64-bit)
 * Sets ZF if the register is zero.
 */
static void emit_test_reg_reg(vtx_code_buffer_t *buf, vtx_reg_t reg)
{
    emit_rex_w(buf, reg, reg);
    vtx_code_buffer_emit_byte(buf, 0x85); /* TEST r/m64, r64 */
    vtx_code_buffer_emit_byte(buf, make_modrm(3, reg, reg));
}

/* ========================================================================== */
/* Helper: emit conditional jump with 32-bit displacement                    */
/* ========================================================================== */

/**
 * Emit a conditional jump with a 32-bit relative offset.
 * The offset is from the end of the jump instruction (6 bytes).
 * Returns the position of the displacement for later patching.
 *
 * @param buf     Code buffer
 * @param cc      Condition code (0x0F 8x + cc)
 * @return        Position in the code buffer where the 4-byte displacement is
 */
static uint32_t emit_jcc32(vtx_code_buffer_t *buf, uint8_t cc)
{
    vtx_code_buffer_emit_byte(buf, 0x0F);
    vtx_code_buffer_emit_byte(buf, (uint8_t)(0x80 | cc));
    uint32_t disp_pos = vtx_code_buffer_position(buf);
    vtx_code_buffer_emit_dword(buf, 0); /* placeholder, will be patched */
    return disp_pos;
}

/* Condition codes for x86-64 */
#define CC_E  0x4  /* equal / zero (ZF=1) */
#define CC_NE 0x5  /* not equal / not zero (ZF=0) */
#define CC_L  0xC  /* less (SF!=OF) */
#define CC_GE 0xD  /* greater or equal (SF=OF) */
#define CC_B  0x2  /* below / unsigned less (CF=1) */
#define CC_AE 0x3  /* above or equal / unsigned !below (CF=0) */
#define CC_A  0x7  /* above / unsigned greater (CF=0 && ZF=0) */
#define CC_BE 0x6  /* below or equal (CF=1 || ZF=1) */
#define CC_O  0x0  /* overflow (OF=1) */

/* ========================================================================== */
/* Guard array operations                                                      */
/* ========================================================================== */

int vtx_guard_array_init(vtx_guard_array_t *arr)
{
    arr->capacity = VTX_GUARDS_INITIAL_CAPACITY;
    arr->count = 0;
    arr->guards = (vtx_guard_info_t *)malloc(
        arr->capacity * sizeof(vtx_guard_info_t));
    if (!arr->guards) return -1;
    return 0;
}

void vtx_guard_array_destroy(vtx_guard_array_t *arr)
{
    if (arr->guards) {
        free(arr->guards);
        arr->guards = NULL;
    }
    arr->count = 0;
    arr->capacity = 0;
}

uint32_t vtx_guard_array_add(vtx_guard_array_t *arr, vtx_guard_info_t guard)
{
    if (arr->count >= arr->capacity) {
        uint32_t new_cap = arr->capacity * 2;
        vtx_guard_info_t *new_guards = (vtx_guard_info_t *)realloc(
            arr->guards, new_cap * sizeof(vtx_guard_info_t));
        if (!new_guards) return UINT32_MAX;
        arr->guards = new_guards;
        arr->capacity = new_cap;
    }
    uint32_t idx = arr->count;
    arr->guards[idx] = guard;
    arr->count++;
    return idx;
}

/* ========================================================================== */
/* Guard emission implementations                                              */
/* ========================================================================== */

void vtx_guard_emit_type_check(vtx_code_buffer_t *buf,
                                vtx_reg_t obj_reg,
                                vtx_typeid_t expected_typeid,
                                vtx_guard_info_t guard_info,
                                vtx_guard_array_t *guard_arr)
{
    guard_info.kind = VTX_GUARD_TYPE_CHECK;
    guard_info.native_pc_offset = vtx_code_buffer_position(buf);
    guard_info.expected_value = (uint64_t)expected_typeid;

    /*
     * The value in obj_reg is a tagged vtx_value_t (NaN-boxed heap pointer).
     * We need to:
     *   1. Extract the heap pointer from the tagged value
     *   2. Load the type_id field from the heap object header
     *   3. Compare against expected_typeid
     *   4. Jump to deopt if not equal
     *
     * Heap pointer extraction from NaN-boxed value:
     *   The heap pointer is stored as: bits[50:3] << 3
     *   raw = (value >> 3) & 0x0000FFFFFFFFFFFF
     *   ptr = raw << 3
     *
     * But this is expensive to compute in-line. Instead, we use the
     * simpler approach: the heap object pointer is directly accessible
     * via vtx_heap_ptr(), which extracts ptr = ((value >> 3) & mask) << 3.
     *
     * For the baseline JIT, we take a simpler approach: we keep the
     * raw heap pointer in the register (not the tagged value) for
     * objects that have already been null-checked and type-identified.
     * This means obj_reg holds a raw vtx_heap_object_t* pointer.
     *
     * If obj_reg still holds a tagged value, we need to untag it first.
     * For now, assume obj_reg holds a raw heap pointer (the codegen
     * ensures this by untagging after null checks).
     *
     * vtx_heap_object_t layout:
     *   offset 0: type_id (uint32_t)
     *   offset 4: gc_mark, gc_age, gc_pinned, gc_remembered (4 bytes)
     *   offset 8: size (uint32_t)
     *   ...
     *
     * So type_id is at offset 0 of the heap object.
     */

    /* cmp dword ptr [obj_reg + 0], expected_typeid */
    emit_cmp_mem_imm32(buf, obj_reg, 0, (uint32_t)expected_typeid);

    /* jne deopt_stub (will be patched later) */
    uint32_t disp_pos = emit_jcc32(buf, CC_NE);
    guard_info.branch_offset = (int32_t)disp_pos;

    /* Record the guard */
    vtx_guard_array_add(guard_arr, guard_info);
}

void vtx_guard_emit_null_check(vtx_code_buffer_t *buf,
                                vtx_reg_t obj_reg,
                                vtx_guard_info_t guard_info,
                                vtx_guard_array_t *guard_arr)
{
    guard_info.kind = VTX_GUARD_NULL_CHECK;
    guard_info.native_pc_offset = vtx_code_buffer_position(buf);

    /*
     * Null check: test if the register value is zero.
     * We test the full 64-bit value. For tagged values,
     * VTX_VALUE_NULL = VTX_NAN_BOX_HEADER | VTX_TAG_NULL, which is non-zero,
     * so we need to compare against the specific null representation.
     *
     * However, in the baseline JIT, we typically work with raw heap pointers
     * for object references (after untagging). A null pointer is simply 0.
     *
     * For tagged value null checks: compare against VTX_VALUE_NULL.
     * For raw pointer null checks: test reg, reg.
     *
     * We use test reg, reg for raw pointer null checks (the common case
     * after untagging). For tagged values, we compare against the null
     * constant.
     *
     * Default: test raw pointer.
     */

    /* test obj_reg, obj_reg */
    emit_test_reg_reg(buf, obj_reg);

    /* jz deopt_stub (jump if zero = null) */
    uint32_t disp_pos = emit_jcc32(buf, CC_E);
    guard_info.branch_offset = (int32_t)disp_pos;

    /* Record the guard */
    vtx_guard_array_add(guard_arr, guard_info);
}

void vtx_guard_emit_bounds_check(vtx_code_buffer_t *buf,
                                  vtx_reg_t index_reg,
                                  vtx_reg_t length_reg,
                                  vtx_guard_info_t guard_info,
                                  vtx_guard_array_t *guard_arr)
{
    guard_info.kind = VTX_GUARD_BOUNDS_CHECK;
    guard_info.native_pc_offset = vtx_code_buffer_position(buf);

    /*
     * Bounds check: 0 <= index < length.
     * We use unsigned comparison.
     *
     * Step 1: Check index >= 0 (unsigned). If CF=1, index is negative
     *         when interpreted as unsigned (i.e., it's very large).
     *         Actually, we use: cmp index, 0; jl deopt (signed less).
     *         Or simpler: use unsigned comparison with length.
     *
     * Simpler approach using a single unsigned comparison:
     *   cmp index, length
     *   jae deopt  (jump if above or equal, unsigned)
     *
     * This catches both index < 0 (wraps to large unsigned) and
     * index >= length. But we need the SMI value, which is the
     * actual integer index. The SMI value needs to be untagged first.
     *
     * In the baseline JIT, index_reg and length_reg hold raw int64_t
     * values (already untagged from SMI). We compare using:
     *   cmp index_reg, length_reg  (64-bit)
     *   jae deopt
     */

    /* cmp index_reg, length_reg */
    emit_rex_w(buf, index_reg, length_reg);
    vtx_code_buffer_emit_byte(buf, 0x39); /* CMP r/m64, r64 */
    vtx_code_buffer_emit_byte(buf, make_modrm(3, length_reg, index_reg));

    /* jae deopt_stub (unsigned above or equal → out of bounds) */
    uint32_t disp_pos = emit_jcc32(buf, CC_AE);
    guard_info.branch_offset = (int32_t)disp_pos;

    /* Record the guard */
    vtx_guard_array_add(guard_arr, guard_info);
}

void vtx_guard_emit_overflow_check(vtx_code_buffer_t *buf,
                                    vtx_guard_info_t guard_info,
                                    vtx_guard_array_t *guard_arr)
{
    guard_info.kind = VTX_GUARD_OVERFLOW;
    guard_info.native_pc_offset = vtx_code_buffer_position(buf);

    /*
     * Overflow check: after an arithmetic instruction (add, sub, mul),
     * check the overflow flag. The arithmetic instruction must have
     * already been emitted before calling this function.
     *
     * jo deopt_stub (jump if overflow flag is set)
     */
    uint32_t disp_pos = emit_jcc32(buf, CC_O);
    guard_info.branch_offset = (int32_t)disp_pos;

    /* Record the guard */
    vtx_guard_array_add(guard_arr, guard_info);
}

void vtx_guard_emit_div_zero_check(vtx_code_buffer_t *buf,
                                    vtx_reg_t divisor_reg,
                                    vtx_guard_info_t guard_info,
                                    vtx_guard_array_t *guard_arr)
{
    guard_info.kind = VTX_GUARD_DIV_ZERO;
    guard_info.native_pc_offset = vtx_code_buffer_position(buf);

    /*
     * Division by zero check: test if divisor is zero.
     * test divisor_reg, divisor_reg
     * jz deopt_stub
     */
    emit_test_reg_reg(buf, divisor_reg);

    uint32_t disp_pos = emit_jcc32(buf, CC_E);
    guard_info.branch_offset = (int32_t)disp_pos;

    /* Record the guard */
    vtx_guard_array_add(guard_arr, guard_info);
}
