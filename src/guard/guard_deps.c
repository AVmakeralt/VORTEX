#include "guard/guard_deps.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Internal helpers: per-guard dependency record lifecycle                      */
/* ========================================================================== */

/**
 * Initialize a single guard dependency record.
 * Sets all fields to zero/empty; arrays are allocated on first insertion.
 */
static void guard_deps_init(vtx_guard_deps_t *deps, uint32_t guard_id)
{
    deps->guard_id              = guard_id;
    deps->dependent_inline_sites = NULL;
    deps->inline_site_count     = 0;
    deps->inline_site_capacity  = 0;
    deps->dependent_sr_objects   = NULL;
    deps->sr_object_count       = 0;
    deps->sr_object_capacity    = 0;
    deps->dependent_branch_pcs  = NULL;
    deps->branch_pc_count       = 0;
    deps->branch_pc_capacity    = 0;
    deps->dependent_value_sites = NULL;
    deps->value_site_count      = 0;
    deps->value_site_capacity   = 0;
    deps->total_dep_count       = 0;
    deps->is_computed           = false;
}

/**
 * Free all dynamically allocated arrays within a guard dependency record.
 */
static void guard_deps_cleanup(vtx_guard_deps_t *deps)
{
    if (deps == NULL) return;

    if (deps->dependent_inline_sites != NULL) {
        free(deps->dependent_inline_sites);
        deps->dependent_inline_sites = NULL;
    }
    if (deps->dependent_sr_objects != NULL) {
        free(deps->dependent_sr_objects);
        deps->dependent_sr_objects = NULL;
    }
    if (deps->dependent_branch_pcs != NULL) {
        free(deps->dependent_branch_pcs);
        deps->dependent_branch_pcs = NULL;
    }
    if (deps->dependent_value_sites != NULL) {
        free(deps->dependent_value_sites);
        deps->dependent_value_sites = NULL;
    }

    deps->inline_site_count   = 0;
    deps->inline_site_capacity = 0;
    deps->sr_object_count     = 0;
    deps->sr_object_capacity  = 0;
    deps->branch_pc_count     = 0;
    deps->branch_pc_capacity  = 0;
    deps->value_site_count    = 0;
    deps->value_site_capacity = 0;
    deps->total_dep_count     = 0;
}

/* ========================================================================== */
/* Internal helpers: dynamic array insertion                                    */
/* ========================================================================== */

/**
 * Append a value to a dynamic uint32_t array.
 * Grows the array if needed (doubling strategy).
 * Returns 0 on success, -1 on allocation failure.
 */
static int array_push(uint32_t **array, uint32_t *count, uint32_t *capacity,
                       uint32_t value)
{
    if (*count >= *capacity) {
        uint32_t new_cap = (*capacity == 0)
            ? VTX_GUARD_DEPS_ARRAY_INITIAL_CAP
            : (*capacity) * 2;
        uint32_t *new_arr = (uint32_t *)realloc(
            *array, new_cap * sizeof(uint32_t));
        if (new_arr == NULL) return -1;
        *array    = new_arr;
        *capacity = new_cap;
    }
    (*array)[*count] = value;
    (*count)++;
    return 0;
}

/* ========================================================================== */
/* Internal helpers: ensure a deps slot exists for a given guard_id             */
/* ========================================================================== */

/**
 * Ensure that the deps array has a slot for the given guard_id.
 * Grows and initializes entries as needed.
 * Returns a pointer to the deps slot, or NULL on failure.
 */
static vtx_guard_deps_t *deps_ensure_slot(vtx_guard_deps_graph_t *graph,
                                            uint32_t guard_id)
{
    if (guard_id == VTX_GUARD_DEPS_ID_INVALID) return NULL;

    /* Grow the deps array if needed */
    while (guard_id >= graph->deps_capacity) {
        uint32_t new_cap = graph->deps_capacity * 2;
        if (new_cap == 0) new_cap = VTX_GUARD_DEPS_INITIAL_CAPACITY;
        vtx_guard_deps_t *new_deps = (vtx_guard_deps_t *)realloc(
            graph->deps, new_cap * sizeof(vtx_guard_deps_t));
        if (new_deps == NULL) return NULL;

        /* Zero-initialize the new slots */
        memset(new_deps + graph->deps_capacity, 0,
               (new_cap - graph->deps_capacity) * sizeof(vtx_guard_deps_t));

        graph->deps         = new_deps;
        graph->deps_capacity = new_cap;
    }

    /* If this is a new entry, initialize it */
    if (guard_id >= graph->deps_count) {
        for (uint32_t i = graph->deps_count; i <= guard_id; i++) {
            guard_deps_init(&graph->deps[i], i);
        }
        graph->deps_count = guard_id + 1;
    }

    return &graph->deps[guard_id];
}

