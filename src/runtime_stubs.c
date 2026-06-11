#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"

/* Stubs for baseline JIT runtime functions not used in T0 interpreter benchmark.
 * These are only called from JIT-compiled code (T1+), not from the interpreter. */

static vtx_gc_t *the_gc = NULL;

vtx_gc_t *vtx_get_current_gc(void) { return the_gc; }
void vtx_set_current_gc(vtx_gc_t *gc) { the_gc = gc; }

vtx_typeid_t vtx_runtime_typeof(vtx_value_t v) { (void)v; return 0; }
void vtx_runtime_call_static(const void *m, ...) { (void)m; }
void vtx_runtime_call_virtual(uint32_t x, const void *m, ...) { (void)x; (void)m; }
void vtx_runtime_call_interface(uint32_t x, const void *m, ...) { (void)x; (void)m; }
void vtx_runtime_monitor_enter(vtx_value_t obj) { (void)obj; }
void vtx_runtime_monitor_exit(vtx_value_t obj) { (void)obj; }
void vtx_runtime_throw(vtx_value_t exc) { (void)exc; abort(); }
