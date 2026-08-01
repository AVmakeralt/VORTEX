/* wrapper.h — VORTEX public API for Rust FFI bindings. */

/* Runtime */
#include "runtime/arena.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/helpers.h"
#include "runtime/safepoint_manager.h"
#include "runtime/vortex_runtime.h"

/* Interpreter */
#include "interp/dispatch.h"
#include "interp/frame.h"
#include "interp/profiler.h"
#include "interp/lookup.h"
#include "interp/type_feedback.h"

/* Baseline JIT (T1) */
#include "baseline/codegen.h"
#include "baseline/guards.h"
#include "baseline/frame_layout.h"
#include "baseline/deopt_stubs.h"
#include "baseline/instrument.h"

/* IR */
#include "ir/node.h"
#include "ir/graph.h"
#include "ir/gvn.h"
#include "ir/constant_prop.h"
#include "ir/dce.h"
#include "ir/schedule.h"
#include "ir/licm.h"
#include "ir/bounds_check.h"
#include "ir/strength_reduce.h"
#include "ir/loop_unroll.h"
#include "ir/smi_tag_elision.h"
#include "ir/rep_infer.h"

/* Code cache */
#include "codecache/cache.h"
#include "codecache/install.h"
#include "codecache/evict.h"
#include "codecache/invalidate.h"
#include "codecache/versioned.h"
#include "codecache/t1_persist.h"

/* Compilation pipeline */
#include "compile/threadpool.h"
#include "compile/request.h"
#include "compile/orchestrator.h"
#include "compile/pipeline.h"
#include "compile/decision.h"
#include "compile/spec_versioning.h"

/* Mid-tier */
#include "midtier/block_layout.h"

/* Lowering */
#include "lower/isel.h"
#include "lower/emit.h"
#include "lower/regalloc.h"
#include "lower/guard_emit.h"

/* PGO */
#include "profile/data.h"
#include "profile/persist.h"
#include "profile/merge.h"
#include "profile/phase.h"
#include "profile/confidence.h"
#include "profile/deterministic.h"
#include "profile/phase_partition.h"
#include "profile/phase_persist.h"
#include "profile/ensemble.h"
#include "profile/input_shape.h"
#include "profile/shape_dispatch.h"
#include "profile/patch_log.h"

/* Deoptimization */
#include "deopt/osr.h"
#include "deopt/deoptless.h"

/* Configuration */
#include "vortex_config.h"
