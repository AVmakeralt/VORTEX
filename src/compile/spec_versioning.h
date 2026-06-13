#ifndef VORTEX_COMPILE_SPEC_VERSIONING_H
#define VORTEX_COMPILE_SPEC_VERSIONING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "vortex_config.h"
#include "compile/version.h"

/**
 * VORTEX Speculative Method Versioning by Argument Type (Level 2B)
 * and Speculative Loop Versioning (Level 2C)
 *
 * Level 2B — Method Versioning:
 *   One compiled version per observed type combination at call sites.
 *   If process(Animal a) is called with Dog 80% and Cat 20%, generate
 *   two specialized versions plus a generic fallback:
 *
 *     process_Dog:     a is Dog — fields known, virtual calls devirtualized
 *     process_Cat:     a is Cat — fields known, virtual calls devirtualized
 *     process_generic: fallback for rare types — vtable dispatch, no speculation
 *
 *   A type-specialized version can devirtualize all virtual calls on the
 *   specialized arguments, fold field offsets to constants, and eliminate
 *   type checks. This is the single most impactful optimization for
 *   object-oriented programs with monomorphic or bimorphic call sites.
 *
 * Level 2C — Loop Versioning:
 *   Clone the entire loop body for each observed execution pattern.
 *   The type guard moves OUTSIDE the loop — one check before the loop
 *   instead of N checks inside the loop. For a loop that iterates 1000
 *   times over Dog objects, this eliminates 999 redundant type checks:
 *
 *     // Version 1: items are all Dog (observed 70% of the time)
 *     // Guard: all items are Dog (checked ONCE before the loop)
 *     for (int i = 0; i < n; i++) {
 *         dog_process(items[i]);  // Inlined, no type check inside loop
 *     }
 *
 *   Loop versioning is triggered when:
 *     1. A loop's dominant item type exceeds a frequency threshold
 *     2. The loop trip count is high enough to amortize the pre-loop guard
 *     3. The loop body contains virtual calls that would benefit from
 *        devirtualization if the item type were known
 *
 * Integration with existing modules:
 *   - Uses vtx_code_version_t from compile/version.h for compiled code
 *   - Arg type profiles complement interp/type_feedback.h data
 *   - Loop version guards integrate with guard/guard_deps.h for
 *     O(dependents) invalidation on deopt
 *   - Method version registries integrate with deopt/deoptless.h for
 *     deoptless transitions when a version's guard fails
 *   - Phase-reactive reactivation (compile/phase_react.h) can park and
 *     reactivate type-specialized versions across execution phases
 *
 * Thread safety:
 *   - The global manager mutex protects all registry operations
 *   - Per-registry operations are safe under the manager mutex
 *   - Version state transitions use the version manager's per-method mutex
 */

/* ========================================================================== */
/* Configuration                                                               */
/* ========================================================================== */

/**
 * Maximum number of type-specialized versions per method.
 * Extends VTX_DEOPTLESS_MAX_VERSIONS (8) to support per-type specialization.
 * 64 versions covers methods with up to 4 reference arguments and 3 types
 * each (3^4 = 81, but most combinations are never observed).
 */
#define VTX_SPEC_VERSION_MAX 64

/**
 * Maximum number of argument positions tracked for type specialization.
 * Most methods have 0-3 reference arguments worth specializing on.
 * The receiver (arg 0) is always tracked if it is a reference type.
 */
#define VTX_SPEC_VERSION_MAX_ARGS 4

/**
 * Frequency threshold for a type to be worth creating a specialized version.
 * A type must appear at least 10% of the time at an argument position
 * to justify the compilation cost and code cache pressure.
 */
#define VTX_SPEC_VERSION_FREQUENCY_THRESHOLD 0.10

/**
 * Minimum number of dispatch observations before creating a specialized
 * version. Prevents premature specialization based on too few samples.
 */
#define VTX_SPEC_VERSION_MIN_OBSERVATIONS 500

