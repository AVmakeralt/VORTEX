/**
 * jit_exec_bench.c — VORTEX JIT Execution Benchmark
 *
 * Demonstrates the VORTEX JIT compiling and executing native x86-64 machine
 * code at near-C speed. The benchmark hand-crafts x86-64 machine code for an
 * iterative Fibonacci function, copies it into executable memory, and calls it
 * via a function pointer — the same mechanism a JIT compiler uses.
 *
 * Three execution modes are compared:
 *   1. Native C       — compiled by GCC/Clang at -O2
 *   2. Hand-crafted JIT — manually encoded x86-64, mmap'd + called
 *   3. VORTEX T0 interpreter — bytecode dispatch loop
 *
 * Additionally, the benchmark demonstrates two VORTEX-internal mechanisms:
 *   - VORTEX code cache  (vtx_code_cache_t) for executable memory allocation
 *   - VORTEX x86-64 emitter (vtx_x86_emit_t) for programmatic code generation
 *
 * Build: linked against vortex_bench_framework, vortex_interp, vortex_runtime,
 *        vortex_codecache, vortex_lower
 */

#include "bench_framework.h"
#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "interp/dispatch.h"
#include "codecache/cache.h"
#include "lower/emit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

/* ========================================================================== */
/* Constants                                                                    */
/* ========================================================================== */

#define FIB_N       20      /* Fibonacci input for benchmarking */
#define FIB_EXPECT  6765    /* fib(20) = 6765 */
#define BENCH_WARMUP   10
#define BENCH_ITERS    200

/* ========================================================================== */
/* x86-64 register numbers (System V encoding)                                 */
/* ========================================================================== */

#define REG_RAX 0
#define REG_RCX 1
#define REG_RDX 2
#define REG_RBX 3
#define REG_RSP 4
#define REG_RBP 5
#define REG_RSI 6
#define REG_RDI 7

/* ========================================================================== */
/* Native C Fibonacci (baseline)                                               */
/* ========================================================================== */

static int64_t native_fib(int64_t n)
{
    if (n <= 1) return n;
    int64_t a = 0, b = 1;
    for (int64_t i = 2; i <= n; i++) {
        int64_t tmp = a + b;
        a = b;
        b = tmp;
    }
    return b;
}

/* ========================================================================== */
/* Hand-crafted x86-64 machine code for fib(n)                                 */
/*                                                                              */
/* System V AMD64 ABI: first arg in RDI, return in RAX.                        */
/*                                                                              */
/* Assembly:                                                                    */
/*   push rbp                                                                   */
/*   mov  rbp, rsp                                                              */
/*   push rbx                     ; save callee-saved                           */
/*   mov  rcx, rdi                ; rcx = n                                     */
/*   xor  rax, rax                ; rax = a = 0                                 */
/*   mov  ebx, 1                  ; rbx = b = 1 (zero-extends to rbx)          */
/* .loop:                                                                       */
/*   test rcx, rcx                ; n <= 0?                                     */
/*   jle  .done                                                                  */
/*   lea  rdx, [rax + rbx]        ; rdx = a + b                                 */
/*   mov  rax, rbx                ; a = b                                        */
/*   mov  rbx, rdx                ; b = tmp                                      */
/*   dec  rcx                     ; n--                                          */
/*   jmp  .loop                                                                  */
/* .done:                                                                       */
/*   pop  rbx                                                                   */
/*   pop  rbp                                                                   */
/*   ret                                                                         */
/*                                                                              */
/* Encoding (46 bytes total):                                                   */
/*   Offset  Bytes                                              Instruction     */
/*   0x00    55                                                 push rbp        */
/*   0x01    48 89 E5                                           mov rbp, rsp    */
/*   0x04    53                                                 push rbx        */
/*   0x05    48 89 F9                                           mov rcx, rdi    */
/*   0x08    48 31 C0                                           xor rax, rax    */
/*   0x0B    BB 01 00 00 00                                     mov ebx, 1      */
/*   0x10    48 85 C9                                           test rcx, rcx   */
/*   0x13    0F 8E 12 00 00 00                                  jle +18 (done)  */
/*   0x19    48 8D 14 18                                        lea rdx,[rax+rbx]*/
/*   0x1D    48 89 D8                                           mov rax, rbx    */
/*   0x20    48 89 D3                                           mov rbx, rdx    */
/*   0x23    48 FF C9                                           dec rcx         */
/*   0x26    E9 E5 FF FF FF                                     jmp -27 (loop)  */
/*   0x2B    5B                                                 pop rbx         */
/*   0x2C    5D                                                 pop rbp         */
/*   0x2D    C3                                                 ret             */
/* ========================================================================== */

