/**
 * bench_c_comparison.c — Standalone C benchmark for honest comparison
 *
 * Compile with:
 *   gcc -O3 -march=native -flto -o bench_c_comparison bench_c_comparison.c -lm
 *
 * This provides the native C baseline that VORTEX JIT benchmarks should
 * be compared against. It uses honest methodology:
 *   1. Varying inputs to prevent constant folding
 *   2. Results consumed (accumulated) to prevent dead code elimination
 *   3. 1M+ iterations for reliable sub-microsecond timing
 *   4. Compiled with maximum optimization (-O3 -march=native -flto)
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

/* ========================================================================== */
/* Benchmark targets                                                           */
/* ========================================================================== */

/* Iterative Fibonacci */
static int64_t fib_iter(int64_t n)
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

/* Recursive Fibonacci */
static int64_t fib_rec(int64_t n)
{
    if (n <= 1) return n;
    return fib_rec(n - 1) + fib_rec(n - 2);
}

/* Sum 1..N */
static int64_t sum_n(int64_t n)
{
    int64_t sum = 0;
    for (int64_t i = 1; i <= n; i++) {
        sum += i;
    }
    return sum;
}

/* Array-style sum */
static int64_t array_sum(int64_t n)
{
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) {
        sum += i;
    }
    return sum;
}

/* Nested loop (matrix-style) */
static int64_t nested_loop(int64_t n)
{
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) {
        for (int64_t j = 0; j < n; j++) {
            sum += 1;
        }
    }
    return sum;
}

/* Matrix multiply (64x64) — numeric kernel */
#define MAT_SIZE 64
static void mat_multiply(double * restrict C,
                          const double * restrict A,
                          const double * restrict B,
                          int n)
{
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < n; k++) {
            double a_ik = A[i * n + k];
            for (int j = 0; j < n; j++) {
                C[i * n + j] += a_ik * B[k * n + j];
            }
        }
    }
}

/* ========================================================================== */
/* Timing utilities                                                            */
/* ========================================================================== */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ========================================================================== */
/* Benchmark runner                                                            */
/* ========================================================================== */

static void run_bench(const char *name, int64_t iters,
                       int64_t (*fn)(int64_t), int64_t n,
                       int warmup)
{
    /* Warmup */
    for (int i = 0; i < warmup; i++) {
        volatile int64_t r = fn(n + (i % 3));
        (void)r;
    }

    /* Measure with varying input */
    int64_t accum = 0;
    uint64_t t0 = now_ns();
    for (int64_t i = 0; i < iters; i++) {
        int64_t nn = n + (i % 5); /* vary input to prevent constant folding */
        accum += fn(nn);          /* consume result */
    }
    uint64_t t1 = now_ns();

    double ns_per_iter = (double)(t1 - t0) / (double)iters;
    printf("  %-35s  %10.1f ns/iter  (accum=%ld)\n",
           name, ns_per_iter, (long)accum);
}

/* ========================================================================== */
/* Main                                                                         */
/* ========================================================================== */

int main(void)
{
    printf("=== VORTEX C Comparison Benchmark ===\n");
    printf("Compiled with: gcc -O3 -march=native -flto\n\n");

    printf("--- Fibonacci ---\n");
    run_bench("fib_iter(20..24) x1M",   1000000, fib_iter,   20, 100);
    run_bench("fib_rec(20..24) x100",        100, fib_rec,    20,  10);

    printf("\n--- Loop-intensive ---\n");
    run_bench("sum(1000..1004) x1M",     1000000, sum_n,     1000, 100);
    run_bench("array_sum(1000..1004) x1M", 1000000, array_sum, 1000, 100);

    printf("\n--- Nested loops ---\n");
    run_bench("nested(100..104) x100K",  100000, nested_loop, 100, 100);

    printf("\n--- Numeric kernel (64x64 matrix multiply) ---\n");
    {
        double A[MAT_SIZE * MAT_SIZE];
        double B[MAT_SIZE * MAT_SIZE];
        double C[MAT_SIZE * MAT_SIZE];

        /* Initialize */
        for (int i = 0; i < MAT_SIZE * MAT_SIZE; i++) {
            A[i] = (double)(i % 10) * 0.1;
            B[i] = (double)(i % 7) * 0.1;
            C[i] = 0.0;
        }

        int mat_iters = 100000;
        int warmup = 100;

        /* Warmup */
        for (int i = 0; i < warmup; i++) {
            for (int j = 0; j < MAT_SIZE * MAT_SIZE; j++) C[j] = 0.0;
            mat_multiply(C, A, B, MAT_SIZE);
        }

        /* Measure */
        double total = 0.0;
        uint64_t t0 = now_ns();
        for (int i = 0; i < mat_iters; i++) {
            for (int j = 0; j < MAT_SIZE * MAT_SIZE; j++) C[j] = 0.0;
            mat_multiply(C, A, B, MAT_SIZE);
            total += C[0]; /* consume result */
        }
        uint64_t t1 = now_ns();

        double ns_per_iter = (double)(t1 - t0) / (double)mat_iters;
        printf("  %-35s  %10.1f ns/iter  (total=%.1f)\n",
               "matmul_64x64 x100K", ns_per_iter, total);
    }

    printf("\n=== Done ===\n");
    return 0;
}