/**
 * Stability threshold for a version. When stability drops below this,
 * the version is a candidate for deactivation (too many deopts).
 *
 * stability = execution_count / (execution_count + deopt_count)
 */
#define VTX_SPEC_VERSION_STABILITY_THRESHOLD 0.90

/**
 * Maximum number of type-specialized loop versions per loop.
 * Most loops are dominated by one type (monomorphic), so 8 is generous.
 */
#define VTX_LOOP_VERSION_MAX 8

/**
 * Minimum loop trip count to justify loop versioning.
 * Short loops don't amortize the pre-loop guard cost.
 */
#define VTX_LOOP_VERSION_MIN_TRIP_COUNT 100

/**
 * Frequency threshold for a type to dominate a loop enough for versioning.
 * A type must appear in >= 70% of iterations to justify cloning the loop.
 */
#define VTX_LOOP_VERSION_FREQUENCY_THRESHOLD 0.70

/**
 * Minimum number of loop iterations observed before considering versioning.
 */
#define VTX_LOOP_VERSION_MIN_OBSERVATIONS 200

/**
 * Initial capacity for the method registries array.
 * Grows via realloc as new methods are registered.
 */
#define VTX_SPEC_VERSION_INITIAL_CAPACITY 256

/**
 * Initial capacity for the loop version arrays.
 */
#define VTX_LOOP_VERSION_INITIAL_CAPACITY 4

/* ========================================================================== */
/* Type signature for method versioning (Level 2B)                             */
/* ========================================================================== */

/**
 * A type signature for method versioning: the concrete types observed
 * at each argument position.
 *
 * Named vtx_spec_type_sig_t to avoid collision with vtx_type_signature_t
 * in interp/type_feedback.h, which tracks call-site receiver+result types
 * for composite guard optimization. This structure tracks argument types
 * for method-level versioning decisions.
 *
 * The signature_hash is an FNV-1a hash of arg_types, consistent with
 * the hash functions used in deopt/deoptless.c and compile/phase_react.c.
 * It enables O(1) signature comparison in the dispatch hot path.
 */
typedef struct {
    uint32_t arg_types[VTX_SPEC_VERSION_MAX_ARGS]; /* concrete type_id per arg */
    uint32_t arg_count;                             /* number of tracked args */
    uint64_t signature_hash;                        /* FNV-1a hash of arg_types */
} vtx_spec_type_sig_t;

/* ========================================================================== */
/* Type-specialized method version (Level 2B)                                  */
/* ========================================================================== */

/**
 * A type-specialized version of a method.
 *
 * Each version is compiled with the assumption that arguments match
 * the type signature. The guard_id identifies the DeoptGuard that
 * checks the type signature at the method entry point. If the guard
 * fails, execution transfers to the generic version or the interpreter.
 *
 * The stability metric tracks how often this version's guard succeeds:
 *   stability = execution_count / (execution_count + deopt_count)
 *
 * When stability drops below VTX_SPEC_VERSION_STABILITY_THRESHOLD,
 * the version is a candidate for deactivation.
 *
 * Versions are stored in a prepend-based linked list (newest first)
 * for O(1) insertion and preferential access to recently created versions.
 */
typedef struct vtx_spec_version vtx_spec_version_t;

struct vtx_spec_version {
    uint32_t              method_id;        /* method this is a version of */
    vtx_spec_type_sig_t   signature;        /* the type signature this version handles */
    vtx_code_version_t   *code_version;     /* compiled code (or NULL if not yet compiled) */
    uint64_t              execution_count;  /* how many times this version was entered */
    uint64_t              deopt_count;      /* how many times this version deopted */
    double                stability;        /* execution_count / (execution_count + deopt_count) */
    bool                  is_active;        /* true if code is installed and callable */
    bool                  is_compiling;     /* true if compilation is in progress */
    uint32_t              guard_id;         /* guard that checks the type signature */
    vtx_spec_version_t   *next;             /* linked list: prepend-based (newest first) */
};

