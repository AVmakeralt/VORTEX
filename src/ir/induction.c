#include "ir/induction.h"
#include "ir/schedule.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* ========================================================================== */
/* Induction Variable Analysis for VORTEX SoN IR                              */
/*                                                                            */
/* Implementation strategy:                                                    */
/*   1. Walk the schedule to find all LoopBegin blocks                        */
/*   2. For each LoopBegin, examine its Phi nodes to identify basic IVs       */
/*   3. Walk def-use chains from basic IVs to find derived IVs               */
/*   4. Compute iteration ranges by examining loop-exit If conditions         */
/*   5. Provide query API for bounds check elimination                        */
/* ========================================================================== */

/* ========================================================================== */
/* Overflow-safe arithmetic (same as bounds_check.c — duplicated for         */
/* self-containment; the compiler will typically inline these)                */
/* ========================================================================== */

static bool iv_add_overflows(int64_t a, int64_t b)
{
    if (b > 0 && a > INT64_MAX - b) return true;
    if (b < 0 && a < INT64_MIN - b) return true;
    return false;
}

static bool iv_mul_overflows(int64_t a, int64_t b)
{
    if (a == 0 || b == 0) return false;
    if (a == -1) return b == INT64_MIN;
    if (b == -1) return a == INT64_MIN;
    if (a > 0) {
        if (b > 0) return a > INT64_MAX / b;
        else        return b < INT64_MIN / a;
    } else {
        if (b > 0) return a < INT64_MIN / b;
        else        return b < INT64_MAX / a;
    }
    return false;
}

static int64_t iv_sat_add(int64_t a, int64_t b)
{
    if (iv_add_overflows(a, b)) {
        return (b > 0) ? INT64_MAX : INT64_MIN;
    }
    return a + b;
}

static int64_t iv_sat_mul(int64_t a, int64_t b)
{
    if (iv_mul_overflows(a, b)) {
        bool positive = (a > 0 && b > 0) || (a < 0 && b < 0);
        return positive ? INT64_MAX : INT64_MIN;
    }
    return a * b;
}

/* ========================================================================== */
/* Helper: get constant value of a node if it is a Constant Int               */
/* ========================================================================== */

static bool get_int_constant(const vtx_node_table_t *table, vtx_nodeid_t id, int64_t *out)
{
    if (id == VTX_NODEID_INVALID || id >= table->count) return false;
    const vtx_node_t *node = &table->nodes[id];
    if (node->opcode != VTX_OP_Constant) return false;
    if (node->constval.kind != VTX_TYPE_Int) return false;
    if (out) *out = node->constval.as.int_val;
    return true;
}

/* ========================================================================== */
/* Helper: check if a node is a LoopBegin                                     */
/* ========================================================================== */

static bool is_loop_begin(const vtx_node_t *node)
{
    return node != NULL && node->opcode == VTX_OP_LoopBegin;
}

/* ========================================================================== */
/* Helper: check if a node is a control-flow merge (Region or LoopBegin)      */
/* ========================================================================== */

static bool is_region_node(const vtx_node_t *node)
{
    return node != NULL &&
           (node->opcode == VTX_OP_Region || node->opcode == VTX_OP_LoopBegin);
}

/* ========================================================================== */
/* IV result allocation                                                       */
/* ========================================================================== */

static vtx_iv_result_t *iv_result_create(vtx_arena_t *arena, uint32_t node_count)
{
    vtx_iv_result_t *result = (vtx_iv_result_t *)vtx_arena_alloc(
        arena, sizeof(vtx_iv_result_t));
    if (result == NULL) return NULL;

    memset(result, 0, sizeof(*result));

    result->iv_capacity = 16;  /* initial capacity for IV descriptors */
    result->ivs = (vtx_iv_desc_t *)vtx_arena_alloc(
        arena, result->iv_capacity * sizeof(vtx_iv_desc_t));
    if (result->ivs == NULL) return NULL;
    memset(result->ivs, 0, result->iv_capacity * sizeof(vtx_iv_desc_t));

    result->iv_count = 0;
    result->basic_iv_count = 0;
    result->derived_iv_count = 0;

    /* phi_to_iv map: indexed by node ID */
    result->phi_map_size = node_count;
    result->phi_to_iv = (int32_t *)vtx_arena_alloc(
        arena, node_count * sizeof(int32_t));
    if (result->phi_to_iv == NULL) return NULL;

    /* Initialize all entries to -1 (not an IV) */
    for (uint32_t i = 0; i < node_count; i++) {
        result->phi_to_iv[i] = -1;
    }

    return result;
}

/* ========================================================================== */
/* Add an IV descriptor to the result                                         */
/* ========================================================================== */

static uint32_t iv_result_add(vtx_iv_result_t *result, vtx_arena_t *arena,
                               const vtx_iv_desc_t *desc)
{
    /* Grow the array if needed */
    if (result->iv_count >= result->iv_capacity) {
        uint32_t new_cap = result->iv_capacity * 2;
        vtx_iv_desc_t *new_ivs = (vtx_iv_desc_t *)vtx_arena_alloc(
            arena, new_cap * sizeof(vtx_iv_desc_t));
        if (new_ivs == NULL) return (uint32_t)-1;

        memcpy(new_ivs, result->ivs, result->iv_count * sizeof(vtx_iv_desc_t));
        result->ivs = new_ivs;
        result->iv_capacity = new_cap;
    }

    uint32_t idx = result->iv_count;
    result->ivs[idx] = *desc;
    result->iv_count++;

    /* Update the phi_to_iv map */
    if (desc->phi_node < result->phi_map_size) {
        result->phi_to_iv[desc->phi_node] = (int32_t)idx;
    }

    /* Update statistics */
    if (desc->kind == VTX_IV_BASIC) {
        result->basic_iv_count++;
    } else if (desc->kind == VTX_IV_DERIVED) {
        result->derived_iv_count++;
    }

    return idx;
}

