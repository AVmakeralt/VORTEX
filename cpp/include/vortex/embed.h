// vortex/embed.h — C API for embedding VORTEX in C applications.
//
// This is the C wrapper around the C++ embedding API. It provides
// the same functionality without requiring C++ at the call site.
//
// All functions return 0 on success, -1 on failure (unless otherwise
// noted). Error messages are available via vtx_embed_last_error().
//
// Usage:
//   #include "vortex/embed.h"
//
//   vtx_embed_runtime_t* rt = vtx_embed_runtime_create();
//   if (!rt) { fprintf(stderr, "error: %s\n", vtx_embed_last_error()); }
//
//   vtx_embed_bytecode_t* bc = vtx_embed_bytecode_load("prog.vtbc");
//   vtx_embed_value_t result = vtx_embed_runtime_run(rt, bc);
//
//   vtx_embed_runtime_destroy(rt);
//   vtx_embed_bytecode_destroy(bc);
//
// Thread safety: A runtime is NOT thread-safe. Use one per thread.

#ifndef VORTEX_EMBED_H
#define VORTEX_EMBED_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle types */
typedef struct vtx_embed_runtime   vtx_embed_runtime_t;
typedef struct vtx_embed_bytecode  vtx_embed_bytecode_t;
typedef struct vtx_embed_object    vtx_embed_object_t;
typedef struct vtx_embed_array     vtx_embed_array_t;

/* Value type — 8 bytes, NaN-boxed.
 * Use the vtx_embed_value_make_* / vtx_embed_value_as_* helpers. */
typedef uint64_t vtx_embed_value_t;

/* ---- Error handling ---- */

// Returns the last error message, or NULL if no error.
// The pointer is valid until the next VORTEX call.
const char* vtx_embed_last_error(void);

// Clear the last error.
void vtx_embed_clear_error(void);

/* ---- Runtime lifecycle ---- */

// Create a new runtime. Returns NULL on failure.
vtx_embed_runtime_t* vtx_embed_runtime_create(void);

// Destroy a runtime. NULL is safe.
void vtx_embed_runtime_destroy(vtx_embed_runtime_t* rt);

// Enable the JIT with `nthreads` compilation threads.
// nthreads=0 for auto-detect. Returns 0 on success.
int vtx_embed_runtime_enable_jit(vtx_embed_runtime_t* rt, uint32_t nthreads);

// Eagerly compile at T1 or T2. Returns 0 on success.
int vtx_embed_runtime_compile_t1(vtx_embed_runtime_t* rt, vtx_embed_bytecode_t* bc);
int vtx_embed_runtime_compile_t2(vtx_embed_runtime_t* rt, vtx_embed_bytecode_t* bc);

// Run bytecode. Returns the result value.
vtx_embed_value_t vtx_embed_runtime_run(vtx_embed_runtime_t* rt,
                                          vtx_embed_bytecode_t* bc);

// Run bytecode with arguments.
vtx_embed_value_t vtx_embed_runtime_run_with_args(vtx_embed_runtime_t* rt,
                                                    vtx_embed_bytecode_t* bc,
                                                    const vtx_embed_value_t* args,
                                                    uint32_t arg_count);

// Compile + run in one call. Returns the result value.
// On failure, returns vtx_embed_value_undefined().
vtx_embed_value_t vtx_embed_runtime_compile_and_run(vtx_embed_runtime_t* rt,
                                                      vtx_embed_bytecode_t* bc,
                                                      uint32_t tier);

/* ---- Bytecode loading ---- */

// Load a .vtbc file from disk.
vtx_embed_bytecode_t* vtx_embed_bytecode_load(const char* path);

// Destroy a bytecode object. NULL is safe.
void vtx_embed_bytecode_destroy(vtx_embed_bytecode_t* bc);

// Disassemble bytecode to a string. The returned pointer is valid
// until the next call on this bytecode object.
const char* vtx_embed_bytecode_disassemble(vtx_embed_bytecode_t* bc);

/* ---- Object creation ---- */

// Create a new empty object. The runtime owns the memory.
// Returns NULL on failure.
vtx_embed_object_t* vtx_embed_object_create(vtx_embed_runtime_t* rt,
                                               uint32_t max_fields);

// Get/set properties by name.
vtx_embed_value_t vtx_embed_object_get(vtx_embed_runtime_t* rt,
                                         vtx_embed_object_t* obj,
                                         const char* name);
int vtx_embed_object_set(vtx_embed_runtime_t* rt,
                          vtx_embed_object_t* obj,
                          const char* name,
                          vtx_embed_value_t value);

