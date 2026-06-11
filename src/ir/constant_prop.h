#ifndef VORTEX_CONSTANT_PROP_H
#define VORTEX_CONSTANT_PROP_H

#include <stdint.h>
#include <stdbool.h>
#include "ir/graph.h"

/**
 * VORTEX Sparse Conditional Constant Propagation (SCCP)
 *
 * Implements the SCCP algorithm on the Sea-of-Nodes IR.
 *
 * Lattice per node:
 *   Top       — unreachable / not yet visited
 *   Constant  — known constant value
 *   Bottom    — overdefined (variable, multiple possible values)
 *
 * Propagation rules per opcode:
 *   - Arithmetic on constants → constant result (fold)
 *   - Phi with one non-Top input → that input
 *   - Phi with all same constant → that constant
 *   - CheckCast with known type → no-op (replace with input)
 *   - Guard with always-true condition → remove
 *   - Comparison of constants → constant result
 *   - Neg/Not on constant → constant result
 *
 * Conditional propagation:
 *   - At If nodes with a constant condition, only propagate into the
 *     reachable branch. Unreachable branches keep their lattice values at Top.
 *
 * Fixed-point iteration:
 *   - Worklist of nodes whose lattice value has changed.
 *   - Process until worklist is empty.
 *   - Nodes remaining at Top after convergence are unreachable (dead).
 *
 * The graph is modified in place: constant-folded nodes are replaced with
 * Constant nodes, and unreachable nodes are marked dead.
 */

/* ========================================================================== */
/* Lattice value                                                               */
/* ========================================================================== */

typedef enum {
    VTX_LATTICE_TOP = 0,      /* unreachable / not yet visited */
    VTX_LATTICE_CONSTANT = 1, /* known constant value */
    VTX_LATTICE_BOTTOM = 2    /* overdefined / variable */
} vtx_lattice_tag_t;

typedef struct {
    vtx_lattice_tag_t tag;
    vtx_constval_t    value;   /* valid only when tag == CONSTANT */
} vtx_lattice_val_t;

/* Lattice constructors */
static inline vtx_lattice_val_t vtx_lattice_top(void)
{
    vtx_lattice_val_t v;
    v.tag = VTX_LATTICE_TOP;
    v.value.kind = VTX_TYPE_Top;
    v.value.as.int_val = 0;
    return v;
}

static inline vtx_lattice_val_t vtx_lattice_bottom(void)
{
    vtx_lattice_val_t v;
    v.tag = VTX_LATTICE_BOTTOM;
    v.value.kind = VTX_TYPE_Bottom;
    v.value.as.int_val = 0;
    return v;
}

static inline vtx_lattice_val_t vtx_lattice_const_int(int64_t val)
{
    vtx_lattice_val_t v;
    v.tag = VTX_LATTICE_CONSTANT;
    v.value = vtx_constval_int(val);
    return v;
}

static inline vtx_lattice_val_t vtx_lattice_const_float(double val)
{
    vtx_lattice_val_t v;
    v.tag = VTX_LATTICE_CONSTANT;
    v.value = vtx_constval_float(val);
    return v;
}

static inline vtx_lattice_val_t vtx_lattice_const_ptr(void *val)
{
    vtx_lattice_val_t v;
    v.tag = VTX_LATTICE_CONSTANT;
    v.value = vtx_constval_ptr(val);
    return v;
}

/* Meet (join) two lattice values: returns the least upper bound. */
vtx_lattice_val_t vtx_lattice_meet(vtx_lattice_val_t a, vtx_lattice_val_t b);

/* ========================================================================== */
/* SCCP run                                                                    */
/* ========================================================================== */

/**
 * Run SCCP on the graph. Modifies the graph in place:
 *   - Replaces foldable expressions with Constant nodes
 *   - Marks unreachable nodes as dead
 *   - Simplifies Phis with one input
 *
 * Returns the number of nodes simplified or marked unreachable.
 */
uint32_t vtx_constant_prop_run(vtx_graph_t *graph);

#endif /* VORTEX_CONSTANT_PROP_H */
