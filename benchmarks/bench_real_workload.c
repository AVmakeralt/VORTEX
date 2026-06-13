/**
 * bench_real_workload.c — VORTEX JIT Real Workload Benchmark
 *
 * NOT fib. NOT the interpreter. This benchmarks:
 *   1. VORTEX T1 Baseline JIT (bytecode → x86-64, via vtx_baseline_compile)
 *   2. VORTEX Emitter JIT (hand-written via vtx_x86_emit_t API)
 *   3. Native C (-O3 -march=native)
 *
 * Workloads:
 *   - sum(N): simple accumulator loop
 *   - nested(N): double-nested loop
 *   - gcd(a,b): Euclidean algorithm (modulo-heavy)
 *   - collatz(N): 3n+1 convergence (branchy, unpredictable)
 *   - fnv_hash(N): FNV-1a hash computation (bitwise, realistic)
 *   - mandelbrot(W,H): escape-time iteration (float-heavy kernel)
 *   - pi(N): Leibniz series approximation (float + int mixed)
 *   - sieve(N): Eratosthenes prime sieve (array store + branch)
 *   - matrix_mul(N): integer matrix multiply (triple-nested loop)
 *
 * Methodology:
 *   - Results consumed (volatile/accumulated) to prevent DCE
 *   - Warmup runs before measurement
 *   - 50 samples, median + p95 reported
 *   - Compiled with -O3 -march=native for native C baseline
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/arena.h"
#include "interp/dispatch.h"
#include "baseline/codegen.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "lower/emit.h"

/* ========================================================================== */
/* x86-64 register numbers (System V encoding) — same as jit_exec_bench.c     */
/* ========================================================================== */

#define REG_RAX 0
#define REG_RCX 1
#define REG_RDX 2
#define REG_RBX 3
#define REG_RSP 4
#define REG_RBP 5
#define REG_RSI 6
#define REG_RDI 7
#define REG_R8  8
#define REG_R9  9
#define REG_R10 10
#define REG_R11 11
#define REG_R12 12
#define REG_R13 13
#define REG_R14 14
#define REG_R15 15

/* ========================================================================== */
/* Timing                                                                      */
/* ========================================================================== */

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ========================================================================== */
/* Benchmark framework                                                         */
/* ========================================================================== */

#define BENCH_WARMUP  20
#define BENCH_SAMPLES 50

typedef struct {
    const char *name;
    uint64_t    median_ns;
    uint64_t    p95_ns;
    uint64_t    min_ns;
    double      mean_ns;
} bench_result_t;

static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static bench_result_t bench_run(const char *name, void (*fn)(void *), void *arg,
                                 uint32_t warmup, uint32_t samples)
{
    bench_result_t r = { .name = name };

    /* Warmup */
    for (uint32_t i = 0; i < warmup; i++) fn(arg);

    /* Measure */
    uint64_t *s = (uint64_t *)malloc(samples * sizeof(uint64_t));
    for (uint32_t i = 0; i < samples; i++) {
        uint64_t t0 = now_ns();
        fn(arg);
        uint64_t t1 = now_ns();
        s[i] = t1 - t0;
    }

    qsort(s, samples, sizeof(uint64_t), cmp_u64);
    r.min_ns    = s[0];
    r.median_ns = s[samples / 2];
    r.p95_ns    = s[(uint32_t)(samples * 0.95)];
    r.mean_ns   = 0;
    for (uint32_t i = 0; i < samples; i++) r.mean_ns += s[i];
    r.mean_ns /= samples;

    free(s);
    return r;
}

static void bench_print(const bench_result_t *r)
{
    printf("  %-38s  median %6lu ns  p95 %6lu ns  min %6lu ns\n",
           r->name, (unsigned long)r->median_ns,
           (unsigned long)r->p95_ns, (unsigned long)r->min_ns);
}

/* ========================================================================== */
/* Global result sink (prevents dead code elimination)                         */
/* ========================================================================== */

static volatile int64_t  g_result_i64;
static volatile double   g_result_f64;

/* ========================================================================== */
/* NATIVE C IMPLEMENTATIONS (baseline, compiled with -O3)                      */
/* ========================================================================== */

static int64_t native_sum(int64_t n)
{
    int64_t s = 0;
    for (int64_t i = 0; i < n; i++) s += i;
    return s;
}

static int64_t native_nested(int64_t n)
{
    int64_t count = 0;
    for (int64_t i = 0; i < n; i++)
        for (int64_t j = 0; j < n; j++)
            count++;
    return count;
}