/* ========================================================================== */
/* Internal helpers: reverse mapping (node_to_guard)                            */
/* ========================================================================== */

/**
 * Ensure the node_to_guard array is large enough for the given node_id.
 * New entries are initialized to VTX_GUARD_DEPS_ID_INVALID (no dependency).
 * Returns 0 on success, -1 on failure.
 */
static int node_to_guard_ensure(vtx_guard_deps_graph_t *graph,
                                 uint32_t node_id)
{
    if (node_id == VTX_NODEID_INVALID) return -1;

    if (node_id >= graph->node_to_guard_capacity) {
        uint32_t new_cap = graph->node_to_guard_capacity * 2;
        if (new_cap == 0) new_cap = VTX_GUARD_DEPS_INITIAL_CAPACITY * 8;
        while (new_cap <= node_id) new_cap *= 2;

        uint32_t *new_arr = (uint32_t *)realloc(
            graph->node_to_guard, new_cap * sizeof(uint32_t));
        if (new_arr == NULL) return -1;

        /* Initialize new entries to VTX_GUARD_DEPS_ID_INVALID */
        for (uint32_t i = graph->node_to_guard_capacity; i < new_cap; i++) {
            new_arr[i] = VTX_GUARD_DEPS_ID_INVALID;
        }

        graph->node_to_guard         = new_arr;
        graph->node_to_guard_capacity = new_cap;
    }

    return 0;
}

/**
 * Record that a node depends on a guard in the reverse mapping.
 * Only overwrites the existing entry if it is VTX_GUARD_DEPS_ID_INVALID.
 * This is a "first-writer-wins" policy: if a node already depends on
 * a guard, we don't change it. This is correct because most nodes
 * have a single primary guard, and the first one encountered during
 * the walk is the most directly related.
 *
 * Returns 0 on success, -1 on failure.
 */