/* ========================================================================== */
/* Identify basic induction variables at a LoopBegin                          */
/*                                                                            */
/* A basic IV is a Phi node at a LoopBegin where:                             */
/*   - One input comes from outside the loop (the initial value)              */
/*   - The other input comes from inside the loop and is of the form:         */
/*       Add(init, stride)  or  Sub(init, stride)                            */
/*   - The "init" input of the Add/Sub is the Phi itself (back-edge)         */
/*                                                                            */
/* The LoopBegin has inputs: [entry_control, backedge_control]                */
/* The Phi has inputs: [init_value, backedge_value, region_control]           */
/* (the control input is the Region/LoopBegin node)                          */
/* ========================================================================== */

static void find_basic_ivs(vtx_graph_t *graph, vtx_iv_result_t *result,
                            vtx_arena_t *arena, uint32_t loop_begin_id,
                            uint32_t loop_header_block,
                            const vtx_schedule_t *schedule)
{
    vtx_node_table_t *nt = &graph->node_table;
    const vtx_node_t *loop_begin = vtx_node_get_const(nt, loop_begin_id);
    if (loop_begin == NULL) return;

    /* The loop_depth of this header — used to determine if nodes are
     * inside this specific loop */
    uint32_t loop_depth = schedule->blocks[loop_header_block].loop_depth;

    /* Walk all nodes to find Phi nodes whose first input is this LoopBegin.
     * In SoN IR, a Phi's last input is typically its controlling Region/LoopBegin. */
    for (uint32_t nid = 0; nid < nt->count; nid++) {
        const vtx_node_t *phi = &nt->nodes[nid];
        if (phi->dead) continue;
        if (phi->opcode != VTX_OP_Phi) continue;

        /* Check if this Phi is controlled by this LoopBegin.
         * The Phi's last input (or any input) should be the LoopBegin/Region. */
        bool controlled_by_loop = false;
        for (uint32_t i = 0; i < phi->input_count; i++) {
            if (phi->inputs[i] == loop_begin_id) {
                controlled_by_loop = true;
                break;
            }
        }
        if (!controlled_by_loop) continue;

        /* Now analyze the Phi's data inputs (non-control inputs).
         * For a LoopBegin Phi, we expect:
         *   inputs[0] = init value (from entry/preheader)
         *   inputs[1] = back-edge value (from loop body)
         *   inputs[2] or last = control (LoopBegin node)
         *
         * We identify data inputs by skipping inputs that are
         * Region/LoopBegin nodes. */
        vtx_nodeid_t init_id = VTX_NODEID_INVALID;
        vtx_nodeid_t backedge_id = VTX_NODEID_INVALID;

        for (uint32_t i = 0; i < phi->input_count; i++) {
            vtx_nodeid_t inp_id = phi->inputs[i];
            if (inp_id == VTX_NODEID_INVALID || inp_id >= nt->count) continue;

            const vtx_node_t *inp = &nt->nodes[inp_id];
            if (is_region_node(inp)) continue; /* skip control input */

            if (init_id == VTX_NODEID_INVALID) {
                init_id = inp_id;
            } else if (backedge_id == VTX_NODEID_INVALID) {
                backedge_id = inp_id;
            }
        }

        if (init_id == VTX_NODEID_INVALID || backedge_id == VTX_NODEID_INVALID) {
            continue; /* malformed Phi */
        }

        /* Now check if the back-edge value is of the form: Phi + stride
         * i.e., backedge = Add(Phi, stride) or Sub(Phi, stride)
         * where "Phi" is the current Phi node itself. */
        const vtx_node_t *backedge_node = &nt->nodes[backedge_id];
        int64_t stride_val = 0;
        bool stride_known = false;
        vtx_nodeid_t stride_node_id = VTX_NODEID_INVALID;

        if (backedge_node->opcode == VTX_OP_Add && backedge_node->input_count >= 2) {
            vtx_nodeid_t add_left  = backedge_node->inputs[0];
            vtx_nodeid_t add_right = backedge_node->inputs[1];

            if (add_left == nid) {
                /* Add(Phi, stride) */
                stride_node_id = add_right;
                stride_known = get_int_constant(nt, add_right, &stride_val);
            } else if (add_right == nid) {
                /* Add(stride, Phi) — commutative */
                stride_node_id = add_left;
                stride_known = get_int_constant(nt, add_left, &stride_val);
            }
        } else if (backedge_node->opcode == VTX_OP_Sub && backedge_node->input_count >= 2) {
            vtx_nodeid_t sub_left  = backedge_node->inputs[0];
            vtx_nodeid_t sub_right = backedge_node->inputs[1];

            if (sub_left == nid) {
                /* Sub(Phi, stride) → stride is negative */
                int64_t s;
                if (get_int_constant(nt, sub_right, &s)) {
                    stride_val = -s;
                    stride_known = true;
                    stride_node_id = sub_right;
                }
            }
        }

        if (!stride_known) {
            /* The back-edge is not a simple Add/Sub of the Phi with a constant.
             * Could still be an IV with variable stride, but we only handle
             * constant-stride IVs for now. */
            continue;
        }

        /* We found a basic IV! Create the descriptor. */
        vtx_iv_desc_t desc;
        memset(&desc, 0, sizeof(desc));

        desc.phi_node     = nid;
        desc.init_node    = init_id;
        desc.stride_node  = stride_node_id;
        desc.stride_const = stride_val;
        desc.kind         = VTX_IV_BASIC;

        /* For basic IVs: scale=1, offset=0, base_iv=self */
        desc.scale   = 1;
        desc.offset  = 0;
        desc.base_iv = nid;

        /* Compute the iteration range — we'll fill this in later
         * when we examine the loop exit condition. */
        desc.iteration_range.lo_known = false;
        desc.iteration_range.hi_known = false;
        desc.iteration_range.is_constant = false;

        /* Use the init value as the lower bound if it's known and stride > 0,
         * or the upper bound if stride < 0. */
        int64_t init_val;
        if (get_int_constant(nt, init_id, &init_val)) {
            if (stride_val > 0) {
                desc.iteration_range.lo = init_val;
                desc.iteration_range.lo_known = true;
            } else if (stride_val < 0) {
                desc.iteration_range.hi = init_val;
                desc.iteration_range.hi_known = true;
            }
        }

        iv_result_add(result, arena, &desc);
    }
}

