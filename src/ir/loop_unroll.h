#ifndef VORTEX_LOOP_UNROLL_H
#define VORTEX_LOOP_UNROLL_H

#include "ir/graph.h"
#include "ir/schedule.h"

/**
 * Unroll loops by duplicating their body.
 *
 * Conservative implementation: only unrolls small loops (<= 20 body nodes)
 * with a single back-edge. The actual body replication is a future
 * enhancement — currently just marks the loop with the unroll factor.
 *
 * @param graph     The IR graph
 * @param schedule  The schedule (for loop structure)
 * @param arena     Arena for allocations
 * @param factor    Unroll factor (2, 3, or 4)
 * @return          Number of loops unrolled
 */
uint32_t vtx_loop_unroll_run(vtx_graph_t *graph,
                              const vtx_schedule_t *schedule,
                              vtx_arena_t *arena,
                              uint32_t factor);

#endif /* VORTEX_LOOP_UNROLL_H */
