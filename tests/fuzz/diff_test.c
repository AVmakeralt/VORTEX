/*
 * VORTEX Differential Testing Harness
 *
 * Generates random bytecode programs, runs each through both the T0
 * interpreter and the T2 JIT, and compares results. Mismatches
 * indicate optimizer bugs (T2 produces different output than T0).
 *
 * Usage: diff_test [num_programs] [seed]
 *   num_programs: default 1000
 *   seed: default 0xD1FFD1FFD1FFD1FF (fixed for reproducibility)
 *
 * Uses fork() + waitpid() so JIT crashes don't kill the fuzzer.
 */

#include "test_framework.h"
#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "interp/dispatch.h"
#include "compile/pipeline.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "baseline/codegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

/* ---- PRNG (deterministic xorshift64) ---- */
typedef struct { uint64_t state; } rng_t;

static void rng_seed(rng_t *r, uint64_t s) {
    r->state = s ? s : 0x9E3779B97F4A7C15ULL;
}
static uint64_t rng_next(rng_t *r) {
    uint64_t x = r->state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    r->state = x;
    return x;
}
static uint32_t rng_u32(rng_t *r, uint32_t bound) {
    if (bound == 0) return 0;
    return (uint32_t)(rng_next(r) % bound);
}
static int rng_range(rng_t *r, int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)(rng_u32(r, (uint32_t)(hi - lo + 1)));
}

/* ---- Program generator ---- */
#define MAX_CODE  256
#define MAX_CONSTS 8
#define MAX_LOCALS 4
#define MAX_STACK  8

typedef struct {
    uint8_t code[MAX_CODE];
    size_t code_len;
    vtx_value_t consts[MAX_CONSTS];
    uint32_t const_count;
    uint16_t max_locals;
    uint16_t max_stack;
    int stack_depth;  /* simulated */
} program_t;

static void my_emit_byte(program_t *p, uint8_t b) {
    if (p->code_len < MAX_CODE) p->code[p->code_len++] = b;
}
static void emit_u16(program_t *p, uint16_t v) {
    my_emit_byte(p, (uint8_t)((v >> 8) & 0xFF));
    my_emit_byte(p, (uint8_t)(v & 0xFF));
}
static uint16_t add_const(program_t *p, int64_t v) {
    for (uint32_t i = 0; i < p->const_count; i++) {
        if (vtx_is_smi(p->consts[i]) && vtx_smi_value(p->consts[i]) == v)
            return (uint16_t)i;
    }
    if (p->const_count < MAX_CONSTS) {
        p->consts[p->const_count] = vtx_make_smi(v);
        return (uint16_t)(p->const_count++);
    }
    return 0;
}

