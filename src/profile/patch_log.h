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

#ifndef VORTEX_PROFILE_PATCH_LOG_H
#define VORTEX_PROFILE_PATCH_LOG_H

/**
 * VORTEX Profile Patching — Append-Only Updates (Sprint 6)
 *
 * Problem: rewriting the whole profile at exit means a crash loses
 * everything. And you can only save at exit, not continuously.
 *
 * Fix: append deltas, not rewrite. After every recompilation, append
 * the new profile delta. Periodically compact in background.
 *
 * Impact: saves become near-free → you can save after every recomp,
 * not just at exit. Crash doesn't lose data (only the last partial
 * delta, which is detected by CRC and skipped).
 *
 * File format (".vpl" — VORTEX Patch Log):
 *
 *   ┌──────────────────────────────────┐
 *   │  Magic: 0x56504F4C ("VPOL")      │  uint32_t
 *   ├──────────────────────────────────┤
 *   │  Version: uint32_t               │  must match VTX_PATCH_LOG_VERSION
 *   ├──────────────────────────────────┤
 *   │  Bytecode hash: 32 bytes         │  SHA-256 of bytecode (version gate)
 *   ├──────────────────────────────────┤
 *   │  Entry count: uint32_t           │  number of delta entries
 *   ├──────────────────────────────────┤
 *   │  Delta entries (repeated):       │
 *   │    type: uint8_t                 │  entry type (see below)
 *   │    method_id: uint32_t           │
 *   │    timestamp_ns: uint64_t        │
 *   │    crc32: uint32_t               │  CRC of this entry's payload
 *   │    payload_len: uint32_t         │
 *   │    payload: payload_len bytes    │  type-specific data
 *   └──────────────────────────────────┘
 *
 * Entry types:
 *   METHOD_SNAPSHOT  — full method profile (compact representation)
 *   BRANCH_UPDATE    — single branch (pc, taken, not_taken)
 *   CALLSITE_UPDATE  — single callsite (index, type_id)
 *   LOOP_UPDATE      — single loop (header_pc, backedge_count)
 *   FIELD_UPDATE     — single field access (offset, shape_id)
 *   INVOCATION_COUNT — method invocation count delta
 *   COMPACTION_MARKER — marks where compaction occurred (entries before
 *                       this are subsumed by a snapshot)
 *
 * Crash safety: each entry has its own CRC32. If a crash happens mid-
 * write, the last entry will have a bad CRC and is skipped on replay.
 * All prior entries are intact.
 *
 * Compaction: when the log grows beyond a threshold, a background thread
 * writes a METHOD_SNAPSHOT for every method (the current state) followed
 * by a COMPACTION_MARKER, then truncates the log to just the snapshot +
 * marker. This bounds the log size and speeds up replay.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "profile/data.h"
#include "profile/persist.h"  /* VTX_PROFILE_HASH_SIZE */

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* Constants                                                                   */
/* ========================================================================== */

/** Magic number: "VPOL" in ASCII. */
#define VTX_PATCH_LOG_MAGIC 0x56504F4Cu

/** File format version. */
#define VTX_PATCH_LOG_VERSION 1u

/** Maximum payload size for a single delta entry (prevents runaway). */
#define VTX_PATCH_LOG_MAX_PAYLOAD 65536u

/** Compaction threshold: when the log has this many entries, compact. */
#define VTX_PATCH_LOG_COMPACTION_THRESHOLD 1000u

/** Entry types. */
typedef enum {
    VTX_PATCH_METHOD_SNAPSHOT  = 1,  /* full method profile */
    VTX_PATCH_BRANCH_UPDATE    = 2,  /* single branch (pc, taken, not_taken) */
    VTX_PATCH_CALLSITE_UPDATE  = 3,  /* single callsite (index, type_id) */
    VTX_PATCH_LOOP_UPDATE      = 4,  /* single loop (header_pc, backedge_count) */
    VTX_PATCH_FIELD_UPDATE     = 5,  /* single field access (offset, shape_id) */
    VTX_PATCH_INVOCATION_COUNT = 6,  /* method invocation count delta */
    VTX_PATCH_COMPACTION_MARKER = 7,  /* marks compaction point */
} vtx_patch_entry_type_t;

/* ========================================================================== */
/* Patch log handle                                                            */
/* ========================================================================== */

/**
 * Append-only patch log handle.
 *
 * The log is opened for appending (O_APPEND) so writes are atomic at
 * the OS level. Multiple processes can't safely share a log (use the
 * profile directory's per-hash files).
 */
typedef struct {
    int      fd;              /* file descriptor (-1 if not open) */
    char     filename[512];   /* path to the .vpl file */
    uint32_t entry_count;     /* entries written since open */
    uint64_t bytes_written;   /* total bytes appended */
    bool     writable;        /* true if opened for writing */
} vtx_patch_log_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Open a patch log for appending (writing).
 *
 * Creates the file if it doesn't exist, with 0600 permissions. If the
 * file exists, validates the header (magic, version, bytecode hash).
 * If the header is invalid, the file is truncated and re-created.
 *
 * @param log           Log handle (caller allocates)
 * @param filename      Path to the .vpl file
 * @param bytecode_hash SHA-256 hash of the bytecode (for version gating)
 * @return              0 on success, -1 on failure
 */
int vtx_patch_log_open(vtx_patch_log_t *log,
                         const char *filename,
                         const uint8_t bytecode_hash[VTX_PROFILE_HASH_SIZE]);

/**
 * Open a patch log for reading (replay).
 *
 * Validates the header. The caller then calls vtx_patch_log_replay()
 * to apply the deltas to a profile.
 *
 * @param log           Log handle (caller allocates)
 * @param filename      Path to the .vpl file
 * @param expected_hash Expected bytecode hash (NULL to skip check)
 * @return              0 on success, -1 on failure
 */
