#include "ir/gvn.h"

#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* GVN hash computation                                                        */
/* ========================================================================== */

uint32_t vtx_gvn_node_hash(const vtx_node_t *node, const vtx_node_table_t *table)
{
    VTX_ASSERT(node != NULL, "node must not be NULL");
    VTX_ASSERT(table != NULL, "table must not be NULL");

    /* FNV-1a hash */
    uint32_t h = 2166136261u;

    /* Mix opcode */
    h ^= (uint32_t)node->opcode;
    h *= 16777619u;

    /* Mix type */
    h ^= (uint32_t)node->type;
    h *= 16777619u;

    /* Mix inputs (use value numbers if available, else raw IDs) */
    /* For commutative binary operations (Add, Mul, And, Or, Xor),
     * sort the two input values so that the hash is order-independent.
     * This ensures Add(x,y) and Add(y,x) produce the same hash. */
    if (node->input_count == 2 && (
        node->opcode == VTX_OP_Add || node->opcode == VTX_OP_Mul ||
        node->opcode == VTX_OP_And || node->opcode == VTX_OP_Or ||
        node->opcode == VTX_OP_Xor)) {
        uint32_t val0 = node->inputs[0];
        uint32_t val1 = node->inputs[1];
        if (val0 != VTX_NODEID_INVALID && val0 < table->count) {
            uint32_t vn = table->nodes[val0].value_number;
            if (vn != 0) val0 = vn;
        }
        if (val1 != VTX_NODEID_INVALID && val1 < table->count) {
            uint32_t vn = table->nodes[val1].value_number;
            if (vn != 0) val1 = vn;
        }
        /* Sort to ensure canonical order */
        if (val0 > val1) { uint32_t tmp = val0; val0 = val1; val1 = tmp; }
        h ^= val0;
        h *= 16777619u;
        h ^= val1;
        h *= 16777619u;
    } else {
        for (uint32_t i = 0; i < node->input_count; i++) {
            vtx_nodeid_t inp = node->inputs[i];
            /* Use the value number of the input if it has been computed,
             * otherwise use the raw node ID. This makes the hash sensitive
             * to the actual values being computed. */
            uint32_t val = inp;
            if (inp != VTX_NODEID_INVALID && inp < table->count) {
                uint32_t vn = table->nodes[inp].value_number;
                if (vn != 0) {
                    val = vn;
                }
            }
            h ^= val;
            h *= 16777619u;
        }
    }

    /* Mix condition code (for If, Cmp, Guard, etc.) */
    if (node->opcode == VTX_OP_If || node->opcode == VTX_OP_Cmp ||
        node->opcode == VTX_OP_CmpP || node->opcode == VTX_OP_CmpF ||
        node->opcode == VTX_OP_CmpD || node->opcode == VTX_OP_Guard ||
        node->opcode == VTX_OP_DeoptGuard) {
        h ^= (uint32_t)node->cond;
        h *= 16777619u;
    }

    /* Mix constant value */
    if (node->opcode == VTX_OP_Constant) {
        h ^= vtx_constval_hash(node->constval);
        h *= 16777619u;
    }

    /* Mix parameter index */
    if (node->opcode == VTX_OP_Parameter) {
        h ^= (uint32_t)node->local_index;
        h *= 16777619u;
    }

    /* Mix field offset */
    if (node->opcode == VTX_OP_LoadField || node->opcode == VTX_OP_StoreField) {
        h ^= (uint32_t)node->field_offset;
        h *= 16777619u;
    }

    /* Mix type ID */
    if (node->opcode == VTX_OP_NewObject || node->opcode == VTX_OP_NewArray ||
        node->opcode == VTX_OP_CheckCast || node->opcode == VTX_OP_InstanceOf) {
        h ^= (uint32_t)node->type_id;
        h *= 16777619u;
    }

    /* Mix method index */
    if (node->opcode == VTX_OP_CallStatic || node->opcode == VTX_OP_CallVirtual ||
        node->opcode == VTX_OP_CallInterface || node->opcode == VTX_OP_CallRuntime) {
        h ^= (uint32_t)node->method_index;
        h *= 16777619u;
    }

    /* Mix projection index */
    if (node->opcode == VTX_OP_Proj) {
        h ^= (uint32_t)node->local_index;
        h *= 16777619u;
    }

    return h;
}

/* ========================================================================== */
/* Congruence check                                                            */
/* ========================================================================== */

