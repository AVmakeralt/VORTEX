#ifndef VORTEX_PROFILE_PERSIST_H
#define VORTEX_PROFILE_PERSIST_H

#include <stdbool.h>
#include "profile/data.h"

/**
 * VORTEX Profile Persistence — Jump-Start Serialization
 *
 * Saves and loads profile data in a binary format so that JIT compilation
 * can benefit from previous runs (Jump-Start compilation).
 *
 * Binary format layout (all multi-byte fields are little-endian):
 *   ┌──────────────────────────┐
 *   │  Magic: 0x564F5254 ("VORT")  uint32_t
 *   ├──────────────────────────┤
 *   │  Version: uint32_t       │  Must match VTX_PROFILE_VERSION
 *   ├──────────────────────────┤
 *   │  CRC32: uint32_t         │  Checksum of everything after this field
 *   ├──────────────────────────┤
 *   │  Method count: uint32_t  │
 *   ├──────────────────────────┤
 *   │  Call edge count: uint32_t │
 *   ├──────────────────────────┤
 *   │  Per-method data ...     │
 *   ├──────────────────────────┤
 *   │  Call edges ...          │
 *   └──────────────────────────┘
 *
 * Per-method data layout:
 *   ┌──────────────────────────────┐
 *   │  method_id: uint32_t         │
 *   │  invocation_count: uint64_t  │
 *   │  call_site_count: uint32_t   │
 *   │  branch_count: uint32_t      │
 *   │  field_access_count: uint32_t│
 *   │  loop_count: uint32_t        │
 *   ├──────────────────────────────┤
 *   │  Call sites (repeated):      │
 *   │    count: uint32_t           │
 *   │    megamorphic: uint8_t      │
 *   │    types[count]: uint32_t    │
 *   ├──────────────────────────────┤
 *   │  Branches (repeated):        │
 *   │    bytecode_pc: uint32_t     │
 *   │    taken: uint64_t           │
 *   │    not_taken: uint64_t       │
 *   ├──────────────────────────────┤
 *   │  Field accesses (repeated):  │
 *   │    field_offset: uint32_t    │
 *   │    count: uint32_t           │
 *   │    megamorphic: uint8_t      │
 *   │    shapes[count]: uint32_t   │
 *   ├──────────────────────────────┤
 *   │  Loops (repeated):           │
 *   │    loop_header_pc: uint32_t  │
 *   │    backedge_count: uint64_t  │
 *   └──────────────────────────────┘
 *
 * Version mismatch or CRC failure → file is silently ignored.
 */

/* Profile file format magic number: "VORT" in ASCII */
#define VTX_PROFILE_MAGIC 0x564F5254u

/* Profile format version. Increment when format changes.
 *
 * Version 2: Added bytecode_hash (32 bytes) to header for version gating.
 *            Profile is invalidated automatically if bytecode changes.
 * Version 1: Original format (no bytecode hash). */
#define VTX_PROFILE_VERSION 2u

/* SHA-256 hash size in bytes */
#define VTX_PROFILE_HASH_SIZE 32

/**
 * Compute SHA-256 hash of bytecode data for version gating.
 * Implemented in sha256.c (public domain, FIPS 180-4).
 */
void vtx_profile_compute_bytecode_hash(const uint8_t *bytecode, size_t len,
                                        uint8_t hash[VTX_PROFILE_HASH_SIZE]);

/* ========================================================================== */
/* Save / Load                                                                */
/* ========================================================================== */

/**
 * Save global profile data to a binary file.
 * Returns true on success, false on failure.
 * The file is written atomically (write to temp, then rename).
 * File permissions are set to 0600 (owner read/write only).
 *
 * @param global         The profile data to save
 * @param filename       Output filename
 * @param bytecode_hash  SHA-256 hash of the bytecode this profile was
 *                       collected against. Stored in the header so that
 *                       a profile from a different bytecode version is
 *                       automatically rejected on load. May be NULL to
 *                       skip the hash (not recommended for production).
 */
bool vtx_profile_save(const vtx_profile_global_t *global, const char *filename,
                      const uint8_t bytecode_hash[VTX_PROFILE_HASH_SIZE]);

/**
 * Load global profile data from a binary file.
 * If the file's version doesn't match VTX_PROFILE_VERSION, the file is ignored
 * and false is returned. If the CRC32 doesn't match, false is returned.
 * If expected_hash is non-NULL and the stored bytecode hash doesn't match,
 * false is returned (profile is from a different bytecode version).
 * On success, the loaded data is MERGED into the existing global profile
 * (existing data is preserved and augmented with loaded data).
 * Returns true on success, false on failure.
 */
bool vtx_profile_load(vtx_profile_global_t *global, const char *filename,
                      const uint8_t expected_hash[VTX_PROFILE_HASH_SIZE]);

/* ========================================================================== */
/* atexit handler                                                             */
/* ========================================================================== */

/**
 * Register an atexit handler that saves the profile to the given filename.
 * The global pointer is captured at registration time.
 * Returns 0 on success, -1 on failure.
 */
int vtx_profile_register_atexit(vtx_profile_global_t *global,
                                 const char *filename,
                                 const uint8_t bytecode_hash[VTX_PROFILE_HASH_SIZE]);

/**
 * Unregister the atexit handler. Clears the stored global pointer
 * so the atexit handler will not attempt to access a destroyed global.
 * Must be called before destroying the registered global.
 */
void vtx_profile_unregister_atexit(void);

#endif /* VORTEX_PROFILE_PERSIST_H */