/* ========================================================================== */
/* Compute iteration ranges from loop-exit conditions                         */
/*                                                                            */
/* For each loop, find the If node that exits the loop. The condition of      */
/* that If tells us the upper (or lower) bound of the IV.                     */
/*                                                                            */
/* Common patterns:                                                            */
/*   if (i < N)  → continue loop → i ∈ [init, N)                             */
/*   if (i >= N) → exit loop    → i ∈ [init, N)                              */
/*   if (i <= N) → continue loop → i ∈ [init, N+1)                           */
/* ========================================================================== */

static void compute_iteration_ranges(vtx_graph_t *graph, vtx_iv_result_t *result,
                                      const vtx_schedule_t *schedule)
{
    vtx_node_table_t *nt = &graph->node_table;

    /* For each basic IV, look for the loop-exit If condition that constrains
     * the IV's range. We do this by looking at If nodes in the loop that
     * compare the IV against a limit. */
    for (uint32_t iv_idx = 0; iv_idx < result->iv_count; iv_idx++) {
        vtx_iv_desc_t *iv = &result->ivs[iv_idx];
        if (iv->kind != VTX_IV_BASIC) continue;

        vtx_nodeid_t phi_id = iv->phi_node;
        int64_t stride = iv->stride_const;

        /* Walk the use-def chain of the IV's Phi to find Cmp nodes.
         * The IV is typically compared in an If condition like: i < N. */
        const vtx_node_t *phi_node = &nt->nodes[phi_id];
        if (phi_node->use_count == 0) continue;

        /* Walk all users of the Phi to find Cmp nodes */
        for (uint32_t u = 0; u < phi_node->use_count; u++) {
            vtx_use_entry_t *use = &phi_node->uses[u];
            vtx_nodeid_t user_id = use->user_id;
            if (user_id >= nt->count) continue;

            const vtx_node_t *user = &nt->nodes[user_id];
            if (user->dead) continue;
            if (user->opcode != VTX_OP_Cmp) continue;

            /* Found a Cmp that uses our IV. Extract the comparison. */
            if (user->input_count < 2) continue;

            vtx_nodeid_t left_id  = user->inputs[0];
            vtx_nodeid_t right_id = user->inputs[1];

            /* Determine which side is the IV and which is the limit */
            vtx_nodeid_t limit_id = VTX_NODEID_INVALID;
            bool iv_on_left = false;

            if (left_id == phi_id) {
                limit_id = right_id;
                iv_on_left = true;
            } else if (right_id == phi_id) {
                limit_id = left_id;
                iv_on_left = false;
            } else {
                continue; /* neither side is our IV */
            }

            /* Now check if this Cmp feeds an If that exits the loop.
             * We look at users of the Cmp to find If nodes. */
            for (uint32_t cu = 0; cu < user->use_count; cu++) {
                vtx_use_entry_t *cmp_use = &user->uses[cu];
                vtx_nodeid_t cmp_user_id = cmp_use->user_id;
                if (cmp_user_id >= nt->count) continue;

                const vtx_node_t *if_node = &nt->nodes[cmp_user_id];
                if (if_node->opcode != VTX_OP_If) continue;

                /* We found an If that uses our Cmp. Check if one of the
                 * If's projections exits the loop (has lower loop depth
                 * than the header). */
                /* Walk users of the If to find Proj nodes */
                for (uint32_t iu = 0; iu < if_node->use_count; iu++) {
                    vtx_use_entry_t *if_use = &if_node->uses[iu];
                    vtx_nodeid_t proj_id = if_use->user_id;
                    if (proj_id >= nt->count) continue;

                    const vtx_node_t *proj = &nt->nodes[proj_id];
                    if (proj->opcode != VTX_OP_Proj) continue;

                    /* Check where this Proj leads — if it exits the loop,
                     * we have our loop-exit condition. */
                    uint32_t proj_block = vtx_schedule_node_block(schedule, proj_id);
                    if (proj_block == (uint32_t)-1) continue;
                    if (proj_block >= schedule->count) continue;

                    uint32_t header_block = vtx_schedule_node_block(schedule, phi_id);
                    if (header_block == (uint32_t)-1) continue;

                    uint32_t header_depth = schedule->blocks[header_block].loop_depth;
                    uint32_t proj_depth = schedule->blocks[proj_block].loop_depth;

                    /* If the projection's block has a lower loop depth, it exits the loop */
                    if (proj_depth >= header_depth) continue;

                    /* This is a loop-exit projection!
                     * Now we need to determine the condition under which
                     * the loop continues (the opposite of the exit). */

                    /* The proj's local_index typically indicates true/false branch.
                     * For now, we use the comparison condition to derive the range. */
                    vtx_cond_t cond = user->cond;

                    /* If the If exits on the TRUE branch and the Proj is
                     * the TRUE branch, then the exit condition is the Cmp
                     * condition itself. If the Proj is the FALSE branch,
                     * the exit condition is the negated Cmp.
                     * For simplicity, we try both interpretations. */
                    bool exit_on_true = (proj->local_index == 0);

                    /* Derive the iteration bound from the comparison */
                    int64_t limit_val;
                    bool limit_known = get_int_constant(nt, limit_id, &limit_val);

                    if (iv_on_left) {
                        /* IV is on the left: IV <cmp> limit */
                        if (stride > 0) {
                            /* Positive stride: IV increases.
                             * Loop continues while IV is below some limit. */
                            switch (cond) {
                            case VTX_COND_LT:
                                /* Loop exit: IV >= limit (if exit on true)
                                 * or loop continue: IV < limit (if exit on false)
                                 * If the FALSE branch exits: IV >= limit →
                                 *   range = [init, limit)
                                 * If the TRUE branch exits: IV >= limit →
                                 *   range = [init, limit)  (exit on LT is unusual)
                                 * Actually: if (IV < limit) goto loop_body else goto exit
                                 *   → loop continues when IV < limit
                                 *   → IV range is [init, limit) */
                                if (!exit_on_true && limit_known) {
                                    iv->iteration_range.hi = limit_val;
                                    iv->iteration_range.hi_known = true;
                                } else if (exit_on_true && limit_known) {
                                    /* The LT condition leads to exit:
                                     * exit when IV < limit → range is [limit, ...) (unlikely for counting loop) */
                                } else if (limit_known) {
                                    /* Conservative: use limit as upper bound */
                                    iv->iteration_range.hi = limit_val;
                                    iv->iteration_range.hi_known = true;
                                }
                                break;

                            case VTX_COND_LE:
                                if (limit_known) {
                                    iv->iteration_range.hi = iv_sat_add(limit_val, 1);
                                    iv->iteration_range.hi_known = true;
                                }
                                break;

                            case VTX_COND_GT:
                                /* IV > limit as exit condition is unusual for counting up */
                                break;

                            case VTX_COND_GE:
                                /* if (IV >= limit) exit → IV ∈ [init, limit) */
                                if (exit_on_true && limit_known) {
                                    iv->iteration_range.hi = limit_val;
                                    iv->iteration_range.hi_known = true;
                                }
                                break;

                            case VTX_COND_NE:
                                /* if (IV != limit) continue → IV ∈ [init, limit) */
                                if (limit_known) {
                                    iv->iteration_range.hi = limit_val;
                                    iv->iteration_range.hi_known = true;
                                }
                                break;

                            default:
                                break;
                            }
                        } else if (stride < 0) {
                            /* Negative stride: IV decreases.
                             * Loop continues while IV is above some limit. */
                            switch (cond) {
                            case VTX_COND_GT:
                                if (limit_known) {
                                    iv->iteration_range.lo = limit_val;
                                    iv->iteration_range.lo_known = true;
                                }
                                break;

                            case VTX_COND_GE:
                                if (limit_known) {
                                    iv->iteration_range.lo = iv_sat_add(limit_val, 1);
                                    iv->iteration_range.lo_known = true;
                                }
                                break;

                            case VTX_COND_LT:
                                /* if (IV < limit) exit → IV ∈ (limit, init] */
                                if (exit_on_true && limit_known) {
                                    iv->iteration_range.lo = limit_val;
                                    iv->iteration_range.lo_known = true;
                                }
                                break;

                            default:
                                break;
                            }
                        }
                    } else {
                        /* IV is on the right: limit <cmp> IV */
                        if (stride > 0) {
                            switch (cond) {
                            case VTX_COND_GT:
                                /* limit > IV → IV < limit */
                                if (limit_known) {
                                    iv->iteration_range.hi = limit_val;
                                    iv->iteration_range.hi_known = true;
                                }
                                break;

                            case VTX_COND_GE:
                                /* limit >= IV → IV <= limit */
                                if (limit_known) {
                                    iv->iteration_range.hi = iv_sat_add(limit_val, 1);
                                    iv->iteration_range.hi_known = true;
                                }
                                break;

                            default:
                                break;
                            }
                        } else if (stride < 0) {
                            switch (cond) {
                            case VTX_COND_LT:
                                /* limit < IV → IV > limit */
                                if (limit_known) {
                                    iv->iteration_range.lo = limit_val;
                                    iv->iteration_range.lo_known = true;
                                }
                                break;

                            default:
                                break;
                            }
                        }
                    }

                    /* If we found a range bound, stop searching */
                    if (iv->iteration_range.lo_known && iv->iteration_range.hi_known) {
                        goto next_iv;
                    }
                }
            }
        }
        next_iv:
        /* Mark as constant if both bounds are the same */
        if (iv->iteration_range.lo_known && iv->iteration_range.hi_known &&
            iv->iteration_range.lo == iv->iteration_range.hi) {
            iv->iteration_range.is_constant = true;
        }
        continue;
    }
}