bool vtx_gvn_nodes_congruent(const vtx_node_t *a, const vtx_node_t *b,
                              const vtx_node_table_t *table)
{
    VTX_ASSERT(a != NULL, "node a must not be NULL");
    VTX_ASSERT(b != NULL, "node b must not be NULL");

    /* Quick checks */
    if (a->opcode != b->opcode) return false;
    if (a->type != b->type) return false;
    if (a->input_count != b->input_count) return false;

    /* Check auxiliary data */
    if (a->opcode == VTX_OP_Constant) {
        if (!vtx_constval_equal(a->constval, b->constval)) return false;
    }

    if (a->opcode == VTX_OP_If || a->opcode == VTX_OP_Cmp ||
        a->opcode == VTX_OP_CmpP || a->opcode == VTX_OP_CmpF ||
        a->opcode == VTX_OP_CmpD || a->opcode == VTX_OP_Guard ||
        a->opcode == VTX_OP_DeoptGuard) {
        if (a->cond != b->cond) return false;
    }

    if (a->opcode == VTX_OP_Parameter) {
        if (a->local_index != b->local_index) return false;
    }

    if (a->opcode == VTX_OP_LoadField || a->opcode == VTX_OP_StoreField) {
        if (a->field_offset != b->field_offset) return false;
    }

    if (a->opcode == VTX_OP_NewObject || a->opcode == VTX_OP_NewArray ||
        a->opcode == VTX_OP_CheckCast || a->opcode == VTX_OP_InstanceOf) {
        if (a->type_id != b->type_id) return false;
    }

    if (a->opcode == VTX_OP_CallStatic || a->opcode == VTX_OP_CallVirtual ||
        a->opcode == VTX_OP_CallInterface || a->opcode == VTX_OP_CallRuntime) {
        if (a->method_index != b->method_index) return false;
    }

    if (a->opcode == VTX_OP_Proj) {
        if (a->local_index != b->local_index) return false;
    }

    /* Check inputs: all corresponding inputs must be congruent.
     * If both inputs have value numbers, compare those; otherwise
     * compare the raw node IDs (which is stricter but correct for
     * the initial partition).
     *
     * For commutative binary operations, if the ordered comparison
     * fails, try the swapped comparison (Add(x,y) ≡ Add(y,x)). */
    bool inputs_match = true;
    for (uint32_t i = 0; i < a->input_count; i++) {
        vtx_nodeid_t a_inp = a->inputs[i];
        vtx_nodeid_t b_inp = b->inputs[i];

        /* Resolve to value number */
        vtx_nodeid_t a_vn = a_inp;
        vtx_nodeid_t b_vn = b_inp;

        if (a_inp != VTX_NODEID_INVALID && a_inp < table->count) {
            uint32_t av = table->nodes[a_inp].value_number;
            if (av != 0) a_vn = av;
        }
        if (b_inp != VTX_NODEID_INVALID && b_inp < table->count) {
            uint32_t bv = table->nodes[b_inp].value_number;
            if (bv != 0) b_vn = bv;
        }

        if (a_vn != b_vn) {
            inputs_match = false;
            break;
        }
    }

    if (!inputs_match) {
        /* For commutative binary operations (Add, Mul, And, Or, Xor),
         * try the swapped comparison: a[0]↔b[1], a[1]↔b[0]. */
        if (a->input_count == 2 && (
            a->opcode == VTX_OP_Add || a->opcode == VTX_OP_Mul ||
            a->opcode == VTX_OP_And || a->opcode == VTX_OP_Or ||
            a->opcode == VTX_OP_Xor)) {
            vtx_nodeid_t a0 = a->inputs[0], a1 = a->inputs[1];
            vtx_nodeid_t b0 = b->inputs[0], b1 = b->inputs[1];

            vtx_nodeid_t a0_vn = a0, a1_vn = a1, b0_vn = b0, b1_vn = b1;

            if (a0 != VTX_NODEID_INVALID && a0 < table->count) {
                uint32_t av = table->nodes[a0].value_number;
                if (av != 0) a0_vn = av;
            }
            if (a1 != VTX_NODEID_INVALID && a1 < table->count) {
                uint32_t av = table->nodes[a1].value_number;
                if (av != 0) a1_vn = av;
            }
            if (b0 != VTX_NODEID_INVALID && b0 < table->count) {
                uint32_t bv = table->nodes[b0].value_number;
                if (bv != 0) b0_vn = bv;
            }
            if (b1 != VTX_NODEID_INVALID && b1 < table->count) {
                uint32_t bv = table->nodes[b1].value_number;
                if (bv != 0) b1_vn = bv;
            }

            if (a0_vn == b1_vn && a1_vn == b0_vn) {
                inputs_match = true;
            }
        }

        if (!inputs_match) return false;
    }

    return true;
}

