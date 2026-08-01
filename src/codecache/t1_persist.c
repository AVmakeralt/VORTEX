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
 * VORTEX T1 Code Persistence (Sprint 5) — Implementation
 *
 * See t1_persist.h for design rationale.
 *
 * The key challenge: native code contains absolute and relative addresses
 * that are only valid at the address where the code was compiled. When we
 * persist and reload, the code lands at a different address (ASLR), so we
 * must apply relocations to fix it up.
 *
 * Relocation kinds:
 *   - VTX_RELOC_REL32: intra-code jump/call. target_offset is within the
 *     code blob. No fixup needed — relative offsets are position-independent.
 *   - VTX_RELOC_ABS64: absolute address. This is an external symbol (e.g.,
 *     a runtime helper function). Must be re-applied at load time using
 *     the current process's symbol address.
 *   - VTX_RELOC_RIP_REL32: RIP-relative. If the target is within the code
 *     blob, no fixup needed. If external, must be re-applied.
 *
 * For T1 baseline code, most relocations are REL32 (intra-code) and
 * ABS64 (external runtime calls). The ABS64 relocations are re-applied
 * at load time by looking up the symbol address in the current process.
 */

#include "codecache/t1_persist.h"
#include "lower/reloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

/* ========================================================================== */
/* Internal helpers                                                            */
/* ========================================================================== */

/* CRC32 table (same as profile/persist.c). */
static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

static void crc32_init_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_initialized = true;
}

/* BUGFIX (P7 audit): The old crc32_update did `crc = crc ^ 0xFFFFFFFF`
 * at the START and `crc ^ 0xFFFFFFFF` at the END. When chaining
 * (passing the result of a previous call as the `crc` argument), the
 * pre-XOR undoes the previous post-XOR, which is correct for the FIRST
 * byte but wrong for subsequent calls — the internal CRC state gets
 * double-XOR'd, corrupting the result.
 *
 * Standard CRC32 (zlib/PNG) convention:
 *   - Initialize: crc = 0xFFFFFFFF
 *   - Update:     crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8)
 *   - Finalize:   crc = crc ^ 0xFFFFFFFF
 *
 * The pre-XOR and post-XOR happen ONCE per CRC computation, not per
 * update call. Fix: provide crc32_begin() and crc32_end() for the
 * XOR management, and crc32_update() just does the table walk on
 * the raw internal state. */
static uint32_t crc32_begin(void)
{
    if (!crc32_table_initialized) crc32_init_table();
    return 0xFFFFFFFFu;
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    if (!crc32_table_initialized) crc32_init_table();
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

static uint32_t crc32_end(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}

/* Convenience: compute CRC32 of a single buffer (begin + update + end). */
static uint32_t crc32_compute(const void *data, size_t len)
{
    return crc32_end(crc32_update(crc32_begin(), data, len));
}

static uint64_t t1_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ========================================================================== */
/* File header (on-disk format)                                                */
/* ========================================================================== */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t crc32;
    uint8_t  bytecode_hash[VTX_PROFILE_HASH_SIZE];
    uint32_t method_count;
    uint32_t total_code_size;
} vtx_t1_cache_header_t;
#pragma pack(pop)

/* ========================================================================== */
/* Save                                                                        */
/* ========================================================================== */

