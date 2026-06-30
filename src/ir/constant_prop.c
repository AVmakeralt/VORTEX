#include "ir/constant_prop.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

/* ========================================================================== */
/* Lattice meet (join)                                                         */
/* ========================================================================== */

vtx_lattice_val_t vtx_lattice_meet(vtx_lattice_val_t a, vtx_lattice_val_t b)
{
    /* Rules:
     *   Top ⊓ x     = x
     *   x   ⊓ Top   = x
     *   Bottom ⊓ x  = Bottom
     *   x   ⊓ Bottom = Bottom
     *   Constant(v1) ⊓ Constant(v2) = Constant(v1) if v1==v2, else Bottom
     */
    if (a.tag == VTX_LATTICE_TOP) return b;
    if (b.tag == VTX_LATTICE_TOP) return a;
    if (a.tag == VTX_LATTICE_BOTTOM || b.tag == VTX_LATTICE_BOTTOM) {
        return vtx_lattice_bottom();
    }
    /* Both constant */
    if (vtx_constval_equal(a.value, b.value)) {
        return a;
    }
    return vtx_lattice_bottom();
}

/* ========================================================================== */
/* Worklist                                                                    */
/* ========================================================================== */

typedef struct {
    vtx_nodeid_t *items;
    uint32_t      count;
    uint32_t      capacity;
    bool         *in_list;   /* quick membership check */
    uint32_t      node_count; /* size of in_list array */
} vtx_worklist_t;

static int worklist_init(vtx_worklist_t *wl, uint32_t node_count)
{
    wl->capacity = (node_count < 64) ? 64 : node_count;
    wl->items = (vtx_nodeid_t *)malloc(wl->capacity * sizeof(vtx_nodeid_t));
    if (wl->items == NULL) return -1;
    wl->in_list = (bool *)calloc(node_count, sizeof(bool));
    if (wl->in_list == NULL) {
        free(wl->items);
        return -1;
    }
    wl->count = 0;
    wl->node_count = node_count;
    return 0;
}

static void worklist_destroy(vtx_worklist_t *wl)
{
    free(wl->items);
    free(wl->in_list);
    wl->items = NULL;
    wl->in_list = NULL;
    wl->count = 0;
    wl->capacity = 0;
}

static void worklist_push(vtx_worklist_t *wl, vtx_nodeid_t id)
{
    if (id >= wl->node_count) return;
    if (wl->in_list[id]) return; /* already in worklist */
    if (wl->count >= wl->capacity) {
        uint32_t new_cap = wl->capacity * 2;
        vtx_nodeid_t *new_items = (vtx_nodeid_t *)realloc(wl->items, new_cap * sizeof(vtx_nodeid_t));
        if (new_items == NULL) return;
        wl->items = new_items;
        wl->capacity = new_cap;
    }
    wl->items[wl->count++] = id;
    wl->in_list[id] = true;
}

static vtx_nodeid_t worklist_pop(vtx_worklist_t *wl)
{
    if (wl->count == 0) return VTX_NODEID_INVALID;
    vtx_nodeid_t id = wl->items[--wl->count];
    wl->in_list[id] = false;
    return id;
}

static bool worklist_is_empty(const vtx_worklist_t *wl)
{
    return wl->count == 0;
}

/* ========================================================================== */
/* SCCP lattice state per node                                                 */
/* ========================================================================== */

/**
 * Evaluate a node given the current lattice state, producing a new lattice
 * value. This implements the transfer function for each opcode.
 */
