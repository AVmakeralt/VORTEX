// test_superinstruction.cpp — Unit tests for the superinstruction
// pre-decode pass.
//
// Tests cover:
//   1. Basic fusion: LOAD_CONST_INT + IADD → LOAD_CONST_INT__IADD
//   2. Basic fusion: LOAD_LOCAL + LOAD_LOCAL → LOAD_LOCAL__LOAD_LOCAL
//   3. Basic fusion: LOAD_LOCAL + STORE_FIELD → LOAD_LOCAL__STORE_FIELD
//   4. No fusion across branch targets
//   5. Idempotency: running twice on already-fused code is a no-op
//   6. Branch target remapping: GOTO target stays correct after fusion
//   7. Edge cases: empty input, single instruction, no fusable pairs

#define typeid typeid_
extern "C" {
#include "runtime/bytecode.h"
#include "runtime/object.h"
}
#undef typeid

#include "vortex/superinstruction.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace vortex;

// Helper: build a vtx_bytecode_t (borrows code+consts; no ownership).
static vtx_bytecode_t make_bc(const std::vector<uint8_t>& code,
                               const std::vector<vtx_value_t>& consts) {
    vtx_bytecode_t bc;
    bc.code = const_cast<uint8_t*>(code.data());
    bc.length = code.size();
    bc.constant_pool = const_cast<vtx_value_t*>(consts.data());
    bc.constant_count = static_cast<uint32_t>(consts.size());
    bc.max_locals = 4;
    bc.max_stack = 8;
    return bc;
}

// Opcode constants for test bytecode construction.
namespace op {
constexpr uint8_t LOAD_LOCAL            = 2;
constexpr uint8_t STORE_LOCAL           = 3;
constexpr uint8_t LOAD_CONST_INT        = 6;
constexpr uint8_t IADD                  = 13;
constexpr uint8_t GOTO                  = 41;
constexpr uint8_t RETURN_VALUE          = 48;
constexpr uint8_t LOAD_CONST_INT__IADD    = 71;
constexpr uint8_t LOAD_LOCAL__LOAD_LOCAL = 72;
constexpr uint8_t LOAD_LOCAL__STORE_FIELD = 73;
}

static void test_basic_const_int_iadd() {
    // LOAD_CONST_INT 0 ; IADD ; RETURN_VALUE
    std::vector<uint8_t> code = {
        op::LOAD_CONST_INT, 0x00, 0x00,  // const 0
        op::IADD,
        op::RETURN_VALUE,
    };
    std::vector<vtx_value_t> consts = { vtx_make_smi(1) };
    vtx_bytecode_t bc = make_bc(code, consts);

    PreDecodeResult r;
    assert(predecode(&bc, &r) == 0);
    assert(r.length == 6);  // 1 (opcode) + 4 (operand) + 1 (RETURN_VALUE)
    assert(r.fused_count == 1);
    assert(r.code[0] == op::LOAD_CONST_INT__IADD);
    // Operand bytes: const_idx=0 (big-endian), unused=0
    assert(r.code[1] == 0x00 && r.code[2] == 0x00);  // const_idx
    assert(r.code[3] == 0x00 && r.code[4] == 0x00);  // unused
    assert(r.code[5] == op::RETURN_VALUE);
    std::free(r.code);
    printf("  [PASS] test_basic_const_int_iadd\n"); fflush(stdout);
}

static void test_basic_load_local_load_local() {
    std::vector<uint8_t> code = {
        op::LOAD_LOCAL, 0x00, 0x01,    // local 1
        op::LOAD_LOCAL, 0x00, 0x02,    // local 2
        op::RETURN_VALUE,
    };
    std::vector<vtx_value_t> consts;
    vtx_bytecode_t bc = make_bc(code, consts);

    PreDecodeResult r;
    assert(predecode(&bc, &r) == 0);
    assert(r.length == 6);  // 1 + 4 + 1
    assert(r.fused_count == 1);
    assert(r.code[0] == op::LOAD_LOCAL__LOAD_LOCAL);
    assert(r.code[1] == 0x00 && r.code[2] == 0x01);  // local a = 1
    assert(r.code[3] == 0x00 && r.code[4] == 0x02);  // local b = 2
    assert(r.code[5] == op::RETURN_VALUE);
    std::free(r.code);
    printf("  [PASS] test_basic_load_local_load_local\n"); fflush(stdout);
}