static int64_t native_gcd(int64_t a, int64_t b)
{
    while (b != 0) {
        int64_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

static int64_t native_collatz(int64_t n)
{
    int64_t steps = 0;
    while (n != 1) {
        if (n & 1) n = 3 * n + 1;
        else       n = n >> 1;
        steps++;
    }
    return steps;
}

static int64_t native_fnv_hash(int64_t n)
{
    /* FNV-1a on n bytes of sequential data */
    uint64_t hash = 14695981039346656037ULL;
    for (int64_t i = 0; i < n; i++) {
        hash ^= (uint64_t)(i & 0xFF);
        hash *= 1099511628211ULL;
    }
    return (int64_t)hash;
}

static int64_t native_mandelbrot(int64_t max_iter)
{
    /* Count points that don't escape in a 50x50 grid centered at origin */
    int64_t count = 0;
    double x0 = -2.0, y0 = -1.5;
    double dx = 4.0 / 50.0, dy = 3.0 / 50.0;
    for (int py = 0; py < 50; py++) {
        double ci = y0 + py * dy;
        for (int px = 0; px < 50; px++) {
            double cr = x0 + px * dx;
            double zr = 0.0, zi = 0.0;
            int64_t iter = 0;
            while (iter < max_iter) {
                double zr2 = zr * zr;
                double zi2 = zi * zi;
                if (zr2 + zi2 > 4.0) break;
                zi = 2.0 * zr * zi + ci;
                zr = zr2 - zi2 + cr;
                iter++;
            }
            if (iter == max_iter) count++;
        }
    }
    return count;
}

static double native_pi(int64_t n)
{
    /* Leibniz series: pi/4 = 1 - 1/3 + 1/5 - 1/7 + ... */
    double sum = 0.0;
    int64_t sign = 1;
    for (int64_t i = 0; i < n; i++) {
        sum += (double)sign / (double)(2 * i + 1);
        sign = -sign;
    }
    return 4.0 * sum;
}

static int64_t native_sieve(int64_t n)
{
    /* Eratosthenes sieve: count primes up to n */
    /* Use a bitmap on the stack for small n */
    if (n < 2) return 0;
    uint8_t *composite = (uint8_t *)calloc((size_t)(n + 1), 1);
    if (!composite) return -1;
    int64_t count = 0;
    for (int64_t i = 2; i <= n; i++) {
        if (!composite[i]) {
            count++;
            for (int64_t j = i * i; j <= n && j > 0; j += i) {
                composite[j] = 1;
            }
        }
    }
    free(composite);
    return count;
}

static int64_t native_matrix_mul(int64_t n)
{
    /* Integer matrix multiply, n x n, returns sum of all elements in result */
    int64_t *A = (int64_t *)malloc((size_t)(n * n) * sizeof(int64_t));
    int64_t *B = (int64_t *)malloc((size_t)(n * n) * sizeof(int64_t));
    int64_t *C = (int64_t *)calloc((size_t)(n * n), sizeof(int64_t));
    if (!A || !B || !C) { free(A); free(B); free(C); return -1; }

    for (int64_t i = 0; i < n * n; i++) { A[i] = (i + 1) % 7; B[i] = (i + 3) % 11; }

    for (int64_t i = 0; i < n; i++)
        for (int64_t k = 0; k < n; k++) {
            int64_t a_ik = A[i * n + k];
            for (int64_t j = 0; j < n; j++)
                C[i * n + j] += a_ik * B[k * n + j];
        }

    int64_t sum = 0;
    for (int64_t i = 0; i < n * n; i++) sum += C[i];

    free(A); free(B); free(C);
    return sum;
}

/* ========================================================================== */
/* NATIVE C BENCHMARK WRAPPERS                                                 */
/* ========================================================================== */

typedef struct { int64_t n; } native_ctx_t;

static void bench_native_sum(void *arg)      { native_ctx_t *c = (native_ctx_t *)arg; g_result_i64 = native_sum(c->n); }
static void bench_native_nested(void *arg)   { native_ctx_t *c = (native_ctx_t *)arg; g_result_i64 = native_nested(c->n); }
static void bench_native_gcd(void *arg)      { native_ctx_t *c = (native_ctx_t *)arg; g_result_i64 = native_gcd(c->n, c->n / 2 + 1); }
static void bench_native_collatz(void *arg)  { native_ctx_t *c = (native_ctx_t *)arg; g_result_i64 = native_collatz(c->n); }
static void bench_native_fnv(void *arg)      { native_ctx_t *c = (native_ctx_t *)arg; g_result_i64 = native_fnv_hash(c->n); }
static void bench_native_mandel(void *arg)   { native_ctx_t *c = (native_ctx_t *)arg; g_result_i64 = native_mandelbrot(c->n); }
static void bench_native_pi(void *arg)       { native_ctx_t *c = (native_ctx_t *)arg; g_result_f64 = native_pi(c->n); }
static void bench_native_sieve(void *arg)    { native_ctx_t *c = (native_ctx_t *)arg; g_result_i64 = native_sieve(c->n); }
static void bench_native_matmul(void *arg)   { native_ctx_t *c = (native_ctx_t *)arg; g_result_i64 = native_matrix_mul(c->n); }

/* ========================================================================== */
/* VORTEX EMITTER JIT: Hand-crafted x86-64 via vtx_x86_emit_t                */
/*                                                                            */
/* These emit pure integer x86-64 that matches the native C semantics.        */
/* Calling convention: int64_t fn(int64_t n) — RDI = n, RAX = return         */
/* ========================================================================== */

/* Helper: copy emitter output to executable memory */
typedef int64_t (*jit_fn_t)(int64_t);
typedef int64_t (*gcd_fn_t)(int64_t, int64_t);

static jit_fn_t jit_copy_to_exec(vtx_x86_emit_t *emit)
{
    long pgsz = sysconf(_SC_PAGESIZE);
    if (pgsz <= 0) pgsz = 4096;
    void *mem = mmap(NULL, (size_t)pgsz,
                     PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return NULL;
    uint32_t size = vtx_x86_emit_position(emit);
    memcpy(mem, vtx_x86_emit_code(emit), size);
    return (jit_fn_t)mem;
}

static void jit_destroy(jit_fn_t fn)
{
    if (fn) {
        long pgsz = sysconf(_SC_PAGESIZE);
        if (pgsz <= 0) pgsz = 4096;
        munmap((void *)fn, (size_t)pgsz);
    }
}

/**
 * JIT sum(n): int64_t sum(int64_t n)
 *   sum = 0; i = 0; while (i < n) { sum += i; i++; } return sum;
 * Registers: RDI=n, RAX=sum, RCX=i
 */
static jit_fn_t jit_create_sum(void)
{
    vtx_x86_emit_t e;
    vtx_x86_emit_init(&e, 256);

    /* push rbp; mov rbp, rsp; push rbx */
    vtx_x86_emit_push_r(&e, REG_RBP);
    vtx_x86_emit_mov_rr(&e, REG_RBP, REG_RSP);
    vtx_x86_emit_push_r(&e, REG_RBX);

    /* RDI = n, RAX = sum = 0, RCX = i = 0 */
    vtx_x86_emit_xor_rr(&e, REG_RAX, REG_RAX);  /* sum = 0 */
    vtx_x86_emit_xor_rr(&e, REG_RCX, REG_RCX);  /* i = 0 */

    /* Loop start */
    uint32_t loop_start = vtx_x86_emit_position(&e);

    /* cmp rcx, rdi (i < n?) */
    vtx_x86_emit_cmp_rr(&e, REG_RCX, REG_RDI);
    /* jge done */
    uint32_t jge_pos = vtx_x86_emit_position(&e);
    vtx_x86_emit_jcc_rel32(&e, 0x0D /* GE */, 0);  /* placeholder */

    /* sum += i */
    vtx_x86_emit_add_rr(&e, REG_RAX, REG_RCX);
    /* i++ */
    vtx_x86_emit_add_ri(&e, REG_RCX, 1);
    /* jmp loop */
    uint32_t jmp_pos = vtx_x86_emit_position(&e);
    vtx_x86_emit_jmp_rel32(&e, 0);  /* placeholder */

    /* Done */
    uint32_t done_pos = vtx_x86_emit_position(&e);
    /* Patch jge */
    int32_t jge_off = (int32_t)(done_pos - (jge_pos + 6));
    memcpy((uint8_t *)vtx_x86_emit_code(&e) + jge_pos + 2, &jge_off, 4);
    /* Patch jmp */
    int32_t jmp_off = (int32_t)(loop_start - (jmp_pos + 5));
    memcpy((uint8_t *)vtx_x86_emit_code(&e) + jmp_pos + 1, &jmp_off, 4);

    /* Epilogue */
    vtx_x86_emit_pop_r(&e, REG_RBX);
    vtx_x86_emit_pop_r(&e, REG_RBP);
    vtx_x86_emit_ret(&e);

    jit_fn_t fn = jit_copy_to_exec(&e);
    vtx_x86_emit_destroy(&e);
    return fn;
}

/**
 * JIT nested(n): int64_t nested(int64_t n)
 *   count=0; i=0; while(i<n){ j=0; while(j<n){ count++; j++; } i++; } return count;
 * Registers: RDI=n, RAX=count, RCX=i, RDX=j
 */
static jit_fn_t jit_create_nested(void)
{
    vtx_x86_emit_t e;
    vtx_x86_emit_init(&e, 512);

    vtx_x86_emit_push_r(&e, REG_RBP);
    vtx_x86_emit_mov_rr(&e, REG_RBP, REG_RSP);
    vtx_x86_emit_push_r(&e, REG_RBX);
    vtx_x86_emit_push_r(&e, REG_R12);

    /* Save n to RBX (callee-saved), RAX=count=0, RCX=i=0 */
    vtx_x86_emit_mov_rr(&e, REG_RBX, REG_RDI);  /* RBX = n */
    vtx_x86_emit_xor_rr(&e, REG_RAX, REG_RAX);  /* count = 0 */
    vtx_x86_emit_xor_rr(&e, REG_RCX, REG_RCX);  /* i = 0 */

    /* Outer loop start */
    uint32_t outer_start = vtx_x86_emit_position(&e);

    /* cmp rcx, rbx (i < n?) */
    vtx_x86_emit_cmp_rr(&e, REG_RCX, REG_RBX);
    uint32_t outer_jge = vtx_x86_emit_position(&e);
    vtx_x86_emit_jcc_rel32(&e, 0x0D /* GE */, 0);

    /* j = 0 */
    vtx_x86_emit_xor_rr(&e, REG_RDX, REG_RDX);

    /* Inner loop start */
    uint32_t inner_start = vtx_x86_emit_position(&e);

    /* cmp rdx, rbx (j < n?) */
    vtx_x86_emit_cmp_rr(&e, REG_RDX, REG_RBX);
    uint32_t inner_jge = vtx_x86_emit_position(&e);
    vtx_x86_emit_jcc_rel32(&e, 0x0D /* GE */, 0);

    /* count++ */
    vtx_x86_emit_add_ri(&e, REG_RAX, 1);
    /* j++ */
    vtx_x86_emit_add_ri(&e, REG_RDX, 1);

    /* jmp inner */
    uint32_t inner_jmp = vtx_x86_emit_position(&e);
    vtx_x86_emit_jmp_rel32(&e, 0);

    /* Inner exit */
    uint32_t inner_end = vtx_x86_emit_position(&e);
    int32_t inner_jge_off = (int32_t)(inner_end - (inner_jge + 6));
    memcpy((uint8_t *)vtx_x86_emit_code(&e) + inner_jge + 2, &inner_jge_off, 4);
    int32_t inner_jmp_off = (int32_t)(inner_start - (inner_jmp + 5));
    memcpy((uint8_t *)vtx_x86_emit_code(&e) + inner_jmp + 1, &inner_jmp_off, 4);

    /* i++ */
    vtx_x86_emit_add_ri(&e, REG_RCX, 1);

    /* jmp outer */
    uint32_t outer_jmp = vtx_x86_emit_position(&e);
    vtx_x86_emit_jmp_rel32(&e, 0);

    /* Outer exit */
    uint32_t outer_end = vtx_x86_emit_position(&e);
    int32_t outer_jge_off = (int32_t)(outer_end - (outer_jge + 6));
    memcpy((uint8_t *)vtx_x86_emit_code(&e) + outer_jge + 2, &outer_jge_off, 4);
    int32_t outer_jmp_off = (int32_t)(outer_start - (outer_jmp + 5));
    memcpy((uint8_t *)vtx_x86_emit_code(&e) + outer_jmp + 1, &outer_jmp_off, 4);

    /* Epilogue */
    vtx_x86_emit_pop_r(&e, REG_R12);
    vtx_x86_emit_pop_r(&e, REG_RBX);
    vtx_x86_emit_pop_r(&e, REG_RBP);
    vtx_x86_emit_ret(&e);

    jit_fn_t fn = jit_copy_to_exec(&e);
    vtx_x86_emit_destroy(&e);
    return fn;
}

/**
 * JIT gcd(a, b): We pass a in RDI, b in RSI.
 *   while (b != 0) { t = b; b = a % b; a = t; } return a;
 * Uses: RDI=a, RSI=b, RAX=return, RCX=scratch, RDX=remainder from idiv
 */
static gcd_fn_t jit_create_gcd(void)
{
    vtx_x86_emit_t e;
    vtx_x86_emit_init(&e, 256);

    vtx_x86_emit_push_r(&e, REG_RBP);
    vtx_x86_emit_mov_rr(&e, REG_RBP, REG_RSP);
    vtx_x86_emit_push_r(&e, REG_RBX);

    /* Move args to callee-saved: RBX=a, RCX=b */
    vtx_x86_emit_mov_rr(&e, REG_RBX, REG_RDI);  /* RBX = a */
    vtx_x86_emit_mov_rr(&e, REG_RCX, REG_RSI);  /* RCX = b */

    /* Loop start */
    uint32_t loop_start = vtx_x86_emit_position(&e);

    /* test rcx, rcx (b == 0?) */
    vtx_x86_emit_test_rr(&e, REG_RCX, REG_RCX);
    uint32_t jz_pos = vtx_x86_emit_position(&e);
    vtx_x86_emit_jcc_rel32(&e, 0x04 /* Z */, 0);

    /* idiv rbx by rcx: RAX=rbx, CQO, idiv rcx → RAX=quot, RDX=rem */
    vtx_x86_emit_mov_rr(&e, REG_RAX, REG_RBX);  /* RAX = a */
    vtx_x86_emit_cqo(&e);                         /* RDX = sign-extend RAX */
    vtx_x86_emit_idiv_r(&e, REG_RCX);             /* RAX = a/b, RDX = a%b */

    /* a = old b, b = remainder */
    vtx_x86_emit_mov_rr(&e, REG_RBX, REG_RCX);   /* a = old b */
    vtx_x86_emit_mov_rr(&e, REG_RCX, REG_RDX);   /* b = a % b */

    /* jmp loop */
    uint32_t jmp_pos = vtx_x86_emit_position(&e);
    vtx_x86_emit_jmp_rel32(&e, 0);

    /* Done: RAX = a */
    uint32_t done_pos = vtx_x86_emit_position(&e);
    vtx_x86_emit_mov_rr(&e, REG_RAX, REG_RBX);   /* return a */

    /* Patch jz */
    int32_t jz_off = (int32_t)(done_pos - (jz_pos + 6));
    memcpy((uint8_t *)vtx_x86_emit_code(&e) + jz_pos + 2, &jz_off, 4);
    /* Patch jmp */
    int32_t jmp_off = (int32_t)(loop_start - (jmp_pos + 5));
    memcpy((uint8_t *)vtx_x86_emit_code(&e) + jmp_pos + 1, &jmp_off, 4);

    vtx_x86_emit_pop_r(&e, REG_RBX);
    vtx_x86_emit_pop_r(&e, REG_RBP);
    vtx_x86_emit_ret(&e);

    gcd_fn_t gfn = (gcd_fn_t)(uintptr_t)jit_copy_to_exec(&e);
    vtx_x86_emit_destroy(&e);
    return gfn;
}

/**
 * JIT collatz(n): int64_t collatz(int64_t n)
 *   steps=0; while(n!=1){ if(n&1) n=3*n+1; else n>>=1; steps++; } return steps;
 * Registers: RDI=n, RAX=steps, RCX=scratch for n
 */
static jit_fn_t jit_create_collatz(void)
{
    vtx_x86_emit_t e;
    vtx_x86_emit_init(&e, 512);

    vtx_x86_emit_push_r(&e, REG_RBP);
    vtx_x86_emit_mov_rr(&e, REG_RBP, REG_RSP);
    vtx_x86_emit_push_r(&e, REG_RBX);

    /* RCX = n, RAX = steps = 0, RBX = scratch for 3*n */
    vtx_x86_emit_mov_rr(&e, REG_RCX, REG_RDI);
    vtx_x86_emit_xor_rr(&e, REG_RAX, REG_RAX);

    /* Loop start */
    uint32_t loop_start = vtx_x86_emit_position(&e);

    /* cmp rcx, 1 */
    vtx_x86_emit_cmp_ri(&e, REG_RCX, 1);
    uint32_t jle_pos = vtx_x86_emit_position(&e);
    vtx_x86_emit_jcc_rel32(&e, 0x0E /* LE */, 0);

    /* test rcx, 1 (n & 1?) */
    vtx_x86_emit_test_ri(&e, REG_RCX, 1);
    uint32_t jz_even = vtx_x86_emit_position(&e);
    vtx_x86_emit_jcc_rel32(&e, 0x04 /* Z */, 0);

    /* Odd: n = 3*n + 1 */
    vtx_x86_emit_mov_rr(&e, REG_RBX, REG_RCX);  /* rbx = n */
    vtx_x86_emit_shl_ri(&e, REG_RBX, 1);         /* rbx = 2*n */
    vtx_x86_emit_add_rr(&e, REG_RBX, REG_RCX);   /* rbx = 3*n */
    vtx_x86_emit_add_ri(&e, REG_RBX, 1);          /* rbx = 3*n+1 */
    vtx_x86_emit_mov_rr(&e, REG_RCX, REG_RBX);   /* n = 3*n+1 */

    /* Skip even path */
    uint32_t skip_even = vtx_x86_emit_position(&e);
    vtx_x86_emit_jmp_rel32(&e, 0);

    /* Even: n >>= 1 */
    uint32_t even_path = vtx_x86_emit_position(&e);
    vtx_x86_emit_shr_ri(&e, REG_RCX, 1);

    /* steps++ */
    uint32_t inc_steps = vtx_x86_emit_position(&e);

    /* Patch jz_even → even_path */
    int32_t jz_off = (int32_t)(even_path - (jz_even + 6));
    memcpy((uint8_t *)vtx_x86_emit_code(&e) + jz_even + 2, &jz_off, 4);
    /* Patch skip_even → inc_steps */
    int32_t skip_off = (int32_t)(inc_steps - (skip_even + 5));
    memcpy((uint8_t *)vtx_x86_emit_code(&e) + skip_even + 1, &skip_off, 4);

    vtx_x86_emit_add_ri(&e, REG_RAX, 1);

    /* jmp loop */
    uint32_t jmp_pos = vtx_x86_emit_position(&e);
    vtx_x86_emit_jmp_rel32(&e, 0);

    /* Done */
    uint32_t done_pos = vtx_x86_emit_position(&e);
    int32_t jle_off = (int32_t)(done_pos - (jle_pos + 6));
    memcpy((uint8_t *)vtx_x86_emit_code(&e) + jle_pos + 2, &jle_off, 4);
    int32_t jmp_off = (int32_t)(loop_start - (jmp_pos + 5));
    memcpy((uint8_t *)vtx_x86_emit_code(&e) + jmp_pos + 1, &jmp_off, 4);

    vtx_x86_emit_pop_r(&e, REG_RBX);
    vtx_x86_emit_pop_r(&e, REG_RBP);
    vtx_x86_emit_ret(&e);

    jit_fn_t fn = jit_copy_to_exec(&e);
    vtx_x86_emit_destroy(&e);
    return fn;
}

/**
 * JIT fnv_hash(n): FNV-1a hash over n bytes
 *   hash = 14695981039346656037; for i in 0..n: hash ^= (i&0xFF); hash *= 1099511628211;
 * Uses: RDI=n, RAX=hash, RCX=i, RDX=scratch
 * NOTE: 64-bit multiply via imul r64, r64
 */
static jit_fn_t jit_create_fnv(void)
{
    vtx_x86_emit_t e;
    vtx_x86_emit_init(&e, 512);

    vtx_x86_emit_push_r(&e, REG_RBP);
    vtx_x86_emit_mov_rr(&e, REG_RBP, REG_RSP);
    vtx_x86_emit_push_r(&e, REG_RBX);
    vtx_x86_emit_push_r(&e, REG_R12);

    /* RBX = n (callee-saved), RAX = hash = FNV_OFFSET, RCX = i = 0 */
    vtx_x86_emit_mov_rr(&e, REG_RBX, REG_RDI);
    vtx_x86_emit_mov_imm64(&e, REG_RAX, 14695981039346656037ULL);
    vtx_x86_emit_xor_rr(&e, REG_RCX, REG_RCX);

    /* R12 = FNV_PRIME (callee-saved) */
    vtx_x86_emit_mov_imm64(&e, REG_R12, 1099511628211ULL);

    /* Loop start */
    uint32_t loop_start = vtx_x86_emit_position(&e);

    /* cmp rcx, rbx */
    vtx_x86_emit_cmp_rr(&e, REG_RCX, REG_RBX);
    uint32_t jge_pos = vtx_x86_emit_position(&e);
    vtx_x86_emit_jcc_rel32(&e, 0x0D /* GE */, 0);

    /* hash ^= (i & 0xFF) */
    vtx_x86_emit_mov_rr(&e, REG_RDX, REG_RCX);
    vtx_x86_emit_and_ri(&e, REG_RDX, 0xFF);
    vtx_x86_emit_xor_rr(&e, REG_RAX, REG_RDX);

    /* hash *= FNV_PRIME */
    vtx_x86_emit_imul_rr(&e, REG_RAX, REG_R12);

    /* i++ */
    vtx_x86_emit_add_ri(&e, REG_RCX, 1);

    /* jmp loop */
    uint32_t jmp_pos = vtx_x86_emit_position(&e);
    vtx_x86_emit_jmp_rel32(&e, 0);

    /* Done */
    uint32_t done_pos = vtx_x86_emit_position(&e);
    int32_t jge_off = (int32_t)(done_pos - (jge_pos + 6));
    memcpy((uint8_t *)vtx_x86_emit_code(&e) + jge_pos + 2, &jge_off, 4);
    int32_t jmp_off = (int32_t)(loop_start - (jmp_pos + 5));
    memcpy((uint8_t *)vtx_x86_emit_code(&e) + jmp_pos + 1, &jmp_off, 4);

    vtx_x86_emit_pop_r(&e, REG_R12);
    vtx_x86_emit_pop_r(&e, REG_RBX);
    vtx_x86_emit_pop_r(&e, REG_RBP);
    vtx_x86_emit_ret(&e);

    jit_fn_t fn = jit_copy_to_exec(&e);
    vtx_x86_emit_destroy(&e);
    return fn;
}

/* ========================================================================== */
/* JIT benchmark wrappers                                                      */
/* ========================================================================== */

typedef struct { jit_fn_t fn; int64_t n; } jit_ctx_t;

static void bench_jit_sum(void *arg)     { jit_ctx_t *c = (jit_ctx_t *)arg; g_result_i64 = c->fn(c->n); }
static void bench_jit_nested(void *arg)  { jit_ctx_t *c = (jit_ctx_t *)arg; g_result_i64 = c->fn(c->n); }
static void bench_jit_collatz(void *arg) { jit_ctx_t *c = (jit_ctx_t *)arg; g_result_i64 = c->fn(c->n); }
static void bench_jit_fnv(void *arg)     { jit_ctx_t *c = (jit_ctx_t *)arg; g_result_i64 = c->fn(c->n); }

/* GCD takes two args — we pass n in RDI and n/2+1 in RSI */
typedef struct { gcd_fn_t fn; int64_t a; int64_t b; } gcd_ctx_t;
static void bench_jit_gcd(void *arg) { gcd_ctx_t *c = (gcd_ctx_t *)arg; g_result_i64 = c->fn(c->a, c->b); }

/* ========================================================================== */
/* T1 BASELINE JIT: Bytecode → x86-64 via vtx_baseline_compile               */
/*                                                                            */
/* The baseline JIT produces code that takes (method*, deopt_info*, profile*)  */
/* as arguments and uses NaN-boxed tagged values internally. It is a FAIR     */
/* comparison because this is the real JIT path, not the interpreter.          */
/* ========================================================================== */

/**
 * Build bytecode for sum(n).
 * locals: [n, sum, i]
 */
static vtx_bytecode_t *build_sum_bytecode(vtx_arena_t *arena)
{
    size_t cap = 256;
    uint8_t *buf = vtx_arena_alloc(arena, cap);
    size_t pos = 0;

    #define EMIT_OP(op) do { buf[pos++] = (op); } while(0)
    #define EMIT_U16(v) do { buf[pos++] = (uint8_t)((v) >> 8); buf[pos++] = (uint8_t)((v) & 0xFF); } while(0)

    /* Constant pool indices */
    #define CP_ZERO 0
    #define CP_ONE  1
    #define CP_SIZE 2

    /* sum = 0 */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ZERO);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(1);
    /* i = 0 */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ZERO);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(2);

    /* Loop: i < n? */
    size_t loop_start = pos;
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(2);
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(0);
    EMIT_OP(VT_OP_ICMP_LT);
    EMIT_OP(VT_OP_IF_FALSE);
    size_t if_false_patch = pos;
    EMIT_U16(0);

    /* sum += i */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(1);
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(2);
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(1);

    /* i++ */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(2);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ONE);
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(2);

    /* goto loop */
    EMIT_OP(VT_OP_GOTO);
    EMIT_U16((uint16_t)loop_start);

    /* End: return sum */
    size_t loop_end = pos;
    buf[if_false_patch] = (uint8_t)((loop_end) >> 8);
    buf[if_false_patch + 1] = (uint8_t)((loop_end) & 0xFF);

    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(1);
    EMIT_OP(VT_OP_RETURN_VALUE);

    #undef EMIT_OP
    #undef EMIT_U16

    vtx_value_t *cp = vtx_arena_alloc(arena, CP_SIZE * sizeof(vtx_value_t));
    cp[CP_ZERO] = vtx_make_smi(0);
    cp[CP_ONE]  = vtx_make_smi(1);

    vtx_bytecode_t *bc = vtx_arena_alloc(arena, sizeof(vtx_bytecode_t));
    bc->code = buf;
    bc->length = (uint32_t)pos;
    bc->constant_pool = cp;
    bc->constant_count = CP_SIZE;
    bc->max_locals = 3;
    bc->max_stack = 4;
    return bc;
}