static const uint8_t fib_x86_64[] = {
    /* 0x00 */ 0x55,                                           /* push rbp       */
    /* 0x01 */ 0x48, 0x89, 0xE5,                              /* mov rbp, rsp   */
    /* 0x04 */ 0x53,                                           /* push rbx       */
    /* 0x05 */ 0x48, 0x89, 0xF9,                              /* mov rcx, rdi   */
    /* 0x08 */ 0x48, 0x31, 0xC0,                              /* xor rax, rax   */
    /* 0x0B */ 0xBB, 0x01, 0x00, 0x00, 0x00,                  /* mov ebx, 1     */
    /* 0x10 */ 0x48, 0x85, 0xC9,                              /* test rcx, rcx  */
    /* 0x13 */ 0x0F, 0x8E, 0x12, 0x00, 0x00, 0x00,           /* jle +18 (done) */
    /* 0x19 */ 0x48, 0x8D, 0x14, 0x18,                        /* lea rdx,[rax+rbx] */
    /* 0x1D */ 0x48, 0x89, 0xD8,                              /* mov rax, rbx   */
    /* 0x20 */ 0x48, 0x89, 0xD3,                              /* mov rbx, rdx   */
    /* 0x23 */ 0x48, 0xFF, 0xC9,                              /* dec rcx        */
    /* 0x26 */ 0xE9, 0xE5, 0xFF, 0xFF, 0xFF,                  /* jmp -27 (loop) */
    /* 0x2B */ 0x5B,                                           /* pop rbx        */
    /* 0x2C */ 0x5D,                                           /* pop rbp        */
    /* 0x2D */ 0xC3                                            /* ret            */
};

static const size_t fib_x86_64_size = sizeof(fib_x86_64);

/* ========================================================================== */
/* Nanosecond timing helper                                                     */
/* ========================================================================== */

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ========================================================================== */
/* JIT method 1: Direct mmap + function pointer                                 */
/* ========================================================================== */

/**
 * Allocate executable memory via mmap, copy hand-crafted x86-64 into it,
 * and return a callable function pointer.
 *
 * Returns NULL on failure.
 */
typedef int64_t (*fib_jit_fn)(int64_t n);

static fib_jit_fn jit_fib_create_mmap(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;

    /* Allocate one page of RWX memory */
    void *mem = mmap(NULL, (size_t)page_size,
                     PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    /* Copy the hand-crafted machine code */
    memcpy(mem, fib_x86_64, fib_x86_64_size);

    return (fib_jit_fn)mem;
}

static void jit_fib_destroy_mmap(fib_jit_fn fn)
{
    if (fn) {
        long page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0) page_size = 4096;
        munmap((void *)fn, (size_t)page_size);
    }
}

/* ========================================================================== */
/* JIT method 2: VORTEX code cache allocation                                   */
/* ========================================================================== */

/**
 * Use the VORTEX segmented code cache to allocate executable memory,
 * copy the hand-crafted machine code, and return a callable pointer.
 */
static fib_jit_fn jit_fib_create_codecache(vtx_code_cache_t *cache)
{
    if (!cache) return NULL;

    /* Allocate space from the code cache (16-byte aligned) */
    void *mem = vtx_code_cache_alloc(cache, (uint32_t)fib_x86_64_size);
    if (!mem) {
        fprintf(stderr, "jit_exec_bench: code cache alloc failed\n");
        return NULL;
    }

    /* Copy the hand-crafted machine code */
    memcpy(mem, fib_x86_64, fib_x86_64_size);

    /* Finalize the segment: make it executable */
    if (vtx_code_cache_finalize(cache) != 0) {
        fprintf(stderr, "jit_exec_bench: code cache finalize failed\n");
        return NULL;
    }

    return (fib_jit_fn)mem;
}

/* ========================================================================== */
/* JIT method 3: VORTEX x86-64 emitter                                         */
/*                                                                              */
/* Uses the VORTEX emitter API to programmatically generate the same fib()     */
/* function that we hand-encoded above. This demonstrates the emitter can      */
/* produce correct, efficient machine code.                                     */
/* ========================================================================== */

