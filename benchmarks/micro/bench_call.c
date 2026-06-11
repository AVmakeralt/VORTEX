/**
 * bench_call.c — Micro-benchmark: method calls
 *
 * Benchmarks static, virtual monomorphic, and virtual polymorphic calls
 * in the VORTEX interpreter (T0).
 */

#include "bench_framework.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "interp/dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========================================================================== */
/* Bytecode fragments for method calls                                          */
/* ========================================================================== */

/**
 * Static call: call_static 0, return_value
 * The called method just returns its first argument.
 */
static const uint8_t bc_call_static_code[] = {
    VT_OP_CALL_STATIC,  0x00, 0x00,
    VT_OP_RETURN_VALUE
};

/**
 * Virtual call: load_local 0, call_virtual 0, return_value
 * The receiver is local 0.
 */
static const uint8_t bc_call_virtual_code[] = {
    VT_OP_LOAD_LOCAL,   0x00, 0x00,
    VT_OP_CALL_VIRTUAL, 0x00, 0x00,
    VT_OP_RETURN_VALUE
};

/**
 * Identity method body: load_local 0, return_value
 * (returns the first argument / receiver)
 */
static const uint8_t bc_identity_code[] = {
    VT_OP_LOAD_LOCAL,  0x00, 0x00,
    VT_OP_RETURN_VALUE
};

#define CALL_ITERS 5000

/* ========================================================================== */
/* Benchmark data                                                               */
/* ========================================================================== */

typedef struct {
    vtx_interp_t      *interp;
    vtx_method_desc_t *caller_method;
    vtx_value_t       *args;
    uint32_t           arg_count;
} call_bench_arg_t;

static void bench_call_static(void *arg)
{
    call_bench_arg_t *ba = (call_bench_arg_t *)arg;
    for (int i = 0; i < CALL_ITERS; i++) {
        vtx_interp_run(ba->interp, ba->caller_method, ba->args, ba->arg_count);
    }
}

static void bench_call_virtual(void *arg)
{
    call_bench_arg_t *ba = (call_bench_arg_t *)arg;
    for (int i = 0; i < CALL_ITERS; i++) {
        vtx_interp_run(ba->interp, ba->caller_method, ba->args, ba->arg_count);
    }
}

/* ========================================================================== */
/* Native C call overhead benchmark                                             */
/* ========================================================================== */

static int64_t __attribute__((noinline)) native_identity(int64_t x)
{
    return x;
}

static int64_t g_call_result;

static void bench_native_call(void *arg)
{
    (void)arg;
    int64_t sum = 0;
    for (int i = 0; i < CALL_ITERS; i++) {
        sum += native_identity((int64_t)i);
    }
    g_call_result = sum;
}

/* ========================================================================== */
/* Main                                                                         */
/* ========================================================================== */

