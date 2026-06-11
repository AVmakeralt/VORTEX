#ifndef VORTEX_PROFILE_DATA_H
#define VORTEX_PROFILE_DATA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "runtime/type_system.h"

/**
 * VORTEX Profile Data Structures
 *
 * Per-method and global profile data used to drive JIT compilation decisions.
 * Collected by the T0 interpreter and T1 baseline JIT, consumed by the T2/T3
 * optimizing compilers for speculative optimizations and inlining decisions.
 *
 * Type observations at call sites are bounded by VTX_POLY_LIMIT (4). When more
 * types are observed than the limit, the site transitions to megamorphic and
 * stops recording individual types (a sentinel VTX_TYPEID_MEGAMORPHIC is stored).
 *
 * Branch frequencies and invocation counts use saturating arithmetic to prevent
 * overflow. All counters are uint64_t; saturation happens at UINT64_MAX.
 */

/* ========================================================================== */
/* Sentinel values                                                            */
/* ========================================================================== */

#define VTX_TYPEID_MEGAMORPHIC 0xFFFFFFFEu

/* ========================================================================== */
/* Per-call-site type observations                                            */
/* ========================================================================== */

/**
 * Records the receiver types observed at a single call site.
 * Up to VTX_POLY_LIMIT types are stored; once exceeded the site is megamorphic.
 */
typedef struct {
    vtx_typeid_t types[VTX_POLY_LIMIT]; /* observed receiver types */
    uint32_t     count;                  /* number of distinct types (<= VTX_POLY_LIMIT) */
    bool         megamorphic;            /* true if more than VTX_POLY_LIMIT types seen */
} vtx_callsite_profile_t;

/* ========================================================================== */
/* Per-branch frequency record                                                */
/* ========================================================================== */

/**
 * Records taken / not-taken counts for a single branch bytecode PC.
 * Both counters use saturating increment.
 */
typedef struct {
    uint32_t bytecode_pc;   /* bytecode PC of the branch instruction */
    uint64_t taken;         /* count of times the branch was taken */
    uint64_t not_taken;     /* count of times the branch was not taken */
} vtx_branch_profile_t;

/* ========================================================================== */
/* Per-field-access shape record                                              */
/* ========================================================================== */

/**
 * Records the shape IDs observed at a single field access site.
 * Up to VTX_POLY_LIMIT shapes are stored; once exceeded the site is megamorphic.
 */
typedef struct {
    uint32_t    field_offset;              /* field offset of the access site */
    vtx_shapeid_t shapes[VTX_POLY_LIMIT]; /* observed holder shape IDs */
    uint32_t    count;                     /* number of distinct shapes */
    bool        megamorphic;               /* true if too many shapes seen */
} vtx_field_profile_t;

/* ========================================================================== */
/* Per-loop back-edge record                                                  */
/* ========================================================================== */

/**
 * Records the back-edge count for a single loop (identified by its header PC).
 */
typedef struct {
    uint32_t loop_header_pc; /* bytecode PC of the loop header */
    uint64_t backedge_count; /* number of times the back edge was taken */
} vtx_loop_profile_t;

/* ========================================================================== */
/* Per-method profile                                                         */
/* ========================================================================== */

/**
 * Complete profile data for a single method.
 *
 * Call sites, branches, field accesses, and loops are stored in dynamically
 * grown arrays keyed by bytecode PC or field offset. Lookup is linear scan
 * (the arrays are small: typical methods have < 100 sites of each kind).
 */
typedef struct vtx_profile_method vtx_profile_method_t;

struct vtx_profile_method {
    uint32_t method_id;          /* unique method identifier (method_index from bytecode module) */
    uint64_t invocation_count;   /* total number of times this method was invoked */

    /* Call site profiles: indexed by callsite index (one per CALL_* instruction) */
    vtx_callsite_profile_t *call_sites;
    uint32_t                call_site_count;
    uint32_t                call_site_capacity;

    /* Branch profiles: indexed by bytecode PC of IF_TRUE/IF_FALSE/GOTO */
    vtx_branch_profile_t *branches;
    uint32_t              branch_count;
    uint32_t              branch_capacity;

    /* Field access profiles: indexed by field offset */
    vtx_field_profile_t *field_accesses;
    uint32_t             field_access_count;
    uint32_t             field_access_capacity;

    /* Loop back-edge profiles */
    vtx_loop_profile_t *loops;
    uint32_t            loop_count;
    uint32_t            loop_capacity;
};

/* ========================================================================== */
/* Call graph edge                                                            */
/* ========================================================================== */

/**
 * A directed edge in the method call graph: caller → callee with frequency.
 */
typedef struct {
    uint32_t caller_method_id;
    uint32_t callee_method_id;
    uint64_t frequency; /* number of times this call edge was traversed */
} vtx_call_edge_t;

/* ========================================================================== */
/* Phase transition edge                                                      */
/* ========================================================================== */

/**
 * A directed edge in the phase transition graph.
 * Represents a transition from one phase (SCC) to another.
 */
typedef struct {
    uint32_t from_phase_id; /* source phase (SCC index) */
    uint32_t to_phase_id;   /* target phase (SCC index) */
    uint64_t frequency;     /* number of times this transition was observed */
} vtx_phase_transition_t;

/* ========================================================================== */
/* Global profile                                                             */
/* ========================================================================== */

#define VTX_PROFILE_INITIAL_METHOD_CAPACITY 256
#define VTX_PROFILE_INITIAL_EDGE_CAPACITY   512