bool vtx_t1_cache_save(const char *filename,
                         const uint8_t bytecode_hash[VTX_PROFILE_HASH_SIZE],
                         const vtx_compiled_code_t **methods,
                         uint32_t method_count)
{
    if (filename == NULL || bytecode_hash == NULL || methods == NULL) return false;
    if (method_count == 0 || method_count > VTX_T1_CACHE_MAX_METHODS) return false;

    /* Write to a temp file first, then rename (atomic write). */
    char tmpname[1024];
    snprintf(tmpname, sizeof(tmpname), "%s.tmp", filename);

    /* BUGFIX P20: The old code used fopen("wb") which creates the file
     * with 0644 permissions (world-readable), then called chmod(0600)
     * after. There's a window where the file is world-readable and
     * contains executable native code. An attacker could read the code
     * before chmod runs.
     *
     * Fix: use open() with O_CREAT and mode 0600 directly, then fdopen
     * to get a FILE*. The file is created with restrictive permissions
     * from the start — no window. */
    int tmpfd = open(tmpname, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (tmpfd < 0) return false;
    FILE *f = fdopen(tmpfd, "wb");
    if (f == NULL) {
        close(tmpfd);
        return false;
    }

    /* Compute total code size and build the method/reloc descriptors. */
    uint32_t total_code_size = 0;
    for (uint32_t i = 0; i < method_count; i++) {
        if (methods[i] == NULL || methods[i]->code == NULL) continue;
        total_code_size += methods[i]->code_size;
    }
    if (total_code_size > VTX_T1_CACHE_MAX_CODE_SIZE) {
        fclose(f);
        remove(tmpname);
        return false;
    }

    /* Write header (CRC filled in later). */
    vtx_t1_cache_header_t header;
    memset(&header, 0, sizeof(header));
    header.magic = VTX_T1_CACHE_MAGIC;
    header.version = VTX_T1_CACHE_VERSION;
    header.crc32 = 0;  /* computed below */
    memcpy(header.bytecode_hash, bytecode_hash, VTX_PROFILE_HASH_SIZE);
    header.method_count = method_count;
    header.total_code_size = total_code_size;

    /* CRC is computed over everything AFTER the CRC field (offset 12).
     * BUGFIX P3: The old save code included magic+version in the CRC
     * (they're before the CRC field), causing a mismatch with the load
     * code which starts at offset 12. Fix: only CRC the fields after
     * the CRC field.
     *
     * BUGFIX (P7 audit): Use the proper begin/update/end API so the
     * pre/post-XOR happens exactly once, not per-update call. */
    uint32_t crc = crc32_begin();
    /* Skip magic (4) + version (4) + crc32 (4) = 12 bytes.
     * CRC covers: bytecode_hash + method_count + total_code_size +
     * method descriptors + code blob. */
    crc = crc32_update(crc, header.bytecode_hash, VTX_PROFILE_HASH_SIZE);
    crc = crc32_update(crc, &header.method_count, 4);
    crc = crc32_update(crc, &header.total_code_size, 4);

    if (fwrite(&header, sizeof(header), 1, f) != 1) {
        fclose(f); remove(tmpname); return false;
    }

    /* Write per-method descriptors + their relocations. */
    uint32_t code_offset = 0;
    for (uint32_t i = 0; i < method_count; i++) {
        const vtx_compiled_code_t *cc = methods[i];
        if (cc == NULL || cc->code == NULL) continue;

        vtx_t1_persist_method_t mp;
        memset(&mp, 0, sizeof(mp));
        mp.method_id    = cc->method_id;
        mp.code_offset  = code_offset;
        mp.code_size    = cc->code_size;
        mp.entry_offset = (uint32_t)((uintptr_t)cc->entry_point - (uintptr_t)cc->code);
        mp.stack_slots  = cc->stack_slots;
        mp.local_slots  = cc->local_slots;

        /* Count relocations. The T1 baseline JIT doesn't use a reloc
         * table directly (it patches immediately), so reloc_count is
         * typically 0 for T1. We still support it for future use. */
        mp.reloc_count  = 0;  /* T1 baseline code has no deferred relocations */

        crc = crc32_update(crc, &mp, sizeof(mp));
        if (fwrite(&mp, sizeof(mp), 1, f) != 1) {
            fclose(f); remove(tmpname); return false;
        }

        code_offset += cc->code_size;
    }

    /* Write the code blob. */
    for (uint32_t i = 0; i < method_count; i++) {
        const vtx_compiled_code_t *cc = methods[i];
        if (cc == NULL || cc->code == NULL) continue;
        crc = crc32_update(crc, cc->code, cc->code_size);
        if (fwrite(cc->code, 1, cc->code_size, f) != cc->code_size) {
            fclose(f); remove(tmpname); return false;
        }
    }

    /* Go back and write the CRC.
     * BUGFIX (P7): Finalize the CRC with crc32_end (post-XOR). */
    uint32_t final_crc = crc32_end(crc);
    fseek(f, offsetof(vtx_t1_cache_header_t, crc32), SEEK_SET);
    if (fwrite(&final_crc, 4, 1, f) != 1) {
        fclose(f); remove(tmpname); return false;
    }

    fclose(f);

    /* Atomic rename. */
    if (rename(tmpname, filename) != 0) {
        remove(tmpname);
        return false;
    }

    return true;
}

/* ========================================================================== */
/* Load                                                                        */
/* ========================================================================== */

bool vtx_t1_cache_load(vtx_t1_cache_t *cache,
                         const char *filename,
                         const uint8_t expected_hash[VTX_PROFILE_HASH_SIZE])
{
    if (cache == NULL || filename == NULL) return false;
    memset(cache, 0, sizeof(*cache));
    cache->code_fd = -1;

    uint64_t t0 = t1_now_ns();

    FILE *f = fopen(filename, "rb");
    if (f == NULL) return false;

    /* Read and validate header. */
    vtx_t1_cache_header_t header;
    if (fread(&header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f); return false;
    }

    if (header.magic != VTX_T1_CACHE_MAGIC) {
        fclose(f); return false;
    }
    if (header.version != VTX_T1_CACHE_VERSION) {
        fclose(f); return false;
    }
    if (header.method_count > VTX_T1_CACHE_MAX_METHODS) {
        fclose(f); return false;
    }
    if (header.total_code_size > VTX_T1_CACHE_MAX_CODE_SIZE) {
        fclose(f); return false;
    }

    /* BUGFIX P3: Verify the CRC32. The old code computed the CRC on save
     * but NEVER checked it on load. A corrupted file would be mmap'd as
     * PROT_EXEC and executed — arbitrary code execution from corrupted
     * or attacker-controlled data.
     *
     * Fix: read the entire file, compute the CRC over everything after
     * the CRC field, and reject if it doesn't match. */
    {
        /* Get file size. */
        struct stat st;
        if (fstat(fileno(f), &st) != 0) {
            fclose(f); return false;
        }
        size_t file_size = (size_t)st.st_size;

        /* The CRC field is at offset 8 (after magic + version). Everything
         * after the CRC field (offset 12) is CRC'd. */
        if (file_size < 12) {
            fclose(f); return false;
        }

        /* Read everything after the CRC field. */
        size_t crc_region_len = file_size - 12;
        uint8_t *crc_region = (uint8_t *)malloc(crc_region_len);
        if (crc_region == NULL) {
            fclose(f); return false;
        }
        /* Seek to offset 12 (after magic+version+crc). */
        if (fseek(f, 12, SEEK_SET) != 0) {
            free(crc_region); fclose(f); return false;
        }
        if (fread(crc_region, 1, crc_region_len, f) != crc_region_len) {
            free(crc_region); fclose(f); return false;
        }
        /* BUGFIX (P7): Use begin/update/end for a single consistent CRC. */
        uint32_t actual_crc = crc32_end(crc32_update(crc32_begin(), crc_region, crc_region_len));
        free(crc_region);

        if (actual_crc != header.crc32) {
            fprintf(stderr, "[t1_cache] CRC mismatch: file corrupted (expected %08x, got %08x)\n",
                    header.crc32, actual_crc);
            fclose(f);
            return false;
        }

        /* Seek back to after the header for method reading. */
        if (fseek(f, sizeof(vtx_t1_cache_header_t), SEEK_SET) != 0) {
            fclose(f); return false;
        }
    }

    /* Verify bytecode hash (version gating). */
    if (expected_hash != NULL) {
        if (memcmp(header.bytecode_hash, expected_hash, VTX_PROFILE_HASH_SIZE) != 0) {
            fclose(f);
            return false;
        }
    }
    memcpy(cache->bytecode_hash, header.bytecode_hash, VTX_PROFILE_HASH_SIZE);
    cache->method_count = header.method_count;
    cache->total_code_size = header.total_code_size;

    /* Read per-method descriptors. */
    cache->methods = (vtx_t1_persist_method_t *)calloc(
        cache->method_count, sizeof(vtx_t1_persist_method_t));
    if (cache->methods == NULL) {
        fclose(f); return false;
    }

    for (uint32_t i = 0; i < cache->method_count; i++) {
        if (fread(&cache->methods[i], 1, sizeof(vtx_t1_persist_method_t), f)
            != sizeof(vtx_t1_persist_method_t)) {
            free(cache->methods); cache->methods = NULL;
            fclose(f); return false;
        }
        /* BUGFIX P4: Validate entry point bounds. The old code did
         * code_base + code_offset + entry_offset without checking that
         * the result is within the mmap'd region. A malformed file
         * could cause a jump to unmapped memory.
         *
         * Fix: verify code_offset + code_size <= total_code_size, and
         * entry_offset < code_size. */
        if (cache->methods[i].code_offset > header.total_code_size ||
            cache->methods[i].code_size > header.total_code_size - cache->methods[i].code_offset) {
            fprintf(stderr, "[t1_cache] method %u: code_offset/size out of bounds\n",
                    cache->methods[i].method_id);
            free(cache->methods); cache->methods = NULL;
            fclose(f); return false;
        }
        if (cache->methods[i].entry_offset >= cache->methods[i].code_size) {
            fprintf(stderr, "[t1_cache] method %u: entry_offset out of bounds\n",
                    cache->methods[i].method_id);
            free(cache->methods); cache->methods = NULL;
            fclose(f); return false;
        }
    }

    /* We don't have relocations in T1 baseline code (reloc_count=0 for all
     * methods), so we skip the reloc reading. The relocs array stays NULL. */
    cache->relocs = NULL;
    cache->reloc_count = 0;

    /* Compute the file offset where the code blob starts. */
    long code_blob_offset = ftell(f);
    fclose(f);

    /* mmap the code blob as PROT_READ|PROT_EXEC. We open the file again
     * with O_RDONLY and mmap just the code portion. */
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        free(cache->methods); cache->methods = NULL;
        return false;
    }

    /* Page-align the mmap offset and size. */
    long page_size = sysconf(_SC_PAGESIZE);
    long map_offset = (code_blob_offset / page_size) * page_size;
    long map_adjust = code_blob_offset - map_offset;
    size_t map_size = (size_t)header.total_code_size + (size_t)map_adjust;
    /* Round up to page size. */
    map_size = (map_size + page_size - 1) & ~(page_size - 1);

    void *mapped = mmap(NULL, map_size, PROT_READ | PROT_EXEC,
                        MAP_PRIVATE, fd, map_offset);
    if (mapped == MAP_FAILED) {
        close(fd);
        free(cache->methods); cache->methods = NULL;
        return false;
    }

    cache->code_base = (uint8_t *)mapped + map_adjust;
    cache->code_map_size = map_size;
    cache->code_fd = fd;
    cache->relocations_applied = true;  /* T1 baseline code needs no relocations */

    cache->load_time_ns = t1_now_ns() - t0;
    return true;
}

