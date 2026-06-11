#include "bench_framework.h"
#include "vortex_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========================================================================== */
/* Comparator for qsort (ascending uint64_t)                                   */
/* ========================================================================== */

static int cmp_uint64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return  1;
    return 0;
}

/* ========================================================================== */
/* Nanosecond timing                                                            */
/* ========================================================================== */

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ========================================================================== */
/* Init                                                                         */
/* ========================================================================== */

void vtx_bench_init(vtx_bench_t *bench)
{
    bench->warmup_iterations = VTX_BENCH_WARMUP;
    bench->measure_iterations = VTX_BENCH_ITERATIONS;
}

/* ========================================================================== */
/* Run benchmark                                                                */
/* ========================================================================== */

vtx_bench_result_t vtx_bench_run_cfg(const vtx_bench_t *cfg,
                                      const char *name,
                                      vtx_bench_fn fn,
                                      void *arg)
{
    vtx_bench_result_t result;
    memset(&result, 0, sizeof(result));
    result.name = name;
    result.warmup_iters = cfg->warmup_iterations;
    result.measure_iters = cfg->measure_iterations;

    /* Allocate array for measurement samples */
    uint64_t *samples = (uint64_t *)malloc(cfg->measure_iterations * sizeof(uint64_t));
    if (samples == NULL) {
        fprintf(stderr, "vtx_bench: out of memory\n");
        return result;
    }

    /* Warmup phase: run without measuring to stabilize caches */
    for (uint32_t i = 0; i < cfg->warmup_iterations; i++) {
        fn(arg);
    }

    /* Measurement phase */
    for (uint32_t i = 0; i < cfg->measure_iterations; i++) {
        uint64_t t0 = now_ns();
        fn(arg);
        uint64_t t1 = now_ns();
        samples[i] = t1 - t0;
    }

    /* Sort samples for percentile computation */
    qsort(samples, cfg->measure_iterations, sizeof(uint64_t), cmp_uint64);

    /* Compute statistics */
    result.min_ns = samples[0];
    result.max_ns = samples[cfg->measure_iterations - 1];

    /* Median: middle value */
    uint32_t mid = cfg->measure_iterations / 2;
    if (cfg->measure_iterations % 2 == 0 && mid > 0) {
        result.median_ns = (samples[mid - 1] + samples[mid]) / 2;
    } else {
        result.median_ns = samples[mid];
    }

    /* 95th percentile */
    uint32_t p95_idx = (uint32_t)((double)cfg->measure_iterations * 0.95);
    if (p95_idx >= cfg->measure_iterations) {
        p95_idx = cfg->measure_iterations - 1;
    }
    result.p95_ns = samples[p95_idx];

    /* Mean */
    uint64_t sum = 0;
    for (uint32_t i = 0; i < cfg->measure_iterations; i++) {
        sum += samples[i];
    }
    result.mean_ns = (double)sum / (double)cfg->measure_iterations;

    free(samples);
    return result;
}

vtx_bench_result_t vtx_bench_run(const char *name, vtx_bench_fn fn, void *arg)
{
    vtx_bench_t cfg;
    vtx_bench_init(&cfg);
    return vtx_bench_run_cfg(&cfg, name, fn, arg);
}

/* ========================================================================== */
/* Reporting                                                                    */
/* ========================================================================== */

void vtx_bench_report(const vtx_bench_result_t *result)
{
    printf("  %-40s  median %8lu ns  p95 %8lu ns  "
           "min %8lu ns  max %8lu ns  mean %10.1f ns  "
           "(%u warmup, %u iters)\n",
           result->name,
           (unsigned long)result->median_ns,
           (unsigned long)result->p95_ns,
           (unsigned long)result->min_ns,
           (unsigned long)result->max_ns,
           result->mean_ns,
           result->warmup_iters,
           result->measure_iters);
}

void vtx_bench_compare(const char *label_a, const vtx_bench_result_t *a,
                        const char *label_b, const vtx_bench_result_t *b)
{
    printf("\n  Comparison: %s vs %s\n", label_a, label_b);
    printf("  %-20s  median %8lu ns\n", label_a, (unsigned long)a->median_ns);
    printf("  %-20s  median %8lu ns\n", label_b, (unsigned long)b->median_ns);

    if (b->median_ns > 0) {
        double ratio = (double)a->median_ns / (double)b->median_ns;
        printf("  Ratio (%s/%s): %.2fx\n", label_a, label_b, ratio);
    }
}