/* ========================================================================== */
/* GVN hash table                                                              */
/* ========================================================================== */

/**
 * Simple open-addressing hash table for GVN.
 * Maps hash → node ID. On collision, check congruence; if congruent,
 * return the existing node; if not, probe next slot.
 */

#define VTX_GVN_TABLE_INITIAL_CAPACITY 1024

typedef struct {
    uint32_t     hash;          /* 0 = empty slot */
    vtx_nodeid_t node_id;       /* the representative node for this hash */
} vtx_gvn_entry_t;

typedef struct {
    vtx_gvn_entry_t *entries;
    uint32_t         capacity;
    uint32_t         count;
} vtx_gvn_table_t;

static int gvn_table_init(vtx_gvn_table_t *t, uint32_t cap)
{
    t->entries = (vtx_gvn_entry_t *)calloc(cap, sizeof(vtx_gvn_entry_t));
    if (t->entries == NULL) return -1;
    t->capacity = cap;
    t->count = 0;
    return 0;
}

static void gvn_table_destroy(vtx_gvn_table_t *t)
{
    free(t->entries);
    t->entries = NULL;
    t->capacity = 0;
    t->count = 0;
}

/**
 * Grow the GVN table when load factor exceeds 0.7.
 */
static int gvn_table_grow(vtx_gvn_table_t *t, vtx_node_table_t *nt);

/**
 * Look up a node in the GVN table. If a congruent node exists, return its ID.
 * Otherwise, insert the node and return VTX_NODEID_INVALID.
 *
 * Returns the existing congruent node's ID, or VTX_NODEID_INVALID if this
 * node is the first of its congruence class.
 */
static vtx_nodeid_t gvn_table_lookup_or_insert(vtx_gvn_table_t *t,
                                                vtx_node_table_t *nt,
                                                uint32_t hash,
                                                vtx_nodeid_t node_id)
{
    VTX_ASSERT(hash != 0, "hash must be non-zero");

    /* IR-6 fix: grow table when load factor exceeds 0.7 */
    if (t->count * 10 >= t->capacity * 7) {
        gvn_table_grow(t, nt);
    }

    uint32_t idx = hash & (t->capacity - 1);
    uint32_t probe = 0;

    while (probe < t->capacity) {
        vtx_gvn_entry_t *entry = &t->entries[idx];

        if (entry->hash == 0) {
            /* Empty slot: insert */
            entry->hash = hash;
            entry->node_id = node_id;
            t->count++;
            return VTX_NODEID_INVALID;
        }

        if (entry->hash == hash) {
            /* Potential match: check congruence */
            const vtx_node_t *existing = vtx_node_get_const(nt, entry->node_id);
            const vtx_node_t *candidate = vtx_node_get_const(nt, node_id);
            if (existing != NULL && candidate != NULL &&
                vtx_gvn_nodes_congruent(existing, candidate, nt)) {
                /* Congruent: return the existing representative */
                return entry->node_id;
            }
            /* Hash collision but not congruent: probe next */
        }

        /* Linear probing */
        probe++;
        idx = (idx + 1) & (t->capacity - 1);
    }

    /* Table is full — should not happen with proper growth */
    return VTX_NODEID_INVALID;
}

/**
 * Grow the GVN table when load factor exceeds 0.7.
 */
static int gvn_table_grow(vtx_gvn_table_t *t, vtx_node_table_t *nt __attribute__((unused)))
{
    uint32_t new_cap = t->capacity * 2;
    vtx_gvn_entry_t *new_entries = (vtx_gvn_entry_t *)calloc(new_cap, sizeof(vtx_gvn_entry_t));
    if (new_entries == NULL) return -1;

    /* Rehash all existing entries */
    for (uint32_t i = 0; i < t->capacity; i++) {
        if (t->entries[i].hash != 0) {
            uint32_t h = t->entries[i].hash;
            vtx_nodeid_t nid = t->entries[i].node_id;
            uint32_t idx = h & (new_cap - 1);
            uint32_t probe = 0;
            while (probe < new_cap) {
                if (new_entries[idx].hash == 0) {
                    new_entries[idx].hash = h;
                    new_entries[idx].node_id = nid;
                    break;
                }
                probe++;
                idx = (idx + 1) & (new_cap - 1);
            }
        }
    }

    free(t->entries);
    t->entries = new_entries;
    t->capacity = new_cap;
    return 0;
}