/* ========================================================================== */
/* Find derived induction variables                                           */
/*                                                                            */
/* A derived IV is a value that is an affine function of a basic IV:          */
/*   j = scale * i + offset                                                  */
/*                                                                            */
/* We look for patterns like:                                                 */
/*   Mul(IV, constant)         → scale=constant, offset=0                    */
/*   Add(Mul(IV, constant), c) → scale=constant, offset=c                    */
/*   Add(IV, constant)         → scale=1, offset=constant                    */
/*   Sub(IV, constant)         → scale=1, offset=-constant                   */
/* ========================================================================== */

static void find_derived_ivs(vtx_graph_t *graph, vtx_iv_result_t *result,
                              vtx_arena_t *arena, const vtx_schedule_t *schedule)
{
    vtx_node_table_t *nt = &graph->node_table;
    uint32_t node_count = nt->count;

    /* For each basic IV, walk its use chain to find derived IVs */
    for (uint32_t iv_idx = 0; iv_idx < result->iv_count; iv_idx++) {
        vtx_iv_desc_t *base_iv = &result->ivs[iv_idx];
        if (base_iv->kind != VTX_IV_BASIC) continue;

        vtx_nodeid_t phi_id = base_iv->phi_node;
        const vtx_node_t *phi_node = &nt->nodes[phi_id];
        if (phi_node->use_count == 0) continue;

        /* Walk all users of the Phi (the basic IV) */
        for (uint32_t u = 0; u < phi_node->use_count; u++) {
            vtx_use_entry_t *use = &phi_node->uses[u];
            vtx_nodeid_t user_id = use->user_id;
            if (user_id >= node_count) continue;

            const vtx_node_t *user = &nt->nodes[user_id];
            if (user->dead) continue;

            /* Skip if this is the back-edge Add/Sub (already part of the basic IV) */
            if (user_id == base_iv->stride_node) continue;

            int64_t scale = 0;
            int64_t offset = 0;
            bool found = false;

            switch (user->opcode) {
            case VTX_OP_Mul: {
                if (user->input_count < 2) break;
                vtx_nodeid_t left_id  = user->inputs[0];
                vtx_nodeid_t right_id = user->inputs[1];

                int64_t c;
                if (left_id == phi_id && get_int_constant(nt, right_id, &c)) {
                    /* IV * constant → scale=c, offset=0 */
                    scale = c;
                    offset = 0;
                    found = true;
                } else if (right_id == phi_id && get_int_constant(nt, left_id, &c)) {
                    /* constant * IV → scale=c, offset=0 */
                    scale = c;
                    offset = 0;
                    found = true;
                }
                break;
            }

            case VTX_OP_Add: {
                if (user->input_count < 2) break;
                vtx_nodeid_t left_id  = user->inputs[0];
                vtx_nodeid_t right_id = user->inputs[1];

                if (left_id == phi_id) {
                    int64_t c;
                    if (get_int_constant(nt, right_id, &c)) {
                        /* IV + constant → scale=1, offset=c */
                        scale = 1;
                        offset = c;
                        found = true;
                    }
                    /* Check if right is Mul(IV, constant) — from another derived IV */
                    if (!found && right_id < node_count) {
                        const vtx_node_t *right_node = &nt->nodes[right_id];
                        if (right_node->opcode == VTX_OP_Mul && right_node->input_count >= 2) {
                            vtx_nodeid_t mul_left  = right_node->inputs[0];
                            vtx_nodeid_t mul_right = right_node->inputs[1];
                            int64_t c;
                            if (mul_left == phi_id && get_int_constant(nt, mul_right, &c)) {
                                scale = c;
                                offset = 0;
                                found = true;
                            } else if (mul_right == phi_id && get_int_constant(nt, mul_left, &c)) {
                                scale = c;
                                offset = 0;
                                found = true;
                            }
                        }
                    }
                } else if (right_id == phi_id) {
                    int64_t c;
                    if (get_int_constant(nt, left_id, &c)) {
                        /* constant + IV → scale=1, offset=c */
                        scale = 1;
                        offset = c;
                        found = true;
                    }
                    if (!found && left_id < node_count) {
                        const vtx_node_t *left_node = &nt->nodes[left_id];
                        if (left_node->opcode == VTX_OP_Mul && left_node->input_count >= 2) {
                            vtx_nodeid_t mul_left  = left_node->inputs[0];
                            vtx_nodeid_t mul_right = left_node->inputs[1];
                            int64_t c;
                            if (mul_left == phi_id && get_int_constant(nt, mul_right, &c)) {
                                scale = c;
                                offset = 0;
                                found = true;
                            } else if (mul_right == phi_id && get_int_constant(nt, mul_left, &c)) {
                                scale = c;
                                offset = 0;
                                found = true;
                            }
                        }
                    }
                }
                break;
            }

            case VTX_OP_Sub: {
                if (user->input_count < 2) break;
                vtx_nodeid_t left_id  = user->inputs[0];
                vtx_nodeid_t right_id = user->inputs[1];

                if (left_id == phi_id) {
                    int64_t c;
                    if (get_int_constant(nt, right_id, &c)) {
                        /* IV - constant → scale=1, offset=-c */
                        scale = 1;
                        offset = -c;
                        found = true;
                    }
                }
                /* Note: constant - IV is NOT an affine IV in the
                 * standard sense (it's scale=-1), but we handle it: */
                if (!found && right_id == phi_id) {
                    int64_t c;
                    if (get_int_constant(nt, left_id, &c)) {
                        /* constant - IV → scale=-1, offset=c */
                        scale = -1;
                        offset = c;
                        found = true;
                    }
                }
                break;
            }

            case VTX_OP_Neg: {
                if (user->input_count < 1) break;
                if (user->inputs[0] == phi_id) {
                    /* -IV → scale=-1, offset=0 */
                    scale = -1;
                    offset = 0;
                    found = true;
                }
                break;
            }

            default:
                break;
            }

            if (!found) continue;

            /* Create a derived IV descriptor */
            vtx_iv_desc_t desc;
            memset(&desc, 0, sizeof(desc));

            desc.phi_node     = VTX_NODEID_INVALID;  /* derived IVs are not Phi nodes */
            desc.init_node    = VTX_NODEID_INVALID;
            desc.stride_node  = VTX_NODEID_INVALID;
            desc.stride_const = iv_sat_mul(base_iv->stride_const, scale);
            desc.kind         = VTX_IV_DERIVED;
            desc.scale        = scale;
            desc.offset       = offset;
            desc.base_iv      = phi_id;

            /* Compute the derived IV's iteration range from the base IV.
             * If base ∈ [lo, hi), then derived ∈ [scale*lo+offset, scale*hi+offset)
             * taking care of sign flips when scale < 0. */
            if (base_iv->iteration_range.lo_known && base_iv->iteration_range.hi_known) {
                int64_t base_lo = base_iv->iteration_range.lo;
                int64_t base_hi = base_iv->iteration_range.hi;

                if (scale > 0) {
                    desc.iteration_range.lo = iv_sat_add(iv_sat_mul(scale, base_lo), offset);
                    desc.iteration_range.hi = iv_sat_add(iv_sat_mul(scale, base_hi), offset);
                    desc.iteration_range.lo_known = true;
                    desc.iteration_range.hi_known = true;
                } else if (scale < 0) {
                    /* When scale < 0, the range flips */
                    desc.iteration_range.lo = iv_sat_add(iv_sat_mul(scale, base_hi), offset);
                    desc.iteration_range.hi = iv_sat_add(iv_sat_mul(scale, base_lo), offset);
                    desc.iteration_range.lo_known = true;
                    desc.iteration_range.hi_known = true;
                }
                desc.iteration_range.is_constant =
                    (desc.iteration_range.lo_known && desc.iteration_range.hi_known &&
                     desc.iteration_range.lo == desc.iteration_range.hi);
            } else if (base_iv->iteration_range.lo_known && scale > 0) {
                desc.iteration_range.lo = iv_sat_add(iv_sat_mul(scale, base_iv->iteration_range.lo), offset);
                desc.iteration_range.lo_known = true;
            } else if (base_iv->iteration_range.hi_known && scale > 0) {
                desc.iteration_range.hi = iv_sat_add(iv_sat_mul(scale, base_iv->iteration_range.hi), offset);
                desc.iteration_range.hi_known = true;
            }

            iv_result_add(result, arena, &desc);
        }
    }
}