/**
 * Emit the iterative Fibonacci function using the VORTEX x86-64 emitter.
 * Returns a pointer to the generated code (caller must copy to executable
 * memory separately), or NULL on failure.
 *
 * @param emit    Initialized emitter context
 * @return        0 on success, -1 on failure
 */
static int jit_fib_emit_with_vortex(vtx_x86_emit_t *emit)
{
    if (!emit) return -1;

    /* Ensure we have enough space for ~64 bytes of code */
    if (vtx_x86_emit_ensure(emit, 64) != 0) return -1;

    /* ---- Prologue ---- */
    /* push rbp */
    vtx_x86_emit_push_r(emit, REG_RBP);
    /* mov rbp, rsp */
    vtx_x86_emit_mov_rr(emit, REG_RBP, REG_RSP);
    /* push rbx (callee-saved) */
    vtx_x86_emit_push_r(emit, REG_RBX);

    /* ---- Body ---- */
    /* mov rcx, rdi  (n = first arg) */
    vtx_x86_emit_mov_rr(emit, REG_RCX, REG_RDI);
    /* xor rax, rax  (a = 0) */
    vtx_x86_emit_xor_rr(emit, REG_RAX, REG_RAX);
    /* mov ebx, 1    (b = 1) — using 32-bit mov which zero-extends */
    vtx_x86_emit_mov_imm32(emit, REG_RBX, 1);

    /* ---- Loop ---- */
    /* Record loop start position for jump back */
    uint32_t loop_start = vtx_x86_emit_position(emit);

    /* test rcx, rcx */
    vtx_x86_emit_test_rr(emit, REG_RCX, REG_RCX);

    /* jle done — record position to patch later */
    uint32_t jle_pos = vtx_x86_emit_position(emit);
    /* Emit placeholder jle with rel32 = 0 (will patch) */
    vtx_x86_emit_jcc_rel32(emit, 0x0E /* LE */, 0);

    /* lea rdx, [rax + rbx]  (tmp = a + b) */
    /* LEA r64, [base + index*1 + disp0] needs SIB byte */
    /* Using the SIB memory emission */
    vtx_x86_emit_sib_mem(emit, 0x8D, 0, REG_RDX, REG_RAX, REG_RBX,
                          1, 0, true);

    /* mov rax, rbx  (a = b) */
    vtx_x86_emit_mov_rr(emit, REG_RAX, REG_RBX);
    /* mov rbx, rdx  (b = tmp) */
    vtx_x86_emit_mov_rr(emit, REG_RBX, REG_RDX);
    /* dec rcx */
    vtx_x86_emit_sub_ri(emit, REG_RCX, 1);

    /* jmp loop — record position to calculate offset */
    uint32_t jmp_pos = vtx_x86_emit_position(emit);
    /* Emit placeholder jmp with rel32 = 0 (will patch) */
    vtx_x86_emit_jmp_rel32(emit, 0);

    /* ---- Done (epilogue) ---- */
    uint32_t done_pos = vtx_x86_emit_position(emit);

    /* Patch the jle: rel32 = done_pos - (jle_pos + 6) */
    int32_t jle_offset = (int32_t)(done_pos - (jle_pos + 6));
    memcpy(emit->buffer + jle_pos + 2, &jle_offset, 4);

    /* Patch the jmp: rel32 = loop_start - (jmp_pos + 5) */
    int32_t jmp_offset = (int32_t)(loop_start - (jmp_pos + 5));
    memcpy(emit->buffer + jmp_pos + 1, &jmp_offset, 4);

    /* pop rbx */
    vtx_x86_emit_pop_r(emit, REG_RBX);
    /* pop rbp */
    vtx_x86_emit_pop_r(emit, REG_RBP);
    /* ret */
    vtx_x86_emit_ret(emit);

    return 0;
}

/* ========================================================================== */
/* Interpreter bytecode for fib(n) iterative                                    */
/*                                                                              */
/* Same bytecode as used in bench_fib.c:                                        */
/*   local0 = n, local1 = a=0, local2 = b=1, local3 = i=2                     */
/*   loop:                                                                      */
/*     if (i > n) goto end                                                      */
/*     local4 = local1 + local2                                                 */
/*     local1 = local2                                                          */
/*     local2 = local4                                                          */
/*     local3 = local3 + 1                                                      */
/*     goto loop                                                                */
/*   end:                                                                       */
/*     return local2                                                             */
/* ========================================================================== */

