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

/* Polymorphic IC: 16-way dispatch for max-aggro.
 * Safe because trap-based guards make 16-way IC free — each shape gets its
 * own guard, and a miss just traps to the deoptless continuation without
 * any interpreter overhead. More shapes inlined = fewer megamorphic falls. */
#define VTX_POLY_LIMIT 16

/* Trace length and depth limits — max-aggro: push hard, deoptless recovers.
 *
 * Longer traces capture more hot context for optimization; deep trees are
 * handled by deoptless which can undo bad decisions at zero interpreter cost;
 * huge hyperblocks compile in the background thread without pausing execution;
 * large native code stays resident in L2 cache (256KB–1MB typical). */
#define VTX_MAX_TRACE_LENGTH 4096
#define VTX_MAX_TREE_DEPTH 32
#define VTX_MAX_HYPERBLOCK_NODES 65536
#define VTX_MAX_NATIVE_SIZE 524288  /* 512KB — hot code stays in L2 cache */

/* Cache configuration */
#define VTX_CACHE_SEGMENT_SIZE 1048576
#define VTX_ARENA_PAGE_SIZE 65536
#define VTX_FRAME_ALIGNMENT 16

/* Inlining — max-aggro: aggressive inlining backed by cheap deoptless undo.
 *
 * VTX_INLINE_SIZE_LIMIT raised for T3 (background compilation tier): if an
 * inline turns out to be a bad decision (e.g. rarely-called path bloats code),
 * deoptless can remove it without re-entering the interpreter.
 *
 * Hyper-inline chain parameters: deep chains of inlined calls are now viable
 * because deoptless versions let us specialize per-call-site without global
 * deopt. Each chain level can be independently deoptless-continued. */
#define VTX_INLINE_THRESHOLD 0.6
#define VTX_INLINE_SIZE_LIMIT 4096     /* T3: 4KB callee limit — deoptless undoes bad inlines cheaply */
#define VTX_INLINE_DEPTH_CHAIN 16      /* Max inlining depth in a single chain — deoptless handles deep trees */
#define VTX_INLINE_BUDGET_CHAIN 16384  /* Total byte budget per inline chain — bigger inlined bodies */

/* Guard configuration */
#define VTX_GUARD_ALPHA 0.1
#define VTX_GUARD_WEAKEN_THRESHOLD 0.01
#define VTX_GUARD_ABANDON_THRESHOLD 0.25

/* Value speculation configuration (P1 — zero-cost deopt) */
#define VTX_VALUE_STABILITY_THRESHOLD 0.95
#define VTX_VALUE_MIN_OBSERVATIONS 100
#define VTX_VALUE_SAMPLE_INTERVAL_DEFAULT 64
#define VTX_VALUE_SAMPLE_INTERVAL_STABLE 256
#define VTX_VALUE_SAMPLE_INTERVAL_UNSTABLE 16

/* Implicit null check configuration (P0 — zero-cost deopt) */
#define VTX_NULL_PAGE_LIMIT 0x10000UL  /* 64KB — covers null + small offsets */

/* Profile and recompilation — max-aggro: react fast, stitch immediately.
 *
 * Low side-exit threshold: start recording a new trace after just 5 exits
 * instead of waiting for 50 — gets optimized code in place sooner.
 * Low stitch threshold: stitch traces together after 10 exits instead of
 * 200 — faster trace linking means fewer interpreter fallbacks.
 * Both are safe because deoptless continuations handle the resulting
 * finer-grained guards without global deopt. */
#define VTX_PROFILE_DIVERGENCE_THRESHOLD 0.5
#define VTX_SIDE_EXIT_THRESHOLD 5
#define VTX_STITCH_THRESHOLD 10

/* Loop speculation — max-aggro: vectorize almost-anything.
 *
 * High CV threshold means we attempt vectorization even for loops with
 * significant iteration-count variance. If the vectorized code hits a
 * trip-count guard failure, deoptless patches to the scalar version
 * without any interpreter detour. */
#define VTX_LOOP_CV_THRESHOLD 0.8

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

/* ======================================================================== */
/* Max Aggro Level 1: Optimization pipeline constants                       */
/* ======================================================================== */

/* GVN convergence — max-aggro: let it run until it stabilizes.
 * GVN is idempotent and will converge; 100 iterations is a generous
 * upper bound. The cost is compile-time only (background thread),
 * and the result is maximally deduplicated IR → smaller/faster native code. */
#define VTX_GVN_MAX_ITERATIONS 100

/* Pipeline iterations for T3 (background recompilation tier) — max-aggro.
 * More passes let the optimizer converge fully: CSE, DCE, LICM, and
 * inlining can interact (e.g. inlining exposes new CSE opportunities).
 * 20 iterations ensures we squeeze out every optimization. Safe because
 * T3 runs in a background compile thread — no pause on the main thread. */
#define VTX_PIPELINE_MAX_ITERATIONS 20

/* Max fields per object — max-aggro: more speculative-reload targets.
 * With 256 fields, the speculative-reload pass can eliminate more
 * field loads by predicting which fields are accessed together.
 * Deoptless guards per field make this safe: a wrong prediction
 * just continues to the deoptless version with that field un-reloaded. */
#define VTX_MAX_FIELDS_PER_OBJ 256

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