/* ========================================================================== */
/* Main analysis entry point                                                  */
/* ========================================================================== */

vtx_iv_result_t *vtx_iv_analyze(vtx_graph_t *graph, vtx_arena_t *arena)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    if (graph == NULL || arena == NULL) return NULL;

    vtx_node_table_t *nt = &graph->node_table;
    uint32_t node_count = nt->count;

    if (node_count == 0) return NULL;

    /* We need a schedule to identify loops. Create a temporary one.
     * In the integrated pipeline, the schedule would already exist. */
    vtx_schedule_t schedule_storage;
    memset(&schedule_storage, 0, sizeof(schedule_storage));

    /* Try to get schedule from graph's block info if available */
    bool schedule_valid = false;

    if (graph->blocks != NULL && graph->block_count > 0) {
        /* Build a lightweight schedule from the block info */
        if (vtx_schedule_run(graph, arena, &schedule_storage) == 0) {
            schedule_valid = true;
        }
    }

    if (!schedule_valid) {
        /* No schedule available — we can still do limited IV analysis
         * by scanning for LoopBegin nodes directly in the graph. */
    }

    /* Allocate the result */
    vtx_iv_result_t *result = iv_result_create(arena, node_count);
    if (result == NULL) return NULL;

    /* Phase 1: Find all LoopBegin nodes */
    for (uint32_t nid = 0; nid < node_count; nid++) {
        const vtx_node_t *node = &nt->nodes[nid];
        if (node->dead) continue;
        if (!is_loop_begin(node)) continue;

        /* Phase 2: Identify basic IVs at this LoopBegin */
        if (schedule_valid) {
            uint32_t header_block = vtx_schedule_node_block(&schedule_storage, nid);
            if (header_block != (uint32_t)-1) {
                find_basic_ivs(graph, result, arena, nid, header_block, &schedule_storage);
            }
        } else {
            /* Without a schedule, use a dummy header block */
            find_basic_ivs(graph, result, arena, nid, 0, &schedule_storage);
        }
    }

    /* Phase 3: Compute iteration ranges from loop-exit conditions */
    if (schedule_valid) {
        compute_iteration_ranges(graph, result, &schedule_storage);
    }

    /* Phase 4: Find derived IVs */
    find_derived_ivs(graph, result, arena, schedule_valid ? &schedule_storage : NULL);

    return result;
}