/* ========================================================================== */
/* Redirect all uses of old_node to new_node                                   */
/* ========================================================================== */

/* redirect_all_uses is now replaced by vtx_node_replace_all_uses which is
 * O(K) in the number of users rather than O(N) scanning all nodes.
 * vtx_node_replace_all_uses also properly maintains use-def lists. */

/* ========================================================================== */
/* Main GVN pass                                                               */
/* ========================================================================== */

uint32_t vtx_gvn_run(vtx_graph_t *graph)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");

    vtx_node_table_t *nt = &graph->node_table;
    uint32_t eliminated = 0;

    /* Clear all value numbers */
    for (uint32_t i = 0; i < nt->count; i++) {
        nt->nodes[i].value_number = 0;
    }

    /* Initialize GVN hash table */
    uint32_t gvn_cap = VTX_GVN_TABLE_INITIAL_CAPACITY;
    /* Ensure capacity is at least 2x the node count */
    while (gvn_cap < nt->count * 2) {
        uint32_t doubled = gvn_cap * 2;
        if (doubled <= gvn_cap) break;
        gvn_cap = doubled;
    }

    vtx_gvn_table_t gvn;
    if (gvn_table_init(&gvn, gvn_cap) != 0) {
        return 0; /* can't run GVN without a hash table */
    }

    /* Assign value numbers and eliminate redundant nodes.
     * We iterate until no more changes occur (fixed point).
     * This is needed because redirecting uses may create new redundancy. */
    bool changed = true;
    uint32_t iteration = 0;
    const uint32_t max_iterations = 10; /* bounded for safety */

    while (changed && iteration < max_iterations) {
        changed = false;
        iteration++;

        /* Rebuild the GVN table for this iteration */
        gvn_table_destroy(&gvn);
        if (gvn_table_init(&gvn, gvn_cap) != 0) {
            return eliminated;
        }

        /* Clear value numbers */
        for (uint32_t i = 0; i < nt->count; i++) {
            nt->nodes[i].value_number = 0;
        }

        /* Process nodes in ID order (topological) */
        for (uint32_t i = 0; i < nt->count; i++) {
            vtx_node_t *node = &nt->nodes[i];
            if (node->dead) continue;

            /* Skip nodes that cannot be CSE'd:
             * - Control flow nodes (Region, If, Goto, etc.) — they have structural meaning
             * - Memory nodes (they have ordering constraints)
             * - Side-effecting nodes
             * - Pinned nodes (Phi, FrameState)
             * - Allocations (each allocation is unique)
             */
            if (vtx_node_is_control(node->opcode)) continue;
            if (vtx_node_is_memory(node->opcode)) continue;
            if (vtx_node_is_side_effecting(node->opcode)) continue;
            if (vtx_nf_has(node->flags, VTX_NF_PINNED)) continue;
            if (node->opcode == VTX_OP_NewObject || node->opcode == VTX_OP_NewArray ||
                node->opcode == VTX_OP_Allocate) continue;

            /* Pure data nodes are eligible for GVN */
            if (!vtx_nf_has(node->flags, VTX_NF_DATA)) continue;

            /* Compute hash */
            uint32_t hash = vtx_gvn_node_hash(node, nt);
            if (hash == 0) hash = 1; /* ensure non-zero */

            /* Look up or insert */
            vtx_nodeid_t existing = gvn_table_lookup_or_insert(&gvn, nt, hash, i);

            if (existing != VTX_NODEID_INVALID) {
                /* Found a congruent node! Eliminate this one. */
                VTX_ASSERT(existing != i, "should not match self");

                /* Redirect all uses of this node to the existing one.
                 * vtx_node_replace_all_uses is O(K) in the number of users
                 * and properly maintains use-def lists. After this call,
                 * the dead node's output_count will be 0 and all its
                 * former users now point to the existing representative. */
                vtx_node_replace_all_uses(nt, (vtx_nodeid_t)i, existing);

                /* Mark this node as dead */
                node->dead = true;
                /* output_count is already 0 after replace_all_uses */

                /* Clear inputs (break the edges) and update use-def lists.
                 * Since this node is dead, remove its use entries from
                 * each producer's use-def list. */
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

                eliminated++;
                changed = true;
            } else {
                /* This node is the representative of its class.
                 * Assign it a value number. */
                node->value_number = i + 1; /* non-zero, unique */
            }
        }
    }

    gvn_table_destroy(&gvn);
    return eliminated;
}