/* ========================================================================== */
/* Per-method spec version registry (Level 2B)                                 */
/* ========================================================================== */

/**
 * Per-method spec version registry.
 *
 * Tracks all type-specialized versions for a single method, plus
 * per-argument type profiling data used to decide which type
 * signatures are worth creating versions for.
 *
 * The arg_profiles array tracks the top 4 types observed at each
 * argument position with their frequencies. This data drives the
 * decision of whether to create a new specialized version:
 *   - If a type's frequency >= VTX_SPEC_VERSION_FREQUENCY_THRESHOLD
 *     and total observations >= VTX_SPEC_VERSION_MIN_OBSERVATIONS,
 *     that type is a candidate for specialization.
 *   - The combination of specialized types across arguments forms
 *     the signature for the new version.
 *
 * Statistics track the dispatch efficiency: how often calls hit a
 * specialized version (direct_hits) vs. fall through to the generic
 * fallback (generic_fallbacks).
 */
typedef struct {
    uint32_t              method_id;
    vtx_spec_version_t   *versions;         /* linked list of type-specialized versions */
    uint32_t              version_count;    /* number of versions */
    vtx_spec_version_t   *default_version;  /* generic fallback version */
    vtx_spec_version_t   *hot_version;      /* most frequently executed version */

    /* Argument type profiling: for each arg position, track the top
     * observed types and their frequencies. Used to decide which
     * type signatures are worth creating versions for. */
    struct {
        uint32_t top_type_ids[4];    /* top 4 types observed at this arg position */
        double   top_frequencies[4]; /* frequency of each type */
        uint64_t total_observations; /* total observations at this arg position */
    } arg_profiles[VTX_SPEC_VERSION_MAX_ARGS];

    /* Statistics */
    uint64_t total_dispatches;       /* total method dispatches */
    uint64_t direct_hits;            /* dispatches that hit a specialized version */
    uint64_t generic_fallbacks;      /* dispatches that fell through to generic */
} vtx_spec_version_registry_t;

/* ========================================================================== */
/* Loop version (Level 2C)                                                     */
/* ========================================================================== */

/**
 * A loop version: a clone of a loop body with specific type assumptions.
 *
 * The key insight is that the type guard moves OUTSIDE the loop.
 * Instead of checking the type on every iteration, we check once
 * before entering the loop and execute the type-specialized loop body.
 *
 * For a loop that iterates N times:
 *   - Without versioning: N type checks inside the loop
 *   - With versioning:    1 type check before the loop
 *   - Savings:            (N-1) type checks eliminated
 *
 * The guard_id identifies the pre-loop guard that checks:
 *   - The loop items are all of item_type_id
 *   - (Optionally) the loop trip count matches the expected range
 *
 * If the pre-loop guard fails, execution falls through to the
 * generic (unversioned) loop.
 */
typedef struct {
    uint32_t            method_id;          /* method containing the loop */
    uint32_t            loop_header_pc;     /* bytecode PC of the loop header */
    uint32_t            item_type_id;       /* the assumed type of loop items */
    vtx_code_version_t *code_version;       /* compiled code for this loop version */
    uint32_t            guard_id;           /* guard that checks the loop's assumptions */
    uint64_t            execution_count;    /* times this loop version was entered */
    bool                is_active;          /* true if code is installed */
} vtx_loop_version_t;

/* ========================================================================== */
/* Per-loop version registry (Level 2C)                                        */
/* ========================================================================== */

/**
 * Per-loop version registry.
 *
 * Tracks all type-specialized versions for a single loop.
 * The versions array is dynamically growing (unlike method registries
 * which use linked lists) because the number of loop versions is
 * typically small (1-3) and array access is faster for dispatch.
 */
