/* ============================================================================ *
 * AI-GENERATED CODE
 *
 * This file was written from scratch by an AI assistant (GLM/Z.ai).
 * It is part of the VORTEX JIT compiler project.
 *
 * Human-written code lives in: src/interp/ (dispatch loop), src/baseline/
 * (codegen), src/runtime/ (GC, type system, arena), src/main_new.c.
 *
 * If reviewing, please verify correctness independently.
 * ============================================================================ */

#ifndef VORTEX_CODECACHE_T1_PERSIST_H
#define VORTEX_CODECACHE_T1_PERSIST_H

/**
 * VORTEX T1 Code Persistence (Sprint 5) — The Cold-Start Killer
 *
 * Problem: PGO only helps on the second run. If the user runs once per
 * container launch, restart, or CI job, you never benefit. This is V8
 * and HotSpot's weakness.
 *
 * Fix: don't just persist profile DATA — persist the T1 compiled CODE
 * itself. On startup, mmap the persisted T1 code cache and start
 * executing from it immediately. Skip the interpreter for known methods.
 *
 * Why T1 not T2/T3: T1 is non-speculative — safe to persist. T2/T3 are
 * speculative and depend on profile matching; persisting them risks
 * wrong-code if the profile doesn't match.
 *
 * Implementation:
 *   - T1 code is position-dependent → we persist relocations so the
 *     code can be loaded at any address (ASLR-aware)
 *   - On startup: mmap persisted code, apply relocations, mark methods
 *     as T1-available
 *   - Interpreter only runs for methods not in the persisted cache
 *
 * File format (VORTEX T1 Cache, ".t1c"):
 *
 *   ┌──────────────────────────────────┐
 *   │  Magic: 0x564F5431 ("VOT1")      │  uint32_t
 *   ├──────────────────────────────────┤
 *   │  Version: uint32_t               │  must match VTX_T1_CACHE_VERSION
 *   ├──────────────────────────────────┤
 *   │  CRC32: uint32_t                 │  checksum of everything after this
 *   ├──────────────────────────────────┤
 *   │  Bytecode hash: 32 bytes         │  SHA-256 of bytecode (version gate)
 *   ├──────────────────────────────────┤
 *   │  Method count: uint32_t          │
 *   ├──────────────────────────────────┤
 *   │  Total code size: uint32_t       │  bytes of native code total
 *   ├──────────────────────────────────┤
 *   │  Per-method entries:             │
 *   │    method_id: uint32_t           │
 *   │    code_offset: uint32_t         │  offset into code blob
 *   │    code_size: uint32_t           │  native code size
 *   │    entry_offset: uint32_t        │  offset to entry point
 *   │    stack_slots: uint32_t         │
 *   │    local_slots: uint32_t         │
 *   │    reloc_count: uint32_t         │
 *   │    relocs[reloc_count]:          │
 *   │      kind: uint32_t              │
 *   │      offset: uint32_t            │
 *   │      target_offset: uint32_t     │
 *   │      target_address: uint64_t    │  (symbol address, re-applied at load)
 *   │      addend: int32_t             │
 *   ├──────────────────────────────────┤
 *   │  Code blob: total_code_size      │  raw native code (position-independent
 *   │                                  │  after relocation)
 *   └──────────────────────────────────┘
 *
 * The code blob is loaded via mmap at an arbitrary address. Relocations
 * are applied to fix up:
 *   - Internal jumps/calls (REL32): target_offset is within the blob
 *   - External calls (ABS64/RIP_REL32): target_address is a runtime
 *     symbol that must be resolved at load time
 *
 * Safety:
 *   - The bytecode hash gates loading: a stale T1 cache from a different
 *     bytecode version is rejected (CRC check catches corruption)
 *   - Only T1 code is persisted (non-speculative)
 *   - The persisted code is mprotected PROT_READ|PROT_EXEC (no WRITE)
 *   - File permissions are 0600 (owner read/write only)
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "codecache/types.h"
#include "profile/persist.h"  /* VTX_PROFILE_HASH_SIZE */

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* Constants                                                                   */
/* ========================================================================== */

/** Magic number: "VOT1" in ASCII. */
#define VTX_T1_CACHE_MAGIC 0x564F5431u

/** File format version. Increment when format changes. */
#define VTX_T1_CACHE_VERSION 1u

/** Maximum number of methods in a single T1 cache file. */
#define VTX_T1_CACHE_MAX_METHODS 4096u

/** Maximum total code size in a single T1 cache file (16 MB). */
#define VTX_T1_CACHE_MAX_CODE_SIZE (16 * 1024 * 1024u)

/* ========================================================================== */
/* Persisted method entry                                                      */
/* ========================================================================== */

/**
 * A single method's persisted T1 code descriptor.
 *
 * This is the on-disk format. The code itself is in the code blob at
 * `code_offset`.
 */
typedef struct {
    uint32_t method_id;       /* method this code is for */
    uint32_t code_offset;     /* offset into the code blob */
    uint32_t code_size;       /* native code size in bytes */
    uint32_t entry_offset;    /* offset to entry point within the code */
    uint32_t stack_slots;     /* JIT frame stack slots */
    uint32_t local_slots;     /* JIT frame local slots */
    uint32_t reloc_count;     /* number of relocations for this method */
} vtx_t1_persist_method_t;

