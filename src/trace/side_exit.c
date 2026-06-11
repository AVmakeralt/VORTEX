/**
 * VORTEX Side Exit Handling — Implementation
 *
 * Manages side exit creation, counter tracking, and hot-exit detection
 * for the trace recorder. Side exits are the mechanism by which compiled
 * traces fall back to the interpreter when the speculative path is invalid.
 */

#include "trace/side_exit.h"
#include "runtime/arena.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Side exit table lifecycle                                                   */
/* ========================================================================== */

int vtx_side_exit_table_init(vtx_side_exit_table_t *table)
{
    VTX_ASSERT(table != NULL, "table must not be NULL");

    table->exits = malloc(sizeof(vtx_side_exit_t *) * VTX_SIDE_EXIT_TABLE_INITIAL_CAPACITY);
    if (table->exits == NULL) {
        return -1;
    }
    memset(table->exits, 0, sizeof(vtx_side_exit_t *) * VTX_SIDE_EXIT_TABLE_INITIAL_CAPACITY);
    table->count = 0;
    table->capacity = VTX_SIDE_EXIT_TABLE_INITIAL_CAPACITY;
    return 0;
}

void vtx_side_exit_table_destroy(vtx_side_exit_table_t *table)
{
    if (table == NULL) return;
    free(table->exits);
    table->exits = NULL;
    table->count = 0;
    table->capacity = 0;
}

/* ========================================================================== */
/* Grow the exit table if needed                                               */
/* ========================================================================== */

static int vtx_side_exit_table_grow(vtx_side_exit_table_t *table)
{
    uint32_t new_capacity = table->capacity * 2;
    vtx_side_exit_t **new_exits = realloc(table->exits,
                                           sizeof(vtx_side_exit_t *) * new_capacity);
    if (new_exits == NULL) {
        return -1;
    }
    /* Zero the newly allocated portion */
    memset(new_exits + table->capacity, 0,
           sizeof(vtx_side_exit_t *) * (new_capacity - table->capacity));
    table->exits = new_exits;
    table->capacity = new_capacity;
    return 0;
}

/* ========================================================================== */
/* Side exit creation                                                          */
/* ========================================================================== */

vtx_side_exit_t *vtx_side_exit_create(vtx_side_exit_table_t *table,
                                       vtx_arena_t *arena,
                                       uint32_t target_pc,
                                       const vtx_nodeid_t *stack,
                                       uint32_t stack_depth,
                                       const vtx_nodeid_t *locals,
                                       uint32_t local_count,
                                       vtx_side_exit_reason_t reason,
                                       vtx_nodeid_t guard_node,
                                       uint32_t trace_id)
{
    VTX_ASSERT(table != NULL, "table must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");
    VTX_ASSERT(stack_depth == 0 || stack != NULL, "stack must not be NULL if depth > 0");
    VTX_ASSERT(local_count == 0 || locals != NULL, "locals must not be NULL if count > 0");

    /* Grow table if necessary */
    if (table->count >= table->capacity) {
        if (vtx_side_exit_table_grow(table) != 0) {
            return NULL;
        }
    }

    /* Allocate the side exit from the arena */
    vtx_side_exit_t *exit = vtx_arena_alloc(arena, sizeof(vtx_side_exit_t));
    if (exit == NULL) {
        return NULL;
    }
    memset(exit, 0, sizeof(vtx_side_exit_t));

    /* Assign a unique ID */
    exit->exit_id = table->count;

    /* Set basic fields */
    exit->target_pc = target_pc;
    exit->reason = reason;
    exit->exit_counter = 0;
    exit->guard_node = guard_node;
    exit->trace_id = trace_id;
    exit->has_branch = false;

    /* Copy the operand stack state */
    exit->stack_state.stack_depth = stack_depth;
    if (stack_depth > 0) {
        exit->stack_state.stack = vtx_arena_alloc(arena,
                                                   sizeof(vtx_nodeid_t) * stack_depth);
        if (exit->stack_state.stack == NULL) {
            return NULL;
        }
        memcpy(exit->stack_state.stack, stack, sizeof(vtx_nodeid_t) * stack_depth);
    } else {
        exit->stack_state.stack = NULL;
    }

    /* Copy the local variable bindings */
    exit->stack_state.local_count = local_count;
    if (local_count > 0) {
        exit->stack_state.locals = vtx_arena_alloc(arena,
                                                    sizeof(vtx_nodeid_t) * local_count);
        if (exit->stack_state.locals == NULL) {
            return NULL;
        }
        memcpy(exit->stack_state.locals, locals, sizeof(vtx_nodeid_t) * local_count);
    } else {
        exit->stack_state.locals = NULL;
    }

    /* Register in table */
    table->exits[table->count] = exit;
    table->count++;

    return exit;
}

/* ========================================================================== */
/* Side exit access                                                            */
/* ========================================================================== */

vtx_side_exit_t *vtx_side_exit_get(vtx_side_exit_table_t *table,
                                    vtx_side_exit_id_t id)
{
    if (table == NULL || id >= table->count) {
        return NULL;
    }
    return table->exits[id];
}

/* ========================================================================== */
/* Side exit counter operations                                                */
/* ========================================================================== */

void vtx_side_exit_increment(vtx_side_exit_t *exit)
{
    if (exit == NULL) return;
    /* Saturating increment */
    if (exit->exit_counter < UINT32_MAX) {
        exit->exit_counter++;
    }
}

bool vtx_side_exit_should_record(const vtx_side_exit_t *exit)
{
    if (exit == NULL) return false;
    return exit->exit_counter > VTX_SIDE_EXIT_THRESHOLD && !exit->has_branch;
}

/* ========================================================================== */
/* Table queries                                                               */
/* ========================================================================== */

uint32_t vtx_side_exit_table_count(const vtx_side_exit_table_t *table)
{
    return table != NULL ? table->count : 0;
}

uint32_t vtx_side_exit_find_hot(const vtx_side_exit_table_t *table,
                                 vtx_side_exit_t **out,
                                 uint32_t max_out)
{
    if (table == NULL || out == NULL || max_out == 0) return 0;

    uint32_t found = 0;
    for (uint32_t i = 0; i < table->count && found < max_out; i++) {
        vtx_side_exit_t *exit = table->exits[i];
        if (exit != NULL && vtx_side_exit_should_record(exit)) {
            out[found++] = exit;
        }
    }
    return found;
}