static const uint8_t fib_iter_bytecode[] = {
    VT_OP_LOAD_LOCAL,     0x00, 0x03,  /* PC  0: load i */
    VT_OP_LOAD_LOCAL,     0x00, 0x00,  /* PC  3: load n */
    VT_OP_ICMP_GT,                      /* PC  6: i > n ? */
    VT_OP_IF_TRUE,        0x00, 0x2D,  /* PC  7: -> PC 45 */
    VT_OP_LOAD_LOCAL,     0x00, 0x01,  /* PC 10: load a */
    VT_OP_LOAD_LOCAL,     0x00, 0x02,  /* PC 13: load b */
    VT_OP_IADD,                         /* PC 16: a + b */
    VT_OP_STORE_LOCAL,    0x00, 0x04,  /* PC 17: store tmp */
    VT_OP_LOAD_LOCAL,     0x00, 0x02,  /* PC 20: load b */
    VT_OP_STORE_LOCAL,    0x00, 0x01,  /* PC 23: a = b */
    VT_OP_LOAD_LOCAL,     0x00, 0x04,  /* PC 26: load tmp */
    VT_OP_STORE_LOCAL,    0x00, 0x02,  /* PC 29: b = tmp */
    VT_OP_LOAD_LOCAL,     0x00, 0x03,  /* PC 32: load i */
    VT_OP_LOAD_CONST_INT, 0x00, 0x01,  /* PC 35: load const 1 */
    VT_OP_IADD,                         /* PC 38: i + 1 */
    VT_OP_STORE_LOCAL,    0x00, 0x03,  /* PC 39: i = i+1 */
    VT_OP_GOTO,           0x00, 0x00,  /* PC 42: -> PC 0 */
    VT_OP_LOAD_LOCAL,     0x00, 0x02,  /* PC 45: load b */
    VT_OP_RETURN_VALUE                  /* PC 48: return b */
};

static vtx_value_t fib_consts[2];

static void fib_consts_init(void)
{
    fib_consts[0] = vtx_make_smi(0);
    fib_consts[1] = vtx_make_smi(1);
}

/* ========================================================================== */
/* Benchmark context                                                            */
/* ========================================================================== */

typedef struct {
    fib_jit_fn        jit_fn;     /* JIT function pointer */
    int64_t           n;          /* fib input */
    int64_t          *result;     /* where to store result (prevent optimization) */
} bench_jit_arg_t;

typedef struct {
    vtx_interp_t      *interp;
    vtx_method_desc_t *method;
    vtx_value_t       *args;
    uint32_t           arg_count;
    int64_t            n;
} bench_interp_arg_t;

/* Global result to prevent dead-code elimination */
static int64_t g_result;

/* ========================================================================== */
/* Benchmark runner: median of N iterations                                     */
/* ========================================================================== */

/**
 * Run a benchmark function `iters` times, return the median time in ns.
 * Includes warmup phase.
 */
static uint64_t bench_median_ns(void (*fn)(void *), void *arg,
                                 uint32_t warmup, uint32_t iters)
{
    uint64_t *samples = (uint64_t *)malloc(iters * sizeof(uint64_t));
    if (!samples) return 0;

    /* Warmup */
    for (uint32_t i = 0; i < warmup; i++) {
        fn(arg);
    }

    /* Measure */
    for (uint32_t i = 0; i < iters; i++) {
        uint64_t t0 = now_ns();
        fn(arg);
        uint64_t t1 = now_ns();
        samples[i] = t1 - t0;
    }

    /* Sort for median */
    for (uint32_t i = 0; i < iters - 1; i++) {
        for (uint32_t j = i + 1; j < iters; j++) {
            if (samples[j] < samples[i]) {
                uint64_t tmp = samples[i];
                samples[i] = samples[j];
                samples[j] = tmp;
            }
        }
    }

    uint64_t median = samples[iters / 2];
    free(samples);
    return median;
}

/* ========================================================================== */
/* Benchmark callback functions                                                 */
/* ========================================================================== */

