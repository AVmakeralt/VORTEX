#ifndef VORTEX_BENCH_FRAMEWORK_H
#define VORTEX_BENCH_FRAMEWORK_H

#include <stdint.h>
#include <stddef.h>

/**
 * VORTEX Benchmark Framework
 *
 * Provides:
 *   - Warmup iterations to stabilize CPU caches and branch prediction
 *   - Measurement iterations with nanosecond timing via clock_gettime
 *   - Median + 95th percentile reporting
 *   - vtx_bench_t struct to hold configuration and results
 */

/* ========================================================================== */
/* Benchmark result                                                             */
/* ========================================================================== */

typedef struct {
    const char *name;           /* benchmark name */
    uint64_t     median_ns;     /* median iteration time in nanoseconds */
    uint64_t     p95_ns;        /* 95th percentile iteration time in nanoseconds */
    uint64_t     min_ns;        /* minimum iteration time */
    uint64_t     max_ns;        /* maximum iteration time */
    double       mean_ns;       /* mean iteration time */
    uint32_t     warmup_iters;  /* number of warmup iterations */
    uint32_t     measure_iters; /* number of measurement iterations */
} vtx_bench_result_t;

/* ========================================================================== */
/* Benchmark configuration                                                      */
/* ========================================================================== */

typedef struct {
    uint32_t warmup_iterations;  /* default: VTX_BENCH_WARMUP (10) */
    uint32_t measure_iterations; /* default: VTX_BENCH_ITERATIONS (100) */
} vtx_bench_t;

/* ========================================================================== */
/* Benchmark function type                                                      */
/* ========================================================================== */

typedef void (*vtx_bench_fn)(void *arg);

/* ========================================================================== */
/* API                                                                          */
/* ========================================================================== */

/**
 * Initialize a benchmark configuration with default settings.
 */
void vtx_bench_init(vtx_bench_t *bench);

/**
 * Run a benchmark: warmup + measure, compute median and p95.
 *
 * @param name     Benchmark name for reporting
 * @param fn       Benchmark function to call
 * @param arg      Opaque argument passed to fn
 * @return         Benchmark result with timing statistics
 */
vtx_bench_result_t vtx_bench_run(const char *name, vtx_bench_fn fn, void *arg);

/**
 * Run a benchmark with explicit configuration.
 */
vtx_bench_result_t vtx_bench_run_cfg(const vtx_bench_t *cfg,
                                      const char *name,
                                      vtx_bench_fn fn,
                                      void *arg);

/**
 * Print a benchmark result to stdout.
 */
void vtx_bench_report(const vtx_bench_result_t *result);

/**
 * Compare two benchmark results (e.g., T0 vs native).
 * Prints a comparison table to stdout.
 */
void vtx_bench_compare(const char *label_a, const vtx_bench_result_t *a,
                        const char *label_b, const vtx_bench_result_t *b);

#endif /* VORTEX_BENCH_FRAMEWORK_H */
