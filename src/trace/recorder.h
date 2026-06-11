#ifndef VORTEX_RECORDER_H
#define VORTEX_RECORDER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "ir/node.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/arena.h"
#include "interp/profiler.h"
#include "profile/data.h"
#include "trace/side_exit.h"

/**
 * VORTEX Trace Recorder
 *
 * Records hot execution traces by walking bytecode following the hot path
 * (determined by branch profiling data) and emitting Sea-of-Nodes IR nodes.
 *
 * Recording algorithm:
 *   1. Start at the loop header PC.
 *   2. Walk bytecode instructions sequentially.
 *   3. At each branch point (IF_TRUE/IF_FALSE), consult branch profile
 *      to determine the hot path. Follow the hot path.
 *   4. At each untaken branch, emit a Guard node that represents the
 *      side exit. The Guard captures the branch condition (negated)
 *      and a FrameState for deoptimization.
 *   5. Continue until: the loop header is reached again (closed trace),
 *      a RETURN is encountered (open trace), maximum trace length is
 *      hit (VTX_MAX_TRACE_LENGTH), or an unsupported opcode is reached.
 *   6. The result is a vtx_trace_t: a linear sequence of SoN node IDs
 *      with associated side exit descriptors.
 *
 * Trace properties:
 *   - Linear: no internal control flow merges (single path)
 *   - Guarded: every deviation from the recorded path is a Guard
 *   - Bounded: never exceeds VTX_MAX_TRACE_LENGTH bytecodes
 *   - Closed or open: closed if the loop header is reached, open if
 *     the method exits before looping back
 */

/* ========================================================================== */
/* Trace structure                                                             */
/* ========================================================================== */

/**
 * Unique identifier for a trace within a compilation context.
 */
typedef uint32_t vtx_trace_id_t;

#define VTX_TRACE_ID_INVALID ((vtx_trace_id_t)0xFFFFFFFF)

/**
 * Classification of a trace based on how it terminates.
 */
typedef enum {
    VTX_TRACE_OPEN = 0,    /* trace ends with a RETURN (doesn't loop back) */
    VTX_TRACE_CLOSED = 1,  /* trace loops back to the entry PC */
    VTX_TRACE_TRUNCATED = 2 /* trace was cut short by max length or error */
} vtx_trace_kind_t;

/**
 * A single recorded trace.
 *
 * Contains the linear sequence of SoN node IDs emitted during recording,
 * plus the side exits created at branch points.
 */
typedef struct vtx_trace vtx_trace_t;

struct vtx_trace {
    vtx_trace_id_t    trace_id;        /* unique identifier */
    vtx_trace_kind_t  kind;            /* open / closed / truncated */
    uint32_t          entry_pc;        /* bytecode PC where recording started */
    uint32_t          exit_pc;         /* bytecode PC where recording ended */
    uint32_t          method_id;       /* method identifier */

    /* Linear sequence of SoN node IDs emitted for this trace */
    vtx_nodeid_t     *nodes;           /* arena-allocated array */
    uint32_t          node_count;      /* number of nodes */
    uint32_t          node_capacity;   /* allocated capacity */

    /* Side exits created during recording */
    vtx_side_exit_t  **side_exits;     /* arena-allocated array of pointers */
    uint32_t          side_exit_count; /* number of side exits */
    uint32_t          side_exit_capacity;

    /* Trace-local operand stack and local variable tracking.
     * These map local/stack slots to the SoN NodeIDs that produce
     * their current values during trace recording. */
    vtx_nodeid_t     *locals;          /* arena-allocated, size = max_locals */
    uint32_t          local_count;

    /* Start node for this trace in the graph */
    vtx_nodeid_t      start_node;      /* the LoopBegin or Region node */
    vtx_nodeid_t      control_node;    /* current control output */
    vtx_nodeid_t      memory_node;     /* current memory state */

    /* Bytecode length (number of bytecode instructions recorded) */
    uint32_t          bytecode_length;
};