static void test_basic_load_local_store_field() {
    std::vector<uint8_t> code = {
        op::LOAD_LOCAL, 0x00, 0x03,    // local 3
        0x05,            0x00, 0x04,    // STORE_FIELD 4
        op::RETURN_VALUE,
    };
    std::vector<vtx_value_t> consts;
    vtx_bytecode_t bc = make_bc(code, consts);

    PreDecodeResult r;
    assert(predecode(&bc, &r) == 0);
    assert(r.length == 6);  // 1 + 4 + 1
    assert(r.fused_count == 1);
    assert(r.code[0] == op::LOAD_LOCAL__STORE_FIELD);
    assert(r.code[1] == 0x00 && r.code[2] == 0x03);  // local idx = 3
    assert(r.code[3] == 0x00 && r.code[4] == 0x04);  // field off = 4
    assert(r.code[5] == op::RETURN_VALUE);
    std::free(r.code);
    printf("  [PASS] test_basic_load_local_store_field\n"); fflush(stdout);
}

static void test_no_fusion_across_branch_target() {
    // Test: if the SECOND op of a pair is a branch target, fusion
    // must NOT happen (a branch would land in the middle of the
    // fused superinstruction).
    //
    // Layout:
    //   PC 0: GOTO 6                 (target = PC 6 = IADD)
    //   PC 3: LOAD_CONST_INT 0       (first op of pair)
    //   PC 6: IADD                   (second op — branch target!)
    //   PC 7: RETURN_VALUE
    //
    // The pair at (3,6) cannot fuse because PC 6 is a branch target.
    // After fusion, the IADD would vanish into the superinstruction,
    // and the GOTO would have nowhere valid to land.
    std::vector<uint8_t> code = {
        op::GOTO, 0x00, 0x06,           // PC=0: GOTO 6
        op::LOAD_CONST_INT, 0x00, 0x00, // PC=3
        op::IADD,                        // PC=6: branch target — block fusion
        op::RETURN_VALUE,                // PC=7
    };
    std::vector<vtx_value_t> consts = { vtx_make_smi(1) };
    vtx_bytecode_t bc = make_bc(code, consts);

    PreDecodeResult r;
    assert(predecode(&bc, &r) == 0);
    assert(r.fused_count == 0);  // pair blocked by branch target on second op
    assert(r.length == code.size());
    std::free(r.code);
    printf("  [PASS] test_no_fusion_across_branch_target\n"); fflush(stdout);
}

static void test_fusion_with_branch_to_first_op() {
    // Test: if the FIRST op of a pair is a branch target, fusion is
    // SAFE (the fused op has the same stack effect as the pair).
    //
    // Layout:
    //   PC 0: GOTO 3                 (target = PC 3 = LOAD_CONST_INT)
    //   PC 3: LOAD_CONST_INT 0       (first op — branch target)
    //   PC 6: IADD                   (second op)
    //   PC 7: RETURN_VALUE
    //
    // The pair at (3,6) should fuse. The GOTO target needs to be
    // remapped from old PC 3 to new PC 3 (no shift, since fusion
    // happens at the start).
    std::vector<uint8_t> code = {
        op::GOTO, 0x00, 0x03,           // PC=0: GOTO 3
        op::LOAD_CONST_INT, 0x00, 0x00, // PC=3: branch target, first of pair
        op::IADD,                        // PC=6: second of pair
        op::RETURN_VALUE,                // PC=7
    };
    std::vector<vtx_value_t> consts = { vtx_make_smi(1) };
    vtx_bytecode_t bc = make_bc(code, consts);

    PreDecodeResult r;
    assert(predecode(&bc, &r) == 0);
    assert(r.fused_count == 1);  // pair IS fusable when only first op is a target
    // After fusion: GOTO (3 bytes) + fused (5 bytes) + RETURN_VALUE (1 byte) = 9
    assert(r.length == 9);
    assert(r.code[0] == op::GOTO);
    uint16_t target = (r.code[1] << 8) | r.code[2];
    assert(target == 3);  // GOTO target was remapped (3 -> 3 since fusion didn't shift it)
    assert(r.code[3] == op::LOAD_CONST_INT__IADD);
    std::free(r.code);
    printf("  [PASS] test_fusion_with_branch_to_first_op\n"); fflush(stdout);
}

static void test_idempotency() {
    // Run predecode twice on already-fused code — should be a no-op.
    std::vector<uint8_t> code = {
        op::LOAD_CONST_INT, 0x00, 0x00,
        op::IADD,
        op::RETURN_VALUE,
    };
    std::vector<vtx_value_t> consts = { vtx_make_smi(1) };
    vtx_bytecode_t bc = make_bc(code, consts);

    PreDecodeResult r1;
    assert(predecode(&bc, &r1) == 0);
    assert(r1.fused_count == 1);
    assert(r1.code[0] == op::LOAD_CONST_INT__IADD);

    // Re-run on the fused code. Keep the vector alive across the call
    // (the original test used a temporary that was destroyed before
    // predecode read it — leading to a dangling-pointer read).
    std::vector<uint8_t> fused_code(r1.code, r1.code + r1.length);
    vtx_bytecode_t bc2 = make_bc(fused_code, consts);
    PreDecodeResult r2;
    assert(predecode(&bc2, &r2) == 0);
    assert(r2.fused_count == 0);  // already fused — no more pairs
    assert(r2.length == r1.length);
    assert(r2.code[0] == op::LOAD_CONST_INT__IADD);
    std::free(r1.code);
    std::free(r2.code);
    printf("  [PASS] test_idempotency\n"); fflush(stdout);
}

