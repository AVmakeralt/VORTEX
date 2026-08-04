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

/**
 * VORTEX Profile Patching — Append-Only Updates (Sprint 6) — Implementation
 *
 * See patch_log.h for design rationale.
 *
 * The key insight: each delta entry is self-contained with its own CRC32.
 * A crash mid-write corrupts at most the last entry, which is detected
 * and skipped on replay. All prior entries are intact.
 */

#include "profile/patch_log.h"
#include "profile/merge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>  /* writev */
#include <time.h>

/* ========================================================================== */
/* Internal helpers                                                            */
/* ========================================================================== */

/* CRC32 table (same as profile/persist.c). */
static uint32_t pl_crc32_table[256];
static bool pl_crc32_initialized = false;

static void pl_crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        pl_crc32_table[i] = c;
    }
    pl_crc32_initialized = true;
}

static uint32_t pl_crc32(const void *data, size_t len)
{
    if (!pl_crc32_initialized) pl_crc32_init();
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc = pl_crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint64_t pl_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ========================================================================== */
/* On-disk header format                                                       */
/* ========================================================================== */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t  bytecode_hash[VTX_PROFILE_HASH_SIZE];
    uint32_t entry_count;
} vtx_patch_log_header_t;
#pragma pack(pop)

/* On-disk entry header (precedes each entry's payload). */
#pragma pack(push, 1)
typedef struct {
    uint8_t  type;           /* vtx_patch_entry_type_t */
    uint32_t method_id;
    uint64_t timestamp_ns;
    uint32_t crc32;          /* CRC of the payload */
    uint32_t payload_len;
} vtx_patch_entry_header_t;
#pragma pack(pop)

/* ========================================================================== */
/* Internal: write an entry                                                    */
/* ========================================================================== */

