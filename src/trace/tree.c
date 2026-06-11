/**
 * VORTEX Trace Trees — Implementation
 *
 * Builds trace trees from hot loops and extends them with branch traces
 * when side exits become hot. The tree structure enables the region
 * formation (stitching) phase to combine the root and hot branches
 * into a single hyperblock for cross-trace optimization.
 */

#include "trace/tree.h"
#include "trace/recorder.h"
#include "trace/side_exit.h"
#include "runtime/arena.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Branch helpers                                                              */
/* ========================================================================== */

static vtx_trace_branch_t *vtx_trace_branch_create(vtx_arena_t *arena,
                                                     vtx_side_exit_id_t parent_exit_id,
                                                     vtx_trace_t *trace,
                                                     uint32_t depth,
                                                     vtx_trace_branch_t *parent)
{
    vtx_trace_branch_t *branch = vtx_arena_alloc(arena, sizeof(vtx_trace_branch_t));
    if (branch == NULL) return NULL;
    memset(branch, 0, sizeof(vtx_trace_branch_t));

    branch->parent_exit_id = parent_exit_id;
    branch->trace = trace;
    branch->depth = depth;
    branch->parent = parent;

    /* Allocate children array */
    branch->child_capacity = 4;
    branch->children = vtx_arena_alloc(arena,
                                        sizeof(vtx_trace_branch_t *) * branch->child_capacity);
    if (branch->children == NULL) return NULL;
    memset(branch->children, 0, sizeof(vtx_trace_branch_t *) * branch->child_capacity);
    branch->child_count = 0;

    return branch;
}

static int vtx_trace_branch_add_child(vtx_arena_t *arena,
                                       vtx_trace_branch_t *parent,
                                       vtx_trace_branch_t *child)
{
    if (parent->child_count >= parent->child_capacity) {
        uint32_t new_cap = parent->child_capacity * 2;
        vtx_trace_branch_t **new_children = vtx_arena_alloc(arena,
                sizeof(vtx_trace_branch_t *) * new_cap);
        if (new_children == NULL) return -1;
        memcpy(new_children, parent->children,
               sizeof(vtx_trace_branch_t *) * parent->child_count);
        memset(new_children + parent->child_count, 0,
               sizeof(vtx_trace_branch_t *) * (new_cap - parent->child_count));
        parent->children = new_children;
        parent->child_capacity = new_cap;
    }
    parent->children[parent->child_count++] = child;
    return 0;
}

/* ========================================================================== */
/* Tree flat branch list helpers                                               */
/* ========================================================================== */

static int vtx_trace_tree_add_to_flat_list(vtx_trace_tree_t *tree,
                                             vtx_trace_branch_t *branch,
                                             vtx_arena_t *arena)
{
    if (tree->branch_count >= tree->all_branch_capacity) {
        uint32_t new_cap = tree->all_branch_capacity * 2;
        vtx_trace_branch_t **new_list = vtx_arena_alloc(arena,
                sizeof(vtx_trace_branch_t *) * new_cap);
        if (new_list == NULL) return -1;
        memcpy(new_list, tree->all_branches,
               sizeof(vtx_trace_branch_t *) * tree->branch_count);
        tree->all_branches = new_list;
        tree->all_branch_capacity = new_cap;
    }
    tree->all_branches[tree->branch_count++] = branch;
    return 0;
}

/**
 * Find the branch that owns a given side exit.
 * Searches the flat branch list for the branch whose trace contains
 * the given side exit.
 */
static vtx_trace_branch_t *vtx_trace_tree_find_branch_for_exit(
    const vtx_trace_tree_t *tree,
    vtx_side_exit_id_t exit_id)
{
    /* Check root branch */
    if (tree->root_branch != NULL && tree->root_branch->trace != NULL) {
        for (uint32_t i = 0; i < tree->root_branch->trace->side_exit_count; i++) {
            vtx_side_exit_t *se = tree->root_branch->trace->side_exits[i];
            if (se != NULL && se->exit_id == exit_id) {
                return tree->root_branch;
            }
        }
    }

    /* Check all other branches */
    for (uint32_t i = 0; i < tree->branch_count; i++) {
        vtx_trace_branch_t *branch = tree->all_branches[i];
        if (branch == NULL || branch->trace == NULL) continue;
        for (uint32_t j = 0; j < branch->trace->side_exit_count; j++) {
            vtx_side_exit_t *se = branch->trace->side_exits[j];
            if (se != NULL && se->exit_id == exit_id) {
                return branch;
            }
        }
    }

    return NULL;
}