static void bench_native_fib(void *arg)
{
    int64_t n = *(int64_t *)arg;
    g_result = native_fib(n);
}

static void bench_jit_fib_mmap(void *arg)
{
    bench_jit_arg_t *ba = (bench_jit_arg_t *)arg;
    g_result = ba->jit_fn(ba->n);
}

static void bench_jit_fib_cache(void *arg)
{
    bench_jit_arg_t *ba = (bench_jit_arg_t *)arg;
    g_result = ba->jit_fn(ba->n);
}

static void bench_jit_fib_emitter(void *arg)
{
    bench_jit_arg_t *ba = (bench_jit_arg_t *)arg;
    g_result = ba->jit_fn(ba->n);
}

static void bench_interp_fib(void *arg)
{
    bench_interp_arg_t *ba = (bench_interp_arg_t *)arg;
    vtx_value_t result = vtx_interp_run(ba->interp, ba->method,
                                         ba->args, ba->arg_count);
    if (vtx_is_smi(result)) {
        g_result = vtx_smi_value(result);
    }
}

/* ========================================================================== */
/* Hex dump helper for verification                                             */
/* ========================================================================== */

static void hex_dump(const uint8_t *code, size_t len, const char *label)
{
    printf("  %s (%zu bytes): ", label, len);
    for (size_t i = 0; i < len && i < 64; i++) {
        printf("%02X ", code[i]);
    }
    if (len > 64) printf("...");
    printf("\n");
}

/* ========================================================================== */
/* Main                                                                         */
/* ========================================================================== */

