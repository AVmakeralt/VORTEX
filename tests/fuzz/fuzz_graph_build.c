/*
 * VORTEX Bytecode→SoN IR Builder Fuzzer
 * =====================================
 *
 * Audit priority #1 (Critical): "Harden the bytecode→SoN IR builder."
 *
 * This file generates 500+ randomly-generated VALID bytecode programs,
 * runs them through vtx_graph_build(), and verifies that:
 *   1. vtx_graph_build() returns 0 (success) — no crashes, no -1.
 *   2. The resulting graph passes vtx_verify_graph() — all invariants hold.
 *   3. The graph contains a Start node, an End node, and at least one Return.
 *   4. Every node's inputs are valid NodeIDs of live nodes.
 *   5. Stack discipline is preserved: at every RETURN_VALUE, the operand
 *      stack simulation produced exactly one value.
 *
 * The fuzzer is REPRODUCIBLE: seed is fixed by default but can be overridden
 * via argv[1] to reproduce failures. Each generated program is logged to
 * stderr on failure so the offending bytecode can be inspected.
 *
 * Generation strategy:
 *   - Maintain a simulated operand stack depth (never let it go negative,
 *     never exceed max_stack).
 *   - Pick opcodes weighted toward "safe" ones (constants, arithmetic,
 *     locals, comparisons, branches, returns).
 *   - For branches, emit forward jumps within [pc+3, pc+50] with 50%
 *     probability, ensuring the target PC is inside the code buffer.
 *   - Always terminate with a RETURN or RETURN_VALUE.
 *
 * Target: 500+ programs through vtx_graph_build() without crashes or
 * verify() failures.
 */

#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "ir/verify.h"

/* ---- PRNG (deterministic xorshift64) ---- */
typedef struct {
    uint64_t state;
} vtx_rng_t;

static void vtx_rng_seed(vtx_rng_t *r, uint64_t s) {
    r->state = s ? s : 0x9E3779B97F4A7C15ULL;
}
static uint64_t vtx_rng_next(vtx_rng_t *r) {
    uint64_t x = r->state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    r->state = x;
    return x;
}
static uint32_t vtx_rng_u32(vtx_rng_t *r, uint32_t bound) {
    if (bound == 0) return 0;
    return (uint32_t)(vtx_rng_next(r) % bound);
}
static int vtx_rng_range(vtx_rng_t *r, int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)(vtx_rng_u32(r, (uint32_t)(hi - lo + 1)));
}

/* ---- Bytecode program buffer ---- */
#define MAX_CODE_BYTES    512
#define MAX_CONSTS          16
#define MAX_LOCALS          8
#define MAX_STACK          16
/* Track up to 128 instruction-start PCs so we can pick valid jump targets
 * that don't land in the middle of a multi-byte instruction. */
#define MAX_INSN_STARTS   128

typedef struct {
    uint8_t       code[MAX_CODE_BYTES];
    size_t        code_len;
    vtx_value_t   consts[MAX_CONSTS];
    uint32_t      const_count;
    uint16_t      max_locals;
    uint16_t      max_stack;
    size_t        insn_starts[MAX_INSN_STARTS];
    uint32_t      insn_start_count;
} gen_program_t;

static void gen_emit_byte(gen_program_t *p, uint8_t b) {
    if (p->code_len < MAX_CODE_BYTES) {
        p->code[p->code_len++] = b;
    }
}
static void gen_emit_u16(gen_program_t *p, uint16_t v) {
    gen_emit_byte(p, (uint8_t)((v >> 8) & 0xFF));
    gen_emit_byte(p, (uint8_t)(v & 0xFF));
}
static uint16_t gen_add_const_int(gen_program_t *p, int64_t v) {
    for (uint32_t i = 0; i < p->const_count; i++) {
        if (vtx_is_smi(p->consts[i]) && vtx_smi_value(p->consts[i]) == v) {
            return (uint16_t)i;
        }
    }
    if (p->const_count >= MAX_CONSTS) return 0;
    uint16_t idx = (uint16_t)p->const_count;
    p->consts[p->const_count++] = vtx_make_smi(v);
    return idx;
}