typedef struct {
    uint32_t              method_id;
    uint32_t              loop_header_pc;
    vtx_loop_version_t   *versions;         /* array of loop versions */
    uint32_t              version_count;
    uint32_t              version_capacity;
    uint32_t              active_version;    /* index of the currently active version */
} vtx_loop_version_registry_t;

/* ========================================================================== */
/* Global spec version manager                                                 */
/* ========================================================================== */

/**
 * Global method versioning manager.
 *
 * Manages per-method registries for type-specialized versioning (2B)
 * and per-loop registries for loop versioning (2C). The manager
 * provides thread-safe access to all registries via a single mutex.
 *
 * Loop versioning registries are stored in a separate array indexed
 * by a combined (method_id, loop_header_pc) key. In practice, most
 * methods have 0-2 hot loops, so a flat array with linear scan is
 * efficient. A hash table could be used if this becomes a bottleneck.
 *
 * The loop_versioning_stats field tracks the aggregate impact of
 * loop versioning: how many loops have been versioned, how many
 * type guards were hoisted out of loops, and the estimated total
 * number of per-iteration type checks eliminated.
 */
typedef struct {
    vtx_spec_version_registry_t *registries;      /* array indexed by method_id */
    uint32_t                     registry_count;
    uint32_t                     registry_capacity;
    pthread_mutex_t              mutex;

    /* Loop versioning support (Level 2C) */
    vtx_loop_version_registry_t *loop_registries; /* array of loop registries */
    uint32_t                     loop_registry_count;
    uint32_t                     loop_registry_capacity;

    struct {
        uint32_t total_loops_versioned;
        uint32_t guards_hoisted;           /* type guards hoisted out of loops */
        uint64_t estimated_checks_saved;   /* per-iteration checks eliminated */
    } loop_versioning_stats;

    /* Statistics */
    uint32_t total_versions_created;
    uint32_t total_versions_deopted;
    uint32_t total_direct_dispatches;
} vtx_spec_version_manager_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the spec version manager.
 *
 * Allocates the registries array with VTX_SPEC_VERSION_INITIAL_CAPACITY
 * entries and initializes the global mutex.
 *
 * @param mgr  Manager to initialize
 * @return     0 on success, -1 on failure
 */
int vtx_spec_version_manager_init(vtx_spec_version_manager_t *mgr);

/**
 * Destroy the spec version manager and release all resources.
 *
 * Frees all version structures, registries, and loop registries.
 * Does NOT free the vtx_code_version_t objects — those are owned
 * by the version manager (compile/version.h).
 *
 * @param mgr  Manager to destroy
 */
void vtx_spec_version_manager_destroy(vtx_spec_version_manager_t *mgr);

/* ========================================================================== */
/* Method versioning (Level 2B)                                                */
/* ========================================================================== */

/**
 * Get or create a spec version registry for a method.
 *
 * The registry is created on first access for a method_id.
 * Subsequent calls with the same method_id return the existing registry.
 *
 * Thread safety: acquires mgr->mutex internally.
 *
 * @param mgr        Spec version manager
 * @param method_id  Method ID
 * @return           Pointer to the registry, or NULL on failure
 */
vtx_spec_version_registry_t *vtx_spec_version_get_registry(
    vtx_spec_version_manager_t *mgr, uint32_t method_id);

/**
 * Find a type-specialized version matching the given type signature.
 *
 * Searches the registry's linked list for a version whose signature
 * matches. Uses signature_hash for fast O(1) rejection, then verifies
 * with vtx_spec_type_sig_equal for full comparison.
 *
 * Thread safety: caller must hold mgr->mutex or ensure the registry
 * is not being concurrently modified.
 *
 * @param registry   Per-method registry
 * @param signature  Type signature to search for
 * @return           Matching version, or NULL if not found
 */
vtx_spec_version_t *vtx_spec_version_find(
    const vtx_spec_version_registry_t *registry,
    const vtx_spec_type_sig_t *signature);