static vtx_lattice_val_t evaluate_node(vtx_node_opcode_t opcode,
                                        vtx_lattice_val_t *inputs,
                                        uint32_t input_count,
                                        const vtx_node_t *node)
{
    switch (opcode) {
    /* ---- Constants ---- */
    case VTX_OP_Constant: {
        vtx_lattice_val_t v;
        v.tag = VTX_LATTICE_CONSTANT;
        v.value = node->constval;
        return v;
    }

    /* ---- Parameter: unknown, assume Bottom ---- */
    case VTX_OP_Parameter:
        return vtx_lattice_bottom();

    /* ---- Arithmetic ---- */
    case VTX_OP_Add: {
        if (input_count < 2) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT && inputs[1].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Int && inputs[1].value.kind == VTX_TYPE_Int) {
                /* IR-8 fix: use unsigned arithmetic to avoid signed overflow UB */
                int64_t a_val = inputs[0].value.as.int_val;
                int64_t b_val = inputs[1].value.as.int_val;
                uint64_t ua = (uint64_t)a_val;
                uint64_t ub = (uint64_t)b_val;
                return vtx_lattice_const_int((int64_t)(ua + ub));
            }
            if (inputs[0].value.kind == VTX_TYPE_Float && inputs[1].value.kind == VTX_TYPE_Float) {
                return vtx_lattice_const_float(inputs[0].value.as.float_val + inputs[1].value.as.float_val);
            }
        }
        /* If any input is Top, result is Top */
        if (inputs[0].tag == VTX_LATTICE_TOP || inputs[1].tag == VTX_LATTICE_TOP) {
            return vtx_lattice_top();
        }
        return vtx_lattice_bottom();
    }

    case VTX_OP_Sub: {
        if (input_count < 2) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT && inputs[1].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Int && inputs[1].value.kind == VTX_TYPE_Int) {
                /* IR-8 fix: use unsigned arithmetic to avoid signed overflow UB */
                int64_t a_val = inputs[0].value.as.int_val;
                int64_t b_val = inputs[1].value.as.int_val;
                uint64_t ua = (uint64_t)a_val;
                uint64_t ub = (uint64_t)b_val;
                return vtx_lattice_const_int((int64_t)(ua - ub));
            }
            if (inputs[0].value.kind == VTX_TYPE_Float && inputs[1].value.kind == VTX_TYPE_Float) {
                return vtx_lattice_const_float(inputs[0].value.as.float_val - inputs[1].value.as.float_val);
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP || inputs[1].tag == VTX_LATTICE_TOP) {
            return vtx_lattice_top();
        }
        return vtx_lattice_bottom();
    }

    case VTX_OP_Mul: {
        if (input_count < 2) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT && inputs[1].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Int && inputs[1].value.kind == VTX_TYPE_Int) {
                /* IR-8 fix: use unsigned arithmetic to avoid signed overflow UB */
                int64_t a_val = inputs[0].value.as.int_val;
                int64_t b_val = inputs[1].value.as.int_val;
                uint64_t ua = (uint64_t)a_val;
                uint64_t ub = (uint64_t)b_val;
                return vtx_lattice_const_int((int64_t)(ua * ub));
            }
            if (inputs[0].value.kind == VTX_TYPE_Float && inputs[1].value.kind == VTX_TYPE_Float) {
                return vtx_lattice_const_float(inputs[0].value.as.float_val * inputs[1].value.as.float_val);
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP || inputs[1].tag == VTX_LATTICE_TOP) {
            return vtx_lattice_top();
        }
        return vtx_lattice_bottom();
    }

    case VTX_OP_Div: {
        if (input_count < 2) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT && inputs[1].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Int && inputs[1].value.kind == VTX_TYPE_Int) {
                if (inputs[1].value.as.int_val != 0) {
                    /* BUGFIX: INT64_MIN / -1 is undefined behavior in C
                     * (signed integer overflow). On x86-64 this triggers
                     * a hardware SIGFPE. Don't fold this case. */
                    int64_t a = inputs[0].value.as.int_val;
                    int64_t b = inputs[1].value.as.int_val;
                    if (a == INT64_MIN && b == -1) {
                        return vtx_lattice_bottom(); /* would overflow */
                    }
                    return vtx_lattice_const_int(a / b);
                }
                /* Division by zero: don't fold, leave as bottom */
            }
            if (inputs[0].value.kind == VTX_TYPE_Float && inputs[1].value.kind == VTX_TYPE_Float) {
                if (inputs[1].value.as.float_val != 0.0) {
                    return vtx_lattice_const_float(inputs[0].value.as.float_val / inputs[1].value.as.float_val);
                }
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP || inputs[1].tag == VTX_LATTICE_TOP) {
            return vtx_lattice_top();
        }
        return vtx_lattice_bottom();
    }

    case VTX_OP_Mod: {
        if (input_count < 2) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT && inputs[1].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Int && inputs[1].value.kind == VTX_TYPE_Int) {
                if (inputs[1].value.as.int_val != 0) {
                    /* BUGFIX: INT64_MIN % -1 is also undefined behavior
                     * in C (same overflow issue as division). */
                    int64_t a = inputs[0].value.as.int_val;
                    int64_t b = inputs[1].value.as.int_val;
                    if (a == INT64_MIN && b == -1) {
                        return vtx_lattice_bottom(); /* would overflow */
                    }
                    return vtx_lattice_const_int(a % b);
                }
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP || inputs[1].tag == VTX_LATTICE_TOP) {
            return vtx_lattice_top();
        }
        return vtx_lattice_bottom();
    }

    case VTX_OP_Min: {
        if (input_count < 2) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT && inputs[1].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Int && inputs[1].value.kind == VTX_TYPE_Int) {
                int64_t a = inputs[0].value.as.int_val;
                int64_t b = inputs[1].value.as.int_val;
                return vtx_lattice_const_int(a < b ? a : b);
            }
            if (inputs[0].value.kind == VTX_TYPE_Float && inputs[1].value.kind == VTX_TYPE_Float) {
                return vtx_lattice_const_float(fmin(inputs[0].value.as.float_val, inputs[1].value.as.float_val));
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP || inputs[1].tag == VTX_LATTICE_TOP) {
            return vtx_lattice_top();
        }
        return vtx_lattice_bottom();
    }

    case VTX_OP_Max: {
        if (input_count < 2) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT && inputs[1].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Int && inputs[1].value.kind == VTX_TYPE_Int) {
                int64_t a = inputs[0].value.as.int_val;
                int64_t b = inputs[1].value.as.int_val;
                return vtx_lattice_const_int(a > b ? a : b);
            }
            if (inputs[0].value.kind == VTX_TYPE_Float && inputs[1].value.kind == VTX_TYPE_Float) {
                return vtx_lattice_const_float(fmax(inputs[0].value.as.float_val, inputs[1].value.as.float_val));
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP || inputs[1].tag == VTX_LATTICE_TOP) {
            return vtx_lattice_top();
        }
        return vtx_lattice_bottom();
    }

    case VTX_OP_Shl: {
        if (input_count < 2) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT && inputs[1].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Int && inputs[1].value.kind == VTX_TYPE_Int) {
                return vtx_lattice_const_int(inputs[0].value.as.int_val << (inputs[1].value.as.int_val & 63));
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP || inputs[1].tag == VTX_LATTICE_TOP) {
            return vtx_lattice_top();
        }
        return vtx_lattice_bottom();
    }

    case VTX_OP_Shr: {
        if (input_count < 2) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT && inputs[1].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Int && inputs[1].value.kind == VTX_TYPE_Int) {
                return vtx_lattice_const_int(inputs[0].value.as.int_val >> (inputs[1].value.as.int_val & 63));
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP || inputs[1].tag == VTX_LATTICE_TOP) {
            return vtx_lattice_top();
        }
        return vtx_lattice_bottom();
    }

    case VTX_OP_And: {
        if (input_count < 2) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT && inputs[1].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Int && inputs[1].value.kind == VTX_TYPE_Int) {
                return vtx_lattice_const_int(inputs[0].value.as.int_val & inputs[1].value.as.int_val);
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP || inputs[1].tag == VTX_LATTICE_TOP) {
            return vtx_lattice_top();
        }
        return vtx_lattice_bottom();
    }

    case VTX_OP_Or: {
        if (input_count < 2) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT && inputs[1].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Int && inputs[1].value.kind == VTX_TYPE_Int) {
                return vtx_lattice_const_int(inputs[0].value.as.int_val | inputs[1].value.as.int_val);
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP || inputs[1].tag == VTX_LATTICE_TOP) {
            return vtx_lattice_top();
        }
        return vtx_lattice_bottom();
    }

    case VTX_OP_Xor: {
        if (input_count < 2) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT && inputs[1].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Int && inputs[1].value.kind == VTX_TYPE_Int) {
                return vtx_lattice_const_int(inputs[0].value.as.int_val ^ inputs[1].value.as.int_val);
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP || inputs[1].tag == VTX_LATTICE_TOP) {
            return vtx_lattice_top();
        }
        return vtx_lattice_bottom();
    }

    case VTX_OP_Neg: {
        if (input_count < 1) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Int) {
                /* IR-8 fix: use unsigned arithmetic to avoid signed overflow UB */
                uint64_t ua = (uint64_t)inputs[0].value.as.int_val;
                return vtx_lattice_const_int((int64_t)(~ua + 1));
            }
            if (inputs[0].value.kind == VTX_TYPE_Float) {
                return vtx_lattice_const_float(-inputs[0].value.as.float_val);
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP) return vtx_lattice_top();
        return vtx_lattice_bottom();
    }

    case VTX_OP_Not: {
        if (input_count < 1) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Int) {
                return vtx_lattice_const_int(~inputs[0].value.as.int_val);
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP) return vtx_lattice_top();
        return vtx_lattice_bottom();
    }

    /* ---- Comparisons ---- */
    case VTX_OP_Cmp: {
        if (input_count < 2) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT && inputs[1].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Int && inputs[1].value.kind == VTX_TYPE_Int) {
                int64_t a = inputs[0].value.as.int_val;
                int64_t b = inputs[1].value.as.int_val;
                bool result = false;
                switch (node->cond) {
                case VTX_COND_EQ:  result = (a == b); break;
                case VTX_COND_NE:  result = (a != b); break;
                case VTX_COND_LT:  result = (a < b);  break;
                case VTX_COND_LE:  result = (a <= b); break;
                case VTX_COND_GT:  result = (a > b);  break;
                case VTX_COND_GE:  result = (a >= b); break;
                default: break;
                }
                return vtx_lattice_const_int(result ? 1 : 0);
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP || inputs[1].tag == VTX_LATTICE_TOP) {
            return vtx_lattice_top();
        }
        return vtx_lattice_bottom();
    }

    case VTX_OP_CmpP: {
        if (input_count < 2) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT && inputs[1].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Ptr && inputs[1].value.kind == VTX_TYPE_Ptr) {
                bool result = (inputs[0].value.as.ptr_val == inputs[1].value.as.ptr_val);
                if (node->cond == VTX_COND_NE) result = !result;
                return vtx_lattice_const_int(result ? 1 : 0);
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP || inputs[1].tag == VTX_LATTICE_TOP) {
            return vtx_lattice_top();
        }
        return vtx_lattice_bottom();
    }

    case VTX_OP_CmpF:
    case VTX_OP_CmpD: {
        if (input_count < 2) return vtx_lattice_bottom();
        if (inputs[0].tag == VTX_LATTICE_CONSTANT && inputs[1].tag == VTX_LATTICE_CONSTANT) {
            if (inputs[0].value.kind == VTX_TYPE_Float && inputs[1].value.kind == VTX_TYPE_Float) {
                double a = inputs[0].value.as.float_val;
                double b = inputs[1].value.as.float_val;
                bool result = false;
                switch (node->cond) {
                case VTX_COND_EQ:  result = (a == b); break;
                case VTX_COND_NE:  result = (a != b); break;
                case VTX_COND_LT:  result = (a < b);  break;
                case VTX_COND_LE:  result = (a <= b); break;
                case VTX_COND_GT:  result = (a > b);  break;
                case VTX_COND_GE:  result = (a >= b); break;
                default: break;
                }
                return vtx_lattice_const_int(result ? 1 : 0);
            }
        }
        if (inputs[0].tag == VTX_LATTICE_TOP || inputs[1].tag == VTX_LATTICE_TOP) {
            return vtx_lattice_top();
        }
        return vtx_lattice_bottom();
    }

    /* ---- Phi: meet of all inputs ---- */
    case VTX_OP_Phi: {
        vtx_lattice_val_t result = vtx_lattice_top();
        /* IR-1 fix: The last input of a Phi is the Region (control) node,
         * which has no data-flow value and evaluates to Bottom in SCCP.
         * Since Bottom meet X = Bottom, including the Region input makes
         * every Phi resolve to Bottom, defeating constant propagation.
         * We must skip the last input (the Region). */
        for (uint32_t i = 0; i + 1 < input_count; i++) {
            result = vtx_lattice_meet(result, inputs[i]);
        }
        return result;
    }

    /* ---- Proj: pass through from input ---- */
    case VTX_OP_Proj: {
        if (input_count >= 1) return inputs[0];
        return vtx_lattice_bottom();
    }

    /* ---- CheckCast: pass through value, type check is a side effect ---- */
    case VTX_OP_CheckCast: {
        if (input_count >= 1) return inputs[0];
        return vtx_lattice_bottom();
    }

    /* ---- InstanceOf: can fold if object type is known ---- */
    case VTX_OP_InstanceOf:
        return vtx_lattice_bottom();

    /* ---- Memory loads: unknown unless we can prove otherwise ---- */
    case VTX_OP_Load:
    case VTX_OP_LoadField:
    case VTX_OP_LoadIndexed:
        return vtx_lattice_bottom();

    /* ---- Control nodes: Void ---- */
    case VTX_OP_Start:
    case VTX_OP_Region:
    case VTX_OP_LoopBegin:
    case VTX_OP_LoopEnd:
    case VTX_OP_Province:
    case VTX_OP_Goto:
    case VTX_OP_Return:
    case VTX_OP_End:
    case VTX_OP_Catch:
    case VTX_OP_Deopt:
        return vtx_lattice_bottom(); /* control nodes don't produce data values */

    /* ---- If: evaluates its condition input ---- */
    case VTX_OP_If: {
        /* The If itself doesn't produce a data value.
         * But we can determine reachability: if the condition is constant,
         * we know which branch is taken. This is handled in the main loop
         * by checking the If's condition lattice value. */
        return vtx_lattice_bottom();
    }

    /* ---- Memory stores, allocations, calls: overdefined ---- */
    case VTX_OP_Store:
    case VTX_OP_StoreField:
    case VTX_OP_StoreIndexed:
    case VTX_OP_MemBar:
    case VTX_OP_Initialize:
    case VTX_OP_NewObject:
    case VTX_OP_NewArray:
    case VTX_OP_Allocate:
    case VTX_OP_InitializeKlass:
    case VTX_OP_CallStatic:
    case VTX_OP_CallVirtual:
    case VTX_OP_CallInterface:
    case VTX_OP_CallRuntime:
    case VTX_OP_Guard:
    case VTX_OP_DeoptGuard:
    case VTX_OP_FrameState:
    case VTX_OP_Unwind:
    case VTX_OP_Switch:
        return vtx_lattice_bottom();

    default:
        return vtx_lattice_bottom();
    }
}