static void test_branch_target_remapping() {
    // GOTO 9 ; LOAD_CONST_INT 0 ; IADD ; RETURN_VALUE
    // PC 0: GOTO 9 (target = start of IADD)
    // PC 3: LOAD_CONST_INT 0
    // PC 6: IADD          <- branch target! So the pair (3,6) is BLOCKED.
    // Actually for remapping test, we need a case where fusion shifts
    // a non-target instruction and a branch needs remapping.
    //
    // Setup:
    //   PC 0: LOAD_CONST_INT 0
    //   PC 3: IADD                 <- pair (0,3) fuses
    //   PC 4: GOTO 8               <- target = PC 8
    //   PC 7: LOAD_CONST_INT 0     <- branch target after fusion?
    //   PC 10: RETURN_VALUE
    //
    // Hmm let me think — after fusion:
    //   new PC 0: LOAD_CONST_INT__IADD (5 bytes)
    //   new PC 5: GOTO ?  (target was PC 8, which after fusion is at PC 5+1+2=8? no, fusion shifts it)
    //
    // Let me build a simpler case:
    //   PC 0: LOAD_CONST_INT 0; IADD   (fuses to LOAD_CONST_INT__IADD at new PC 0, 5 bytes)
    //   PC 4: GOTO 7                   (target = old PC 7, which is new PC 5)
    //   PC 7: LOAD_CONST_INT 1
    //   PC 10: RETURN_VALUE
    //
    // After fusion:
    //   new PC 0: LOAD_CONST_INT__IADD (5 bytes)
    //   new PC 5: GOTO 5                (target remapped: 7 -> 5)
    //   new PC 8: LOAD_CONST_INT 1
    //   new PC 11: RETURN_VALUE
    std::vector<uint8_t> code = {
        op::LOAD_CONST_INT, 0x00, 0x00,  // PC 0
        op::IADD,                          // PC 3
        op::GOTO, 0x00, 0x07,             // PC 4: GOTO old PC 7
        op::LOAD_CONST_INT, 0x00, 0x01,  // PC 7
        op::RETURN_VALUE,                  // PC 10
    };
    std::vector<vtx_value_t> consts = { vtx_make_smi(1), vtx_make_smi(2) };
    vtx_bytecode_t bc = make_bc(code, consts);

    PreDecodeResult r;
    assert(predecode(&bc, &r) == 0);
    assert(r.fused_count == 1);
    assert(r.code[0] == op::LOAD_CONST_INT__IADD);
    // new PC 5: GOTO with remapped target
    assert(r.code[5] == op::GOTO);
    uint16_t new_target = (r.code[6] << 8) | r.code[7];
    // old PC 7 maps to new PC 5 (LOAD_CONST_INT__IADD is 5 bytes, so
    // the next instruction GOTO is at new PC 5; the LOAD_CONST_INT 1
    // after GOTO is at new PC 5 + 3 = 8).
    assert(new_target == 8);
    std::free(r.code);
    printf("  [PASS] test_branch_target_remapping\n"); fflush(stdout);
}

static void test_empty_and_single() {
    // Empty input.
    std::vector<uint8_t> empty_code;
    std::vector<vtx_value_t> empty_consts;
    vtx_bytecode_t empty_bc = make_bc(empty_code, empty_consts);
    PreDecodeResult r;
    assert(predecode(&empty_bc, &r) == 0);
    assert(r.length == 0);
    assert(r.fused_count == 0);
    std::free(r.code);

    // Single instruction — no pair possible.
    std::vector<uint8_t> code = { op::RETURN_VALUE };
    vtx_bytecode_t bc = make_bc(code, empty_consts);
    assert(predecode(&bc, &r) == 0);
    assert(r.length == 1);
    assert(r.fused_count == 0);
    std::free(r.code);
    printf("  [PASS] test_empty_and_single\n"); fflush(stdout);
}

int main() {
    printf("=== Superinstruction pre-decode tests ===\n"); fflush(stdout);
    test_basic_const_int_iadd();
    test_basic_load_local_load_local();
    test_basic_load_local_store_field();
    test_no_fusion_across_branch_target();
    test_fusion_with_branch_to_first_op();
    test_idempotency();
    test_branch_target_remapping();
    test_empty_and_single();
    printf("=== All tests passed ===\n"); fflush(stdout);
    return 0;
}