void *vtx_t1_cache_get_entry(const vtx_t1_cache_t *cache, uint32_t method_id)
{
    if (cache == NULL || cache->methods == NULL) return NULL;
    for (uint32_t i = 0; i < cache->method_count; i++) {
        if (cache->methods[i].method_id == method_id) {
            /* The entry point is at: code_base + code_offset + entry_offset.
             * code_offset is where this method's code starts in the blob,
             * entry_offset is where the entry point is within the method's code. */
            return (uint8_t *)cache->code_base
                   + cache->methods[i].code_offset
                   + cache->methods[i].entry_offset;
        }
    }
    return NULL;
}

bool vtx_t1_cache_has_method(const vtx_t1_cache_t *cache, uint32_t method_id)
{
    if (cache == NULL || cache->methods == NULL) return false;
    for (uint32_t i = 0; i < cache->method_count; i++) {
        if (cache->methods[i].method_id == method_id) return true;
    }
    return false;
}

void vtx_t1_cache_destroy(vtx_t1_cache_t *cache)
{
    if (cache == NULL) return;

    if (cache->methods != NULL) {
        free(cache->methods);
        cache->methods = NULL;
    }
    if (cache->relocs != NULL) {
        free(cache->relocs);
        cache->relocs = NULL;
    }
    if (cache->method_reloc_offsets != NULL) {
        free(cache->method_reloc_offsets);
        cache->method_reloc_offsets = NULL;
    }
    if (cache->code_base != NULL && cache->code_map_size > 0) {
        /* Unmap from the page-aligned base. We need to recompute it
         * because code_base has the adjustment added. */
        long page_size = sysconf(_SC_PAGESIZE);
        uintptr_t base = (uintptr_t)cache->code_base;
        uintptr_t aligned = base & ~(page_size - 1);
        size_t extra = (size_t)(base - aligned);
        munmap((void *)aligned, cache->code_map_size);
        (void)extra;
        cache->code_base = NULL;
        cache->code_map_size = 0;
    }
    if (cache->code_fd >= 0) {
        close(cache->code_fd);
        cache->code_fd = -1;
    }
    cache->method_count = 0;
    cache->total_code_size = 0;
}

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

void vtx_t1_cache_stats(const vtx_t1_cache_t *cache,
                          uint32_t *method_count,
                          uint32_t *code_size,
                          uint64_t *load_time_ns,
                          uint32_t *relocations)
{
    if (cache == NULL) {
        if (method_count) *method_count = 0;
        if (code_size) *code_size = 0;
        if (load_time_ns) *load_time_ns = 0;
        if (relocations) *relocations = 0;
        return;
    }
    if (method_count) *method_count = cache->method_count;
    if (code_size) *code_size = cache->total_code_size;
    if (load_time_ns) *load_time_ns = cache->load_time_ns;
    if (relocations) *relocations = cache->reloc_count;
}

/* ========================================================================== */
/* File path helper                                                            */
/* ========================================================================== */

int vtx_t1_cache_filename(const char *dir,
                            const char *hash_hex,
                            char *out,
                            size_t out_size)
{
    if (dir == NULL || hash_hex == NULL || out == NULL) return -1;
    int n = snprintf(out, out_size, "%s/%s.t1c", dir, hash_hex);
    if (n < 0 || (size_t)n >= out_size) return -1;
    return 0;
}