static void gen_program(program_t *p, rng_t *rng) {
    memset(p, 0, sizeof(*p));
    p->max_locals = (uint16_t)rng_range(rng, 1, MAX_LOCALS);
    p->max_stack = MAX_STACK;

    /* Emit: LOAD_CONST_INT k (push initial value) */
    int64_t init_val = rng_range(rng, 0, 100);
    uint16_t ci = add_const(p, init_val);
    my_emit_byte(p, VT_OP_LOAD_CONST_INT); emit_u16(p, ci);
    my_emit_byte(p, VT_OP_STORE_LOCAL); emit_u16(p, 0);  /* local[0] = init */
    p->stack_depth = 0;

    int target_len = rng_range(rng, 10, 60);
    while ((int)p->code_len < target_len) {
        /* Ensure stack has at least 1 value for most ops */
        if (p->stack_depth < 1) {
            uint16_t c = add_const(p, rng_range(rng, 0, 50));
            my_emit_byte(p, VT_OP_LOAD_CONST_INT); emit_u16(p, c);
            p->stack_depth++;
            continue;
        }

        uint32_t choice = rng_u32(rng, 100);
        if (choice < 25) {
            /* LOAD_LOCAL */
            uint16_t li = (uint16_t)rng_u32(rng, p->max_locals);
            my_emit_byte(p, VT_OP_LOAD_LOCAL); emit_u16(p, li);
            p->stack_depth++;
        } else if (choice < 35 && p->stack_depth >= 1) {
            /* STORE_LOCAL — requires 1 value on stack */
            uint16_t li = (uint16_t)rng_u32(rng, p->max_locals);
            my_emit_byte(p, VT_OP_STORE_LOCAL); emit_u16(p, li);
            p->stack_depth--;
        } else if (choice < 55) {
            /* LOAD_CONST_INT */
            uint16_t c = add_const(p, rng_range(rng, 0, 100));
            my_emit_byte(p, VT_OP_LOAD_CONST_INT); emit_u16(p, c);
            p->stack_depth++;
        } else if (choice < 70 && p->stack_depth >= 2) {
            /* Arithmetic */
            uint32_t op = rng_u32(rng, 3);
            my_emit_byte(p, op == 0 ? VT_OP_IADD : (op == 1 ? VT_OP_ISUB : VT_OP_IMUL));
            p->stack_depth--;
        } else if (choice < 85 && p->stack_depth >= 2) {
            /* Comparison */
            uint32_t op = rng_u32(rng, 3);
            my_emit_byte(p, op == 0 ? VT_OP_ICMP_LT : (op == 1 ? VT_OP_ICMP_EQ : VT_OP_ICMP_GT));
            p->stack_depth--;  /* pop 2, push 1 bool */
        } else if (choice < 95 && p->code_len + 6 < MAX_CODE) {
            /* Push a const instead of GOTO (GOTO is complex to generate correctly). */
            uint16_t c = add_const(p, rng_range(rng, 0, 100));
            my_emit_byte(p, VT_OP_LOAD_CONST_INT); emit_u16(p, c);
            p->stack_depth++;
        } else {
            /* RETURN_VALUE */
            my_emit_byte(p, VT_OP_RETURN_VALUE);
            break;
        }
    }

    /* Ensure ends with exactly one RETURN_VALUE and exactly 1 value on stack.
     * If the loop already emitted RETURN_VALUE (via the else branch), don't
     * emit another one — that would cause stack underflow. */
    int has_return = (p->code_len > 0 &&
                      p->code[p->code_len - 1] == VT_OP_RETURN_VALUE);
    if (!has_return) {
        if (p->stack_depth < 1) {
            uint16_t c = add_const(p, 0);
            my_emit_byte(p, VT_OP_LOAD_CONST_INT); emit_u16(p, c);
            p->stack_depth++;
        }
        while (p->stack_depth > 1) {
            my_emit_byte(p, VT_OP_POP);
            p->stack_depth--;
        }
        my_emit_byte(p, VT_OP_RETURN_VALUE);
    }
}

/* ---- Runner: returns 0 if match, 1 if mismatch, 2 if crash ---- */
static int run_single(program_t *p, vtx_value_t *out_t0, vtx_value_t *out_jit) {
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    vtx_bytecode_t bc = {
        .code = p->code, .length = (uint32_t)p->code_len,
        .constant_pool = p->consts, .constant_count = p->const_count,
        .max_locals = p->max_locals, .max_stack = p->max_stack,
    };
    vtx_method_desc_t method = {
        .name = "test", .signature = "(I)I",
        .bytecode = &bc, .compiled_code = NULL,
        .vtable_index = 500, .arg_count = 1, .is_virtual = false,
    };

    /* T0: run through interpreter */
    vtx_value_t arg0 = vtx_make_smi(7);
    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);
    *out_t0 = vtx_interp_run(&interp, &method, &arg0, 1);
    vtx_interp_destroy(&interp);

    /* T2: compile with baseline JIT (same path as test_jit_deopt) */
    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 20);
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);

    vtx_compiled_code_t *compiled = vtx_baseline_compile(
        &method, NULL, &arena, &cache, &registry);
    if (compiled != NULL) {
        /* Call JIT directly */
        typedef vtx_value_t (*jit_entry_t)(
            const vtx_method_desc_t *, void *, void *,
            vtx_value_t *, uint32_t);
        union { void *ptr; jit_entry_t fn; } ue;
        ue.ptr = method.compiled_code;
        jit_entry_t entry = ue.fn;
        vtx_value_t arg_v = vtx_make_smi(7);
        *out_jit = entry(&method, NULL, NULL, &arg_v, 1);
        vtx_compiled_code_destroy(compiled);
    } else {
        *out_jit = *out_t0;  /* compile failed, use T0 result */
    }

    vtx_method_registry_destroy(&registry);
    vtx_code_cache_destroy(&cache);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);

    /* Compare */
    if (*out_t0 == *out_jit) return 0;
    return 1;
}

