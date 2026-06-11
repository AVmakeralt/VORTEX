#ifndef VORTEX_SIDE_EXIT_H
#define VORTEX_SIDE_EXIT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/node.h"

/**
 * VORTEX Side Exit Handling
 *
 * Each side exit represents a point where a compiled trace diverges from
 * the recorded hot path. When the guard at a side exit fails at runtime,
 * execution leaves compiled code and either returns to the interpreter
 * or (if the exit is hot enough) triggers recording of a new trace branch.
 *
 * Side exit lifecycle:
 *   1. Created during trace recording at each untaken branch point.
 *   2. At runtime, a failing guard jumps to a side exit stub that
 *      reconstructs interpreter state and increments the exit counter.
 *   3. When exit_counter > VTX_SIDE_EXIT_THRESHOLD, the side exit
 *      triggers recording of a new trace branch from its PC back to
 *      the loop header, extending the trace tree.
 *
 * The stack state captured at a side exit contains the NodeIDs that
 * represent the interpreter operand stack at the exit point. These
 * are needed to reconstruct the FrameState for deoptimization.
 */

/* ========================================================================== */
/* Side exit structure                                                         */
/* ========================================================================== */

/**
 * Unique identifier for a side exit within a compilation context.
 */
typedef uint32_t vtx_side_exit_id_t;

#define VTX_SIDE_EXIT_ID_INVALID ((vtx_side_exit_id_t)0xFFFFFFFF)

/**
 * Reason why a side exit was created. Used for deoptimization reporting
 * and guard optimization decisions.
 */
typedef enum {
    VTX_EXIT_BRANCH_NOT_TAKEN = 0,  /* untaken branch in profile data */
    VTX_EXIT_TYPE_CHECK_FAILED,     /* guard on receiver type failed */
    VTX_EXIT_NULL_CHECK_FAILED,     /* guard on non-null failed */
    VTX_EXIT_BOUNDS_CHECK_FAILED,   /* array bounds check failed */
    VTX_EXIT_LOOP_BACK_EDGE,        /* loop back-edge (re-entry) */
    VTX_EXIT_UNKNOWN                /* catch-all */
} vtx_side_exit_reason_t;

/**
 * Interpreter stack state at the point of a side exit.
 *
 * Captures the NodeIDs on the operand stack so that the interpreter
 * state can be reconstructed when the guard fails. The locals array
 * captures the current local variable bindings.
 */
typedef struct {
    vtx_nodeid_t *stack;       /* operand stack NodeIDs (arena-allocated) */
    uint32_t      stack_depth; /* number of entries in the stack */
    vtx_nodeid_t *locals;      /* local variable NodeIDs (arena-allocated) */
    uint32_t      local_count; /* number of local variables */
} vtx_exit_stack_state_t;

/**
 * Side exit descriptor.
 *
 * Represents a single point where a trace may exit to the interpreter.
 * Each side exit is associated with a Guard node in the SoN graph and
 * tracks how many times it has been taken at runtime.
 */
typedef struct vtx_side_exit vtx_side_exit_t;

struct vtx_side_exit {
    vtx_side_exit_id_t  exit_id;       /* unique identifier */
    uint32_t            target_pc;     /* bytecode PC to resume at in interpreter */
    vtx_exit_stack_state_t stack_state;/* interpreter stack state at exit */
    vtx_side_exit_reason_t reason;     /* why this exit exists */
    uint32_t            exit_counter;  /* number of times this exit was taken */
    vtx_nodeid_t        guard_node;    /* the Guard node in SoN graph */
    uint32_t            trace_id;      /* which trace this exit belongs to */
    bool                has_branch;    /* true if a branch trace has been recorded */
};

/* ========================================================================== */
/* Side exit table                                                             */
/* ========================================================================== */

#define VTX_SIDE_EXIT_TABLE_INITIAL_CAPACITY 32

/**
 * Table of all side exits in the current compilation context.
 * Exits are identified by their exit_id (index into the array).
 */
typedef struct {
    vtx_side_exit_t **exits;    /* array of exit pointers (arena-allocated) */
    uint32_t          count;    /* number of exits in the table */
    uint32_t          capacity; /* allocated capacity */
} vtx_side_exit_table_t;

/* ========================================================================== */
/* Lifecycle functions                                                         */
/* ========================================================================== */

/**
 * Initialize a side exit table.
 * Returns 0 on success, -1 on failure.
 */
int vtx_side_exit_table_init(vtx_side_exit_table_t *table);

/**
 * Destroy a side exit table.
 * Does NOT free the exits themselves (they are arena-allocated).
 */
void vtx_side_exit_table_destroy(vtx_side_exit_table_t *table);

/* ========================================================================== */
/* Side exit creation and access                                               */
/* ========================================================================== */

/**
 * Create a new side exit with the given parameters.
 *
 * The stack_state and locals arrays are copied into arena-allocated memory.
 * The exit is registered in the table and assigned a unique exit_id.
 *
 * Returns a pointer to the new side exit, or NULL on failure.
 */
vtx_side_exit_t *vtx_side_exit_create(vtx_side_exit_table_t *table,
                                       vtx_arena_t *arena,
                                       uint32_t target_pc,
                                       const vtx_nodeid_t *stack,
                                       uint32_t stack_depth,
                                       const vtx_nodeid_t *locals,
                                       uint32_t local_count,
                                       vtx_side_exit_reason_t reason,
                                       vtx_nodeid_t guard_node,
                                       uint32_t trace_id);

/**
 * Look up a side exit by its ID.
 * Returns the exit, or NULL if the ID is invalid.
 */
vtx_side_exit_t *vtx_side_exit_get(vtx_side_exit_table_t *table,
                                    vtx_side_exit_id_t id);

/**
 * Increment the exit counter for a side exit.
 * Saturates at UINT32_MAX.
 */
void vtx_side_exit_increment(vtx_side_exit_t *exit);

/**
 * Check whether a side exit has been taken enough times to warrant
 * recording a new trace branch.
 *
 * Returns true if exit_counter > VTX_SIDE_EXIT_THRESHOLD.
 */
bool vtx_side_exit_should_record(const vtx_side_exit_t *exit);

/**
 * Get the number of active (non-invalid) side exits in the table.
 */
uint32_t vtx_side_exit_table_count(const vtx_side_exit_table_t *table);

/**
 * Find all side exits that should be recorded (counter > threshold).
 * Writes up to max_out exit pointers into the out array.
 * Returns the actual number of hot exits found.
 */
uint32_t vtx_side_exit_find_hot(const vtx_side_exit_table_t *table,
                                 vtx_side_exit_t **out,
                                 uint32_t max_out);

#endif /* VORTEX_SIDE_EXIT_H */