/**
 * Create a type-specialized version for the given type signature.
 *
 * Allocates a new vtx_spec_version_t, initializes it with the
 * signature, and prepends it to the registry's linked list.
 * The version starts in is_compiling=true, is_active=false state.
 *
 * If the registry is at capacity (VTX_SPEC_VERSION_MAX versions),
 * the least stable version is evicted to make room.
 *
 * Thread safety: acquires mgr->mutex internally.
 *
 * @param mgr        Spec version manager
 * @param method_id  Method ID
 * @param signature  Type signature for the new version
 * @return           New version, or NULL on failure
 */
vtx_spec_version_t *vtx_spec_version_create(
    vtx_spec_version_manager_t *mgr,
    uint32_t method_id,
    const vtx_spec_type_sig_t *signature);

/**
 * Check whether a type signature is worth creating a version for.
 *
 * A signature should be specialized when:
 *   1. At least one argument position has a type with frequency
 *      >= VTX_SPEC_VERSION_FREQUENCY_THRESHOLD
 *   2. Total dispatch observations >= VTX_SPEC_VERSION_MIN_OBSERVATIONS
 *   3. The registry is not at capacity, or a less-stable version
 *      can be evicted
 *
 * Thread safety: caller must hold mgr->mutex or ensure the registry
 * is not being concurrently modified.
 *
 * @param registry   Per-method registry
 * @param signature  Type signature to evaluate
 * @return           true if specialization is recommended
 */
bool vtx_spec_version_should_specialize(
    const vtx_spec_version_registry_t *registry,
    const vtx_spec_type_sig_t *signature);

/**
 * Record a method dispatch with the given type signature.
 *
 * Updates the registry's dispatch statistics and argument type profiles.
 * If the signature matches an existing specialized version, increments
 * that version's execution_count. Otherwise, increments generic_fallbacks.
 *
 * The argument type profiles are updated for each position: the observed
 * type is either inserted into the top-4 list or increments an existing
 * entry's frequency.
 *
 * Thread safety: acquires mgr->mutex internally.
 *
 * @param registry   Per-method registry (must be valid)
 * @param signature  Type signature of the dispatch arguments
 */
void vtx_spec_version_record_dispatch(
    vtx_spec_version_registry_t *registry,
    const vtx_spec_type_sig_t *signature);

/* ========================================================================== */
/* Type signature helpers                                                      */
/* ========================================================================== */

/**
 * Compute the FNV-1a hash of a type signature.
 *
 * Uses the same FNV-1a constants as deopt/deoptless.c and
 * compile/phase_react.c for consistency:
 *   offset_basis = 14695981039346656037ULL
 *   prime        = 1099511628211ULL
 *
 * The hash is computed over arg_types[0..arg_count-1], each hashed
 * byte-by-byte for robust distribution.
 *
 * @param sig  Type signature
 * @return     64-bit FNV-1a hash
 */
uint64_t vtx_spec_type_sig_hash(const vtx_spec_type_sig_t *sig);

/**
 * Compare two type signatures for equality.
 *
 * Two signatures are equal if and only if:
 *   - arg_count is the same
 *   - All arg_types[0..arg_count-1] are the same
 *
 * Uses signature_hash for fast rejection before full comparison.
 *
 * @param a  First signature
 * @param b  Second signature
 * @return   true if the signatures are equal
 */
bool vtx_spec_type_sig_equal(const vtx_spec_type_sig_t *a,
                              const vtx_spec_type_sig_t *b);

/**
 * Initialize a type signature from an array of argument type IDs.
 *
 * Sets arg_types[0..arg_count-1] from the input array, zeroes
 * remaining positions, and computes the signature_hash.
 *
 * @param sig        Signature to initialize (must not be NULL)
 * @param arg_count  Number of argument types (must be <= VTX_SPEC_VERSION_MAX_ARGS)
 * @param arg_types  Array of type IDs (may be NULL if arg_count is 0)
 */