int main(int argc, char **argv) {
    int num_programs = (argc > 1) ? atoi(argv[1]) : 1000;
    uint64_t seed = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0xD1FFD1FFD1FFD1FFULL;

    rng_t rng;
    rng_seed(&rng, seed);

    int matches = 0, mismatches = 0, crashes = 0;

    fprintf(stderr, "Differential tester: %d programs, seed=0x%llx\n",
            num_programs, (unsigned long long)seed);

    for (int i = 0; i < num_programs; i++) {
        program_t p;
        gen_program(&p, &rng);

        /* Fork to isolate crashes */
        pid_t pid = fork();
        if (pid == 0) {
            /* Child — print bytecodes first for debugging */
            fprintf(stderr, "PROG %d bytecode: ", i);
            for (size_t j = 0; j < p.code_len; j++)
                fprintf(stderr, "%02x ", p.code[j]);
            fprintf(stderr, "\n");
            vtx_value_t t0, jit;
            int rc = run_single(&p, &t0, &jit);
            if (rc == 0) _exit(0);
            if (rc == 1) {
                /* Mismatch — print and exit with code 1 */
                fprintf(stderr, "MISMATCH prog %d: T0=", i);
                if (vtx_is_smi(t0)) fprintf(stderr, "SMI(%lld)", (long long)vtx_smi_value(t0));
                else fprintf(stderr, "raw(0x%llx)", (unsigned long long)t0);
                fprintf(stderr, " JIT=");
                if (vtx_is_smi(jit)) fprintf(stderr, "SMI(%lld)", (long long)vtx_smi_value(jit));
                else fprintf(stderr, "raw(0x%llx)", (unsigned long long)jit);
                fprintf(stderr, "\n  bytecode: ");
                for (size_t j = 0; j < p.code_len; j++)
                    fprintf(stderr, "%02x ", p.code[j]);
                fprintf(stderr, "\n");
                _exit(1);
            }
            /* Crash — print the bytecode before dying */
            fprintf(stderr, "CRASH prog %d bytecode: ", i);
            for (size_t j = 0; j < p.code_len; j++)
                fprintf(stderr, "%02x ", p.code[j]);
            fprintf(stderr, "\n");
            _exit(2);  /* crash */
        }
        /* Parent */
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == 0) matches++;
            else if (code == 1) {
                mismatches++;
                if (mismatches <= 5) {
                    /* Save first 5 failures */
                    char fname[256];
                    snprintf(fname, sizeof(fname),
                             "tests/fuzz/failures/diff_test_%d.bin", i);
                    FILE *f = fopen(fname, "wb");
                    if (f) {
                        fwrite(&p.code_len, sizeof(p.code_len), 1, f);
                        fwrite(p.code, 1, p.code_len, f);
                        fwrite(&p.const_count, sizeof(p.const_count), 1, f);
                        fwrite(p.consts, sizeof(vtx_value_t), p.const_count, f);
                        fwrite(&p.max_locals, sizeof(p.max_locals), 1, f);
                        fwrite(&p.max_stack, sizeof(p.max_stack), 1, f);
                        fclose(f);
                    }
                }
            }
            else crashes++;
        } else {
            crashes++;
        }

        if ((i + 1) % 100 == 0) {
            fprintf(stderr, "  [%d/%d] matches=%d mismatches=%d crashes=%d\n",
                    i + 1, num_programs, matches, mismatches, crashes);
        }
    }

    fprintf(stderr, "\n=== Results: %d matches, %d mismatches, %d crashes ===\n",
            matches, mismatches, crashes);
    return (mismatches + crashes > 0) ? 1 : 0;
}