/* ========================================================================== */
/* Main SCCP pass                                                              */
/* ========================================================================== */

uint32_t vtx_constant_prop_run(vtx_graph_t *graph)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");

    vtx_node_table_t *nt = &graph->node_table;
    uint32_t node_count = nt->count;
    uint32_t simplified = 0;

    if (node_count == 0) return 0;

    /* Allocate lattice state: one lattice value per node */
    vtx_lattice_val_t *lattice = (vtx_lattice_val_t *)malloc(node_count * sizeof(vtx_lattice_val_t));
    if (lattice == NULL) return 0;

    /* Initialize all to Top */
    for (uint32_t i = 0; i < node_count; i++) {
        lattice[i] = vtx_lattice_top();
    }

    /* Initialize worklist */
    vtx_worklist_t wl;
    if (worklist_init(&wl, node_count) != 0) {
        free(lattice);
        return 0;
    }

    /* Seed: Start node is reachable (Bottom) */
    if (graph->start_node < node_count) {
        lattice[graph->start_node] = vtx_lattice_bottom();
        worklist_push(&wl, graph->start_node);
        /* Push Start's users since its lattice value changed from TOP to BOTTOM.
         * Without this, downstream nodes never get evaluated because when Start
         * is popped from the worklist, its value doesn't change (already BOTTOM),
         * so the change-detection logic doesn't push its users. */
        vtx_node_t *start = &nt->nodes[graph->start_node];
        for (uint32_t u = 0; u < start->use_count; u++) {
            vtx_use_entry_t *use = &start->uses[u];
            if (use->user_id < node_count && !nt->nodes[use->user_id].dead) {
                worklist_push(&wl, use->user_id);
            }
        }
    }

    /* Also seed all Constant nodes — they are always known.
     * Also seed Parameter nodes as BOTTOM — they are always reachable
     * (method inputs) but their values are unknown (not constant).
     * Push their users for the same reason as Start above. */
    for (uint32_t i = 0; i < node_count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;
        if (node->opcode == VTX_OP_Constant) {
            vtx_lattice_val_t v;
            v.tag = VTX_LATTICE_CONSTANT;
            v.value = node->constval;
            lattice[i] = v;
            worklist_push(&wl, i);
            /* Push Constant's users */
            for (uint32_t u = 0; u < node->use_count; u++) {
                vtx_use_entry_t *use = &node->uses[u];
                if (use->user_id < node_count && !nt->nodes[use->user_id].dead) {
                    worklist_push(&wl, use->user_id);
                }
            }
        }
        if (node->opcode == VTX_OP_Parameter) {
            lattice[i] = vtx_lattice_bottom();
            worklist_push(&wl, i);
            /* Push Parameter's users */
            for (uint32_t u = 0; u < node->use_count; u++) {
                vtx_use_entry_t *use = &node->uses[u];
                if (use->user_id < node_count && !nt->nodes[use->user_id].dead) {
                    worklist_push(&wl, use->user_id);
                }
            }
        }
    }

    /* Propagate until fixed point */
    while (!worklist_is_empty(&wl)) {
        vtx_nodeid_t nid = worklist_pop(&wl);
        if (nid == VTX_NODEID_INVALID || nid >= node_count) continue;
        vtx_node_t *node = &nt->nodes[nid];
        if (node->dead) continue;

        /* Compute the input lattice values */
        uint32_t ic = node->input_count;
        vtx_lattice_val_t *inp_vals = NULL;
        if (ic > 0) {
            inp_vals = (vtx_lattice_val_t *)malloc(ic * sizeof(vtx_lattice_val_t));
            if (inp_vals == NULL) continue;
            for (uint32_t j = 0; j < ic; j++) {
                vtx_nodeid_t inp = node->inputs[j];
                if (inp != VTX_NODEID_INVALID && inp < node_count) {
                    inp_vals[j] = lattice[inp];
                } else {
                    inp_vals[j] = vtx_lattice_top();
                }
            }
        }

        /* Evaluate the node */
        vtx_lattice_val_t new_val = evaluate_node(node->opcode, inp_vals, ic, node);
        free(inp_vals);

        /* If the lattice value changed, update and propagate */
        vtx_lattice_val_t old_val = lattice[nid];
        if (new_val.tag != old_val.tag ||
            (new_val.tag == VTX_LATTICE_CONSTANT && !vtx_constval_equal(new_val.value, old_val.value))) {
            lattice[nid] = new_val;

            /* Bug #14 fix: Add users to the worklist using the
             * O(K) use-def list instead of the O(N^2) scan of
             * all nodes' inputs. This dramatically improves compile
             * time for large graphs. */
            vtx_node_t *changed_node = &nt->nodes[nid];
            for (uint32_t u = 0; u < changed_node->use_count; u++) {
                vtx_use_entry_t *use = &changed_node->uses[u];
                if (use->user_id < node_count && !nt->nodes[use->user_id].dead) {
                    worklist_push(&wl, use->user_id);
                }
            }
        }

        /* Special handling for If nodes with constant condition:
         * If the condition is a constant, we can determine which branch
         * is taken and mark the other as unreachable.
         * The If's data input (condition) determines reachability. */
        if (node->opcode == VTX_OP_If && ic >= 2) {
            vtx_nodeid_t cond_id = node->inputs[1]; /* second input is condition */
            if (cond_id != VTX_NODEID_INVALID && cond_id < node_count) {
                vtx_lattice_val_t cond_val = lattice[cond_id];
                if (cond_val.tag == VTX_LATTICE_CONSTANT && cond_val.value.kind == VTX_TYPE_Int) {
                    /* We know which branch is taken.
                     * The If's Proj nodes will have their lattice values set.
                     * We don't eliminate the other Proj here — DCE will handle it
                     * after we mark unreachable code. But we do mark the unreachable
                     * Proj as Top so downstream nodes stay Top. */
                    bool taken = (cond_val.value.as.int_val != 0);
                    if (node->cond == VTX_COND_EQ) taken = !taken; /* IF_FALSE */

                    /* Find Proj nodes that use this If */
                    for (uint32_t u = 0; u < node_count; u++) {
                        vtx_node_t *user = &nt->nodes[u];
                        if (user->dead) continue;
                        if (user->opcode == VTX_OP_Proj && user->input_count >= 1 &&
                            user->inputs[0] == nid) {
                            /* Proj 0 = true branch, Proj 1 = false branch */
                            bool this_branch_taken = (user->local_index == 0) ? taken : !taken;
                            if (this_branch_taken) {
                                /* This projection is reachable */
                                if (lattice[u].tag == VTX_LATTICE_TOP) {
                                    lattice[u] = vtx_lattice_bottom();
                                    worklist_push(&wl, u);
                                }
                            }
                            /* Unreachable projections stay Top — their users will
                             * remain Top and be eliminated by DCE */
                        }
                    }
                }
            }
        }
    }

    /* Phase 2: Replace nodes with constant values where possible.
     * For nodes that reached a constant lattice value, we replace them
     * with a Constant node. */
    for (uint32_t i = 0; i < node_count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;

        /* Skip nodes that are already constants or non-data */
        if (node->opcode == VTX_OP_Constant) continue;
        if (!vtx_nf_has(node->flags, VTX_NF_DATA)) continue;
        if (vtx_nf_has(node->flags, VTX_NF_SIDE_EFFECT)) continue;
        if (vtx_nf_has(node->flags, VTX_NF_PINNED)) continue;

        if (lattice[i].tag == VTX_LATTICE_CONSTANT) {
            /* Create a new Constant node with the discovered value */
            vtx_nodeid_t const_node = vtx_node_create(nt, VTX_OP_Constant);
            if (const_node == VTX_NODEID_INVALID) continue;
            vtx_node_t *cn = vtx_node_get(nt, const_node);
            cn->constval = lattice[i].value;

            /* Set the type based on the constant kind */
            switch (lattice[i].value.kind) {
            case VTX_TYPE_Int:   cn->type = VTX_TYPE_Int; break;
            case VTX_TYPE_Float: cn->type = VTX_TYPE_Float; break;
            case VTX_TYPE_Ptr:   cn->type = VTX_TYPE_Ptr; break;
            default:             cn->type = VTX_TYPE_Void; break;
            }

            /* Redirect all uses of this node to the new constant.
             * Bug #6 fix (complete): Use O(K) replace_all_uses instead
             * of the O(N^2) scan. vtx_node_replace_all_uses properly
             * maintains use-def lists, output counts, and inputs for
             * all users. After this call, this node's output_count
             * is 0 and all its former users point to the constant. */
            vtx_node_replace_all_uses(nt, (vtx_nodeid_t)i, const_node);

            /* Disconnect this node's inputs from their producers and
             * remove use-def entries, since this node is now dead. */
            for (uint32_t j = 0; j < node->input_count; j++) {
                if (node->inputs[j] != VTX_NODEID_INVALID && node->inputs[j] < nt->count) {
                    vtx_node_t *producer = &nt->nodes[node->inputs[j]];
                    vtx_node_remove_use_entry(producer, (vtx_nodeid_t)i, j);
                    if (producer->output_count > 0) {
                        producer->output_count--;
                    }
                }
            }
            node->input_count = 0;
            node->use_count = 0; /* Clear dead node's use list */
            node->dead = true;
            node->output_count = 0;
            simplified++;
        } else if (lattice[i].tag == VTX_LATTICE_TOP) {
            /* Node is unreachable — mark as dead */
            /* But be careful: control nodes and side-effecting nodes
             * might still be needed structurally. Only mark pure data
             * nodes that are unreachable. */
            if (vtx_nf_has(node->flags, VTX_NF_DATA) &&
                !vtx_nf_has(node->flags, VTX_NF_SIDE_EFFECT) &&
                !vtx_nf_has(node->flags, VTX_NF_CONTROL)) {
                /* Bug #4 fix: disconnect inputs on dead nodes AND
                 * remove use-def entries from producers. Without
                 * vtx_node_remove_use_entry, the producers' use-def
                 * lists retain stale entries pointing to the dead node,
                 * which corrupts later passes that traverse use-def
                 * lists (GVN, LICM, bounds check). */
                for (uint32_t j = 0; j < node->input_count; j++) {
                    if (node->inputs[j] != VTX_NODEID_INVALID && node->inputs[j] < nt->count) {
                        vtx_node_t *producer = &nt->nodes[node->inputs[j]];
                        vtx_node_remove_use_entry(producer, (vtx_nodeid_t)i, j);
                        if (producer->output_count > 0) {
                            producer->output_count--;
                        }
                    }
                }
                node->input_count = 0;
                node->use_count = 0; /* Clear dead node's use list */
                node->dead = true;
                simplified++;
            }
        }
    }

    /* Phase 3: Simplify Phis with only one non-Top input.
     *
     * BUGFIX (if-then-else wrong result): The old code skipped inputs with
     * lattice value TOP, treating them as "unreachable predecessor". But
     * TOP means "not yet visited", NOT "unreachable". If a Phi input is
     * still TOP when Phase 3 runs, the Phi should NOT be simplified — it
     * means SCCP hasn't converged for that input yet.
     *
     * Fix: Only simplify a Phi if ALL data inputs are non-top AND they all
     * agree. If any input is still TOP, skip the Phi (don't simplify). */
    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (node->dead || node->opcode != VTX_OP_Phi) continue;

        vtx_nodeid_t unique_val = VTX_NODEID_INVALID;
        bool all_same = true;
        bool has_non_top = false;
        bool has_top = false;

        /* Check data inputs (skip control inputs) */
        for (uint32_t j = 0; j < node->input_count; j++) {
            vtx_nodeid_t inp = node->inputs[j];
            if (inp == VTX_NODEID_INVALID || inp >= node_count) continue;

            /* Skip Region/control inputs — they're control, not data */
            const vtx_node_t *inp_node = vtx_node_get_const(nt, inp);
            if (inp_node != NULL && vtx_nf_has(inp_node->flags, VTX_NF_CONTROL)) continue;

            if (lattice[inp].tag == VTX_LATTICE_TOP) {
                /* Input not yet visited — don't simplify this Phi */
                has_top = true;
                continue;
            }

            has_non_top = true;
            if (unique_val == VTX_NODEID_INVALID) {
                unique_val = inp;
            } else if (inp != unique_val) {
                all_same = false;
                break;
            }
        }

        /* Only simplify if: no TOP inputs, at least one non-top input,
         * and all non-top inputs agree. */
        /* BUGFIX: Don't simplify Phis that have back-edge inputs.
         * A loop Phi has inputs [forward, Region, back_edge]. The back_edge
         * input is from a node inside the loop body, whose value changes
         * each iteration. Even if all non-TOP inputs currently agree,
         * the back-edge input may change in future iterations. Simplifying
         * the Phi to a single value would lose the loop-carried dependency.
         *
         * Check if this Phi has a back-edge input by looking for inputs
         * that are Phis (merge point Phis) or nodes from loop bodies. */
        bool has_back_edge = false;
        for (uint32_t j = 0; j < node->input_count; j++) {
            vtx_nodeid_t inp = node->inputs[j];
            if (inp == VTX_NODEID_INVALID || inp >= node_count) continue;
            const vtx_node_t *inp_node = vtx_node_get_const(nt, inp);
            if (inp_node != NULL && vtx_nf_has(inp_node->flags, VTX_NF_CONTROL)) continue;
            /* Check if this input is itself a Phi (merge point) — indicates
             * a loop-carried value that changes each iteration */
            if (inp_node != NULL && inp_node->opcode == VTX_OP_Phi) {
                has_back_edge = true;
                break;
            }
        }

        if (!has_top && has_non_top && all_same && unique_val != VTX_NODEID_INVALID && !has_back_edge) {
            /* Replace Phi with its single value */
            /* Bug #5 fix: Use O(K) replace_all_uses instead of O(N^2)
             * scan. Also properly disconnect the Phi's inputs from
             * their producers, removing use-def entries and decrementing
             * output_count, to prevent stale use-def list entries that
             * corrupt later passes. */
            vtx_node_replace_all_uses(nt, (vtx_nodeid_t)i, unique_val);

            /* Disconnect the Phi's inputs from their producers */
            for (uint32_t j = 0; j < node->input_count; j++) {
                if (node->inputs[j] != VTX_NODEID_INVALID && node->inputs[j] < nt->count) {
                    vtx_node_t *producer = &nt->nodes[node->inputs[j]];
                    vtx_node_remove_use_entry(producer, (vtx_nodeid_t)i, j);
                    if (producer->output_count > 0) {
                        producer->output_count--;
                    }
                }
            }
            node->input_count = 0;
            node->use_count = 0; /* Clear dead node's use list */
            node->dead = true;
            node->output_count = 0;
            simplified++;
        }
    }

    /* NOTE: SCCP does NOT eliminate guards.
     *
     * DESIGN PRINCIPLE: No pass may reduce the number of runtime decision
     * points without explicit authorization. A guard is a runtime decision
     * point — it checks a speculative assumption and deopts if it fails.
     * Even if SCCP determines the guard's condition is constant true,
     * removing the guard eliminates a safety check. If the SCCP analysis
     * is wrong (e.g., due to incomplete type info, or a deopt that
     * invalidates the assumption), the missing guard would produce
     * silent wrong-code instead of a safe deopt.
     *
     * Guard elimination must be done by an explicit, authorized pass
     * (e.g., bounds_check_elimination with a formal proof, or
     * guard_strength_adaptation which can weaken but not remove guards).
     * SCCP's job is to propagate constants and simplify pure data flow,
     * not to make safety decisions. */

    free(lattice);
    worklist_destroy(&wl);
    return simplified;
}