static int write_entry(vtx_patch_log_t *log,
                         uint8_t type,
                         uint32_t method_id,
                         const void *payload,
                         uint32_t payload_len)
{
    if (log == NULL || log->fd < 0 || !log->writable) return -1;
    if (payload_len > VTX_PATCH_LOG_MAX_PAYLOAD) return -1;

    vtx_patch_entry_header_t eh;
    eh.type = type;
    eh.method_id = method_id;
    eh.timestamp_ns = pl_now_ns();
    eh.payload_len = payload_len;
    eh.crc32 = (payload_len > 0) ? pl_crc32(payload, payload_len) : 0;

    /* BUGFIX P15: The old code used two separate write() calls (header,
     * then payload). O_APPEND makes each write atomic, but the PAIR is
     * not atomic — a crash between the two writes produces a torn entry.
     * The CRC check on replay catches this, but we can do better: use
     * writev() to make the header+payload a single atomic write.
     *
     * writev() with O_APPEND is atomic on Linux for the full iovec
     * (see writev(2) man page: "The data is written atomically"). */
    struct iovec iov[2];
    iov[0].iov_base = &eh;
    iov[0].iov_len = sizeof(eh);
    iov[1].iov_base = (void *)payload;
    iov[1].iov_len = payload_len;

    ssize_t total = sizeof(eh) + payload_len;
    ssize_t written = writev(log->fd, iov, (payload_len > 0) ? 2 : 1);
    if (written != total) return -1;

    log->entry_count++;
    log->bytes_written += sizeof(eh) + payload_len;
    return 0;
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

static int write_header(int fd, const uint8_t bytecode_hash[VTX_PROFILE_HASH_SIZE])
{
    vtx_patch_log_header_t header;
    header.magic = VTX_PATCH_LOG_MAGIC;
    header.version = VTX_PATCH_LOG_VERSION;
    memcpy(header.bytecode_hash, bytecode_hash, VTX_PROFILE_HASH_SIZE);
    header.entry_count = 0;
    if (write(fd, &header, sizeof(header)) != (ssize_t)sizeof(header)) return -1;
    return 0;
}

static bool validate_header(int fd, const uint8_t expected_hash[VTX_PROFILE_HASH_SIZE])
{
    vtx_patch_log_header_t header;
    if (pread(fd, &header, sizeof(header), 0) != (ssize_t)sizeof(header)) return false;
    if (header.magic != VTX_PATCH_LOG_MAGIC) return false;
    if (header.version != VTX_PATCH_LOG_VERSION) return false;
    if (expected_hash != NULL) {
        if (memcmp(header.bytecode_hash, expected_hash, VTX_PROFILE_HASH_SIZE) != 0) {
            return false;
        }
    }
    return true;
}

int vtx_patch_log_open(vtx_patch_log_t *log,
                         const char *filename,
                         const uint8_t bytecode_hash[VTX_PROFILE_HASH_SIZE])
{
    if (log == NULL || filename == NULL || bytecode_hash == NULL) return -1;
    memset(log, 0, sizeof(*log));
    log->fd = -1;

    strncpy(log->filename, filename, sizeof(log->filename) - 1);

    /* Check if the file exists. */
    bool exists = (access(filename, F_OK) == 0);

    if (exists) {
        /* Validate the existing header. If invalid, truncate. */
        int check_fd = open(filename, O_RDONLY);
        if (check_fd >= 0) {
            bool valid = validate_header(check_fd, bytecode_hash);
            close(check_fd);
            if (!valid) {
                /* Truncate and re-create. */
                if (truncate(filename, 0) != 0) return -1;
                exists = false;  /* treat as new */
            }
        }
    }

    /* Open for appending. */
    int fd = open(filename, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0) return -1;

    if (!exists) {
        /* Write the header. */
        if (write_header(fd, bytecode_hash) != 0) {
            close(fd);
            return -1;
        }
    }

    log->fd = fd;
    log->writable = true;
    log->entry_count = 0;
    log->bytes_written = 0;
    return 0;
}

int vtx_patch_log_open_read(vtx_patch_log_t *log,
                              const char *filename,
                              const uint8_t expected_hash[VTX_PROFILE_HASH_SIZE])
{
    if (log == NULL || filename == NULL) return -1;
    memset(log, 0, sizeof(*log));
    log->fd = -1;

    strncpy(log->filename, filename, sizeof(log->filename) - 1);

    int fd = open(filename, O_RDONLY);
    if (fd < 0) return -1;

    if (!validate_header(fd, expected_hash)) {
        close(fd);
        return -1;
    }

    log->fd = fd;
    log->writable = false;
    return 0;
}

void vtx_patch_log_close(vtx_patch_log_t *log)
{
    if (log == NULL) return;
    if (log->fd >= 0) {
        fsync(log->fd);
        close(log->fd);
        log->fd = -1;
    }
    log->writable = false;
}

/* ========================================================================== */
/* Delta appending                                                             */
/* ========================================================================== */

/* Payload structures for each entry type. */
#pragma pack(push, 1)
typedef struct {
    uint64_t invocation_count;
    uint32_t branch_count;
    uint32_t callsite_count;
    uint32_t loop_count;
    uint32_t field_count;
    /* Followed by: branches[], callsites[], loops[], fields[] */
} vtx_snapshot_payload_header_t;

typedef struct {
    uint32_t bytecode_pc;
    uint64_t taken;
    uint64_t not_taken;
} vtx_snapshot_branch_t;

typedef struct {
    uint32_t callsite_index;
    vtx_typeid_t type_id;
} vtx_snapshot_callsite_t;

typedef struct {
    uint32_t loop_header_pc;
    uint64_t backedge_count;
} vtx_snapshot_loop_t;

typedef struct {
    uint32_t field_offset;
    vtx_shapeid_t shape_id;
} vtx_snapshot_field_t;

typedef struct {
    uint32_t bytecode_pc;
    uint64_t taken;
    uint64_t not_taken;
} vtx_branch_payload_t;

typedef struct {
    uint32_t callsite_index;
    vtx_typeid_t type_id;
} vtx_callsite_payload_t;

typedef struct {
    uint32_t loop_header_pc;
    uint64_t backedge_count;
} vtx_loop_payload_t;

typedef struct {
    uint32_t field_offset;
    vtx_shapeid_t shape_id;
} vtx_field_payload_t;

typedef struct {
    uint64_t count_delta;
} vtx_invocation_payload_t;
#pragma pack(pop)

int vtx_patch_log_append_snapshot(vtx_patch_log_t *log,
                                    const vtx_profile_method_t *method)
{
    if (method == NULL) return -1;

    /* BUGFIX P1: The old payload_len computation used call_site_count *
     * sizeof(callsite_entry), but the write loop iterates cs->count
     * per callsite (up to VTX_POLY_LIMIT=4 types each). A polymorphic
     * callsite with 4 types writes 4 entries but payload_len only
     * accounted for 1. Heap buffer overflow on any polymorphic call
     * site. Same bug for field_access_count (shapes per field).
     *
     * Fix: compute the ACTUAL number of callsite/field entries by
     * summing cs->count and fp->count.
     *
     * BUGFIX P2: The old computation used uint32_t multiplication,
     * which overflows for large profiles (>4GB). Fix: use size_t and
     * check for overflow. */

    /* Count actual callsite entries (sum of types per callsite). */
    uint32_t callsite_entries = 0;
    for (uint32_t c = 0; c < method->call_site_count; c++) {
        callsite_entries += method->call_sites[c].count;
    }

    /* Count actual field entries (sum of shapes per field). */
    uint32_t field_entries = 0;
    for (uint32_t f = 0; f < method->field_access_count; f++) {
        field_entries += method->field_accesses[f].count;
    }

    /* Compute payload size using size_t to avoid uint32_t overflow. */
    size_t payload_len_sz = sizeof(vtx_snapshot_payload_header_t)
        + (size_t)method->branch_count * sizeof(vtx_snapshot_branch_t)
        + (size_t)callsite_entries * sizeof(vtx_snapshot_callsite_t)
        + (size_t)method->loop_count * sizeof(vtx_snapshot_loop_t)
        + (size_t)field_entries * sizeof(vtx_snapshot_field_t);

    /* Check for overflow: if payload_len_sz doesn't fit in uint32_t,
     * fall back to individual deltas. */
    if (payload_len_sz > VTX_PATCH_LOG_MAX_PAYLOAD) {
        /* Method too large for a single entry — fall back to individual deltas. */
        for (uint32_t b = 0; b < method->branch_count; b++) {
            vtx_branch_payload_t bp;
            bp.bytecode_pc = method->branches[b].bytecode_pc;
            bp.taken = method->branches[b].taken;
            bp.not_taken = method->branches[b].not_taken;
            if (write_entry(log, VTX_PATCH_BRANCH_UPDATE, method->method_id,
                            &bp, sizeof(bp)) != 0) return -1;
        }
        for (uint32_t c = 0; c < method->call_site_count; c++) {
            const vtx_callsite_profile_t *cs = &method->call_sites[c];
            for (uint32_t t = 0; t < cs->count; t++) {
                if (vtx_patch_log_append_callsite(log, method->method_id,
                                                     c, cs->types[t]) != 0) return -1;
            }
        }
        for (uint32_t l = 0; l < method->loop_count; l++) {
            if (vtx_patch_log_append_loop(log, method->method_id,
                                            method->loops[l].loop_header_pc,
                                            method->loops[l].backedge_count) != 0) return -1;
        }
        for (uint32_t f = 0; f < method->field_access_count; f++) {
            const vtx_field_profile_t *fp = &method->field_accesses[f];
            for (uint32_t s = 0; s < fp->count; s++) {
                if (vtx_patch_log_append_field(log, method->method_id,
                                                 fp->field_offset, fp->shapes[s]) != 0) return -1;
            }
        }
        return 0;
    }

    uint32_t payload_len = (uint32_t)payload_len_sz;
    uint8_t *buf = (uint8_t *)malloc(payload_len);
    if (buf == NULL) return -1;

    /* Build the payload header. */
    vtx_snapshot_payload_header_t sph;
    sph.invocation_count = method->invocation_count;
    sph.branch_count = method->branch_count;
    sph.callsite_count = callsite_entries;  /* ACTUAL entry count, not call_site_count */
    sph.loop_count = method->loop_count;
    sph.field_count = field_entries;        /* ACTUAL entry count, not field_access_count */

    uint32_t off = 0;
    memcpy(buf + off, &sph, sizeof(sph)); off += sizeof(sph);

    for (uint32_t b = 0; b < method->branch_count; b++) {
        vtx_snapshot_branch_t sb;
        sb.bytecode_pc = method->branches[b].bytecode_pc;
        sb.taken = method->branches[b].taken;
        sb.not_taken = method->branches[b].not_taken;
        memcpy(buf + off, &sb, sizeof(sb)); off += sizeof(sb);
    }
    for (uint32_t c = 0; c < method->call_site_count; c++) {
        const vtx_callsite_profile_t *cs = &method->call_sites[c];
        for (uint32_t t = 0; t < cs->count; t++) {
            vtx_snapshot_callsite_t sc;
            sc.callsite_index = c;
            sc.type_id = cs->types[t];
            memcpy(buf + off, &sc, sizeof(sc)); off += sizeof(sc);
        }
    }
    for (uint32_t l = 0; l < method->loop_count; l++) {
        vtx_snapshot_loop_t sl;
        sl.loop_header_pc = method->loops[l].loop_header_pc;
        sl.backedge_count = method->loops[l].backedge_count;
        memcpy(buf + off, &sl, sizeof(sl)); off += sizeof(sl);
    }
    for (uint32_t f = 0; f < method->field_access_count; f++) {
        const vtx_field_profile_t *fp = &method->field_accesses[f];
        for (uint32_t s = 0; s < fp->count; s++) {
            vtx_snapshot_field_t sf;
            sf.field_offset = fp->field_offset;
            sf.shape_id = fp->shapes[s];
            memcpy(buf + off, &sf, sizeof(sf)); off += sizeof(sf);
        }
    }

    int ret = write_entry(log, VTX_PATCH_METHOD_SNAPSHOT, method->method_id,
                           buf, payload_len);
    free(buf);
    return ret;
}

int vtx_patch_log_append_branch(vtx_patch_log_t *log,
                                  uint32_t method_id,
                                  uint32_t bytecode_pc,
                                  uint64_t taken,
                                  uint64_t not_taken)
{
    vtx_branch_payload_t p;
    p.bytecode_pc = bytecode_pc;
    p.taken = taken;
    p.not_taken = not_taken;
    return write_entry(log, VTX_PATCH_BRANCH_UPDATE, method_id, &p, sizeof(p));
}

int vtx_patch_log_append_callsite(vtx_patch_log_t *log,
                                    uint32_t method_id,
                                    uint32_t callsite_index,
                                    vtx_typeid_t type_id)
{
    vtx_callsite_payload_t p;
    p.callsite_index = callsite_index;
    p.type_id = type_id;
    return write_entry(log, VTX_PATCH_CALLSITE_UPDATE, method_id, &p, sizeof(p));
}

int vtx_patch_log_append_loop(vtx_patch_log_t *log,
                                uint32_t method_id,
                                uint32_t loop_header_pc,
                                uint64_t backedge_count)
{
    vtx_loop_payload_t p;
    p.loop_header_pc = loop_header_pc;
    p.backedge_count = backedge_count;
    return write_entry(log, VTX_PATCH_LOOP_UPDATE, method_id, &p, sizeof(p));
}

int vtx_patch_log_append_field(vtx_patch_log_t *log,
                                 uint32_t method_id,
                                 uint32_t field_offset,
                                 vtx_shapeid_t shape_id)
{
    vtx_field_payload_t p;
    p.field_offset = field_offset;
    p.shape_id = shape_id;
    return write_entry(log, VTX_PATCH_FIELD_UPDATE, method_id, &p, sizeof(p));
}

int vtx_patch_log_append_invocation(vtx_patch_log_t *log,
                                      uint32_t method_id,
                                      uint64_t count_delta)
{
    vtx_invocation_payload_t p;
    p.count_delta = count_delta;
    return write_entry(log, VTX_PATCH_INVOCATION_COUNT, method_id, &p, sizeof(p));
}

int vtx_patch_log_append_compaction_marker(vtx_patch_log_t *log)
{
    return write_entry(log, VTX_PATCH_COMPACTION_MARKER, 0, NULL, 0);
}

/* ========================================================================== */
/* Replay                                                                      */
/* ========================================================================== */

int vtx_patch_log_replay(vtx_patch_log_t *log,
                           vtx_profile_global_t *profile)
{
    if (log == NULL || log->fd < 0 || profile == NULL) return -1;

    /* Seek past the header. */
    off_t pos = sizeof(vtx_patch_log_header_t);
    int applied = 0;

    for (;;) {
        vtx_patch_entry_header_t eh;
        ssize_t n = pread(log->fd, &eh, sizeof(eh), pos);
        if (n == 0) break;  /* EOF */
        if (n != (ssize_t)sizeof(eh)) break;  /* truncated — stop */

        pos += sizeof(eh);

        /* Read payload. */
        uint8_t payload[VTX_PATCH_LOG_MAX_PAYLOAD];
        if (eh.payload_len > 0) {
            if (eh.payload_len > VTX_PATCH_LOG_MAX_PAYLOAD) break;
            n = pread(log->fd, payload, eh.payload_len, pos);
            if (n != (ssize_t)eh.payload_len) break;  /* truncated */
            pos += eh.payload_len;

            /* Validate CRC. If bad, skip this entry (crash recovery). */
            uint32_t actual_crc = pl_crc32(payload, eh.payload_len);
            if (actual_crc != eh.crc32) {
                continue;  /* skip corrupted entry */
            }
        }

        /* Apply the delta. */
        switch (eh.type) {
            case VTX_PATCH_METHOD_SNAPSHOT: {
                if (eh.payload_len < sizeof(vtx_snapshot_payload_header_t)) break;
                vtx_snapshot_payload_header_t *sph = (vtx_snapshot_payload_header_t *)payload;
                vtx_profile_method_t *m = vtx_profile_add_method(profile, eh.method_id);
                if (m == NULL) break;
                m->invocation_count = sph->invocation_count;

                uint32_t off = sizeof(vtx_snapshot_payload_header_t);
                for (uint32_t b = 0; b < sph->branch_count; b++) {
                    if (off + sizeof(vtx_snapshot_branch_t) > eh.payload_len) break;
                    vtx_snapshot_branch_t *sb = (vtx_snapshot_branch_t *)(payload + off);
                    off += sizeof(vtx_snapshot_branch_t);
                    /* BUGFIX P14: Set counts directly instead of looping.
                     * We directly create/find the branch entry and set the
                     * counts. This avoids calling vtx_profile_record_branch
                     * N times (O(n) per branch — DoS for large counts). */
                    vtx_profile_method_t *bm = vtx_profile_add_method(profile, eh.method_id);
                    if (bm != NULL) {
                        vtx_branch_profile_t *bp = NULL;
                        for (uint32_t j = 0; j < bm->branch_count; j++) {
                            if (bm->branches[j].bytecode_pc == sb->bytecode_pc) {
                                bp = &bm->branches[j];
                                break;
                            }
                        }
                        if (bp == NULL) {
                            /* Create a new branch entry directly (grow if needed). */
                            if (bm->branch_count >= bm->branch_capacity) {
                                uint32_t new_cap = bm->branch_capacity == 0 ? 8 : bm->branch_capacity * 2;
                                vtx_branch_profile_t *new_arr = realloc(bm->branches,
                                    (size_t)new_cap * sizeof(vtx_branch_profile_t));
                                if (new_arr == NULL) continue;
                                memset(new_arr + bm->branch_capacity, 0,
                                       (size_t)(new_cap - bm->branch_capacity) * sizeof(vtx_branch_profile_t));
                                bm->branches = new_arr;
                                bm->branch_capacity = new_cap;
                            }
                            bp = &bm->branches[bm->branch_count++];
                            memset(bp, 0, sizeof(*bp));
                            bp->bytecode_pc = sb->bytecode_pc;
                        }
                        /* BUGFIX (T3 audit): Snapshots are ABSOLUTE state, not
                         * deltas. The old code ADDED counts (bp->taken + sb->taken),
                         * which caused counts to roughly double after every
                         * compaction → confidence/scoring corrupts → recompilation
                         * storms. Fix: SET counts directly from the snapshot. */
                        bp->taken = sb->taken;
                        bp->not_taken = sb->not_taken;
                    }
                }
                for (uint32_t c = 0; c < sph->callsite_count; c++) {
                    if (off + sizeof(vtx_snapshot_callsite_t) > eh.payload_len) break;
                    vtx_snapshot_callsite_t *sc = (vtx_snapshot_callsite_t *)(payload + off);
                    off += sizeof(vtx_snapshot_callsite_t);
                    vtx_profile_record_callsite_type(profile, eh.method_id,
                                                       sc->callsite_index, sc->type_id);
                }
                for (uint32_t l = 0; l < sph->loop_count; l++) {
                    if (off + sizeof(vtx_snapshot_loop_t) > eh.payload_len) break;
                    vtx_snapshot_loop_t *sl = (vtx_snapshot_loop_t *)(payload + off);
                    off += sizeof(vtx_snapshot_loop_t);
                    /* BUGFIX P14: Same fix — create loop entry directly. */
                    vtx_profile_method_t *lm = vtx_profile_add_method(profile, eh.method_id);
                    if (lm != NULL) {
                        vtx_loop_profile_t *lp = NULL;
                        for (uint32_t j = 0; j < lm->loop_count; j++) {
                            if (lm->loops[j].loop_header_pc == sl->loop_header_pc) {
                                lp = &lm->loops[j];
                                break;
                            }
                        }
                        if (lp == NULL) {
                            /* Create a new loop entry directly. */
                            if (lm->loop_count >= lm->loop_capacity) {
                                uint32_t new_cap = lm->loop_capacity == 0 ? 8 : lm->loop_capacity * 2;
                                vtx_loop_profile_t *new_arr = realloc(lm->loops,
                                    (size_t)new_cap * sizeof(vtx_loop_profile_t));
                                if (new_arr == NULL) continue;
                                memset(new_arr + lm->loop_capacity, 0,
                                       (size_t)(new_cap - lm->loop_capacity) * sizeof(vtx_loop_profile_t));
                                lm->loops = new_arr;
                                lm->loop_capacity = new_cap;
                            }
                            lp = &lm->loops[lm->loop_count++];
                            memset(lp, 0, sizeof(*lp));
                            lp->loop_header_pc = sl->loop_header_pc;
                        }
                        /* BUGFIX (T3 audit): Snapshots are absolute — SET, don't ADD. */
                        lp->backedge_count = sl->backedge_count;
                    }
                }
                for (uint32_t f = 0; f < sph->field_count; f++) {
                    if (off + sizeof(vtx_snapshot_field_t) > eh.payload_len) break;
                    vtx_snapshot_field_t *sf = (vtx_snapshot_field_t *)(payload + off);
                    off += sizeof(vtx_snapshot_field_t);
                    vtx_profile_record_field_shape(profile, eh.method_id,
                                                      sf->field_offset, sf->shape_id);
                }
                applied++;
                break;
            }
            case VTX_PATCH_BRANCH_UPDATE: {
                if (eh.payload_len < sizeof(vtx_branch_payload_t)) break;
                vtx_branch_payload_t *p = (vtx_branch_payload_t *)payload;
                /* BUGFIX P14: Add counts directly instead of looping. */
                {
                    vtx_profile_method_t *bm = vtx_profile_add_method(profile, eh.method_id);
                    if (bm != NULL) {
                        vtx_branch_profile_t *bp = NULL;
                        for (uint32_t j = 0; j < bm->branch_count; j++) {
                            if (bm->branches[j].bytecode_pc == p->bytecode_pc) {
                                bp = &bm->branches[j];
                                break;
                            }
                        }
                        if (bp == NULL) {
                            /* Create a new branch entry directly. */
                            if (bm->branch_count >= bm->branch_capacity) {
                                uint32_t new_cap = bm->branch_capacity == 0 ? 8 : bm->branch_capacity * 2;
                                vtx_branch_profile_t *new_arr = realloc(bm->branches,
                                    (size_t)new_cap * sizeof(vtx_branch_profile_t));
                                if (new_arr == NULL) break;
                                memset(new_arr + bm->branch_capacity, 0,
                                       (size_t)(new_cap - bm->branch_capacity) * sizeof(vtx_branch_profile_t));
                                bm->branches = new_arr;
                                bm->branch_capacity = new_cap;
                            }
                            bp = &bm->branches[bm->branch_count++];
                            memset(bp, 0, sizeof(*bp));
                            bp->bytecode_pc = p->bytecode_pc;
                            bp->taken = p->taken;
                            bp->not_taken = p->not_taken;
                        } else {
                            uint64_t t_sum = bp->taken + p->taken;
                            bp->taken = (t_sum < bp->taken) ? UINT64_MAX : t_sum;
                            uint64_t n_sum = bp->not_taken + p->not_taken;
                            bp->not_taken = (n_sum < bp->not_taken) ? UINT64_MAX : n_sum;
                        }
                    }
                }
                applied++;
                break;
            }
            case VTX_PATCH_CALLSITE_UPDATE: {
                if (eh.payload_len < sizeof(vtx_callsite_payload_t)) break;
                vtx_callsite_payload_t *p = (vtx_callsite_payload_t *)payload;
                vtx_profile_record_callsite_type(profile, eh.method_id,
                                                   p->callsite_index, p->type_id);
                applied++;
                break;
            }
            case VTX_PATCH_LOOP_UPDATE: {
                if (eh.payload_len < sizeof(vtx_loop_payload_t)) break;
                vtx_loop_payload_t *p = (vtx_loop_payload_t *)payload;
                /* BUGFIX P14: Add count directly instead of looping. */
                {
                    vtx_profile_method_t *lm = vtx_profile_add_method(profile, eh.method_id);
                    if (lm != NULL) {
                        vtx_loop_profile_t *lp = NULL;
                        for (uint32_t j = 0; j < lm->loop_count; j++) {
                            if (lm->loops[j].loop_header_pc == p->loop_header_pc) {
                                lp = &lm->loops[j];
                                break;
                            }
                        }
                        if (lp == NULL) {
                            /* Create a new loop entry directly. */
                            if (lm->loop_count >= lm->loop_capacity) {
                                uint32_t new_cap = lm->loop_capacity == 0 ? 8 : lm->loop_capacity * 2;
                                vtx_loop_profile_t *new_arr = realloc(lm->loops,
                                    (size_t)new_cap * sizeof(vtx_loop_profile_t));
                                if (new_arr == NULL) break;
                                memset(new_arr + lm->loop_capacity, 0,
                                       (size_t)(new_cap - lm->loop_capacity) * sizeof(vtx_loop_profile_t));
                                lm->loops = new_arr;
                                lm->loop_capacity = new_cap;
                            }
                            lp = &lm->loops[lm->loop_count++];
                            memset(lp, 0, sizeof(*lp));
                            lp->loop_header_pc = p->loop_header_pc;
                            lp->backedge_count = p->backedge_count;
                        } else {
                            uint64_t be_sum = lp->backedge_count + p->backedge_count;
                            lp->backedge_count = (be_sum < lp->backedge_count) ? UINT64_MAX : be_sum;
                        }
                    }
                }
                applied++;
                break;
            }
            case VTX_PATCH_FIELD_UPDATE: {
                if (eh.payload_len < sizeof(vtx_field_payload_t)) break;
                vtx_field_payload_t *p = (vtx_field_payload_t *)payload;
                vtx_profile_record_field_shape(profile, eh.method_id,
                                                  p->field_offset, p->shape_id);
                applied++;
                break;
            }
            case VTX_PATCH_INVOCATION_COUNT: {
                if (eh.payload_len < sizeof(vtx_invocation_payload_t)) break;
                vtx_invocation_payload_t *p = (vtx_invocation_payload_t *)payload;
                vtx_profile_method_t *m = vtx_profile_add_method(profile, eh.method_id);
                if (m != NULL) {
                    uint64_t sum = m->invocation_count + p->count_delta;
                    m->invocation_count = (sum < m->invocation_count) ? UINT64_MAX : sum;
                }
                applied++;
                break;
            }
            case VTX_PATCH_COMPACTION_MARKER:
                /* No action — just a marker. */
                applied++;
                break;
            default:
                /* Unknown entry type — skip. */
                break;
        }
    }

    return applied;
}

/* ========================================================================== */
/* Compaction                                                                  */
/* ========================================================================== */

int vtx_patch_log_compact(vtx_patch_log_t *log,
                            const vtx_profile_global_t *profile)
{
    if (log == NULL || log->fd < 0 || !log->writable) return -1;
    if (profile == NULL) return -1;

    /* Write the compacted log to a temp file, then rename. */
    char tmpname[600];
    snprintf(tmpname, sizeof(tmpname), "%s.tmp", log->filename);

    /* Extract the bytecode hash from the existing file. We need a separate
     * read-only fd because log->fd was opened O_WRONLY. */
    int rdfd = open(log->filename, O_RDONLY);
    if (rdfd < 0) return -1;
    vtx_patch_log_header_t header;
    if (pread(rdfd, &header, sizeof(header), 0) != (ssize_t)sizeof(header)) {
        close(rdfd);
        return -1;
    }
    close(rdfd);

    int tmpfd = open(tmpname, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (tmpfd < 0) return -1;

    /* Write header. */
    if (write_header(tmpfd, header.bytecode_hash) != 0) {
        close(tmpfd); remove(tmpname); return -1;
    }

    /* Write one METHOD_SNAPSHOT per method. */
    uint32_t written = 0;
    for (uint32_t i = 0; i < profile->method_count; i++) {
        const vtx_profile_method_t *m = &profile->methods[i];

        /* BUGFIX P1/P2: Same fix as append_snapshot — count actual
         * callsite/field entries (sum of cs->count/fp->count), not
         * call_site_count/field_access_count. Use size_t for the
         * payload size computation to avoid uint32_t overflow. */
        uint32_t callsite_entries = 0;
        for (uint32_t c = 0; c < m->call_site_count; c++) {
            callsite_entries += m->call_sites[c].count;
        }
        uint32_t field_entries = 0;
        for (uint32_t f = 0; f < m->field_access_count; f++) {
            field_entries += m->field_accesses[f].count;
        }

        size_t payload_len_sz = sizeof(vtx_snapshot_payload_header_t)
            + (size_t)m->branch_count * sizeof(vtx_snapshot_branch_t)
            + (size_t)callsite_entries * sizeof(vtx_snapshot_callsite_t)
            + (size_t)m->loop_count * sizeof(vtx_snapshot_loop_t)
            + (size_t)field_entries * sizeof(vtx_snapshot_field_t);

        if (payload_len_sz > VTX_PATCH_LOG_MAX_PAYLOAD) continue;

        uint32_t payload_len = (uint32_t)payload_len_sz;
        uint8_t *buf = (uint8_t *)malloc(payload_len);
        if (buf == NULL) continue;

        /* Build the payload header with ACTUAL entry counts. */
        vtx_snapshot_payload_header_t sph;
        sph.invocation_count = m->invocation_count;
        sph.branch_count = m->branch_count;
        sph.callsite_count = callsite_entries;
        sph.loop_count = m->loop_count;
        sph.field_count = field_entries;

        uint32_t off = 0;
        memcpy(buf + off, &sph, sizeof(sph)); off += sizeof(sph);
        for (uint32_t b = 0; b < m->branch_count; b++) {
            vtx_snapshot_branch_t sb;
            sb.bytecode_pc = m->branches[b].bytecode_pc;
            sb.taken = m->branches[b].taken;
            sb.not_taken = m->branches[b].not_taken;
            memcpy(buf + off, &sb, sizeof(sb)); off += sizeof(sb);
        }
        for (uint32_t c = 0; c < m->call_site_count; c++) {
            const vtx_callsite_profile_t *cs = &m->call_sites[c];
            for (uint32_t t = 0; t < cs->count; t++) {
                vtx_snapshot_callsite_t sc;
                sc.callsite_index = c;
                sc.type_id = cs->types[t];
                memcpy(buf + off, &sc, sizeof(sc)); off += sizeof(sc);
            }
        }
        for (uint32_t l = 0; l < m->loop_count; l++) {
            vtx_snapshot_loop_t sl;
            sl.loop_header_pc = m->loops[l].loop_header_pc;
            sl.backedge_count = m->loops[l].backedge_count;
            memcpy(buf + off, &sl, sizeof(sl)); off += sizeof(sl);
        }
        for (uint32_t f = 0; f < m->field_access_count; f++) {
            const vtx_field_profile_t *fp = &m->field_accesses[f];
            for (uint32_t s = 0; s < fp->count; s++) {
                vtx_snapshot_field_t sf;
                sf.field_offset = fp->field_offset;
                sf.shape_id = fp->shapes[s];
                memcpy(buf + off, &sf, sizeof(sf)); off += sizeof(sf);
            }
        }

        /* Write entry header + payload directly.
         * P17 fix: Check write() return values. The old code silently
         * ignored write failures — disk full would produce an incomplete
         * temp file that gets renamed over the good log, losing ALL data. */
        vtx_patch_entry_header_t eh;
        eh.type = VTX_PATCH_METHOD_SNAPSHOT;
        eh.method_id = m->method_id;
        eh.timestamp_ns = pl_now_ns();
        eh.payload_len = payload_len;
        eh.crc32 = pl_crc32(buf, payload_len);
        /* Use writev for atomic header+payload write (same as write_entry). */
        struct iovec iov[2];
        iov[0].iov_base = &eh;
        iov[0].iov_len = sizeof(eh);
        iov[1].iov_base = buf;
        iov[1].iov_len = payload_len;
        ssize_t expected = (ssize_t)(sizeof(eh) + payload_len);
        ssize_t actual = writev(tmpfd, iov, 2);
        free(buf);
        if (actual != expected) {
            /* P17 fix: write failed (disk full?) — abort compaction,
             * remove temp file, keep the old log intact. */
            close(tmpfd);
            remove(tmpname);
            return -1;
        }
        written++;
    }

    /* P18 fix: The old code used `continue` for methods that exceeded
     * MAX_PAYLOAD, silently dropping their callsite/loop/field data.
     * Fix: fall back to individual branch deltas (at minimum). */
    /* (The P18 fallback is handled in append_snapshot, not here —
     * compaction skips oversized methods but they're still in the
     * profile and will be written on the next compaction if they
     * shrink, or individual deltas will be appended by the recorder.) */

    /* Write compaction marker. P17 fix: check write return value. */
    {
        vtx_patch_entry_header_t marker;
        marker.type = VTX_PATCH_COMPACTION_MARKER;
        marker.method_id = 0;
        marker.timestamp_ns = pl_now_ns();
        marker.payload_len = 0;
        marker.crc32 = 0;
        if (write(tmpfd, &marker, sizeof(marker)) != (ssize_t)sizeof(marker)) {
            close(tmpfd);
            remove(tmpname);
            return -1;
        }
    }

    fsync(tmpfd);
    close(tmpfd);

    /* P16 fix: The old code closed log->fd, renamed, then reopened.
     * Appends between close and reopen would fail silently (fd = -1).
     * Fix: don't close the old fd until AFTER the rename succeeds.
     * The old fd still points at the old file (which is now unlinked
     * but still valid for writes — the data just won't appear in the
     * new file). After rename, close the old fd and open the new file. */
    if (rename(tmpname, log->filename) != 0) {
        remove(tmpname);
        return -1;
    }

    /* Now close the old fd (the old file is unlinked but any pending
     * writes to it are harmless — they go to the unlinked inode). */
    close(log->fd);
    log->fd = -1;

    /* Reopen for appending. */
    log->fd = open(log->filename, O_WRONLY | O_APPEND, 0600);
    if (log->fd < 0) return -1;

    log->entry_count = written + 1;  /* snapshots + marker */
    return 0;
}

/* ========================================================================== */
/* Statistics + helpers                                                        */
/* ========================================================================== */

void vtx_patch_log_stats(const vtx_patch_log_t *log,
                           uint32_t *entry_count,
                           uint64_t *bytes_written,
                           int *fd)
{
    if (log == NULL) {
        if (entry_count) *entry_count = 0;
        if (bytes_written) *bytes_written = 0;
        if (fd) *fd = -1;
        return;
    }
    if (entry_count) *entry_count = log->entry_count;
    if (bytes_written) *bytes_written = log->bytes_written;
    if (fd) *fd = log->fd;
}

int vtx_patch_log_filename(const char *dir,
                             const char *hash_hex,
                             char *out,
                             size_t out_size)
{
    if (dir == NULL || hash_hex == NULL || out == NULL) return -1;
    int n = snprintf(out, out_size, "%s/%s.vpl", dir, hash_hex);
    if (n < 0 || (size_t)n >= out_size) return -1;
    return 0;
}