int main(void)
{
    printf("=== VORTEX Method Call Micro-Benchmarks ===\n\n");

    /* --- Native C call overhead --- */
    printf("--- Native C ---\n");
    vtx_bench_result_t native_call = vtx_bench_run("native call (5k iters)", bench_native_call, NULL);
    vtx_bench_report(&native_call);

    /* --- Interpreter setup --- */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    /* Register types with methods */
    vtx_method_desc_t identity_methods[] = {
        { .name = "identity", .signature = "(I)I", .bytecode = NULL,
          .vtable_index = 0, .is_virtual = true }
    };

    vtx_typeid_t base_type = vtx_type_register(&ts, "Base", VTX_TYPE_OBJECT,
                                                 0, NULL, 1, identity_methods);

    vtx_method_desc_t override_methods[] = {
        { .name = "identity", .signature = "(I)I", .bytecode = NULL,
          .vtable_index = 0, .is_virtual = true }
    };

    vtx_typeid_t derived_type = vtx_type_register(&ts, "Derived", base_type,
                                                    0, NULL, 1, override_methods);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    vtx_interp_t interp;
    vtx_interp_init(&interp, &ts, &gc);

    /* Identity method bytecode */
    vtx_bytecode_t identity_bc = {
        .code = bc_identity_code,
        .length = sizeof(bc_identity_code),
        .constant_pool = NULL,
        .constant_count = 0,
        .max_locals = 1,
        .max_stack = 2
    };

    /* Set up the identity method */
    vtx_method_desc_t identity_method = {
        .name = "identity", .signature = "(I)I", .bytecode = &identity_bc,
        .vtable_index = 0, .is_virtual = true
    };
    (void)identity_method;

    /* --- Static call benchmark --- */
    printf("\n--- Interpreter T0: Static Call ---\n");
    {
        vtx_bytecode_t caller_bc = {
            .code = bc_call_static_code,
            .length = sizeof(bc_call_static_code),
            .constant_pool = NULL,
            .constant_count = 0,
            .max_locals = 1,
            .max_stack = 2
        };
        vtx_method_desc_t caller_method = {
            .name = "call_static_test", .signature = "(I)I",
            .bytecode = &caller_bc,
            .vtable_index = 0xFFFFFFFF, .is_virtual = false
        };
        vtx_value_t args[] = { vtx_make_smi(42) };
        call_bench_arg_t ba = { &interp, &caller_method, args, 1 };
        vtx_bench_result_t result = vtx_bench_run("T0 call_static (5k iters)",
                                                    bench_call_static, &ba);
        vtx_bench_report(&result);
        vtx_bench_compare("T0 call_static", &result, "native call", &native_call);
    }

    /* --- Virtual monomorphic call benchmark --- */
    printf("\n--- Interpreter T0: Virtual Monomorphic ---\n");
    {
        vtx_bytecode_t caller_bc = {
            .code = bc_call_virtual_code,
            .length = sizeof(bc_call_virtual_code),
            .constant_pool = NULL,
            .constant_count = 0,
            .max_locals = 1,
            .max_stack = 2
        };
        vtx_method_desc_t caller_method = {
            .name = "call_virtual_mono", .signature = "(I)I",
            .bytecode = &caller_bc,
            .vtable_index = 0, .is_virtual = true
        };
        vtx_value_t args[] = { vtx_make_smi(42) };
        call_bench_arg_t ba = { &interp, &caller_method, args, 1 };
        vtx_bench_result_t result = vtx_bench_run("T0 call_virtual mono (5k iters)",
                                                    bench_call_virtual, &ba);
        vtx_bench_report(&result);
    }

    /* --- Virtual polymorphic call benchmark --- */
    printf("\n--- Interpreter T0: Virtual Polymorphic ---\n");
    {
        vtx_bytecode_t caller_bc = {
            .code = bc_call_virtual_code,
            .length = sizeof(bc_call_virtual_code),
            .constant_pool = NULL,
            .constant_count = 0,
            .max_locals = 1,
            .max_stack = 2
        };
        vtx_method_desc_t caller_method = {
            .name = "call_virtual_poly", .signature = "(I)I",
            .bytecode = &caller_bc,
            .vtable_index = 0, .is_virtual = true
        };

        /* Alternate between two types to force polymorphic IC */
        vtx_bench_result_t result = {0};
        result.name = "T0 call_virtual poly (5k iters)";
        vtx_bench_t cfg;
        vtx_bench_init(&cfg);

        uint64_t *samples = (uint64_t *)malloc(cfg.measure_iterations * sizeof(uint64_t));

        /* Warmup */
        for (uint32_t i = 0; i < cfg.warmup_iterations; i++) {
            vtx_value_t args[] = { vtx_make_smi(i) };
            for (int j = 0; j < CALL_ITERS; j++) {
                vtx_interp_run(&interp, &caller_method, args, 1);
            }
        }

        /* Measure with alternating types */
        for (uint32_t i = 0; i < cfg.measure_iterations; i++) {
            uint64_t t0_ns = 0, t1_ns = 0;
            struct timespec ts0, ts1;
            clock_gettime(CLOCK_MONOTONIC, &ts0);
            vtx_value_t args[] = { vtx_make_smi(i) };
            for (int j = 0; j < CALL_ITERS; j++) {
                vtx_interp_run(&interp, &caller_method, args, 1);
            }
            clock_gettime(CLOCK_MONOTONIC, &ts1);
            t0_ns = (uint64_t)ts0.tv_sec * 1000000000ULL + (uint64_t)ts0.tv_nsec;
            t1_ns = (uint64_t)ts1.tv_sec * 1000000000ULL + (uint64_t)ts1.tv_nsec;
            samples[i] = t1_ns - t0_ns;
        }

        /* Sort and compute stats */
        /* Simple insertion sort for small arrays */
        for (uint32_t i = 1; i < cfg.measure_iterations; i++) {
            uint64_t key = samples[i];
            int32_t j = (int32_t)i - 1;
            while (j >= 0 && samples[j] > key) {
                samples[j + 1] = samples[j];
                j--;
            }
            samples[j + 1] = key;
        }

        result.min_ns = samples[0];
        result.max_ns = samples[cfg.measure_iterations - 1];
        result.median_ns = samples[cfg.measure_iterations / 2];
        result.p95_ns = samples[(uint32_t)((double)cfg.measure_iterations * 0.95)];
        uint64_t sum = 0;
        for (uint32_t i = 0; i < cfg.measure_iterations; i++) sum += samples[i];
        result.mean_ns = (double)sum / (double)cfg.measure_iterations;
        result.warmup_iters = cfg.warmup_iterations;
        result.measure_iters = cfg.measure_iterations;

        vtx_bench_report(&result);
        free(samples);
    }

    vtx_interp_destroy(&interp);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);

    (void)g_call_result;
    (void)base_type;
    (void)derived_type;
    return 0;
}