int vtx_patch_log_open_read(vtx_patch_log_t *log,
                              const char *filename,
                              const uint8_t expected_hash[VTX_PROFILE_HASH_SIZE]);

/**
 * Close the patch log.
 */
void vtx_patch_log_close(vtx_patch_log_t *log);

/* ========================================================================== */
/* Delta appending (write path)                                               */
/* ========================================================================== */

/**
 * Append a full method snapshot to the log.
 *
 * This is the "compaction" entry — it captures the complete current
 * state of a method's profile. Used during background compaction and
 * as the initial entry when saving a profile for the first time.
 *
 * @param log     Log handle (must be open for writing)
 * @param method  Method profile to snapshot
 * @return        0 on success, -1 on failure
 */
int vtx_patch_log_append_snapshot(vtx_patch_log_t *log,
                                    const vtx_profile_method_t *method);

/**
 * Append a branch update delta.
 *
 * @param log         Log handle
 * @param method_id   Method ID
 * @param bytecode_pc Branch bytecode PC
 * @param taken       Taken count
 * @param not_taken   Not-taken count
 * @return            0 on success, -1 on failure
 */
int vtx_patch_log_append_branch(vtx_patch_log_t *log,
                                  uint32_t method_id,
                                  uint32_t bytecode_pc,
                                  uint64_t taken,
                                  uint64_t not_taken);

/**
 * Append a callsite type update delta.
 *
 * @param log           Log handle
 * @param method_id     Method ID
 * @param callsite_index Call site index
 * @param type_id       Observed type ID
 * @return              0 on success, -1 on failure
 */
int vtx_patch_log_append_callsite(vtx_patch_log_t *log,
                                    uint32_t method_id,
                                    uint32_t callsite_index,
                                    vtx_typeid_t type_id);

/**
 * Append a loop backedge update delta.
 *
 * @param log            Log handle
 * @param method_id      Method ID
 * @param loop_header_pc Loop header bytecode PC
 * @param backedge_count Backedge count
 * @return               0 on success, -1 on failure
 */
int vtx_patch_log_append_loop(vtx_patch_log_t *log,
                                uint32_t method_id,
                                uint32_t loop_header_pc,
                                uint64_t backedge_count);

/**
 * Append a field shape update delta.
 *
 * @param log          Log handle
 * @param method_id    Method ID
 * @param field_offset Field offset
 * @param shape_id     Observed shape ID
 * @return             0 on success, -1 on failure
 */
int vtx_patch_log_append_field(vtx_patch_log_t *log,
                                 uint32_t method_id,
                                 uint32_t field_offset,
                                 vtx_shapeid_t shape_id);

/**
 * Append an invocation count update delta.
 *
 * @param log         Log handle
 * @param method_id   Method ID
 * @param count_delta Invocation count delta to add
 * @return            0 on success, -1 on failure
 */
int vtx_patch_log_append_invocation(vtx_patch_log_t *log,
                                      uint32_t method_id,
                                      uint64_t count_delta);

/**
 * Append a compaction marker.
 *
 * This marks the point where all prior entries have been subsumed by
 * a snapshot. On replay, entries before the last compaction marker
 * can be skipped (the snapshot captures their state).
 *
 * @param log  Log handle
 * @return     0 on success, -1 on failure
 */
int vtx_patch_log_append_compaction_marker(vtx_patch_log_t *log);

/* ========================================================================== */
/* Replay (read path)                                                          */
/* ========================================================================== */

/**
 * Replay the patch log, applying all deltas to a profile.
 *
 * Reads each entry in order, validates its CRC, and applies the delta
 * to the given profile. Entries with bad CRCs are skipped (crash
 * recovery). Entries before the last compaction marker are skipped
 * (they're subsumed by the snapshot).
 *
 * @param log     Log handle (must be open for reading)
 * @param profile Profile to apply deltas to
 * @return        Number of entries applied, or -1 on fatal error
 */
int vtx_patch_log_replay(vtx_patch_log_t *log,
                           vtx_profile_global_t *profile);

/* ========================================================================== */
/* Compaction                                                                  */
/* ========================================================================== */

/**
 * Compact the patch log.
 *
 * Reads the current log, computes the final profile state for each
 * method, writes a new log containing just METHOD_SNAPSHOT entries +
 * a COMPACTION_MARKER, then atomically replaces the old log.
 *
 * After compaction, the log is small (one snapshot per method) and
 * replay is fast.
 *
 * @param log           Log handle (must be open for writing)
 * @param profile       Current profile state (the source of truth for
 *                      the snapshots). If NULL, the log is replayed
 *                      first to build the profile, then compacted.
 * @return              0 on success, -1 on failure
 */
int vtx_patch_log_compact(vtx_patch_log_t *log,
                            const vtx_profile_global_t *profile);

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

/**
 * Get patch log statistics.
 *
 * @param log             Log handle
 * @param entry_count     Out: entries written since open
 * @param bytes_written   Out: bytes appended since open
 * @param fd              Out: file descriptor (-1 if closed)
 */
void vtx_patch_log_stats(const vtx_patch_log_t *log,
                           uint32_t *entry_count,
                           uint64_t *bytes_written,
                           int *fd);

/* ========================================================================== */
/* File path helper                                                            */
/* ========================================================================== */

/**
 * Build the patch log filename for a given bytecode hash.
 *
 * Format: <dir>/<hash_hex>.vpl
 */
int vtx_patch_log_filename(const char *dir,
                             const char *hash_hex,
                             char *out,
                             size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* VORTEX_PROFILE_PATCH_LOG_H */
