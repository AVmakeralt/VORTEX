#ifndef VORTEX_CONFIG_H
#define VORTEX_CONFIG_H

/* Generated from CMake configuration */

#define VORTEX_ENABLE_ASSERTIONS
#define VORTEX_ENABLE_VERIFY
#define VORTEX_ENABLE_PROFILING
#define VORTEX_ENABLE_SOTA

#define VORTEX_CACHE_MAX_SIZE 268435456
#define VORTEX_T1_THRESHOLD 1000
#define VORTEX_T2_THRESHOLD 10000
#define VORTEX_COMPILE_THREADS 0

/* Derived constants — principled, not magic */

/* Heap alignment is 8 bytes → 3 low bits for tags */
#define VTX_TAG_BITS 3
#define VTX_TAG_MASK ((1ULL << VTX_TAG_BITS) - 1)

/* NaN-boxing: 46 bits for SMI (sign + 45 magnitude) */
#define VTX_SMI_MAX ((1LL << 46) - 1)
#define VTX_SMI_MIN (-(1LL << 46))

/* Polymorphic IC: >4 types is slower than megamorphic */
#define VTX_POLY_LIMIT 4

/* Trace length and depth limits */
#define VTX_MAX_TRACE_LENGTH 512
#define VTX_MAX_TREE_DEPTH 8
#define VTX_MAX_HYPERBLOCK_NODES 4096
#define VTX_MAX_NATIVE_SIZE 32768

/* Cache configuration */
#define VTX_CACHE_SEGMENT_SIZE 1048576
#define VTX_ARENA_PAGE_SIZE 65536
#define VTX_FRAME_ALIGNMENT 16

/* Inlining */
#define VTX_INLINE_THRESHOLD 0.6
#define VTX_INLINE_SIZE_LIMIT 256

/* Guard configuration */
#define VTX_GUARD_ALPHA 0.1
#define VTX_GUARD_WEAKEN_THRESHOLD 0.01
#define VTX_GUARD_ABANDON_THRESHOLD 0.25

/* Profile and recompilation */
#define VTX_PROFILE_DIVERGENCE_THRESHOLD 0.5
#define VTX_SIDE_EXIT_THRESHOLD 50
#define VTX_STITCH_THRESHOLD 200

/* Loop speculation */
#define VTX_LOOP_CV_THRESHOLD 0.2

/* Inlining feedback */
#define VTX_SPILL_THRESHOLD 0.1
#define VTX_INLINE_DEOPT_THRESHOLD 0.05

/* Phase detection */
#define VTX_PHASE_MIN_METHODS 3
#define VTX_PHASE_MIN_FREQUENCY 1000

/* LRU */
#define VTX_LRU_UPDATE_INTERVAL 100

/* Benchmarks — increased for reliable sub-microsecond timing */
#define VTX_BENCH_WARMUP 100
#define VTX_BENCH_ITERATIONS 1000000

/* Feedback window */
#define VTX_FEEDBACK_WINDOW 10000

/* LRU timestamp update interval */
#define VTX_PROFILE_TYPE_BUFFER_SIZE 32

/* Assertion macro */
#ifdef VORTEX_ENABLE_ASSERTIONS
  #include <stdio.h>
  #include <stdlib.h>
  #define VTX_ASSERT(cond, msg) do { \
    if (!(cond)) { \
      fprintf(stderr, "VORTEX ASSERT FAILED: %s:%d: %s — %s\n", \
              __FILE__, __LINE__, #cond, msg); \
      abort(); \
    } \
  } while(0)
#else
  #define VTX_ASSERT(cond, msg) ((void)0)
#endif

/* Verify macro — only in debug builds with verification enabled */
#if defined(VORTEX_ENABLE_VERIFY) && defined(VORTEX_ENABLE_ASSERTIONS)
  #define VTX_VERIFY(cond, msg) VTX_ASSERT(cond, msg)
#else
  #define VTX_VERIFY(cond, msg) ((void)0)
#endif

#endif /* VORTEX_CONFIG_H */