/**
 * Find the side exit descriptor across all traces in the tree.
 */
vtx_side_exit_t *vtx_trace_tree_find_exit(
    const vtx_trace_tree_t *tree,
    vtx_side_exit_id_t exit_id)
{
    /* Search root trace */
    if (tree->root != NULL) {
        for (uint32_t i = 0; i < tree->root->side_exit_count; i++) {
            vtx_side_exit_t *se = tree->root->side_exits[i];
            if (se != NULL && se->exit_id == exit_id) return se;
        }
    }

    /* Search all branches */
    for (uint32_t i = 0; i < tree->branch_count; i++) {
        vtx_trace_branch_t *branch = tree->all_branches[i];
        if (branch == NULL || branch->trace == NULL) continue;
        for (uint32_t j = 0; j < branch->trace->side_exit_count; j++) {
            vtx_side_exit_t *se = branch->trace->side_exits[j];
            if (se != NULL && se->exit_id == exit_id) return se;
        }
    }

    return NULL;
}

/* ========================================================================== */
/* Tree building: root                                                         */
/* ========================================================================== */

vtx_trace_tree_t *vtx_trace_tree_build_root(
    vtx_trace_recorder_t *recorder,
    vtx_graph_t *graph,
    const vtx_bytecode_t *bytecode,
    const vtx_method_desc_t *method,
    uint32_t entry_pc,
    const vtx_profiler_t *profiler,
    const vtx_profile_global_t *profile,
    vtx_arena_t *arena)
{
    VTX_ASSERT(recorder != NULL, "recorder must not be NULL");
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(bytecode != NULL, "bytecode must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    /* Allocate the tree */
    vtx_trace_tree_t *tree = vtx_arena_alloc(arena, sizeof(vtx_trace_tree_t));
    if (tree == NULL) return NULL;
    memset(tree, 0, sizeof(vtx_trace_tree_t));

    tree->method = method;
    tree->loop_header_pc = entry_pc;
    tree->depth = 0;
    tree->branch_count = 0;

    /* Allocate flat branch list */
    tree->all_branch_capacity = 8;
    tree->all_branches = vtx_arena_alloc(arena,
            sizeof(vtx_trace_branch_t *) * tree->all_branch_capacity);
    if (tree->all_branches == NULL) return NULL;
    memset(tree->all_branches, 0,
           sizeof(vtx_trace_branch_t *) * tree->all_branch_capacity);

    /* Record the root trace */
    vtx_trace_t *root = vtx_trace_recorder_record(
        recorder, graph, bytecode, method, entry_pc,
        profiler, profile, arena);
    if (root == NULL) return NULL;

    tree->root = root;

    /* Create the root branch wrapper */
    tree->root_branch = vtx_trace_branch_create(arena,
        VTX_SIDE_EXIT_ID_INVALID, root, 0, NULL);
    if (tree->root_branch == NULL) return NULL;

    /* Add root branch to the flat list */
    if (vtx_trace_tree_add_to_flat_list(tree, tree->root_branch, arena) != 0) {
        return NULL;
    }

    return tree;
}

/* ========================================================================== */
/* Tree building: add branch                                                   */
/* ========================================================================== */

vtx_trace_branch_t *vtx_trace_tree_add_branch(
    vtx_trace_recorder_t *recorder,
    vtx_graph_t *graph,
    const vtx_bytecode_t *bytecode,
    const vtx_method_desc_t *method,
    vtx_trace_tree_t *tree,
    vtx_side_exit_id_t side_exit_id,
    vtx_arena_t *arena)
{
    VTX_ASSERT(recorder != NULL, "recorder must not be NULL");
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(bytecode != NULL, "bytecode must not be NULL");
    VTX_ASSERT(tree != NULL, "tree must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    /* Check depth limit */
    if (tree->depth >= VTX_MAX_TREE_DEPTH) {
        return NULL;
    }

    /* Find the side exit descriptor */
    vtx_side_exit_t *exit = vtx_trace_tree_find_exit(tree, side_exit_id);
    if (exit == NULL) {
        return NULL;
    }

    /* Check if this exit already has a branch */
    if (exit->has_branch) {
        return NULL;
    }

    /* Find the parent branch (the branch whose trace contains this exit) */
    vtx_trace_branch_t *parent_branch = vtx_trace_tree_find_branch_for_exit(
        tree, side_exit_id);

    uint32_t new_depth = (parent_branch != NULL) ? parent_branch->depth + 1 : 1;

    /* Check depth limit again with the computed depth */
    if (new_depth > VTX_MAX_TREE_DEPTH) {
        return NULL;
    }

    /* Record a new trace from the side exit's target PC */
    vtx_trace_t *branch_trace = vtx_trace_recorder_record(
        recorder, graph, bytecode, method, exit->target_pc,
        NULL, NULL, arena);
    if (branch_trace == NULL) {
        return NULL;
    }

    /* Create the branch */
    vtx_trace_branch_t *branch = vtx_trace_branch_create(
        arena, side_exit_id, branch_trace, new_depth, parent_branch);
    if (branch == NULL) {
        return NULL;
    }

    /* Link as child of parent */
    if (parent_branch != NULL) {
        if (vtx_trace_branch_add_child(arena, parent_branch, branch) != 0) {
            return NULL;
        }
    }

    /* Mark the side exit as having a branch */
    exit->has_branch = true;

    /* Add to the tree's flat branch list */
    if (vtx_trace_tree_add_to_flat_list(tree, branch, arena) != 0) {
        return NULL;
    }

    /* Update tree depth */
    if (new_depth > tree->depth) {
        tree->depth = new_depth;
    }

    return branch;
}

/* ========================================================================== */
/* Tree queries                                                                */
/* ========================================================================== */

vtx_trace_t *vtx_trace_tree_root(const vtx_trace_tree_t *tree)
{
    return tree != NULL ? tree->root : NULL;
}

uint32_t vtx_trace_tree_trace_count(const vtx_trace_tree_t *tree)
{
    if (tree == NULL) return 0;
    return 1 + tree->branch_count; /* root + branches */
}

uint32_t vtx_trace_tree_depth(const vtx_trace_tree_t *tree)
{
    return tree != NULL ? tree->depth : 0;
}

bool vtx_trace_tree_can_branch(const vtx_trace_tree_t *tree)
{
    return tree != NULL && tree->depth < VTX_MAX_TREE_DEPTH;
}

uint32_t vtx_trace_tree_find_hot_exits(const vtx_trace_tree_t *tree,
                                         vtx_side_exit_id_t *out,
                                         uint32_t max_out)
{
    if (tree == NULL || out == NULL || max_out == 0) return 0;

    uint32_t found = 0;

    /* Check root trace side exits */
    if (tree->root != NULL) {
        for (uint32_t i = 0; i < tree->root->side_exit_count && found < max_out; i++) {
            vtx_side_exit_t *se = tree->root->side_exits[i];
            if (se != NULL && vtx_side_exit_should_record(se)) {
                out[found++] = se->exit_id;
            }
        }
    }

    /* Check all branch trace side exits */
    for (uint32_t i = 0; i < tree->branch_count && found < max_out; i++) {
        vtx_trace_branch_t *branch = tree->all_branches[i];
        if (branch == NULL || branch->trace == NULL) continue;
        for (uint32_t j = 0; j < branch->trace->side_exit_count && found < max_out; j++) {
            vtx_side_exit_t *se = branch->trace->side_exits[j];
            if (se != NULL && vtx_side_exit_should_record(se)) {
                out[found++] = se->exit_id;
            }
        }
    }

    return found;
}

vtx_trace_branch_t *vtx_trace_tree_get_branch(const vtx_trace_tree_t *tree,
                                                uint32_t index)
{
    if (tree == NULL || index >= tree->branch_count) return NULL;
    return tree->all_branches[index];
}