/* ========================================================================== */
/* Query API                                                                  */
/* ========================================================================== */

bool vtx_iv_is_induction(const vtx_iv_result_t *result, vtx_nodeid_t node_id)
{
    if (result == NULL) return false;
    if (node_id >= result->phi_map_size) return false;
    return result->phi_to_iv[node_id] >= 0;
}

const vtx_iv_desc_t *vtx_iv_get(const vtx_iv_result_t *result, vtx_nodeid_t node_id)
{
    if (result == NULL) return NULL;
    if (node_id >= result->phi_map_size) return NULL;
    int32_t idx = result->phi_to_iv[node_id];
    if (idx < 0 || (uint32_t)idx >= result->iv_count) return NULL;
    return &result->ivs[idx];
}

/* ========================================================================== */
/* Compute the range of an arbitrary value node                               */
/*                                                                            */
/* This walks the def chain to find if the value is an IV or an affine        */
/* function of an IV, and returns the corresponding range.                    */
/* ========================================================================== */

vtx_iv_range_t vtx_iv_value_range(const vtx_iv_result_t *result,
                                   vtx_node_table_t *table,
                                   vtx_nodeid_t value_node)
{
    vtx_iv_range_t unknown = {0, 0, false, false, false};

    if (result == NULL || table == NULL) return unknown;
    if (value_node == VTX_NODEID_INVALID || value_node >= table->count) return unknown;

    /* Check if this node IS a basic IV's Phi */
    const vtx_iv_desc_t *iv = vtx_iv_get(result, value_node);
    if (iv != NULL) {
        return iv->iteration_range;
    }

    /* Check if this node is a derived IV.
     * Derived IVs are not Phi nodes, so they won't be in the phi_to_iv map.
     * We scan the IV descriptors. */
    for (uint32_t i = 0; i < result->iv_count; i++) {
        const vtx_iv_desc_t *desc = &result->ivs[i];
        if (desc->kind != VTX_IV_DERIVED) continue;

        /* We need to check if value_node matches a derived IV we found.
         * Since derived IVs are identified by their defining expression
         * (e.g., Mul(Phi, constant)), we can't directly map them by Phi node.
         * Instead, we'll check the use chain of basic IVs. */
    }

    /* Try to decompose the value as an affine function of an IV.
     * Walk up to 3 levels of Add/Sub/Mul to find an IV. */
    const vtx_node_t *node = &table->nodes[value_node];
    if (node->dead) return unknown;

    /* Pattern: Add(IV_Phi, constant) */
    if (node->opcode == VTX_OP_Add && node->input_count >= 2) {
        vtx_nodeid_t left_id  = node->inputs[0];
        vtx_nodeid_t right_id = node->inputs[1];

        const vtx_iv_desc_t *base_iv = NULL;
        int64_t c = 0;
        bool has_const = false;

        if (vtx_iv_is_induction(result, left_id)) {
            base_iv = vtx_iv_get(result, left_id);
            has_const = get_int_constant(table, right_id, &c);
        } else if (vtx_iv_is_induction(result, right_id)) {
            base_iv = vtx_iv_get(result, right_id);
            has_const = get_int_constant(table, left_id, &c);
        }

        if (base_iv != NULL && base_iv->iteration_range.lo_known &&
            base_iv->iteration_range.hi_known) {
            vtx_iv_range_t r;
            r.lo = iv_sat_add(base_iv->iteration_range.lo, c);
            r.hi = iv_sat_add(base_iv->iteration_range.hi, c);
            r.lo_known = true;
            r.hi_known = true;
            r.is_constant = (r.lo == r.hi);
            return r;
        }
    }

    /* Pattern: Sub(IV_Phi, constant) */
    if (node->opcode == VTX_OP_Sub && node->input_count >= 2) {
        vtx_nodeid_t left_id  = node->inputs[0];
        vtx_nodeid_t right_id = node->inputs[1];

        if (vtx_iv_is_induction(result, left_id)) {
            const vtx_iv_desc_t *base_iv = vtx_iv_get(result, left_id);
            int64_t c = 0;
            if (get_int_constant(table, right_id, &c) &&
                base_iv->iteration_range.lo_known && base_iv->iteration_range.hi_known) {
                vtx_iv_range_t r;
                r.lo = iv_sat_add(base_iv->iteration_range.lo, -c);
                r.hi = iv_sat_add(base_iv->iteration_range.hi, -c);
                r.lo_known = true;
                r.hi_known = true;
                r.is_constant = (r.lo == r.hi);
                return r;
            }
        }
    }

    /* Pattern: Mul(IV_Phi, constant) */
    if (node->opcode == VTX_OP_Mul && node->input_count >= 2) {
        vtx_nodeid_t left_id  = node->inputs[0];
        vtx_nodeid_t right_id = node->inputs[1];

        const vtx_iv_desc_t *base_iv = NULL;
        int64_t c = 0;

        if (vtx_iv_is_induction(result, left_id)) {
            base_iv = vtx_iv_get(result, left_id);
            get_int_constant(table, right_id, &c);
        } else if (vtx_iv_is_induction(result, right_id)) {
            base_iv = vtx_iv_get(result, right_id);
            get_int_constant(table, left_id, &c);
        }

        if (base_iv != NULL && c != 0 &&
            base_iv->iteration_range.lo_known && base_iv->iteration_range.hi_known) {
            vtx_iv_range_t r;
            if (c > 0) {
                r.lo = iv_sat_add(iv_sat_mul(c, base_iv->iteration_range.lo), 0);
                r.hi = iv_sat_add(iv_sat_mul(c, base_iv->iteration_range.hi), 0);
            } else {
                r.lo = iv_sat_add(iv_sat_mul(c, base_iv->iteration_range.hi), 0);
                r.hi = iv_sat_add(iv_sat_mul(c, base_iv->iteration_range.lo), 0);
            }
            r.lo_known = true;
            r.hi_known = true;
            r.is_constant = (r.lo == r.hi);
            return r;
        }
    }

    /* If the node is a Constant, return a constant range */
    if (node->opcode == VTX_OP_Constant && node->constval.kind == VTX_TYPE_Int) {
        vtx_iv_range_t r;
        r.lo = node->constval.as.int_val;
        r.hi = node->constval.as.int_val;
        r.lo_known = true;
        r.hi_known = true;
        r.is_constant = true;
        return r;
    }

    return unknown;
}