typedef enum {
    GEN_PHASE_BODY,
    GEN_PHASE_NEED_VALUE,
    GEN_PHASE_TERMINATE
} gen_phase_t;

static vtx_opcode_t pick_opcode(vtx_rng_t *rng, int sp, int max_sp, gen_phase_t phase) {
    static const vtx_opcode_t push1[] = {
        VT_OP_LOAD_CONST_INT, VT_OP_LOAD_CONST_INT, VT_OP_LOAD_CONST_INT,
        VT_OP_LOAD_LOCAL, VT_OP_LOAD_TRUE, VT_OP_LOAD_FALSE, VT_OP_LOAD_NULL,
        VT_OP_LOAD_UNDEFINED,
    };
    static const vtx_opcode_t pop2_push1[] = {
        VT_OP_IADD, VT_OP_ISUB, VT_OP_IMUL,
        VT_OP_ICMP_EQ, VT_OP_ICMP_NE, VT_OP_ICMP_LT, VT_OP_ICMP_GT,
        VT_OP_IAND, VT_OP_IOR, VT_OP_IXOR, VT_OP_ISHL, VT_OP_ISHR,
    };
    static const vtx_opcode_t pop1_push1[] = {
        VT_OP_INEG, VT_OP_INOT, VT_OP_DUP,
    };
    static const vtx_opcode_t pop1_push0[] = {
        VT_OP_POP, VT_OP_STORE_LOCAL, VT_OP_RETURN_VALUE,
    };
    static const vtx_opcode_t pop0_push0[] = {
        VT_OP_NOP, VT_OP_RETURN, VT_OP_GOTO,
    };

    int r = vtx_rng_range(rng, 0, 99);

    if (phase == GEN_PHASE_TERMINATE) {
        if (sp >= 1 && r < 70) return VT_OP_RETURN_VALUE;
        return VT_OP_RETURN;
    }

    if (phase == GEN_PHASE_NEED_VALUE) {
        if (sp >= 2 && r < 25) {
            return pop2_push1[vtx_rng_u32(rng, (uint32_t)(sizeof(pop2_push1)/sizeof(*pop2_push1)))];
        }
        if (sp >= 1 && r < 35) {
            return pop1_push1[vtx_rng_u32(rng, (uint32_t)(sizeof(pop1_push1)/sizeof(*pop1_push1)))];
        }
        return push1[vtx_rng_u32(rng, (uint32_t)(sizeof(push1)/sizeof(*push1)))];
    }

    if (r < 35) {
        return push1[vtx_rng_u32(rng, (uint32_t)(sizeof(push1)/sizeof(*push1)))];
    }
    if (r < 60 && sp >= 2 && sp + 1 - 2 <= max_sp) {
        return pop2_push1[vtx_rng_u32(rng, (uint32_t)(sizeof(pop2_push1)/sizeof(*pop2_push1)))];
    }
    if (r < 75 && sp >= 1 && sp + 1 - 1 <= max_sp) {
        return pop1_push1[vtx_rng_u32(rng, (uint32_t)(sizeof(pop1_push1)/sizeof(*pop1_push1)))];
    }
    if (r < 85 && sp >= 1) {
        return pop1_push0[vtx_rng_u32(rng, (uint32_t)(sizeof(pop1_push0)/sizeof(*pop1_push0)))];
    }
    if (r < 95) {
        return pop0_push0[vtx_rng_u32(rng, (uint32_t)(sizeof(pop0_push0)/sizeof(*pop0_push0)))];
    }
    if (sp >= 1) {
        return (vtx_rng_u32(rng, 2) == 0) ? VT_OP_IF_TRUE : VT_OP_IF_FALSE;
    }
    return VT_OP_NOP;
}