/* ========================================================================== */
/* Trace recorder                                                              */
/* ========================================================================== */

/**
 * The trace recorder holds the state needed to record one trace at a time.
 */
typedef struct {
    vtx_trace_id_t next_trace_id;       /* monotonically increasing ID counter */
    uint32_t       max_trace_length;    /* configurable, default VTX_MAX_TRACE_LENGTH */
} vtx_trace_recorder_t;

/* ========================================================================== */
/* Trace list                                                                  */
/* ========================================================================== */

#define VTX_TRACE_LIST_INITIAL_CAPACITY 8

/**
 * A list of recorded traces.
 */
typedef struct {
    vtx_trace_t  **traces;     /* array of trace pointers */
    uint32_t       count;      /* number of traces */
    uint32_t       capacity;   /* allocated capacity */
} vtx_trace_list_t;

/* ========================================================================== */
/* Recorder lifecycle                                                          */
/* ========================================================================== */

/**
 * Initialize the trace recorder.
 * Returns 0 on success, -1 on failure.
 */
int vtx_trace_recorder_init(vtx_trace_recorder_t *recorder);

/**
 * Destroy the trace recorder.
 */
void vtx_trace_recorder_destroy(vtx_trace_recorder_t *recorder);

/* ========================================================================== */
/* Trace recording                                                             */
/* ========================================================================== */

/**
 * Record a hot trace starting from entry_pc.
 *
 * Walks the bytecode of the given method following the hot path (as
 * determined by branch profiling data), emitting SoN nodes into the
 * provided graph. Side exits are created at branch points where the
 * untaken path diverges.
 *
 * Parameters:
 *   recorder  - the trace recorder
 *   graph     - the SoN graph to emit nodes into
 *   bytecode  - the bytecode module
 *   method    - the method descriptor (contains max_locals, max_stack)
 *   entry_pc  - the bytecode PC to start recording from
 *   profiler  - interpreter profiling data (for branch probabilities)
 *   profile   - global profile data (for detailed branch/loop info)
 *   arena     - arena allocator for trace data
 *
 * Returns the recorded trace, or NULL on failure.
 */
vtx_trace_t *vtx_trace_recorder_record(
    vtx_trace_recorder_t *recorder,
    vtx_graph_t *graph,
    const vtx_bytecode_t *bytecode,
    const vtx_method_desc_t *method,
    uint32_t entry_pc,
    const vtx_profiler_t *profiler,
    const vtx_profile_global_t *profile,
    vtx_arena_t *arena);

/* ========================================================================== */
/* Trace access                                                                */
/* ========================================================================== */

/**
 * Get the number of nodes in a trace.
 */
uint32_t vtx_trace_node_count(const vtx_trace_t *trace);

/**
 * Get the number of side exits in a trace.
 */
uint32_t vtx_trace_side_exit_count(const vtx_trace_t *trace);

/**
 * Check if a trace is closed (loops back to entry).
 */
bool vtx_trace_is_closed(const vtx_trace_t *trace);

/**
 * Get a node ID from the trace by index.
 * Returns VTX_NODEID_INVALID if index is out of bounds.
 */
vtx_nodeid_t vtx_trace_get_node(const vtx_trace_t *trace, uint32_t index);

/**
 * Get a side exit from the trace by index.
 * Returns NULL if index is out of bounds.
 */
vtx_side_exit_t *vtx_trace_get_side_exit(const vtx_trace_t *trace, uint32_t index);

/* ========================================================================== */
/* Trace list operations                                                       */
/* ========================================================================== */

/**
 * Initialize a trace list.
 */
int vtx_trace_list_init(vtx_trace_list_t *list);

/**
 * Destroy a trace list.
 */
void vtx_trace_list_destroy(vtx_trace_list_t *list);

/**
 * Append a trace to the list.
 * Returns 0 on success, -1 on failure.
 */
int vtx_trace_list_append(vtx_trace_list_t *list, vtx_trace_t *trace);

#endif /* VORTEX_RECORDER_H */