static int record_node_dependency(vtx_guard_deps_graph_t *graph,
                                   uint32_t node_id, uint32_t guard_id)
{
    if (node_to_guard_ensure(graph, node_id) != 0) return -1;
    if (graph->node_to_guard[node_id] == VTX_GUARD_DEPS_ID_INVALID) {
        graph->node_to_guard[node_id] = guard_id;
    }
    return 0;
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_guard_deps_graph_init(vtx_guard_deps_graph_t *graph)
{
    if (graph == NULL) return -1;

    graph->deps = (vtx_guard_deps_t *)malloc(
        VTX_GUARD_DEPS_INITIAL_CAPACITY * sizeof(vtx_guard_deps_t));
    if (graph->deps == NULL) return -1;

    /* Zero-initialize all slots */
    memset(graph->deps, 0,
           VTX_GUARD_DEPS_INITIAL_CAPACITY * sizeof(vtx_guard_deps_t));

    graph->deps_count    = 0;
    graph->deps_capacity = VTX_GUARD_DEPS_INITIAL_CAPACITY;

    /* node_to_guard is allocated lazily when we know the node table size */
    graph->node_to_guard         = NULL;
    graph->node_to_guard_capacity = 0;

    /* Statistics */
    graph->total_inline_deps = 0;
    graph->total_sr_deps     = 0;
    graph->total_branch_deps = 0;
    graph->total_value_deps  = 0;

    return 0;
}

void vtx_guard_deps_graph_destroy(vtx_guard_deps_graph_t *graph)
{
    if (graph == NULL) return;

    /* Free all per-guard dependency arrays */
    if (graph->deps != NULL) {
        for (uint32_t i = 0; i < graph->deps_count; i++) {
            guard_deps_cleanup(&graph->deps[i]);
        }
        free(graph->deps);
        graph->deps = NULL;
    }

    /* Free the reverse mapping */
    if (graph->node_to_guard != NULL) {
        free(graph->node_to_guard);
        graph->node_to_guard = NULL;
    }

    graph->deps_count            = 0;
    graph->deps_capacity         = 0;
    graph->node_to_guard_capacity = 0;
    graph->total_inline_deps     = 0;
    graph->total_sr_deps         = 0;
    graph->total_branch_deps     = 0;
    graph->total_value_deps      = 0;
}

/* ========================================================================== */
/* Build the graph from a SoN graph                                            */
/* ========================================================================== */

/**
 * Internal: allocate the node_to_guard array sized for the full node table.
 * Returns 0 on success, -1 on failure.
 */
static int alloc_node_to_guard(vtx_guard_deps_graph_t *graph,
                                uint32_t node_count)
{
    if (node_count == 0) return 0;

    /* Allocate generously to avoid repeated reallocs */
    uint32_t cap = node_count + 64;
    uint32_t *arr = (uint32_t *)malloc(cap * sizeof(uint32_t));
    if (arr == NULL) return -1;

    /* Initialize all entries to VTX_GUARD_DEPS_ID_INVALID */
    for (uint32_t i = 0; i < cap; i++) {
        arr[i] = VTX_GUARD_DEPS_ID_INVALID;
    }

    graph->node_to_guard          = arr;
    graph->node_to_guard_capacity = cap;
    return 0;
}

/**
 * Internal: find all DeoptGuard nodes in the graph and ensure deps slots.
 * Returns the number of DeoptGuard nodes found, or -1 on failure.
 */
static int find_deopt_guards(const vtx_graph_t *son_graph,
                              vtx_guard_deps_graph_t *graph)
{
    const vtx_node_table_t *nt = &son_graph->node_table;
    int guard_count = 0;

    for (uint32_t i = 0; i < nt->count; i++) {
        const vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;

        if (node->opcode == VTX_OP_DeoptGuard || node->opcode == VTX_OP_Guard) {
            /* Use the node's ID as the guard_id.
             * This matches the convention in deoptless.h where
             * vtx_guard_id_t = vtx_nodeid_t. */
            vtx_guard_deps_t *deps = deps_ensure_slot(graph, node->id);
            if (deps == NULL) return -1;
            guard_count++;
        }
    }

    return guard_count;
}

/**
 * Internal: for each DeoptGuard, find CheckCast/InstanceOf nodes at the
 * same bytecode_pc. These represent type narrows that depend on the guard.
 *
 * Also records the dependency in the node_to_guard reverse mapping.
 */
static int build_type_narrow_deps(const vtx_graph_t *son_graph,
                                   vtx_guard_deps_graph_t *graph)
{
    const vtx_node_table_t *nt = &son_graph->node_table;

    for (uint32_t i = 0; i < nt->count; i++) {
        const vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;

        if (node->opcode != VTX_OP_CheckCast &&
            node->opcode != VTX_OP_InstanceOf) {
            continue;
        }

        uint32_t pc = node->bytecode_pc;

        /* Find the DeoptGuard at the same bytecode_pc.
         * A CheckCast at PC X depends on the type guard at PC X.
         * If the type guard fails, the CheckCast's narrowed type
         * is invalid and must be undone. */
        for (uint32_t j = 0; j < nt->count; j++) {
            const vtx_node_t *guard_node = &nt->nodes[j];
            if (guard_node->dead) continue;

            if ((guard_node->opcode == VTX_OP_DeoptGuard ||
                 guard_node->opcode == VTX_OP_Guard) &&
                guard_node->bytecode_pc == pc) {

                /* Record the type-check node as depending on this guard.
                 * The CheckCast/InstanceOf is an inline dependency because
                 * it typically enables inlining of the guarded call. */
                vtx_guard_deps_t *deps = deps_ensure_slot(graph, guard_node->id);
                if (deps == NULL) return -1;

                if (vtx_guard_deps_add_inline(graph, guard_node->id, pc) != 0) {
                    return -1;
                }

                /* Record in reverse mapping */
                if (record_node_dependency(graph, node->id, guard_node->id) != 0) {
                    return -1;
                }

                break; /* Only one guard per PC */
            }
        }
    }

    return 0;
}

/**
 * Internal: for each inlined CallVirtual/CallInterface, find which guard
 * devirtualized it by checking if the guard's output feeds the call's
 * receiver type check.
 *
 * A virtual call is devirtualized when a guard proves the receiver type,
 * allowing the compiler to replace the virtual dispatch with a direct call
 * (or inline it entirely). The call site's bytecode_pc is recorded as a
 * dependent inline site.
 */
static int build_inline_deps(const vtx_graph_t *son_graph,
                              vtx_guard_deps_graph_t *graph)
{
    const vtx_node_table_t *nt = &son_graph->node_table;

    for (uint32_t i = 0; i < nt->count; i++) {
        const vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;

        /* Look for virtual calls that may have been devirtualized */
        if (node->opcode != VTX_OP_CallVirtual &&
            node->opcode != VTX_OP_CallInterface) {
            continue;
        }

        uint32_t call_pc = node->bytecode_pc;

        /* Walk this node's inputs to find a guard that feeds the
         * receiver type check. In the SoN graph, a devirtualized call
         * has a guard node as one of its inputs (the guard that proved
         * the receiver type). */
        for (uint32_t inp = 0; inp < node->input_count; inp++) {
            vtx_nodeid_t input_id = node->inputs[inp];
            if (input_id == VTX_NODEID_INVALID) continue;

            const vtx_node_t *input_node = vtx_node_get_const(nt, input_id);
            if (input_node == NULL) continue;

            if (input_node->opcode == VTX_OP_DeoptGuard ||
                input_node->opcode == VTX_OP_Guard) {

                /* This call depends on this guard for devirtualization */
                if (vtx_guard_deps_add_inline(graph, input_id, call_pc) != 0) {
                    return -1;
                }

                /* Record in reverse mapping */
                if (record_node_dependency(graph, node->id, input_id) != 0) {
                    return -1;
                }
            }
        }
    }

    return 0;
}

/**
 * Internal: for each allocation that was scalar-replaced (NoEscape in PEA),
 * find which guard proved that the object doesn't escape.
 *
 * A scalar-replaced allocation depends on a guard if:
 *   - The guard is a type check that proves the allocation's class
 *   - The allocation is only reachable through the guard's control path
 *     (i.e., the guard eliminates a path where the allocation would escape)
 *
 * We check this by seeing if any DeoptGuard is a control input to the
 * allocation's region of the graph.
 */
static int build_sr_deps(const vtx_graph_t *son_graph,
                          const vtx_pea_analysis_t *pea,
                          vtx_guard_deps_graph_t *graph)
{
    if (pea == NULL) return 0; /* No PEA — nothing to do */

    const vtx_node_table_t *nt = &son_graph->node_table;

    for (uint32_t i = 0; i < nt->count; i++) {
        const vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;

        /* Only consider allocation nodes */
        if (node->opcode != VTX_OP_NewObject &&
            node->opcode != VTX_OP_NewArray &&
            node->opcode != VTX_OP_Allocate) {
            continue;
        }

        /* Check if this allocation was scalar-replaced */
        if (!vtx_pea_is_scalar_replaceable(pea, node->id)) {
            continue;
        }

        /* Find which guard this scalar replacement depends on.
         *
         * Heuristic: walk the allocation's inputs (control + data) and
         * find any DeoptGuard/Guard node. An allocation that is
         * scalar-replaced typically depends on the guard that proved:
         *   - The allocation doesn't escape on the guarded path
         *   - The allocation's type is known (enabling field access)
         *
         * We also check the use-def list: if any guard directly uses
         * this allocation's output (e.g., a type check on the object),
         * that guard is the dependency. */
        uint32_t dep_guard_id = VTX_GUARD_DEPS_ID_INVALID;

        /* Check inputs for guard dependencies */
        for (uint32_t inp = 0; inp < node->input_count; inp++) {
            vtx_nodeid_t input_id = node->inputs[inp];
            if (input_id == VTX_NODEID_INVALID) continue;

            const vtx_node_t *input_node = vtx_node_get_const(nt, input_id);
            if (input_node == NULL) continue;

            if (input_node->opcode == VTX_OP_DeoptGuard ||
                input_node->opcode == VTX_OP_Guard) {
                dep_guard_id = input_id;
                break;
            }
        }

        /* If no direct input guard, check users of this allocation.
         * A guard that checks this object's type creates a dependency. */
        if (dep_guard_id == VTX_GUARD_DEPS_ID_INVALID) {
            const vtx_use_entry_t *use = vtx_node_uses_begin_const(node);
            const vtx_use_entry_t *end = vtx_node_uses_end_const(node);
            for (; use != end; ++use) {
                const vtx_node_t *user = vtx_node_get_const(nt, use->user_id);
                if (user == NULL) continue;
                if (user->opcode == VTX_OP_DeoptGuard ||
                    user->opcode == VTX_OP_Guard) {
                    dep_guard_id = use->user_id;
                    break;
                }
            }
        }

        /* If we still haven't found a guard, try a broader search:
         * find a guard at the same bytecode_pc as the allocation. */
        if (dep_guard_id == VTX_GUARD_DEPS_ID_INVALID) {
            uint32_t alloc_pc = node->bytecode_pc;
            for (uint32_t j = 0; j < nt->count; j++) {
                const vtx_node_t *guard_node = &nt->nodes[j];
                if (guard_node->dead) continue;
                if ((guard_node->opcode == VTX_OP_DeoptGuard ||
                     guard_node->opcode == VTX_OP_Guard) &&
                    guard_node->bytecode_pc == alloc_pc) {
                    dep_guard_id = guard_node->id;
                    break;
                }
            }
        }

        if (dep_guard_id != VTX_GUARD_DEPS_ID_INVALID) {
            if (vtx_guard_deps_add_sr_object(graph, dep_guard_id, node->id) != 0) {
                return -1;
            }
            if (record_node_dependency(graph, node->id, dep_guard_id) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

/**
 * Internal: for each If node whose branch was eliminated (one direction is
 * unreachable), find which guard proved the branch condition.
 *
 * A branch is eliminated when a guard proves the condition:
 *   guard(obj != null) → if (obj != null) { A } else { B }
 *   After guard: the else branch is unreachable → replace If with Goto
 *
 * The eliminated If's bytecode_pc is recorded as a dependent branch PC.
 */
static int build_branch_deps(const vtx_graph_t *son_graph,
                              vtx_guard_deps_graph_t *graph)
{
    const vtx_node_table_t *nt = &son_graph->node_table;

    for (uint32_t i = 0; i < nt->count; i++) {
        const vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;

        if (node->opcode != VTX_OP_If) continue;

        /* An If node with a condition of VTX_COND_ALWAYS or VTX_COND_NEVER
         * has had one branch eliminated. We need to find which guard
         * proved the condition. */
        if (node->cond != VTX_COND_ALWAYS && node->cond != VTX_COND_NEVER) {
            continue;
        }

        uint32_t branch_pc = node->bytecode_pc;
        uint32_t dep_guard_id = VTX_GUARD_DEPS_ID_INVALID;

        /* Walk the If's condition input to find the originating guard.
         * The If node's first input is typically the condition value,
         * which may come from a CmpP/Cmp that takes a guard as input,
         * or directly from a guard node. */
        for (uint32_t inp = 0; inp < node->input_count; inp++) {
            vtx_nodeid_t input_id = node->inputs[inp];
            if (input_id == VTX_NODEID_INVALID) continue;

            const vtx_node_t *input_node = vtx_node_get_const(nt, input_id);
            if (input_node == NULL) continue;

            /* Direct guard input */
            if (input_node->opcode == VTX_OP_DeoptGuard ||
                input_node->opcode == VTX_OP_Guard) {
                dep_guard_id = input_id;
                break;
            }

            /* CmpP/Cmp that takes a guard's condition as input:
             * Walk one more level of indirection. */
            if (input_node->opcode == VTX_OP_CmpP ||
                input_node->opcode == VTX_OP_Cmp) {
                for (uint32_t k = 0; k < input_node->input_count; k++) {
                    vtx_nodeid_t cmp_input_id = input_node->inputs[k];
                    if (cmp_input_id == VTX_NODEID_INVALID) continue;

                    const vtx_node_t *cmp_input = vtx_node_get_const(nt, cmp_input_id);
                    if (cmp_input == NULL) continue;

                    if (cmp_input->opcode == VTX_OP_DeoptGuard ||
                        cmp_input->opcode == VTX_OP_Guard) {
                        dep_guard_id = cmp_input_id;
                        break;
                    }
                }
                if (dep_guard_id != VTX_GUARD_DEPS_ID_INVALID) break;
            }
        }

        /* Fallback: find a guard at the same bytecode_pc */
        if (dep_guard_id == VTX_GUARD_DEPS_ID_INVALID) {
            for (uint32_t j = 0; j < nt->count; j++) {
                const vtx_node_t *guard_node = &nt->nodes[j];
                if (guard_node->dead) continue;
                if ((guard_node->opcode == VTX_OP_DeoptGuard ||
                     guard_node->opcode == VTX_OP_Guard) &&
                    guard_node->bytecode_pc == branch_pc) {
                    dep_guard_id = guard_node->id;
                    break;
                }
            }
        }

        if (dep_guard_id != VTX_GUARD_DEPS_ID_INVALID) {
            if (vtx_guard_deps_add_branch(graph, dep_guard_id, branch_pc) != 0) {
                return -1;
            }
            if (record_node_dependency(graph, node->id, dep_guard_id) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

/**
 * Internal: for each LoadField that was constant-folded based on a value
 * speculation guard, record the load's bytecode_pc as a dependent value site.
 *
 * A LoadField is constant-folded when a value guard proves that the field
 * always holds a specific constant value. If the guard fails, the load
 * must be re-materialized from memory (the constant replacement is invalid).
 */
static int build_value_deps(const vtx_graph_t *son_graph,
                             const vtx_value_profile_table_t *value_profiles,
                             vtx_guard_deps_graph_t *graph)
{
    const vtx_node_table_t *nt = &son_graph->node_table;

    for (uint32_t i = 0; i < nt->count; i++) {
        const vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;

        if (node->opcode != VTX_OP_LoadField) continue;

        /* Check if this load was constant-folded (its type is a concrete
         * constant and it has a value guard as input). A constant-folded
         * load typically has been replaced by a Constant node, but we can
         * detect the original pattern by checking:
         *   1. There is a value profile for this bytecode_pc
         *   2. A DeoptGuard is among the load's inputs */
        uint32_t load_pc = node->bytecode_pc;
        bool has_value_profile = false;

        if (value_profiles != NULL) {
            const vtx_value_profile_t *vp =
                vtx_value_profile_lookup(value_profiles, load_pc);
            has_value_profile = (vp != NULL && vp->is_stable);
        }

        if (!has_value_profile) continue;

        /* Find the guard that backs this value speculation */
        uint32_t dep_guard_id = VTX_GUARD_DEPS_ID_INVALID;

        for (uint32_t inp = 0; inp < node->input_count; inp++) {
            vtx_nodeid_t input_id = node->inputs[inp];
            if (input_id == VTX_NODEID_INVALID) continue;

            const vtx_node_t *input_node = vtx_node_get_const(nt, input_id);
            if (input_node == NULL) continue;

            if (input_node->opcode == VTX_OP_DeoptGuard ||
                input_node->opcode == VTX_OP_Guard) {
                dep_guard_id = input_id;
                break;
            }
        }

        /* Fallback: find a guard at the same bytecode_pc */
        if (dep_guard_id == VTX_GUARD_DEPS_ID_INVALID) {
            for (uint32_t j = 0; j < nt->count; j++) {
                const vtx_node_t *guard_node = &nt->nodes[j];
                if (guard_node->dead) continue;
                if ((guard_node->opcode == VTX_OP_DeoptGuard ||
                     guard_node->opcode == VTX_OP_Guard) &&
                    guard_node->bytecode_pc == load_pc) {
                    dep_guard_id = guard_node->id;
                    break;
                }
            }
        }

        if (dep_guard_id != VTX_GUARD_DEPS_ID_INVALID) {
            if (vtx_guard_deps_add_value_site(graph, dep_guard_id, load_pc) != 0) {
                return -1;
            }
            if (record_node_dependency(graph, node->id, dep_guard_id) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

/**
 * Internal: for each DeoptGuard/Guard node, walk its use-def list and
 * record all users as depending on the guard. This catches nodes that
 * aren't found by the category-specific passes above.
 *
 * This is a catch-all pass that ensures the reverse mapping is complete.
 */
static int build_use_def_deps(const vtx_graph_t *son_graph,
                               vtx_guard_deps_graph_t *graph)
{
    const vtx_node_table_t *nt = &son_graph->node_table;

    for (uint32_t i = 0; i < nt->count; i++) {
        const vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;

        if (node->opcode != VTX_OP_DeoptGuard &&
            node->opcode != VTX_OP_Guard) {
            continue;
        }

        uint32_t guard_id = node->id;

        /* Walk all users of this guard node */
        const vtx_use_entry_t *use = vtx_node_uses_begin_const(node);
        const vtx_use_entry_t *end = vtx_node_uses_end_const(node);
        for (; use != end; ++use) {
            /* Record the user node as depending on this guard.
             * The first-writer-wins policy means this won't overwrite
             * a more specific dependency recorded by a category pass. */
            if (record_node_dependency(graph, use->user_id, guard_id) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

/**
 * Internal: mark all guard deps as computed and update statistics.
 */
static void finalize_graph(vtx_guard_deps_graph_t *graph)
{
    /* Reset statistics */
    graph->total_inline_deps = 0;
    graph->total_sr_deps     = 0;
    graph->total_branch_deps = 0;
    graph->total_value_deps  = 0;

    for (uint32_t i = 0; i < graph->deps_count; i++) {
        vtx_guard_deps_t *deps = &graph->deps[i];
        deps->is_computed = true;

        graph->total_inline_deps += deps->inline_site_count;
        graph->total_sr_deps     += deps->sr_object_count;
        graph->total_branch_deps += deps->branch_pc_count;
        graph->total_value_deps  += deps->value_site_count;
    }
}

int vtx_guard_deps_build(vtx_guard_deps_graph_t *graph,
                          const vtx_graph_t *son_graph,
                          const vtx_pea_analysis_t *pea,
                          const vtx_value_profile_table_t *value_profiles)
{
    if (graph == NULL || son_graph == NULL) return -1;

    /* Step 0: Allocate the reverse mapping array sized for the node table */
    if (alloc_node_to_guard(graph, son_graph->node_table.count) != 0) {
        return -1;
    }

    /* Step 1: Find all DeoptGuard nodes and ensure deps slots */
    if (find_deopt_guards(son_graph, graph) < 0) {
        return -1;
    }

    /* Step 2: Build type-narrow dependencies (CheckCast/InstanceOf) */
    if (build_type_narrow_deps(son_graph, graph) != 0) {
        return -1;
    }

    /* Step 3: Build inline dependencies (devirtualized calls) */
    if (build_inline_deps(son_graph, graph) != 0) {
        return -1;
    }

    /* Step 4: Build scalar replacement dependencies (PEA) */
    if (build_sr_deps(son_graph, pea, graph) != 0) {
        return -1;
    }

    /* Step 5: Build branch elimination dependencies */
    if (build_branch_deps(son_graph, graph) != 0) {
        return -1;
    }

    /* Step 6: Build value speculation dependencies */
    if (build_value_deps(son_graph, value_profiles, graph) != 0) {
        return -1;
    }

    /* Step 7: Walk use-def lists for catch-all reverse mapping */
    if (build_use_def_deps(son_graph, graph) != 0) {
        return -1;
    }

    /* Step 8: Finalize — mark computed, update statistics */
    finalize_graph(graph);

    return 0;
}

/* ========================================================================== */
/* Query                                                                       */
/* ========================================================================== */

const vtx_guard_deps_t *vtx_guard_deps_lookup(const vtx_guard_deps_graph_t *graph,
                                                 uint32_t guard_id)
{
    if (graph == NULL) return NULL;
    if (guard_id == VTX_GUARD_DEPS_ID_INVALID) return NULL;
    if (guard_id >= graph->deps_count) return NULL;

    const vtx_guard_deps_t *deps = &graph->deps[guard_id];
    if (deps->guard_id != guard_id) return NULL;

    return deps;
}

uint32_t vtx_guard_deps_for_node(const vtx_guard_deps_graph_t *graph,
                                   uint32_t node_id)
{
    if (graph == NULL) return VTX_GUARD_DEPS_ID_INVALID;
    if (node_id == VTX_NODEID_INVALID) return VTX_GUARD_DEPS_ID_INVALID;
    if (graph->node_to_guard == NULL) return VTX_GUARD_DEPS_ID_INVALID;
    if (node_id >= graph->node_to_guard_capacity) return VTX_GUARD_DEPS_ID_INVALID;

    return graph->node_to_guard[node_id];
}

/* ========================================================================== */
/* Invalidation                                                                */
/* ========================================================================== */

uint32_t vtx_guard_deps_invalidate(vtx_guard_deps_graph_t *graph,
                                     uint32_t guard_id,
                                     uint32_t **out_inline_sites,
                                     uint32_t *out_inline_count,
                                     uint32_t **out_sr_objects,
                                     uint32_t *out_sr_count,
                                     uint32_t **out_branch_pcs,
                                     uint32_t *out_branch_count,
                                     uint32_t **out_value_sites,
                                     uint32_t *out_value_count)
{
    if (graph == NULL || out_inline_count == NULL || out_sr_count == NULL ||
        out_branch_count == NULL || out_value_count == NULL) {
        return VTX_DEP_INVALIDATE_NONE;
    }

    /* Initialize all outputs to "no results" */
    *out_inline_count = 0;
    *out_sr_count     = 0;
    *out_branch_count = 0;
    *out_value_count  = 0;

    if (out_inline_sites != NULL) *out_inline_sites = NULL;
    if (out_sr_objects != NULL)   *out_sr_objects   = NULL;
    if (out_branch_pcs != NULL)   *out_branch_pcs   = NULL;
    if (out_value_sites != NULL)  *out_value_sites  = NULL;

    const vtx_guard_deps_t *deps = vtx_guard_deps_lookup(graph, guard_id);
    if (deps == NULL) {
        return VTX_DEP_INVALIDATE_NONE;
    }

    uint32_t mask = VTX_DEP_INVALIDATE_NONE;

    /* Inline sites */
    if (deps->inline_site_count > 0) {
        mask |= VTX_DEP_INVALIDATE_INLINE;
        if (out_inline_sites != NULL) {
            *out_inline_sites = deps->dependent_inline_sites;
        }
        if (out_inline_count != NULL) {
            *out_inline_count = deps->inline_site_count;
        }
    }

    /* Scalar replacement objects */
    if (deps->sr_object_count > 0) {
        mask |= VTX_DEP_INVALIDATE_SR;
        if (out_sr_objects != NULL) {
            *out_sr_objects = deps->dependent_sr_objects;
        }
        if (out_sr_count != NULL) {
            *out_sr_count = deps->sr_object_count;
        }
    }

    /* Branch PCs */
    if (deps->branch_pc_count > 0) {
        mask |= VTX_DEP_INVALIDATE_BRANCH;
        if (out_branch_pcs != NULL) {
            *out_branch_pcs = deps->dependent_branch_pcs;
        }
        if (out_branch_count != NULL) {
            *out_branch_count = deps->branch_pc_count;
        }
    }

    /* Value sites */
    if (deps->value_site_count > 0) {
        mask |= VTX_DEP_INVALIDATE_VALUE;
        if (out_value_sites != NULL) {
            *out_value_sites = deps->dependent_value_sites;
        }
        if (out_value_count != NULL) {
            *out_value_count = deps->value_site_count;
        }
    }

    return mask;
}

/* ========================================================================== */
/* Manual dependency addition                                                   */
/* ========================================================================== */

int vtx_guard_deps_add_inline(vtx_guard_deps_graph_t *graph,
                                uint32_t guard_id, uint32_t bytecode_pc)
{
    if (graph == NULL) return -1;

    vtx_guard_deps_t *deps = deps_ensure_slot(graph, guard_id);
    if (deps == NULL) return -1;

    /* Check for duplicate */
    for (uint32_t i = 0; i < deps->inline_site_count; i++) {
        if (deps->dependent_inline_sites[i] == bytecode_pc) {
            return 0; /* already recorded */
        }
    }

    if (array_push(&deps->dependent_inline_sites,
                   &deps->inline_site_count,
                   &deps->inline_site_capacity,
                   bytecode_pc) != 0) {
        return -1;
    }

    deps->total_dep_count++;
    graph->total_inline_deps++;

    return 0;
}

int vtx_guard_deps_add_sr_object(vtx_guard_deps_graph_t *graph,
                                   uint32_t guard_id, uint32_t alloc_node_id)
{
    if (graph == NULL) return -1;

    vtx_guard_deps_t *deps = deps_ensure_slot(graph, guard_id);
    if (deps == NULL) return -1;

    /* Check for duplicate */
    for (uint32_t i = 0; i < deps->sr_object_count; i++) {
        if (deps->dependent_sr_objects[i] == alloc_node_id) {
            return 0; /* already recorded */
        }
    }

    if (array_push(&deps->dependent_sr_objects,
                   &deps->sr_object_count,
                   &deps->sr_object_capacity,
                   alloc_node_id) != 0) {
        return -1;
    }

    deps->total_dep_count++;
    graph->total_sr_deps++;

    /* Also record in reverse mapping */
    if (record_node_dependency(graph, alloc_node_id, guard_id) != 0) {
        return -1;
    }

    return 0;
}

int vtx_guard_deps_add_branch(vtx_guard_deps_graph_t *graph,
                                uint32_t guard_id, uint32_t bytecode_pc)
{
    if (graph == NULL) return -1;

    vtx_guard_deps_t *deps = deps_ensure_slot(graph, guard_id);
    if (deps == NULL) return -1;

    /* Check for duplicate */
    for (uint32_t i = 0; i < deps->branch_pc_count; i++) {
        if (deps->dependent_branch_pcs[i] == bytecode_pc) {
            return 0; /* already recorded */
        }
    }

    if (array_push(&deps->dependent_branch_pcs,
                   &deps->branch_pc_count,
                   &deps->branch_pc_capacity,
                   bytecode_pc) != 0) {
        return -1;
    }

    deps->total_dep_count++;
    graph->total_branch_deps++;

    return 0;
}

int vtx_guard_deps_add_value_site(vtx_guard_deps_graph_t *graph,
                                    uint32_t guard_id, uint32_t bytecode_pc)
{
    if (graph == NULL) return -1;

    vtx_guard_deps_t *deps = deps_ensure_slot(graph, guard_id);
    if (deps == NULL) return -1;

    /* Check for duplicate */
    for (uint32_t i = 0; i < deps->value_site_count; i++) {
        if (deps->dependent_value_sites[i] == bytecode_pc) {
            return 0; /* already recorded */
        }
    }

    if (array_push(&deps->dependent_value_sites,
                   &deps->value_site_count,
                   &deps->value_site_capacity,
                   bytecode_pc) != 0) {
        return -1;
    }

    deps->total_dep_count++;
    graph->total_value_deps++;

    return 0;
}

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

uint32_t vtx_guard_deps_active_guard_count(const vtx_guard_deps_graph_t *graph)
{
    if (graph == NULL) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < graph->deps_count; i++) {
        if (graph->deps[i].total_dep_count > 0) {
            count++;
        }
    }
    return count;
}

uint32_t vtx_guard_deps_total_dependency_count(const vtx_guard_deps_graph_t *graph)
{
    if (graph == NULL) return 0;

    return graph->total_inline_deps +
           graph->total_sr_deps +
           graph->total_branch_deps +
           graph->total_value_deps;
}