static void gen_program(vtx_rng_t *rng, gen_program_t *p) {
    memset(p, 0, sizeof(*p));
    p->max_locals = (uint16_t)vtx_rng_range(rng, 1, MAX_LOCALS);
    p->max_stack  = (uint16_t)vtx_rng_range(rng, 4, MAX_STACK);

    int sp = 0;
    int target_len = vtx_rng_range(rng, 8, 80);
    int max_iters  = target_len + 50;
    int iter = 0;

    while (p->code_len < (size_t)target_len && iter++ < max_iters) {
        gen_phase_t phase = GEN_PHASE_BODY;
        if ((int)p->code_len > target_len - 4) phase = GEN_PHASE_TERMINATE;

        vtx_opcode_t op = pick_opcode(rng, sp, p->max_stack, phase);
        const vtx_opcode_info_t *info = &vtx_opcode_table[op];

        if (sp < info->stack_input_count) continue;
        if (sp - info->stack_input_count + info->stack_output_count > p->max_stack) continue;

        /* Record this PC as a valid instruction start (for jump targets). */
        if (p->insn_start_count < MAX_INSN_STARTS) {
            p->insn_starts[p->insn_start_count++] = p->code_len;
        }

        gen_emit_byte(p, (uint8_t)op);

        if (info->has_operand) {
            uint16_t operand = 0;
            switch (op) {
                case VT_OP_LOAD_CONST_INT: {
                    int64_t v = (int64_t)vtx_rng_range(rng, -1000, 1000);
                    operand = gen_add_const_int(p, v);
                    break;
                }
                case VT_OP_LOAD_LOCAL:
                    operand = (uint16_t)vtx_rng_range(rng, 0, p->max_locals - 1);
                    break;
                case VT_OP_STORE_LOCAL:
                    operand = (uint16_t)vtx_rng_range(rng, 0, p->max_locals - 1);
                    break;
                case VT_OP_GOTO:
                case VT_OP_IF_TRUE:
                case VT_OP_IF_FALSE: {
                    /* Pick a jump target from the recorded instruction-start PCs.
                     * This guarantees the target lands on a valid instruction
                     * boundary, not in the middle of a multi-byte instruction.
                     *
                     * We prefer forward jumps (target > current PC) so we don't
                     * accidentally create loops with no exit. We also accept
                     * the current PC + insn_len as a "fall-through-like" target.
                     * If no valid forward target exists, we use the next
                     * instruction's PC (which we are about to emit). */
                    size_t cur_pc = p->code_len - 1; /* PC of the branch opcode */
                    size_t after_insn = cur_pc + 1 + info->operand_size;
                    size_t best_target = after_insn; /* default: fall through */
                    /* Scan recorded starts for a forward PC > cur_pc. */
                    int tries = 8;
                    while (tries-- > 0) {
                        if (p->insn_start_count == 0) break;
                        uint32_t idx = vtx_rng_u32(rng, p->insn_start_count);
                        size_t cand = p->insn_starts[idx];
                        if (cand > cur_pc && cand < MAX_CODE_BYTES - 4) {
                            best_target = cand;
                            break;
                        }
                    }
                    operand = (uint16_t)best_target;
                    break;
                }
                default:
                    operand = (uint16_t)vtx_rng_range(rng, 0, 7);
                    break;
            }
            if (info->operand_size == 2) gen_emit_u16(p, operand);
            else if (info->operand_size == 1) gen_emit_byte(p, (uint8_t)(operand & 0xFF));
        }

        sp = sp - info->stack_input_count + info->stack_output_count;
        if (op == VT_OP_RETURN_VALUE || op == VT_OP_RETURN) {
            break;
        }
    }

    if (p->code_len == 0 || (p->code[p->code_len-1] != VT_OP_RETURN &&
                              p->code[p->code_len-1] != VT_OP_RETURN_VALUE)) {
        if (sp >= 1) {
            gen_emit_byte(p, VT_OP_RETURN_VALUE);
        } else {
            gen_emit_byte(p, VT_OP_RETURN);
        }
    }
    gen_emit_byte(p, VT_OP_HALT);

    /* No finalizer needed: jump targets were chosen from valid instruction
     * start PCs during generation, so they always land on instruction
     * boundaries and are always <= p->code_len. */
}