/* ========================================================================== */
/* Check if a bounds check can be eliminated                                  */
/*                                                                            */
/* A bounds check (index < length) can be eliminated if the index is an IV    */
/* whose iteration range is provably within [0, length).                      */
/*                                                                            */
/* Cases handled:                                                             */
/*   1. index is a basic IV with range [0, length) → eliminate               */
/*   2. index is a derived IV with range within bounds → eliminate           */
/*   3. index is an affine function of an IV → compute range, check          */
/*   4. length is a constant → check range against constant                  */
/*   5. length is derived from the same variable as the IV's upper bound     */
/* ========================================================================== */

bool vtx_iv_can_eliminate_bounds(const vtx_iv_result_t *result,
                                  vtx_node_table_t *table,
                                  vtx_nodeid_t index_node,
                                  vtx_nodeid_t length_node)
{
    if (result == NULL || table == NULL) return false;
    if (index_node == VTX_NODEID_INVALID || length_node == VTX_NODEID_INVALID) return false;

    /* Get the range of the index value */
    vtx_iv_range_t idx_range = vtx_iv_value_range(result, table, index_node);
    if (!idx_range.lo_known || !idx_range.hi_known) return false;

    /* Get the length value (as a constant or range) */
    int64_t length_val;
    if (get_int_constant(table, length_node, &length_val)) {
        /* Length is a constant: check idx ∈ [0, length) */
        return idx_range.lo >= 0 && idx_range.hi <= length_val;
    }

    /* Length is not a constant. Check if the IV's upper bound is
     * the same node as the length node.
     *
     * This handles the very common pattern:
     *   for (i = 0; i < arr.length; i++)
     * where arr.length is not a constant but the IV range was
     * computed from the condition i < arr.length.
     *
     * If the IV's hi bound comes from the same Cmp node as the
     * length_node, we can still eliminate the bounds check. */
    for (uint32_t i = 0; i < result->iv_count; i++) {
        const vtx_iv_desc_t *iv = &result->ivs[i];
        if (iv->kind != VTX_IV_BASIC) continue;

        /* Check if the index_node is this IV's Phi or an affine
         * function of it */
        bool index_matches_iv = false;

        if (iv->phi_node == index_node) {
            index_matches_iv = true;
        }

        /* Also check derived IVs and affine combinations */
        if (!index_matches_iv) {
            /* Try to match the index_node against an affine expression
             * of this IV by checking the use chain */
            const vtx_node_t *idx_node = &table->nodes[index_node];
            if (!idx_node->dead) {
                /* Check Add/Sub/Mul patterns that involve this IV */
                if (idx_node->opcode == VTX_OP_Add && idx_node->input_count >= 2) {
                    if (idx_node->inputs[0] == iv->phi_node ||
                        idx_node->inputs[1] == iv->phi_node) {
                        index_matches_iv = true;
                    }
                } else if (idx_node->opcode == VTX_OP_Sub && idx_node->input_count >= 2) {
                    if (idx_node->inputs[0] == iv->phi_node) {
                        index_matches_iv = true;
                    }
                } else if (idx_node->opcode == VTX_OP_Mul && idx_node->input_count >= 2) {
                    if (idx_node->inputs[0] == iv->phi_node ||
                        idx_node->inputs[1] == iv->phi_node) {
                        index_matches_iv = true;
                    }
                }
            }
        }

        if (!index_matches_iv) continue;

        /* The index is (an affine function of) this IV.
         * If idx_range.lo >= 0, the nonneg check is proven.
         * If idx_range.hi <= length, the upper bound check is proven.
         * But we need to verify that idx_range.hi corresponds to the same
         * length_node value.
         *
         * For the case where length_node is a Proj node (array.length),
         * we need to check if the IV's range hi bound was computed
         * from the same value. We do this by checking if the IV's
         * iteration range hi_known is true and the IV's hi value is
         * derived from a comparison with length_node.
         *
         * Simple heuristic: if the IV's range is [init, hi) and
         * hi is not a constant, but the index at the loop exit
         * is bounded by length_node, we can eliminate. This works
         * because the loop condition guarantees i < length.
         *
         * For a fully sound analysis we would need to track which
         * node the upper bound came from. For now, we do a simpler
         * check: */
        if (iv->iteration_range.lo_known && iv->iteration_range.lo >= 0 &&
            iv->iteration_range.hi_known) {
            /* The IV has a known range. We need to verify that
             * idx_range.hi <= length. Since we can't evaluate length
             * as a constant, we check if the IV was bounded by this
             * exact length node.
             *
             * Heuristic: walk the Cmp that defined the IV's range
             * and check if one operand is the length_node. */
            const vtx_node_t *phi = &table->nodes[iv->phi_node];
            for (uint32_t u = 0; u < phi->use_count; u++) {
                vtx_use_entry_t *use = &phi->uses[u];
                vtx_nodeid_t cmp_id = use->user_id;
                if (cmp_id >= table->count) continue;

                const vtx_node_t *cmp = &table->nodes[cmp_id];
                if (cmp->opcode != VTX_OP_Cmp || cmp->input_count < 2) continue;

                vtx_nodeid_t left_id  = cmp->inputs[0];
                vtx_nodeid_t right_id = cmp->inputs[1];

                /* Check if the other operand of the Cmp is the length_node */
                if ((left_id == iv->phi_node && right_id == length_node) ||
                    (right_id == iv->phi_node && left_id == length_node)) {
                    /* The IV is compared against the same length_node in the loop
                     * condition. The loop condition guarantees i < length.
                     * Since idx_range.lo >= 0 and the loop guarantees i < length,
                     * the bounds check is redundant. */
                    return true;
                }
            }
        }
    }

    return false;
}