/**
 * A single persisted relocation entry.
 */
typedef struct {
    uint32_t kind;            /* vtx_reloc_kind_t cast to uint32 */
    uint32_t offset;          /* offset in code where fixup goes */
    uint32_t target_offset;   /* intra-code target offset (for REL32) */
    uint64_t target_address;  /* external target address (for ABS64) */
    int32_t  addend;          /* additional offset */
} vtx_t1_persist_reloc_t;

/* ========================================================================== */
/* Save: persist T1 compiled code to disk                                      */
/* ========================================================================== */

/**
 * Save a collection of T1 compiled methods to a T1 cache file.
 *
 * @param filename       Output filename
 * @param bytecode_hash  SHA-256 hash of the bytecode (for version gating)
 * @param methods        Array of compiled code pointers (T1 only)
 * @param method_count   Number of methods
 * @return               true on success, false on failure
 */
bool vtx_t1_cache_save(const char *filename,
                         const uint8_t bytecode_hash[VTX_PROFILE_HASH_SIZE],
                         const vtx_compiled_code_t **methods,
                         uint32_t method_count);

/* ========================================================================== */
/* Load: mmap persisted T1 code and apply relocations                          */
/* ========================================================================== */

/**
 * Loaded T1 cache: an mmap'd code blob with per-method descriptors.
 *
 * The code blob is mmap'd as PROT_READ|PROT_EXEC. Method entry points
 * are computed as (code_base + entry_offset).
 */
typedef struct {
    /* Header data */
    uint8_t  bytecode_hash[VTX_PROFILE_HASH_SIZE];  /* from file header */
    uint32_t method_count;
    uint32_t total_code_size;

    /* Per-method descriptors (heap-allocated copy) */
    vtx_t1_persist_method_t *methods;
    uint32_t                *method_reloc_offsets;  /* offset into relocs[] per method */
    vtx_t1_persist_reloc_t  *relocs;
    uint32_t                 reloc_count;

    /* mmap'd code blob */
    void    *code_base;       /* mmap'd base address */
    size_t   code_map_size;   /* total mmap size (page-aligned) */
    int      code_fd;         /* file descriptor (-1 if not mmap'd) */

    /* Load statistics */
    uint64_t load_time_ns;    /* time spent loading + relocating */
    bool     relocations_applied; /* true if relocations succeeded */
} vtx_t1_cache_t;

/**
 * Load a T1 cache file, mmap the code, and apply relocations.
 *
 * If the file's bytecode hash doesn't match `expected_hash`, the load
 * fails (returns false). If the CRC doesn't match, the load fails.
 *
 * On success, the caller can call vtx_t1_cache_get_entry() to get the
 * native entry point for any method in the cache.
 *
 * @param cache          Cache structure to populate (caller allocates)
 * @param filename       T1 cache file path
 * @param expected_hash  Expected bytecode hash (for version gating)
 * @return               true on success, false on failure (bad magic,
 *                       version mismatch, CRC failure, hash mismatch,
 *                       or relocation error)
 */
bool vtx_t1_cache_load(vtx_t1_cache_t *cache,
                         const char *filename,
                         const uint8_t expected_hash[VTX_PROFILE_HASH_SIZE]);

/**
 * Get the native entry point for a method in the loaded T1 cache.
 *
 * Returns NULL if the method is not in the cache.
 *
 * @param cache      Loaded T1 cache
 * @param method_id  Method to look up
 * @return           Native entry point, or NULL
 */
void *vtx_t1_cache_get_entry(const vtx_t1_cache_t *cache, uint32_t method_id);

/**
 * Check if a method is present in the loaded T1 cache.
 */
bool vtx_t1_cache_has_method(const vtx_t1_cache_t *cache, uint32_t method_id);

/**
 * Destroy a loaded T1 cache: unmap the code blob and free descriptors.
 */
void vtx_t1_cache_destroy(vtx_t1_cache_t *cache);

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

/**
 * Get T1 cache statistics.
 *
 * @param cache             Loaded T1 cache
 * @param method_count      Out: number of methods in cache
 * @param code_size         Out: total native code size
 * @param load_time_ns      Out: time spent loading
 * @param relocations       Out: number of relocations applied
 */
void vtx_t1_cache_stats(const vtx_t1_cache_t *cache,
                          uint32_t *method_count,
                          uint32_t *code_size,
                          uint64_t *load_time_ns,
                          uint32_t *relocations);

/* ========================================================================== */
/* File path helper                                                            */
/* ========================================================================== */

/**
 * Build the T1 cache filename for a given bytecode hash.
 *
 * Format: <dir>/<hash_hex>.t1c
 *
 * @param dir         Profile directory
 * @param hash_hex    Bytecode hash as 32-char hex string
 * @param out         Output buffer
 * @param out_size    Size of output buffer
 * @return            0 on success, -1 on overflow
 */
int vtx_t1_cache_filename(const char *dir,
                            const char *hash_hex,
                            char *out,
                            size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* VORTEX_CODECACHE_T1_PERSIST_H */
