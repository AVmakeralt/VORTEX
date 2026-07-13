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

#ifndef VORTEX_PROFILE_PHASE_PERSIST_H
#define VORTEX_PROFILE_PHASE_PERSIST_H

/**
 * VORTEX Phase-Aware Profile Persistence (Sprint 2.2)
 *
 * Saves and loads per-phase profiles to/from a directory. Each phase is
 * stored as a separate file:
 *
 *   <dir>/<hash_hex>.<phase_id>.prof
 *
 * where:
 *   - <dir>      is the profile directory (e.g. ~/.cache/vortex/profiles)
 *   - <hash_hex> is the first 16 bytes of the SHA-256 bytecode hash as hex
 *   - <phase_id> is the phase ID (or "default" for VTX_PHASE_NONE)
 *
 * This deliberately reuses the existing single-profile file format
 * (profile/persist.c) — each phase file is a standard VORTEX profile
 * file. The partition manager just calls vtx_profile_save/load per phase.
 *
 * Benefits:
 *   - No master file to rewrite when phases are added/removed.
 *   - A crash during save only corrupts the phase being written, not
 *     all phases.
 *   - The existing vortex-profile CLI can dump any phase file directly.
 *   - Phase files can be deleted individually to "reset" a single phase.
 *
 * The default phase (phase_id == VTX_PHASE_NONE) is saved with the
 * suffix "default" rather than the numeric value, so it's distinguishable
 * in directory listings.
 */

#include <stdbool.h>
#include <stdint.h>
#include "profile/data.h"
#include "profile/phase_partition.h"
#include "profile/persist.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* Filename helpers                                                            */
/* ========================================================================== */

/**
 * Build the filename for a specific phase's profile.
 *
 * Format: <dir>/<hash_hex>.<phase_label>.prof
 *   where phase_label is "default" for VTX_PHASE_NONE, else the decimal
 *   phase ID.
 *
 * @param dir         Profile directory (NUL-terminated)
 * @param hash_hex    Bytecode hash as 32-char hex string (NUL-terminated)
 * @param phase_id    Phase ID (VTX_PHASE_NONE for default)
 * @param out         Output buffer
 * @param out_size    Size of output buffer (recommended: 600+)
 * @return            0 on success, -1 on buffer overflow
 */
int vtx_phase_partition_filename(const char *dir,
                                   const char *hash_hex,
                                   uint32_t phase_id,
                                   char *out,
                                   size_t out_size);

/* ========================================================================== */
/* Save / Load                                                                 */
/* ========================================================================== */

/**
 * Save all phases' profiles to a directory.
 *
 * For each valid phase entry in `part`, saves its profile to:
 *   <dir>/<hash_hex>.<phase_label>.prof
 *
 * The bytecode hash is included in each file's header so that a stale
 * profile from a different bytecode version is automatically rejected
 * on load.
 *
 * @param part         Partition manager
 * @param dir          Profile directory (must exist)
 * @param hash_hex     Bytecode hash as 32-char hex string
 * @param bytecode_hash Raw 32-byte bytecode hash (passed to vtx_profile_save)
 * @return             Number of phases saved, or -1 on error
 */
int vtx_phase_partition_save_all(vtx_phase_partition_t *part,
                                   const char *dir,
                                   const char *hash_hex,
                                   const uint8_t bytecode_hash[VTX_PROFILE_HASH_SIZE]);

/**
 * Load all phase profiles from a directory.
 *
 * Scans the directory for files matching <hash_hex>.*.prof, parses each
 * one, and adds the corresponding phase entry to `part`. The active
 * phase is set to the default (VTX_PHASE_NONE) after loading.
 *
 * Files that fail to load (bad magic, version mismatch, CRC failure,
 * hash mismatch) are silently skipped.
 *
 * @param part           Partition manager (must be initialized)
 * @param dir            Profile directory
 * @param hash_hex       Bytecode hash as 32-char hex string
 * @param expected_hash  Raw 32-byte bytecode hash for version gating
 * @return               Number of phases loaded, or -1 on error
 */
int vtx_phase_partition_load_all(vtx_phase_partition_t *part,
                                   const char *dir,
                                   const char *hash_hex,
                                   const uint8_t expected_hash[VTX_PROFILE_HASH_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* VORTEX_PROFILE_PHASE_PERSIST_H */