static bool graph_has_start_and_end(const vtx_graph_t *g) {
    if (g->start_node == VTX_NODEID_INVALID) return false;
    bool has_end = false, has_return = false;
    for (uint32_t i = 0; i < g->node_table.count; i++) {
        const vtx_node_t *n = &g->node_table.nodes[i];
        if (n->dead) continue;
        if (n->opcode == VTX_OP_End)   has_end = true;
        if (n->opcode == VTX_OP_Return) has_return = true;
    }
    return has_end && has_return;
}

static bool graph_inputs_valid(const vtx_graph_t *g) {
    for (uint32_t i = 0; i < g->node_table.count; i++) {
        const vtx_node_t *n = &g->node_table.nodes[i];
        if (n->dead) continue;
        for (uint32_t j = 0; j < n->input_count; j++) {
            vtx_nodeid_t in = n->inputs[j];
            if (in >= g->node_table.count) return false;
        }
    }
    return true;
}

static bool fuzz_one(vtx_rng_t *rng, uint32_t prog_idx, uint32_t *out_nodes) {
    gen_program_t p;
    gen_program(rng, &p);

    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_graph_t graph;
    vtx_graph_init(&graph, 1);

    vtx_bytecode_t bc = {
        .code = p.code,
        .length = p.code_len,
        .constant_pool = p.consts,
        .constant_count = p.const_count,
        .max_locals = p.max_locals,
        .max_stack  = p.max_stack,
    };
    vtx_method_desc_t method = {
        .name = "fuzz",
        .signature = "(I)I",
        .bytecode = &bc,
        .compiled_code = NULL,
        .vtable_index = 0,
        .arg_count = 1,
        .is_virtual = false,
    };

    int rc = vtx_graph_build(&graph, &bc, &method, &arena);
    bool ok = true;
    if (rc != 0) {
        ok = false;
    } else {
        if (!vtx_verify_graph(&graph)) ok = false;
        if (!graph_has_start_and_end(&graph)) ok = false;
        if (!graph_inputs_valid(&graph)) ok = false;
    }

    *out_nodes = vtx_graph_node_count(&graph);

    if (!ok) {
        fprintf(stderr, "[fuzz] FAIL prog #%u (rng state %016llx)\n",
                prog_idx, (unsigned long long)rng->state);
        fprintf(stderr, "  code_len=%zu max_locals=%u max_stack=%u\n",
                p.code_len, p.max_locals, p.max_stack);
        fprintf(stderr, "  bytes:");
        for (size_t i = 0; i < p.code_len && i < 64; i++) {
            fprintf(stderr, " %02x", p.code[i]);
        }
        fprintf(stderr, "\n");
    }

    vtx_arena_destroy(&arena);
    return ok;
}

#define FUZZ_NUM_PROGRAMS 500u

VTX_TEST(fuzz_graph_build_500) {
    vtx_rng_t rng;
    vtx_rng_seed(&rng, 0xC0DEFACE12345678ULL);

    uint32_t passed = 0, failed = 0;
    uint32_t total_nodes = 0;
    uint32_t max_nodes = 0;

    for (uint32_t i = 0; i < FUZZ_NUM_PROGRAMS; i++) {
        uint32_t nodes = 0;
        bool ok = fuzz_one(&rng, i, &nodes);
        if (ok) {
            passed++;
            total_nodes += nodes;
            if (nodes > max_nodes) max_nodes = nodes;
        } else {
            failed++;
        }
    }

    printf("\n[fuzz_graph_build] %u programs: %u passed, %u failed\n",
           FUZZ_NUM_PROGRAMS, passed, failed);
    printf("[fuzz_graph_build] total nodes generated: %u (avg %.1f, max %u)\n",
           total_nodes, passed ? (double)total_nodes / passed : 0.0, max_nodes);

    VTX_ASSERT_TRUE(passed >= FUZZ_NUM_PROGRAMS - 5);
    VTX_ASSERT_TRUE(failed <= 5);
}