void vtx_spec_type_sig_init(vtx_spec_type_sig_t *sig,
                              uint32_t arg_count,
                              const uint32_t *arg_types);

/* ========================================================================== */
/* Loop versioning (Level 2C)                                                  */
/* ========================================================================== */

/**
 * Get or create a loop version registry for a loop.
 *
 * The registry is created on first access for the given
 * (method_id, loop_header_pc) pair. Subsequent calls return
 * the existing registry.
 *
 * Thread safety: acquires mgr->mutex internally.
 *
 * @param mgr             Spec version manager
 * @param method_id       Method ID containing the loop
 * @param loop_header_pc  Bytecode PC of the loop header
 * @return                Pointer to the registry, or NULL on failure
 */
vtx_loop_version_registry_t *vtx_loop_version_get_registry(
    vtx_spec_version_manager_t *mgr,
    uint32_t method_id,
    uint32_t loop_header_pc);

/**
 * Create a loop version for the given item type.
 *
 * Allocates a new vtx_loop_version_t, initializes it, and adds it
 * to the registry's versions array. The version starts in
 * is_active=false state.
 *
 * If the registry is at capacity (VTX_LOOP_VERSION_MAX versions),
 * the version with the lowest execution_count is evicted.
 *
 * Thread safety: acquires mgr->mutex internally.
 *
 * @param mgr             Spec version manager
 * @param method_id       Method ID containing the loop
 * @param loop_header_pc  Bytecode PC of the loop header
 * @param item_type_id    Assumed type of loop items
 * @return                New loop version, or NULL on failure
 */
vtx_loop_version_t *vtx_loop_version_create(
    vtx_spec_version_manager_t *mgr,
    uint32_t method_id,
    uint32_t loop_header_pc,
    uint32_t item_type_id);

/**
 * Find a loop version matching the given item type.
 *
 * Searches the registry's versions array for a version with
 * the specified item_type_id. Returns NULL if not found.
 *
 * Thread safety: caller must hold mgr->mutex or ensure the registry
 * is not being concurrently modified.
 *
 * @param registry       Per-loop registry
 * @param item_type_id   Item type to search for
 * @return               Matching loop version, or NULL
 */
vtx_loop_version_t *vtx_loop_version_find(
    const vtx_loop_version_registry_t *registry,
    uint32_t item_type_id);

/**
 * Check whether a loop should be versioned for the given item type.
 *
 * A loop should be versioned when:
 *   1. The item type's frequency >= VTX_LOOP_VERSION_FREQUENCY_THRESHOLD
 *   2. The loop has been observed at least VTX_LOOP_VERSION_MIN_OBSERVATIONS
 *      times
 *   3. The expected trip count >= VTX_LOOP_VERSION_MIN_TRIP_COUNT
 *      (amortizes the pre-loop guard cost)
 *
 * Thread safety: caller must hold mgr->mutex or ensure the registry
 * is not being concurrently modified.
 *
 * @param registry       Per-loop registry
 * @param item_type_id   Item type to evaluate
 * @param frequency      Observed frequency of this item type (0.0-1.0)
 * @return               true if loop versioning is recommended
 */
bool vtx_loop_should_version(
    const vtx_loop_version_registry_t *registry,
    uint32_t item_type_id,
    double frequency);

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

/**
 * Get global spec versioning statistics.
 *
 * @param mgr                 Spec version manager
 * @param versions_created    Output: total type-specialized versions created
 * @param versions_deopted    Output: total versions that deopted excessively
 * @param direct_dispatches   Output: total dispatches hitting specialized versions
 */
void vtx_spec_version_get_stats(const vtx_spec_version_manager_t *mgr,
                                  uint32_t *versions_created,
                                  uint32_t *versions_deopted,
                                  uint32_t *direct_dispatches);

#endif /* VORTEX_COMPILE_SPEC_VERSIONING_H */