// Check if a property exists (walks prototype chain).
bool vtx_embed_object_has(vtx_embed_runtime_t* rt,
                           vtx_embed_object_t* obj,
                           const char* name);

// Delete a property. Returns true if it existed.
bool vtx_embed_object_del(vtx_embed_runtime_t* rt,
                           vtx_embed_object_t* obj,
                           const char* name);

// Prototype chain.
vtx_embed_object_t* vtx_embed_object_prototype(vtx_embed_runtime_t* rt,
                                                 vtx_embed_object_t* obj);
int vtx_embed_object_set_prototype(vtx_embed_runtime_t* rt,
                                    vtx_embed_object_t* obj,
                                    vtx_embed_object_t* proto);

// Convert between object and value.
vtx_embed_value_t vtx_embed_object_as_value(vtx_embed_object_t* obj);

/* ---- Array creation ---- */

vtx_embed_array_t* vtx_embed_array_create(vtx_embed_runtime_t* rt,
                                            uint32_t length);
uint32_t vtx_embed_array_length(vtx_embed_array_t* arr);
vtx_embed_value_t vtx_embed_array_get(vtx_embed_array_t* arr, uint32_t index);
int vtx_embed_array_set(vtx_embed_array_t* arr, uint32_t index,
                         vtx_embed_value_t value);
vtx_embed_value_t vtx_embed_array_as_value(vtx_embed_array_t* arr);

/* ---- Host function registration ---- */

// Host function type: takes (argc, argv, user_data), returns a value.
typedef vtx_embed_value_t (*vtx_embed_host_fn)(int argc,
                                                  const vtx_embed_value_t* argv,
                                                  void* user_data);

// Register a host function by name. Returns the function ID (used in
// CALL_RUNTIME bytecode operand), or UINT32_MAX on failure.
uint32_t vtx_embed_register_host_function(const char* name,
                                            vtx_embed_host_fn fn,
                                            void* user_data);

/* ---- Value helpers ---- */

// Type predicates
bool vtx_embed_value_is_int(vtx_embed_value_t v);
bool vtx_embed_value_is_double(vtx_embed_value_t v);
bool vtx_embed_value_is_bool(vtx_embed_value_t v);
bool vtx_embed_value_is_null(vtx_embed_value_t v);
bool vtx_embed_value_is_undefined(vtx_embed_value_t v);
bool vtx_embed_value_is_object(vtx_embed_value_t v);
bool vtx_embed_value_is_truthy(vtx_embed_value_t v);

// Constructors
vtx_embed_value_t vtx_embed_value_make_int(int64_t i);
vtx_embed_value_t vtx_embed_value_make_double(double d);
vtx_embed_value_t vtx_embed_value_make_bool(bool b);
vtx_embed_value_t vtx_embed_value_make_null(void);
vtx_embed_value_t vtx_embed_value_make_undefined(void);

// Accessors
int64_t vtx_embed_value_as_int(vtx_embed_value_t v);
double vtx_embed_value_as_double(vtx_embed_value_t v);
bool vtx_embed_value_as_bool(vtx_embed_value_t v);

// Numeric coercion (int → int, double → int (truncates), bool → 0/1)
int64_t vtx_embed_value_to_int(vtx_embed_value_t v);
double vtx_embed_value_to_double(vtx_embed_value_t v);

// String representation (for debugging). Returns a pointer to a static
// buffer — valid until the next call.
const char* vtx_embed_value_to_string(vtx_embed_value_t v);

// Sentinel values
#define VTX_EMBED_VALUE_NULL      ((vtx_embed_value_t)0x7FF8000000000004ULL)
#define VTX_EMBED_VALUE_UNDEFINED ((vtx_embed_value_t)0x7FF8000000000003ULL)
#define VTX_EMBED_VALUE_TRUE      ((vtx_embed_value_t)0x7FF8000000000009ULL)
#define VTX_EMBED_VALUE_FALSE     ((vtx_embed_value_t)0x7FF8000000000001ULL)

/* ---- GC control ---- */

// Force a garbage collection cycle.
void vtx_embed_gc_collect(vtx_embed_runtime_t* rt);

// Heap statistics.
typedef struct {
    size_t young_used;
    size_t young_size;
    size_t old_used;
    size_t old_size;
    size_t total_allocations;
    size_t total_collections;
} vtx_embed_heap_stats_t;

vtx_embed_heap_stats_t vtx_embed_heap_stats(vtx_embed_runtime_t* rt);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // VORTEX_EMBED_H