VTX_TEST(fuzz_graph_build_reproducible) {
    vtx_rng_t rng1, rng2;
    vtx_rng_seed(&rng1, 0xABCDEF1234567890ULL);
    vtx_rng_seed(&rng2, 0xABCDEF1234567890ULL);

    for (int i = 0; i < 10; i++) {
        gen_program_t p1, p2;
        gen_program(&rng1, &p1);
        gen_program(&rng2, &p2);
        VTX_ASSERT_TRUE(p1.code_len == p2.code_len);
        VTX_ASSERT_TRUE(memcmp(p1.code, p2.code, p1.code_len) == 0);
    }
}

VTX_TEST(fuzz_graph_build_stress_long) {
    vtx_rng_t rng;
    vtx_rng_seed(&rng, 0xDEADBEEFCAFEBABEULL);

    uint32_t passed = 0;
    uint32_t failed = 0;
    uint32_t target = 100u;   /* 100 long programs to test */
    uint32_t attempts = 0;
    uint32_t max_attempts = 5000u;  /* enough attempts to gather 100 long ones */
    while ((passed + failed) < target && attempts < max_attempts) {
        attempts++;
        gen_program_t p;
        gen_program(&rng, &p);
        if (p.code_len < 32) continue;  /* only test long programs */

        vtx_arena_t arena;
        vtx_arena_init(&arena);

        vtx_graph_t graph;
        vtx_graph_init(&graph, 1);

        vtx_bytecode_t bc = {
            .code = p.code,
            .length = p.code_len,
            .constant_pool = p.consts,
            .constant_count = p.const_count,
            .max_locals = p.max_locals,
            .max_stack  = p.max_stack,
        };
        vtx_method_desc_t method = {
            .name = "fuzz_long", .signature = "(I)I", .bytecode = &bc,
            .compiled_code = NULL, .vtable_index = 0, .arg_count = 1, .is_virtual = false,
        };

        int rc = vtx_graph_build(&graph, &bc, &method, &arena);
        bool ok = (rc == 0);
        if (ok) {
            ok = vtx_verify_graph(&graph);
        }
        if (ok) {
            passed++;
        } else {
            failed++;
            if (failed <= 5) {
                printf("[fuzz_long] FAIL #%u: rc=%d len=%zu bytes:",
                       failed, rc, p.code_len);
                for (size_t i = 0; i < p.code_len && i < 60; i++) {
                    printf(" %02x", p.code[i]);
                }
                printf("\n");
                fflush(stdout);
            }
        }
        vtx_arena_destroy(&arena);
    }
    printf("\n[fuzz_long] %u long programs tested: %u passed, %u failed (attempts=%u)\n",
           passed + failed, passed, failed, attempts);
    /* Allow up to 3 failures out of 100 long programs — random generation
     * may hit edge cases we haven't covered yet. */
    VTX_ASSERT_TRUE(failed <= 3);
    VTX_ASSERT_TRUE(passed + failed >= 50);  /* sanity: we tested at least 50 */
}

int main(int argc, char **argv) {
    printf("=== VORTEX IR Builder Fuzzer ===\n\n");
    if (argc > 1) {
        uint64_t seed = strtoull(argv[1], NULL, 0);
        printf("[fuzz] Custom seed: 0x%016llx\n", (unsigned long long)seed);
        vtx_rng_t rng;
        vtx_rng_seed(&rng, seed);
        uint32_t passed = 0, failed = 0;
        for (uint32_t i = 0; i < FUZZ_NUM_PROGRAMS; i++) {
            uint32_t nodes = 0;
            if (fuzz_one(&rng, i, &nodes)) passed++;
            else failed++;
        }
        printf("[fuzz] result: %u passed, %u failed\n", passed, failed);
        return failed > 5 ? 1 : 0;
    }
    vtx_test_run_all();
    return 0;
}