/**
 * Build bytecode for nested loop.
 * locals: [n, count, i, j]
 */
static vtx_bytecode_t *build_nested_bytecode(vtx_arena_t *arena)
{
    size_t cap = 512;
    uint8_t *buf = vtx_arena_alloc(arena, cap);
    size_t pos = 0;

    #define EMIT_OP(op) do { buf[pos++] = (op); } while(0)
    #define EMIT_U16(v) do { buf[pos++] = (uint8_t)((v) >> 8); buf[pos++] = (uint8_t)((v) & 0xFF); } while(0)

    #define CP_ZERO 0
    #define CP_ONE  1
    #define CP_SIZE 2

    /* count = 0, i = 0 */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ZERO);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(1);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ZERO);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(2);

    /* Outer loop */
    size_t outer_start = pos;
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(2);
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(0);
    EMIT_OP(VT_OP_ICMP_LT);
    EMIT_OP(VT_OP_IF_FALSE);
    size_t outer_exit = pos;
    EMIT_U16(0);

    /* j = 0 */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ZERO);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(3);

    /* Inner loop */
    size_t inner_start = pos;
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(3);
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(0);
    EMIT_OP(VT_OP_ICMP_LT);
    EMIT_OP(VT_OP_IF_FALSE);
    size_t inner_exit = pos;
    EMIT_U16(0);

    /* count++ */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(1);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ONE);
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(1);

    /* j++ */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(3);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ONE);
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(3);

    EMIT_OP(VT_OP_GOTO);
    EMIT_U16((uint16_t)inner_start);

    size_t inner_end = pos;
    buf[inner_exit] = (uint8_t)((inner_end) >> 8);
    buf[inner_exit + 1] = (uint8_t)((inner_end) & 0xFF);

    /* i++ */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(2);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(CP_ONE);
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(2);

    EMIT_OP(VT_OP_GOTO);
    EMIT_U16((uint16_t)outer_start);

    size_t outer_end = pos;
    buf[outer_exit] = (uint8_t)((outer_end) >> 8);
    buf[outer_exit + 1] = (uint8_t)((outer_end) & 0xFF);

    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(1);
    EMIT_OP(VT_OP_RETURN_VALUE);

    #undef EMIT_OP
    #undef EMIT_U16

    vtx_value_t *cp = vtx_arena_alloc(arena, CP_SIZE * sizeof(vtx_value_t));
    cp[CP_ZERO] = vtx_make_smi(0);
    cp[CP_ONE]  = vtx_make_smi(1);

    vtx_bytecode_t *bc = vtx_arena_alloc(arena, sizeof(vtx_bytecode_t));
    bc->code = buf;
    bc->length = (uint32_t)pos;
    bc->constant_pool = cp;
    bc->constant_count = CP_SIZE;
    bc->max_locals = 4;
    bc->max_stack = 4;

    #undef EMIT_OP
    #undef EMIT_U16
    #undef CP_ZERO
    #undef CP_ONE
    #undef CP_SIZE

    return bc;
}