int main(void)
{
    printf("=== VORTEX JIT Execution Benchmark ===\n\n");

    int64_t fib_n = FIB_N;
    int rc = 0;

    /* -------------------------------------------------------------------- */
    /* Phase 0: Verify all implementations produce the correct result        */
    /* -------------------------------------------------------------------- */

    printf("--- Correctness Verification ---\n\n");

    int64_t expected = native_fib(FIB_N);
    printf("  Native C fib(%d) = %ld\n", FIB_N, (long)expected);

    /* Verify hand-crafted JIT via mmap */
    fib_jit_fn jit_mmap = jit_fib_create_mmap();
    if (!jit_mmap) {
        fprintf(stderr, "FATAL: could not create mmap JIT function\n");
        return 1;
    }
    int64_t jit_result = jit_mmap(FIB_N);
    printf("  Hand-crafted JIT (mmap) fib(%d) = %ld  %s\n",
           FIB_N, (long)jit_result,
           jit_result == expected ? "OK" : "WRONG");
    if (jit_result != expected) {
        fprintf(stderr, "ERROR: hand-crafted JIT produced wrong result\n");
        rc = 1;
        goto cleanup_mmap;
    }

    /* Verify JIT via VORTEX code cache */
    vtx_code_cache_t cache;
    if (vtx_code_cache_init(&cache, VORTEX_CACHE_MAX_SIZE) != 0) {
        fprintf(stderr, "FATAL: code cache init failed\n");
        rc = 1;
        goto cleanup_mmap;
    }

    fib_jit_fn jit_cache = jit_fib_create_codecache(&cache);
    if (!jit_cache) {
        fprintf(stderr, "FATAL: could not create code-cache JIT function\n");
        rc = 1;
        goto cleanup_cache;
    }
    int64_t cache_result = jit_cache(FIB_N);
    printf("  Hand-crafted JIT (code cache) fib(%d) = %ld  %s\n",
           FIB_N, (long)cache_result,
           cache_result == expected ? "OK" : "WRONG");
    if (cache_result != expected) {
        fprintf(stderr, "ERROR: code-cache JIT produced wrong result\n");
        rc = 1;
        goto cleanup_cache;
    }

    /* Verify JIT via VORTEX emitter */
    vtx_x86_emit_t emitter;
    if (vtx_x86_emit_init(&emitter, 256) != 0) {
        fprintf(stderr, "FATAL: emitter init failed\n");
        rc = 1;
        goto cleanup_cache;
    }

    if (jit_fib_emit_with_vortex(&emitter) != 0) {
        fprintf(stderr, "FATAL: emitter code generation failed\n");
        rc = 1;
        goto cleanup_emitter;
    }

    /* Copy emitter output to executable memory */
    fib_jit_fn jit_emitted = NULL;
    {
        long pgsz = sysconf(_SC_PAGESIZE);
        if (pgsz <= 0) pgsz = 4096;
        void *mem = mmap(NULL, (size_t)pgsz,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) {
            perror("mmap for emitter output");
            rc = 1;
            goto cleanup_emitter;
        }
        uint32_t code_size = vtx_x86_emit_position(&emitter);
        memcpy(mem, vtx_x86_emit_code(&emitter), code_size);
        jit_emitted = (fib_jit_fn)mem;
    }

    int64_t emitted_result = jit_emitted(FIB_N);
    printf("  VORTEX Emitter JIT fib(%d) = %ld  %s\n",
           FIB_N, (long)emitted_result,
           emitted_result == expected ? "OK" : "WRONG");
    if (emitted_result != expected) {
        fprintf(stderr, "ERROR: emitter JIT produced wrong result\n");
        rc = 1;
        goto cleanup_emitter;
    }

    /* Show code sizes */
    printf("\n--- Code Size Comparison ---\n\n");
    hex_dump(fib_x86_64, fib_x86_64_size, "Hand-crafted");
    hex_dump(vtx_x86_emit_code(&emitter), vtx_x86_emit_position(&emitter),
             "VORTEX Emitter");

    /* -------------------------------------------------------------------- */
    /* Phase 1: Interpreter setup                                           */
    /* -------------------------------------------------------------------- */

    fib_consts_init();

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    vtx_bytecode_t fib_bc = {
        .code = fib_iter_bytecode,
        .length = sizeof(fib_iter_bytecode),
        .constant_pool = fib_consts,
        .constant_count = 2,
        .max_locals = 5,
        .max_stack = 4
    };

    vtx_method_desc_t fib_method = {
        .name = "fib_iter",
        .signature = "(IIIII)I",
        .bytecode = &fib_bc,
        .vtable_index = 0xFFFFFFFF,
        .is_virtual = false
    };

    /* Args: n=FIB_N, a=0, b=1, i=2, tmp=0 */
    vtx_value_t fib_args[] = {
        vtx_make_smi(FIB_N),   /* local 0: n */
        vtx_make_smi(0),       /* local 1: a */
        vtx_make_smi(1),       /* local 2: b */
        vtx_make_smi(2),       /* local 3: i */
        vtx_make_smi(0)        /* local 4: tmp */
    };

    /* Verify interpreter produces correct result */
    vtx_value_t interp_result = vtx_interp_run(&interp, &fib_method,
                                                fib_args, 5);
    if (vtx_is_smi(interp_result)) {
        int64_t ival = vtx_smi_value(interp_result);
        printf("  VORTEX T0 interp fib(%d) = %ld  %s\n",
               FIB_N, (long)ival,
               ival == expected ? "OK" : "WRONG");
    } else {
        printf("  VORTEX T0 interp fib(%d) = <non-smi>  WRONG\n", FIB_N);
    }

    /* -------------------------------------------------------------------- */
    /* Phase 2: Performance benchmarks                                      */
    /* -------------------------------------------------------------------- */

    printf("\n--- Performance Benchmarks ---\n\n");

    /* Native C */
    uint64_t native_ns = bench_median_ns(bench_native_fib, &fib_n,
                                          BENCH_WARMUP, BENCH_ITERS);

    /* Hand-crafted JIT (mmap) */
    bench_jit_arg_t jit_mmap_arg = { jit_mmap, FIB_N, &g_result };
    uint64_t jit_mmap_ns = bench_median_ns(bench_jit_fib_mmap, &jit_mmap_arg,
                                            BENCH_WARMUP, BENCH_ITERS);

    /* Hand-crafted JIT (code cache) */
    bench_jit_arg_t jit_cache_arg = { jit_cache, FIB_N, &g_result };
    uint64_t jit_cache_ns = bench_median_ns(bench_jit_fib_cache, &jit_cache_arg,
                                             BENCH_WARMUP, BENCH_ITERS);

    /* VORTEX Emitter JIT */
    bench_jit_arg_t jit_emit_arg = { jit_emitted, FIB_N, &g_result };
    uint64_t jit_emit_ns = bench_median_ns(bench_jit_fib_emitter, &jit_emit_arg,
                                            BENCH_WARMUP, BENCH_ITERS);

    /* VORTEX T0 Interpreter */
    bench_interp_arg_t interp_arg = {
        &interp, &fib_method, fib_args, 5, FIB_N
    };
    uint64_t interp_ns = bench_median_ns(bench_interp_fib, &interp_arg,
                                          BENCH_WARMUP, BENCH_ITERS);

    /* -------------------------------------------------------------------- */
    /* Phase 3: Report results                                              */
    /* -------------------------------------------------------------------- */

    printf("  %-30s  %6lu ns/call\n", "fib(20) — Native C:",
           (unsigned long)native_ns);
    printf("  %-30s  %6lu ns/call\n", "fib(20) — Hand-crafted JIT (mmap):",
           (unsigned long)jit_mmap_ns);
    printf("  %-30s  %6lu ns/call\n", "fib(20) — Hand-crafted JIT (code cache):",
           (unsigned long)jit_cache_ns);
    printf("  %-30s  %6lu ns/call\n", "fib(20) — VORTEX Emitter JIT:",
           (unsigned long)jit_emit_ns);
    printf("  %-30s  %6lu ns/call\n", "fib(20) — VORTEX T0 interp:",
           (unsigned long)interp_ns);

    printf("\n");

    /* Use the mmap JIT as the reference JIT since it's the simplest path */
    if (interp_ns > 0 && jit_mmap_ns > 0) {
        double speedup = (double)interp_ns / (double)jit_mmap_ns;
        printf("  Speedup: JIT is %.1fx faster than interpreter\n", speedup);
    }

    if (native_ns > 0 && jit_mmap_ns > 0) {
        double pct = 100.0 * (double)native_ns / (double)jit_mmap_ns;
        printf("  Speedup: JIT is %.1f%% of native C speed\n", pct);
    }

    printf("\n");

    /* Also report code cache statistics */
    printf("--- Code Cache Statistics ---\n\n");
    printf("  Total bytes used:    %lu\n",
           (unsigned long)vtx_code_cache_total_used(&cache));
    printf("  Segment count:       %u\n",
           vtx_code_cache_segment_count(&cache));
    printf("  Is full:             %s\n",
           vtx_code_cache_is_full(&cache) ? "yes" : "no");

    printf("\n");

    /* -------------------------------------------------------------------- */
    /* Phase 4: Detailed benchmark using the framework                      */
    /* -------------------------------------------------------------------- */

    printf("--- Detailed Benchmark (framework) ---\n\n");

    vtx_bench_result_t r_native = vtx_bench_run("native fib(20)",
                                                 bench_native_fib, &fib_n);
    vtx_bench_report(&r_native);

    vtx_bench_result_t r_jit_mmap = vtx_bench_run("JIT mmap fib(20)",
                                                    bench_jit_fib_mmap,
                                                    &jit_mmap_arg);
    vtx_bench_report(&r_jit_mmap);

    vtx_bench_result_t r_jit_cache = vtx_bench_run("JIT cache fib(20)",
                                                     bench_jit_fib_cache,
                                                     &jit_cache_arg);
    vtx_bench_report(&r_jit_cache);

    vtx_bench_result_t r_jit_emit = vtx_bench_run("JIT emitter fib(20)",
                                                    bench_jit_fib_emitter,
                                                    &jit_emit_arg);
    vtx_bench_report(&r_jit_emit);

    vtx_bench_result_t r_interp = vtx_bench_run("T0 interp fib(20)",
                                                  bench_interp_fib,
                                                  &interp_arg);
    vtx_bench_report(&r_interp);

    /* Compare JIT vs interpreter */
    vtx_bench_compare("JIT mmap", &r_jit_mmap, "T0 interp", &r_interp);
    vtx_bench_compare("JIT mmap", &r_jit_mmap, "native C", &r_native);

    /* -------------------------------------------------------------------- */
    /* Cleanup                                                               */
    /* -------------------------------------------------------------------- */

    if (jit_emitted) {
        long pgsz = sysconf(_SC_PAGESIZE);
        if (pgsz <= 0) pgsz = 4096;
        munmap((void *)jit_emitted, (size_t)pgsz);
    }

cleanup_emitter:
    vtx_x86_emit_destroy(&emitter);

cleanup_cache:
    vtx_code_cache_destroy(&cache);

cleanup_mmap:
    jit_fib_destroy_mmap(jit_mmap);

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);

    (void)g_result;
    return rc;
}