/**
 * Global profile data spanning all methods.
 *
 * Owns the method profile array, the call graph edges, and the phase
 * transition graph. The phase transition graph is populated by
 * vtx_phase_detect() in phase.h.
 */
typedef struct {
    /* Method profiles */
    vtx_profile_method_t *methods;      /* array of method profiles */
    uint32_t              method_count;
    uint32_t              method_capacity;

    /* Call graph edges */
    vtx_call_edge_t      *call_edges;   /* directed caller→callee edges */
    uint32_t              call_edge_count;
    uint32_t              call_edge_capacity;

    /* Phase transitions (populated by phase detection) */
    vtx_phase_transition_t *phase_transitions;
    uint32_t                phase_transition_count;
    uint32_t                phase_transition_capacity;

    /* Phase graph pointer (owned, created by vtx_phase_detect) */
    struct vtx_phase_graph *phase_graph;
} vtx_profile_global_t;

/* ========================================================================== */
/* Lifecycle                                                                  */
/* ========================================================================== */

/**
 * Initialize a global profile structure. Allocates initial arrays.
 * Returns 0 on success, -1 on failure.
 */
int vtx_profile_global_init(vtx_profile_global_t *global);

/**
 * Destroy a global profile structure and free all memory.
 */
void vtx_profile_global_destroy(vtx_profile_global_t *global);

/* ========================================================================== */
/* Method profile management                                                  */
/* ========================================================================== */

/**
 * Add a new method profile for the given method_id.
 * Returns a pointer to the new method profile, or NULL on failure.
 * If a profile for method_id already exists, returns the existing one.
 */
vtx_profile_method_t *vtx_profile_add_method(vtx_profile_global_t *global,
                                              uint32_t method_id);

/**
 * Look up a method profile by method_id.
 * Returns the profile, or NULL if not found.
 */
vtx_profile_method_t *vtx_profile_get_method(const vtx_profile_global_t *global,
                                              uint32_t method_id);

/* ========================================================================== */
/* Recording functions                                                        */
/* ========================================================================== */

/**
 * Record a method invocation. Increments the invocation counter (saturating).
 * Creates the method profile if it does not exist.
 */
void vtx_profile_record_invocation(vtx_profile_global_t *global,
                                    uint32_t method_id);

/**
 * Record a receiver type observation at a call site.
 * Creates the method profile and call site entry if they do not exist.
 * If the call site is already megamorphic, this is a no-op.
 * If the type is already recorded, this is a no-op.
 * If VTX_POLY_LIMIT distinct types have been seen and this is a new type,
 * the site transitions to megamorphic.
 */
void vtx_profile_record_callsite_type(vtx_profile_global_t *global,
                                       uint32_t method_id,
                                       uint32_t callsite_index,
                                       vtx_typeid_t receiver_type);

/**
 * Record a branch outcome at the given bytecode PC.
 * Creates the method profile and branch entry if they do not exist.
 * taken=true increments the taken counter; taken=false increments not_taken.
 * Both use saturating arithmetic.
 */
void vtx_profile_record_branch(vtx_profile_global_t *global,
                                uint32_t method_id,
                                uint32_t bytecode_pc,
                                bool taken);

/**
 * Record a field access shape observation.
 * Creates the method profile and field access entry if they do not exist.
 * If the site is already megamorphic, this is a no-op.
 * If the shape is already recorded, this is a no-op.
 * If VTX_POLY_LIMIT distinct shapes have been seen and this is a new shape,
 * the site transitions to megamorphic.
 */
void vtx_profile_record_field_shape(vtx_profile_global_t *global,
                                     uint32_t method_id,
                                     uint32_t field_offset,
                                     vtx_shapeid_t shape_id);

/**
 * Record a loop back-edge at the given loop header PC.
 * Creates the method profile and loop entry if they do not exist.
 * Increments the back-edge counter (saturating).
 */
void vtx_profile_record_loop_backedge(vtx_profile_global_t *global,
                                       uint32_t method_id,
                                       uint32_t loop_header_pc);

/**
 * Record a call graph edge (caller → callee).
 * If an edge with the same (caller, callee) pair already exists,
 * increments its frequency (saturating). Otherwise creates a new edge.
 */
void vtx_profile_record_call_edge(vtx_profile_global_t *global,
                                   uint32_t caller_method_id,
                                   uint32_t callee_method_id);

/* ========================================================================== */
/* Query helpers                                                              */
/* ========================================================================== */

/**
 * Get the callsite profile for a method at the given callsite index.
 * Returns NULL if the method or callsite does not exist.
 */
const vtx_callsite_profile_t *vtx_profile_get_callsite(
    const vtx_profile_global_t *global,
    uint32_t method_id,
    uint32_t callsite_index);

/**
 * Get the branch profile for a method at the given bytecode PC.
 * Returns NULL if the method or branch does not exist.
 */
const vtx_branch_profile_t *vtx_profile_get_branch(
    const vtx_profile_global_t *global,
    uint32_t method_id,
    uint32_t bytecode_pc);

/**
 * Compute the branch taken probability for a branch profile.
 * Returns 0.0 if no observations, otherwise taken / (taken + not_taken).
 */
double vtx_profile_branch_probability(const vtx_branch_profile_t *branch);

/**
 * Check if a method is "hot" (invocation count >= threshold).
 */
bool vtx_profile_method_is_hot(const vtx_profile_method_t *method,
                                uint64_t threshold);

#endif /* VORTEX_PROFILE_DATA_H */