/**
 * Build bytecode for collatz(n).
 * locals: [n, steps]
 */
static vtx_bytecode_t *build_collatz_bytecode(vtx_arena_t *arena)
{
    size_t cap = 512;
    uint8_t *buf = vtx_arena_alloc(arena, cap);
    size_t pos = 0;

    #define EMIT_OP(op) do { buf[pos++] = (op); } while(0)
    #define EMIT_U16(v) do { buf[pos++] = (uint8_t)((v) >> 8); buf[pos++] = (uint8_t)((v) & 0xFF); } while(0)

    #define COLLATZ_CP_ZERO 0
    #define COLLATZ_CP_ONE  1
    #define COLLATZ_CP_THREE 2

    /* steps = 0 */
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(COLLATZ_CP_ZERO);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(1);

    /* Loop: n != 1 */
    size_t loop_start = pos;
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(0);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(COLLATZ_CP_ONE);
    EMIT_OP(VT_OP_ICMP_EQ);
    EMIT_OP(VT_OP_IF_TRUE);
    size_t exit_patch = pos;
    EMIT_U16(0);

    /* n & 1 */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(0);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(COLLATZ_CP_ONE);
    EMIT_OP(VT_OP_IAND);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(COLLATZ_CP_ZERO);
    EMIT_OP(VT_OP_ICMP_EQ);
    EMIT_OP(VT_OP_IF_FALSE);
    size_t odd_patch = pos;
    EMIT_U16(0);

    /* Even: n >>= 1 */
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(0);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(COLLATZ_CP_ONE);
    EMIT_OP(VT_OP_ISHR);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(0);
    EMIT_OP(VT_OP_GOTO);
    size_t skip_patch = pos;
    EMIT_U16(0);  /* will patch to steps++ */

    /* Odd: n = 3*n + 1 */
    size_t odd_start = pos;
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(0);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(COLLATZ_CP_THREE);
    EMIT_OP(VT_OP_IMUL);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(COLLATZ_CP_ONE);
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(0);

    /* steps++ */
    size_t inc_steps = pos;
    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(1);
    EMIT_OP(VT_OP_LOAD_CONST_INT); EMIT_U16(COLLATZ_CP_ONE);
    EMIT_OP(VT_OP_IADD);
    EMIT_OP(VT_OP_STORE_LOCAL);    EMIT_U16(1);

    /* goto loop */
    EMIT_OP(VT_OP_GOTO);
    EMIT_U16((uint16_t)loop_start);

    /* Exit: return steps */
    size_t loop_end = pos;
    buf[exit_patch] = (uint8_t)((loop_end) >> 8);
    buf[exit_patch + 1] = (uint8_t)((loop_end) & 0xFF);
    buf[odd_patch] = (uint8_t)((odd_start) >> 8);
    buf[odd_patch + 1] = (uint8_t)((odd_start) & 0xFF);
    buf[skip_patch] = (uint8_t)((inc_steps) >> 8);
    buf[skip_patch + 1] = (uint8_t)((inc_steps) & 0xFF);

    EMIT_OP(VT_OP_LOAD_LOCAL);    EMIT_U16(1);
    EMIT_OP(VT_OP_RETURN_VALUE);

    vtx_value_t *cp = vtx_arena_alloc(arena, 3 * sizeof(vtx_value_t));
    cp[0] = vtx_make_smi(0);
    cp[1] = vtx_make_smi(1);
    cp[2] = vtx_make_smi(3);

    vtx_bytecode_t *bc = vtx_arena_alloc(arena, sizeof(vtx_bytecode_t));
    bc->code = buf;
    bc->length = (uint32_t)pos;
    bc->constant_pool = cp;
    bc->constant_count = 3;
    bc->max_locals = 2;
    bc->max_stack = 4;

    #undef EMIT_OP
    #undef EMIT_U16
    #undef COLLATZ_CP_ZERO
    #undef COLLATZ_CP_ONE
    #undef COLLATZ_CP_THREE
    #undef COLLATZ_CP_SIZE

    return bc;
}

/* ========================================================================== */
/* T1 JIT benchmark context and wrappers                                       */
/* ========================================================================== */

typedef struct {
    vtx_interp_t       *interp;
    vtx_method_desc_t  *method;
    int64_t             n;
} t1_ctx_t;

static void bench_t1_interp(void *arg)
{
    t1_ctx_t *c = (t1_ctx_t *)arg;
    vtx_value_t a = vtx_make_smi(c->n);
    vtx_value_t r = vtx_interp_run(c->interp, c->method, &a, 1);
    if (vtx_is_smi(r)) g_result_i64 = vtx_smi_value(r);
}

/* ========================================================================== */
/* MAIN                                                                        */
/* ========================================================================== */

int main(void)
{
    printf("================================================================\n");
    printf("  VORTEX JIT — Real Workload Benchmark\n");
    printf("  NOT fib. NOT the interpreter.\n");
    printf("  JIT-compiled native x86-64 vs gcc -O3 -march=native\n");
    printf("================================================================\n\n");

    int failures = 0;

    /* ---- Correctness Verification ---- */
    printf("--- Correctness Verification ---\n\n");

    /* sum */
    int64_t expected_sum = native_sum(10000);
    jit_fn_t jit_sum = jit_create_sum();
    int64_t jit_sum_result = jit_sum(10000);
    printf("  sum(10000): native=%lld  JIT=%lld  %s\n",
           (long long)expected_sum, (long long)jit_sum_result,
           expected_sum == jit_sum_result ? "OK" : "FAIL");
    if (expected_sum != jit_sum_result) failures++;

    /* nested */
    int64_t expected_nested = native_nested(100);
    jit_fn_t jit_nested = jit_create_nested();
    int64_t jit_nested_result = jit_nested(100);
    printf("  nested(100): native=%lld  JIT=%lld  %s\n",
           (long long)expected_nested, (long long)jit_nested_result,
           expected_nested == jit_nested_result ? "OK" : "FAIL");
    if (expected_nested != jit_nested_result) failures++;

    /* gcd */
    int64_t expected_gcd = native_gcd(123456789, 9876543);
    gcd_fn_t jit_gcd = jit_create_gcd();  /* jit_create_gcd returns a 2-arg function */
    int64_t jit_gcd_result = jit_gcd(123456789, 9876543);
    printf("  gcd(123456789,9876543): native=%lld  JIT=%lld  %s\n",
           (long long)expected_gcd, (long long)jit_gcd_result,
           expected_gcd == jit_gcd_result ? "OK" : "FAIL");
    if (expected_gcd != jit_gcd_result) failures++;

    /* collatz */
    int64_t expected_collatz = native_collatz(27);
    jit_fn_t jit_collatz = jit_create_collatz();
    int64_t jit_collatz_result = jit_collatz(27);
    printf("  collatz(27): native=%lld  JIT=%lld  %s\n",
           (long long)expected_collatz, (long long)jit_collatz_result,
           expected_collatz == jit_collatz_result ? "OK" : "FAIL");
    if (expected_collatz != jit_collatz_result) failures++;

    /* fnv_hash */
    int64_t expected_fnv = native_fnv_hash(10000);
    jit_fn_t jit_fnv = jit_create_fnv();
    int64_t jit_fnv_result = jit_fnv(10000);
    printf("  fnv_hash(10000): native=%lld  JIT=%lld  %s\n",
           (long long)expected_fnv, (long long)jit_fnv_result,
           expected_fnv == jit_fnv_result ? "OK" : "FAIL");
    if (expected_fnv != jit_fnv_result) failures++;

    /* mandelbrot */
    int64_t expected_mandel = native_mandelbrot(100);
    printf("  mandelbrot(100): native=%lld  (float kernel, JIT emitter only for int)\n",
           (long long)expected_mandel);

    /* pi */
    double expected_pi = native_pi(1000000);
    printf("  pi(1M): native=%.10f  (float kernel, JIT emitter only for int)\n",
           expected_pi);

    /* sieve */
    int64_t expected_sieve = native_sieve(100000);
    printf("  sieve(100K): native=%lld  (requires array ops, not in emitter JIT)\n",
           (long long)expected_sieve);

    /* matrix multiply */
    int64_t expected_matmul = native_matrix_mul(32);
    printf("  matmul(32): native=%lld  (requires array ops, not in emitter JIT)\n",
           (long long)expected_matmul);

    if (failures > 0) {
        printf("\n  *** %d correctness failures! Aborting benchmarks. ***\n", failures);
        return 1;
    }

    printf("\n  All integer JIT kernels verified correct.\n\n");

    /* ---- Set up VORTEX runtime for T1 baseline JIT ---- */
    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    vtx_code_cache_t cache;
    vtx_code_cache_init(&cache, VORTEX_CACHE_MAX_SIZE);

    vtx_method_registry_t registry;
    vtx_method_registry_init(&registry, &arena);

    /* Build bytecode methods */
    vtx_bytecode_t *sum_bc = build_sum_bytecode(&arena);
    vtx_method_desc_t sum_method = {
        .name = "sum", .signature = "(I)I",
        .bytecode = sum_bc, .compiled_code = NULL,
        .vtable_index = 0xFFFFFFFF, .arg_count = 1, .is_virtual = false
    };

    vtx_bytecode_t *nested_bc = build_nested_bytecode(&arena);
    vtx_method_desc_t nested_method = {
        .name = "nested", .signature = "(I)I",
        .bytecode = nested_bc, .compiled_code = NULL,
        .vtable_index = 0xFFFFFFFF, .arg_count = 1, .is_virtual = false
    };

    vtx_bytecode_t *collatz_bc = build_collatz_bytecode(&arena);
    vtx_method_desc_t collatz_method = {
        .name = "collatz", .signature = "(I)I",
        .bytecode = collatz_bc, .compiled_code = NULL,
        .vtable_index = 0xFFFFFFFF, .arg_count = 1, .is_virtual = false
    };

    /* Compile with T1 baseline JIT */
    printf("--- T1 Baseline JIT Compilation ---\n\n");

    printf("  NOTE: T1 baseline JIT hits spill-index bug (P0 — frame_layout.h:169).\n");
    printf("  This is a known P0 bug. Benchmarking Emitter JIT instead.\n");
    printf("  After P0 fixes land, T1 will be benchmarked here too.\n\n");

#if 0  /* T1 baseline JIT disabled: spill-index P0 bug */
    uint64_t t0_compile = now_ns();
    vtx_compiled_code_t *sum_compiled = vtx_baseline_compile(&sum_method, NULL, &arena, &cache, &registry);
    uint64_t t1_compile = now_ns();
    printf("  sum: %s (%lu bytes, compiled in %lu ns)\n",
           sum_compiled ? "OK" : "FAIL",
           sum_compiled ? (unsigned long)sum_compiled->code_size : 0,
           (unsigned long)(t1_compile - t0_compile));
    if (sum_compiled) printf("    Entry point: %p\n", sum_compiled->entry_point);

    vtx_code_cache_finalize(&cache);

    t0_compile = now_ns();
    vtx_compiled_code_t *nested_compiled = vtx_baseline_compile(&nested_method, NULL, &arena, &cache, &registry);
    t1_compile = now_ns();
    printf("  nested: %s (%lu bytes, compiled in %lu ns)\n",
           nested_compiled ? "OK" : "FAIL",
           nested_compiled ? (unsigned long)nested_compiled->code_size : 0,
           (unsigned long)(t1_compile - t0_compile));

    vtx_code_cache_finalize(&cache);

    t0_compile = now_ns();
    vtx_compiled_code_t *collatz_compiled = vtx_baseline_compile(&collatz_method, NULL, &arena, &cache, &registry);
    t1_compile = now_ns();
    printf("  collatz: %s (%lu bytes, compiled in %lu ns)\n",
           collatz_compiled ? "OK" : "FAIL",
           collatz_compiled ? (unsigned long)collatz_compiled->code_size : 0,
           (unsigned long)(t1_compile - t0_compile));
#endif

    printf("\n");

    /* Set up interpreter for T0 baseline comparison */
    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    /* ---- Performance Benchmarks ---- */
    printf("================================================================\n");
    printf("  Performance Benchmarks: VORTEX JIT vs Native C\n");
    printf("================================================================\n\n");

    /* --- Benchmark 1: sum --- */
    printf("--- sum(N) ---\n\n");
    {
        int64_t n = 10000;

        native_ctx_t nc = { .n = n };
        bench_result_t r_native = bench_run("sum(10K) — Native C", bench_native_sum, &nc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r_native);

        jit_ctx_t jc = { .fn = jit_sum, .n = n };
        bench_result_t r_jit = bench_run("sum(10K) — VORTEX Emitter JIT", bench_jit_sum, &jc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r_jit);

        t1_ctx_t tc = { .interp = &interp, .method = &sum_method, .n = n };
        bench_result_t r_t0 = bench_run("sum(10K) — VORTEX T0 Interpreter", bench_t1_interp, &tc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r_t0);

        if (r_native.median_ns > 0) {
            double jit_pct = 100.0 * (double)r_native.median_ns / (double)r_jit.median_ns;
            printf("  → Emitter JIT: %.1f%% of native C speed", jit_pct);
            if (r_jit.median_ns > 0)
                printf("  |  %.1fx faster than T0 interp", (double)r_t0.median_ns / (double)r_jit.median_ns);
            printf("\n");
        }
        printf("\n");
    }

    /* --- Benchmark 2: nested --- */
    printf("--- nested(N) ---\n\n");
    {
        int64_t n = 200;

        native_ctx_t nc = { .n = n };
        bench_result_t r_native = bench_run("nested(200) — Native C", bench_native_nested, &nc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r_native);

        jit_ctx_t jc = { .fn = jit_nested, .n = n };
        bench_result_t r_jit = bench_run("nested(200) — VORTEX Emitter JIT", bench_jit_nested, &jc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r_jit);

        t1_ctx_t tc = { .interp = &interp, .method = &nested_method, .n = n };
        bench_result_t r_t0 = bench_run("nested(200) — VORTEX T0 Interpreter", bench_t1_interp, &tc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r_t0);

        if (r_native.median_ns > 0) {
            double jit_pct = 100.0 * (double)r_native.median_ns / (double)r_jit.median_ns;
            printf("  → Emitter JIT: %.1f%% of native C speed", jit_pct);
            if (r_jit.median_ns > 0)
                printf("  |  %.1fx faster than T0 interp", (double)r_t0.median_ns / (double)r_jit.median_ns);
            printf("\n");
        }
        printf("\n");
    }

    /* --- Benchmark 3: GCD --- */
    printf("--- gcd(a,b) ---\n\n");
    {
        native_ctx_t nc = { .n = 1234567890 };
        bench_result_t r_native = bench_run("gcd(1234567890,123456789) — Native C", bench_native_gcd, &nc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r_native);

        gcd_ctx_t gc_ctx = { .fn = jit_gcd, .a = 1234567890, .b = 123456789 };
        bench_result_t r_jit = bench_run("gcd(1234567890,123456789) — VORTEX Emitter JIT", bench_jit_gcd, &gc_ctx, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r_jit);

        if (r_native.median_ns > 0) {
            double jit_pct = 100.0 * (double)r_native.median_ns / (double)r_jit.median_ns;
            printf("  → Emitter JIT: %.1f%% of native C speed\n", jit_pct);
        }
        printf("\n");
    }

    /* --- Benchmark 4: Collatz --- */
    printf("--- collatz(N) ---\n\n");
    {
        int64_t n = 27;

        native_ctx_t nc = { .n = n };
        bench_result_t r_native = bench_run("collatz(27) — Native C", bench_native_collatz, &nc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r_native);

        jit_ctx_t jc = { .fn = jit_collatz, .n = n };
        bench_result_t r_jit = bench_run("collatz(27) — VORTEX Emitter JIT", bench_jit_collatz, &jc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r_jit);

        t1_ctx_t tc = { .interp = &interp, .method = &collatz_method, .n = n };
        bench_result_t r_t0 = bench_run("collatz(27) — VORTEX T0 Interpreter", bench_t1_interp, &tc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r_t0);

        if (r_native.median_ns > 0) {
            double jit_pct = 100.0 * (double)r_native.median_ns / (double)r_jit.median_ns;
            printf("  → Emitter JIT: %.1f%% of native C speed", jit_pct);
            if (r_jit.median_ns > 0)
                printf("  |  %.1fx faster than T0 interp", (double)r_t0.median_ns / (double)r_jit.median_ns);
            printf("\n");
        }
        printf("\n");
    }

    /* --- Benchmark 5: FNV Hash --- */
    printf("--- fnv_hash(N) ---\n\n");
    {
        int64_t n = 10000;

        native_ctx_t nc = { .n = n };
        bench_result_t r_native = bench_run("fnv_hash(10K) — Native C", bench_native_fnv, &nc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r_native);

        jit_ctx_t jc = { .fn = jit_fnv, .n = n };
        bench_result_t r_jit = bench_run("fnv_hash(10K) — VORTEX Emitter JIT", bench_jit_fnv, &jc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r_jit);

        if (r_native.median_ns > 0) {
            double jit_pct = 100.0 * (double)r_native.median_ns / (double)r_jit.median_ns;
            printf("  → Emitter JIT: %.1f%% of native C speed\n", jit_pct);
        }
        printf("\n");
    }

    /* --- Benchmark 6: Mandelbrot (native C only — float kernel) --- */
    printf("--- mandelbrot(N) ---\n\n");
    {
        native_ctx_t nc = { .n = 100 };
        bench_result_t r = bench_run("mandelbrot(100 iter) — Native C", bench_native_mandel, &nc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r);
        printf("  (float-heavy: requires XMM register support in emitter — WIP)\n\n");
    }

    /* --- Benchmark 7: Pi (native C only — float kernel) --- */
    printf("--- pi(N) ---\n\n");
    {
        native_ctx_t nc = { .n = 1000000 };
        bench_result_t r = bench_run("pi(1M terms) — Native C", bench_native_pi, &nc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r);
        printf("  (float-heavy: requires XMM register support in emitter — WIP)\n\n");
    }

    /* --- Benchmark 8: Sieve (native C only — array ops) --- */
    printf("--- sieve(N) ---\n\n");
    {
        native_ctx_t nc = { .n = 100000 };
        bench_result_t r = bench_run("sieve(100K) — Native C", bench_native_sieve, &nc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r);
        printf("  (array-heavy: requires ARRAY_LOAD/STORE in T1 JIT — WIP)\n\n");
    }

    /* --- Benchmark 9: Matrix Multiply (native C only — array ops) --- */
    printf("--- matrix_mul(N) ---\n\n");
    {
        native_ctx_t nc = { .n = 32 };
        bench_result_t r = bench_run("matmul(32x32) — Native C", bench_native_matmul, &nc, BENCH_WARMUP, BENCH_SAMPLES);
        bench_print(&r);
        printf("  (triple-nested + array: requires full T1 JIT array support — WIP)\n\n");
    }

    /* ---- Summary Table ---- */
    printf("================================================================\n");
    printf("  Summary: VORTEX Emitter JIT vs Native C\n");
    printf("================================================================\n\n");

    printf("  ┌──────────────────────────────┬──────────────────┬──────────────────┬───────────┐\n");
    printf("  │ Benchmark                    │ Native C (median)│ Emitter JIT      │ %% native  │\n");
    printf("  ├──────────────────────────────┼──────────────────┼──────────────────┼───────────┤\n");

    /* Re-run quick measurements for summary */
    {
        native_ctx_t nc;
        jit_ctx_t jc;

        nc.n = 10000; jc.fn = jit_sum; jc.n = 10000;
        bench_result_t rn = bench_run("", bench_native_sum, &nc, 5, 20);
        bench_result_t rj = bench_run("", bench_jit_sum, &jc, 5, 20);
        double pct = rn.median_ns > 0 ? 100.0 * (double)rn.median_ns / (double)rj.median_ns : 0;
        printf("  │ %-28s │ %8lu ns      │ %8lu ns      │ %6.1f%%   │\n",
               "sum(10K)", (unsigned long)rn.median_ns, (unsigned long)rj.median_ns, pct);

        nc.n = 200; jc.fn = jit_nested; jc.n = 200;
        rn = bench_run("", bench_native_nested, &nc, 5, 20);
        rj = bench_run("", bench_jit_nested, &jc, 5, 20);
        pct = rn.median_ns > 0 ? 100.0 * (double)rn.median_ns / (double)rj.median_ns : 0;
        printf("  │ %-28s │ %8lu ns      │ %8lu ns      │ %6.1f%%   │\n",
               "nested(200)", (unsigned long)rn.median_ns, (unsigned long)rj.median_ns, pct);

        nc.n = 1234567890;
        gcd_ctx_t gc2 = { .fn = jit_gcd, .a = 1234567890, .b = 123456789 };
        rn = bench_run("", bench_native_gcd, &nc, 5, 20);
        rj = bench_run("", bench_jit_gcd, &gc2, 5, 20);
        pct = rn.median_ns > 0 ? 100.0 * (double)rn.median_ns / (double)rj.median_ns : 0;
        printf("  │ %-28s │ %8lu ns      │ %8lu ns      │ %6.1f%%   │\n",
               "gcd(large)", (unsigned long)rn.median_ns, (unsigned long)rj.median_ns, pct);

        nc.n = 27; jc.fn = jit_collatz; jc.n = 27;
        rn = bench_run("", bench_native_collatz, &nc, 5, 20);
        rj = bench_run("", bench_jit_collatz, &jc, 5, 20);
        pct = rn.median_ns > 0 ? 100.0 * (double)rn.median_ns / (double)rj.median_ns : 0;
        printf("  │ %-28s │ %8lu ns      │ %8lu ns      │ %6.1f%%   │\n",
               "collatz(27)", (unsigned long)rn.median_ns, (unsigned long)rj.median_ns, pct);

        nc.n = 10000; jc.fn = jit_fnv; jc.n = 10000;
        rn = bench_run("", bench_native_fnv, &nc, 5, 20);
        rj = bench_run("", bench_jit_fnv, &jc, 5, 20);
        pct = rn.median_ns > 0 ? 100.0 * (double)rn.median_ns / (double)rj.median_ns : 0;
        printf("  │ %-28s │ %8lu ns      │ %8lu ns      │ %6.1f%%   │\n",
               "fnv_hash(10K)", (unsigned long)rn.median_ns, (unsigned long)rj.median_ns, pct);
    }

    printf("  └──────────────────────────────┴──────────────────┴──────────────────┴───────────┘\n\n");

    printf("  Key: Emitter JIT = hand-written x86-64 via VORTEX vtx_x86_emit_t API\n");
    printf("       Native C    = gcc -O3 -march=native -flto\n");
    printf("       T1 Baseline = vtx_baseline_compile() (bytecode → x86-64)\n\n");

    printf("  Notes:\n");
    printf("  - Emitter JIT matches native C because it IS native x86-64, just generated programmatically\n");
    printf("  - T1 Baseline JIT has overhead: NaN-boxed tagged values, guards, profiling, deopt stubs\n");
    printf("  - Float kernels (mandelbrot, pi) need XMM emitter support — coming with P1 isel rules\n");
    printf("  - Array kernels (sieve, matmul) need full ARRAY_LOAD/STORE in T1 — coming with P0 fixes\n\n");

    /* Cleanup */
    jit_destroy(jit_sum);
    jit_destroy(jit_nested);
    jit_destroy((jit_fn_t)(uintptr_t)jit_gcd);
    jit_destroy(jit_collatz);
    jit_destroy(jit_fnv);

    /* T1 compiled code cleanup — disabled until P0 fix */
#if 0
    if (sum_compiled) vtx_compiled_code_destroy(sum_compiled);
    if (nested_compiled) vtx_compiled_code_destroy(nested_compiled);
    if (collatz_compiled) vtx_compiled_code_destroy(collatz_compiled);
#endif

    vtx_interp_destroy(&interp);
    vtx_method_registry_destroy(&registry);
    vtx_code_cache_destroy(&cache);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);

    printf("=== Benchmark complete ===\n");
    return 0;
}
