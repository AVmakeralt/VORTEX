#define _POSIX_C_SOURCE 200809L
#include "profile/persist.h"
#include "profile/merge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ========================================================================== */
/* CRC32 implementation (ISO 3309 / ITU-T V.42)                              */
/* ========================================================================== */

static uint32_t crc32_table[256];
static bool     crc32_table_initialized = false;

static void crc32_init_table(void)
{
    if (crc32_table_initialized) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
        crc32_table[i] = crc;
    }
    crc32_table_initialized = true;
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    crc32_init_table();
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

/* ========================================================================== */
/* Internal: buffered writer                                                  */
/* ========================================================================== */

#define VTX_PERSIST_BUF_SIZE 8192

typedef struct {
    FILE   *fp;
    uint8_t buf[VTX_PERSIST_BUF_SIZE];
    size_t  pos;
    uint32_t crc;
    bool    compute_crc;
    bool    error;
} vtx_writer_t;

static void writer_init(vtx_writer_t *w, FILE *fp, bool compute_crc)
{
    w->fp = fp;
    w->pos = 0;
    w->crc = 0;
    w->compute_crc = compute_crc;
    w->error = false;
}

static void writer_flush(vtx_writer_t *w)
{
    if (w->pos > 0 && !w->error) {
        if (fwrite(w->buf, 1, w->pos, w->fp) != w->pos) {
            w->error = true;
        }
        w->pos = 0;
    }
}

static void writer_write(vtx_writer_t *w, const void *data, size_t len)
{
    if (w->error) return;

    if (w->compute_crc) {
        w->crc = crc32_update(w->crc, data, len);
    }

    const uint8_t *p = (const uint8_t *)data;
    size_t remaining = len;

    while (remaining > 0) {
        size_t avail = VTX_PERSIST_BUF_SIZE - w->pos;
        size_t chunk = (remaining < avail) ? remaining : avail;
        memcpy(w->buf + w->pos, p, chunk);
        w->pos += chunk;
        p += chunk;
        remaining -= chunk;

        if (w->pos == VTX_PERSIST_BUF_SIZE) {
            writer_flush(w);
            if (w->error) return;
        }
    }
}

/* Convenience: write little-endian uint32 */
static void writer_u32(vtx_writer_t *w, uint32_t val)
{
    uint8_t buf[4];
    buf[0] = (uint8_t)(val);
    buf[1] = (uint8_t)(val >> 8);
    buf[2] = (uint8_t)(val >> 16);
    buf[3] = (uint8_t)(val >> 24);
    writer_write(w, buf, 4);
}

/* Convenience: write little-endian uint64 */
static void writer_u64(vtx_writer_t *w, uint64_t val)
{
    uint8_t buf[8];
    for (int i = 0; i < 8; i++) {
        buf[i] = (uint8_t)(val >> (i * 8));
    }
    writer_write(w, buf, 8);
}

/* Convenience: write uint8 */
static void writer_u8(vtx_writer_t *w, uint8_t val)
{
    writer_write(w, &val, 1);
}

/* ========================================================================== */
/* Internal: buffered reader                                                  */
/* ========================================================================== */

typedef struct {
    FILE   *fp;
    uint32_t crc;
    bool    compute_crc;
    bool    error;
} vtx_reader_t;

static void reader_init(vtx_reader_t *r, FILE *fp, bool compute_crc)
{
    r->fp = fp;
    r->crc = 0;
    r->compute_crc = compute_crc;
    r->error = false;
}

static bool reader_read(vtx_reader_t *r, void *data, size_t len)
{
    if (r->error) return false;
    if (fread(data, 1, len, r->fp) != len) {
        r->error = true;
        return false;
    }
    if (r->compute_crc) {
        r->crc = crc32_update(r->crc, data, len);
    }
    return true;
}

static bool reader_u32(vtx_reader_t *r, uint32_t *out)
{
    uint8_t buf[4];
    if (!reader_read(r, buf, 4)) return false;
    *out = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    return true;
}

static bool reader_u64(vtx_reader_t *r, uint64_t *out)
{
    uint8_t buf[8];
    if (!reader_read(r, buf, 8)) return false;
    *out = 0;
    for (int i = 0; i < 8; i++) {
        *out |= (uint64_t)buf[i] << (i * 8);
    }
    return true;
}

static bool reader_u8(vtx_reader_t *r, uint8_t *out)
{
    return reader_read(r, out, 1);
}

/* ========================================================================== */
/* Save implementation                                                        */
/* ========================================================================== */

static bool write_method_profile(vtx_writer_t *w, const vtx_profile_method_t *m)
{
    writer_u32(w, m->method_id);
    writer_u64(w, m->invocation_count);
    writer_u32(w, m->call_site_count);
    writer_u32(w, m->branch_count);
    writer_u32(w, m->field_access_count);
    writer_u32(w, m->loop_count);

    /* Call sites */
    for (uint32_t i = 0; i < m->call_site_count; i++) {
        const vtx_callsite_profile_t *cs = &m->call_sites[i];
        writer_u32(w, cs->count);
        writer_u8(w, cs->megamorphic ? 1 : 0);
        for (uint32_t j = 0; j < cs->count; j++) {
            writer_u32(w, cs->types[j]);
        }
    }

    /* Branches */
    for (uint32_t i = 0; i < m->branch_count; i++) {
        const vtx_branch_profile_t *b = &m->branches[i];
        writer_u32(w, b->bytecode_pc);
        writer_u64(w, b->taken);
        writer_u64(w, b->not_taken);
    }

    /* Field accesses */
    for (uint32_t i = 0; i < m->field_access_count; i++) {
        const vtx_field_profile_t *f = &m->field_accesses[i];
        writer_u32(w, f->field_offset);
        writer_u32(w, f->count);
        writer_u8(w, f->megamorphic ? 1 : 0);
        for (uint32_t j = 0; j < f->count; j++) {
            writer_u32(w, f->shapes[j]);
        }
    }

    /* Loops */
    for (uint32_t i = 0; i < m->loop_count; i++) {
        const vtx_loop_profile_t *l = &m->loops[i];
        writer_u32(w, l->loop_header_pc);
        writer_u64(w, l->backedge_count);
    }

    return !w->error;
}

bool vtx_profile_save(const vtx_profile_global_t *global, const char *filename)
{
    if (!global || !filename) return false;

    /* Build temp filename: filename + ".tmp" */
    size_t flen = strlen(filename);
    char *tmpname = malloc(flen + 5);
    if (!tmpname) return false;
    memcpy(tmpname, filename, flen);
    memcpy(tmpname + flen, ".tmp", 5);

    FILE *fp = fopen(tmpname, "w+b");
    if (!fp) {
        free(tmpname);
        return false;
    }

    /* Phase 1: Write header with placeholder CRC, compute actual CRC */
    vtx_writer_t w;
    writer_init(&w, fp, false); /* first pass: no CRC yet */

    /* Write magic + version (these are part of the CRC'd region) */
    /* We need to compute CRC over everything after the CRC field.
     * So the layout is: magic(4) | version(4) | crc(4) | data...
     * CRC covers everything after crc field. */

    /* Write magic and version */
    writer_u32(&w, VTX_PROFILE_MAGIC);
    writer_u32(&w, VTX_PROFILE_VERSION);
    /* Placeholder CRC = 0 */
    writer_u32(&w, 0u);
    /* Method count and call edge count */
    writer_u32(&w, global->method_count);
    writer_u32(&w, global->call_edge_count);

    /* Write method profiles */
    for (uint32_t i = 0; i < global->method_count; i++) {
        if (!write_method_profile(&w, &global->methods[i])) {
            writer_flush(&w);
            fclose(fp);
            remove(tmpname);
            free(tmpname);
            return false;
        }
    }

    /* Write call edges */
    for (uint32_t i = 0; i < global->call_edge_count; i++) {
        const vtx_call_edge_t *e = &global->call_edges[i];
        writer_u32(&w, e->caller_method_id);
        writer_u32(&w, e->callee_method_id);
        writer_u64(&w, e->frequency);
    }

    writer_flush(&w);
    fflush(fp);
    if (w.error) {
        fclose(fp);
        remove(tmpname);
        free(tmpname);
        return false;
    }

    /* Phase 2: Compute CRC of the data region (after the CRC field) */
    long data_offset = 12; /* magic(4) + version(4) + crc(4) */
    long file_size = ftell(fp);

    /* Read the data region back and compute CRC */
    fseek(fp, data_offset, SEEK_SET);
    uint32_t crc = 0;
    uint8_t crcbuf[4096];
    long remaining = file_size - data_offset;
    while (remaining > 0) {
        size_t to_read = (size_t)remaining;
        if (to_read > sizeof(crcbuf)) to_read = sizeof(crcbuf);
        size_t got = fread(crcbuf, 1, to_read, fp);
        if (got == 0) break;
        crc = crc32_update(crc, crcbuf, got);
        remaining -= (long)got;
    }

    /* Write the CRC into the header at offset 8 */
    fseek(fp, 8, SEEK_SET);
    uint8_t crc_bytes[4];
    crc_bytes[0] = (uint8_t)(crc);
    crc_bytes[1] = (uint8_t)(crc >> 8);
    crc_bytes[2] = (uint8_t)(crc >> 16);
    crc_bytes[3] = (uint8_t)(crc >> 24);
    if (fwrite(crc_bytes, 1, 4, fp) != 4) {
        fclose(fp);
        remove(tmpname);
        free(tmpname);
        return false;
    }
    fflush(fp);

    fclose(fp);

    /* Atomic rename */
    if (rename(tmpname, filename) != 0) {
        remove(tmpname);
        free(tmpname);
        return false;
    }

    free(tmpname);
    return true;
}

/* ========================================================================== */
/* Load implementation                                                        */
/* ========================================================================== */

static bool read_method_profile(vtx_reader_t *r, vtx_profile_method_t *m)
{
    memset(m, 0, sizeof(*m));

    if (!reader_u32(r, &m->method_id)) return false;
    if (!reader_u64(r, &m->invocation_count)) return false;

    uint32_t cs_count, br_count, fa_count, lp_count;
    if (!reader_u32(r, &cs_count)) return false;
    if (!reader_u32(r, &br_count)) return false;
    if (!reader_u32(r, &fa_count)) return false;
    if (!reader_u32(r, &lp_count)) return false;

    /* Call sites */
    if (cs_count > 0) {
        m->call_sites = calloc(cs_count, sizeof(vtx_callsite_profile_t));
        if (!m->call_sites) return false;
    }
    m->call_site_capacity = cs_count;
    m->call_site_count = cs_count;

    for (uint32_t i = 0; i < cs_count; i++) {
        vtx_callsite_profile_t *cs = &m->call_sites[i];
        if (!reader_u32(r, &cs->count)) return false;
        uint8_t mega;
        if (!reader_u8(r, &mega)) return false;
        cs->megamorphic = (mega != 0);
        if (cs->count > VTX_POLY_LIMIT) return false; /* corrupt data */
        for (uint32_t j = 0; j < cs->count; j++) {
            if (!reader_u32(r, &cs->types[j])) return false;
        }
    }

    /* Branches */
    if (br_count > 0) {
        m->branches = calloc(br_count, sizeof(vtx_branch_profile_t));
        if (!m->branches) return false;
    }
    m->branch_capacity = br_count;
    m->branch_count = br_count;

    for (uint32_t i = 0; i < br_count; i++) {
        vtx_branch_profile_t *b = &m->branches[i];
        if (!reader_u32(r, &b->bytecode_pc)) return false;
        if (!reader_u64(r, &b->taken)) return false;
        if (!reader_u64(r, &b->not_taken)) return false;
    }

    /* Field accesses */
    if (fa_count > 0) {
        m->field_accesses = calloc(fa_count, sizeof(vtx_field_profile_t));
        if (!m->field_accesses) return false;
    }
    m->field_access_capacity = fa_count;
    m->field_access_count = fa_count;

    for (uint32_t i = 0; i < fa_count; i++) {
        vtx_field_profile_t *f = &m->field_accesses[i];
        if (!reader_u32(r, &f->field_offset)) return false;
        if (!reader_u32(r, &f->count)) return false;
        uint8_t mega;
        if (!reader_u8(r, &mega)) return false;
        f->megamorphic = (mega != 0);
        if (f->count > VTX_POLY_LIMIT) return false;
        for (uint32_t j = 0; j < f->count; j++) {
            if (!reader_u32(r, &f->shapes[j])) return false;
        }
    }

    /* Loops */
    if (lp_count > 0) {
        m->loops = calloc(lp_count, sizeof(vtx_loop_profile_t));
        if (!m->loops) return false;
    }
    m->loop_capacity = lp_count;
    m->loop_count = lp_count;

    for (uint32_t i = 0; i < lp_count; i++) {
        vtx_loop_profile_t *l = &m->loops[i];
        if (!reader_u32(r, &l->loop_header_pc)) return false;
        if (!reader_u64(r, &l->backedge_count)) return false;
    }

    return true;
}

bool vtx_profile_load(vtx_profile_global_t *global, const char *filename)
{
    if (!global || !filename) return false;

    FILE *fp = fopen(filename, "rb");
    if (!fp) return false;

    /* Read magic and version */
    vtx_reader_t r;
    reader_init(&r, fp, false);

    uint32_t magic, version, stored_crc;
    if (!reader_u32(&r, &magic))    goto fail;
    if (!reader_u32(&r, &version))  goto fail;
    if (!reader_u32(&r, &stored_crc)) goto fail;

    if (magic != VTX_PROFILE_MAGIC)    goto fail;
    if (version != VTX_PROFILE_VERSION) goto fail;

    /* Now compute CRC of the remaining data */
    long data_start = ftell(fp);
    if (data_start < 0) goto fail;

    /* Seek to end to get file size */
    if (fseek(fp, 0, SEEK_END) != 0) goto fail;
    long file_size = ftell(fp);
    if (file_size < data_start) goto fail;

    /* Read and CRC the data region */
    fseek(fp, data_start, SEEK_SET);
    uint32_t computed_crc = 0;
    uint8_t crcbuf[4096];
    long remaining = file_size - data_start;
    while (remaining > 0) {
        size_t to_read = (size_t)remaining;
        if (to_read > sizeof(crcbuf)) to_read = sizeof(crcbuf);
        size_t got = fread(crcbuf, 1, to_read, fp);
        if (got == 0) break;
        computed_crc = crc32_update(computed_crc, crcbuf, got);
        remaining -= (long)got;
    }

    if (computed_crc != stored_crc) goto fail;

    /* Seek back to data region for reading */
    fseek(fp, data_start, SEEK_SET);
    reader_init(&r, fp, false);

    /* Read method count and call edge count */
    uint32_t method_count, edge_count;
    if (!reader_u32(&r, &method_count)) goto fail;
    if (!reader_u32(&r, &edge_count))   goto fail;

    /* Read method profiles and merge into global */
    for (uint32_t i = 0; i < method_count; i++) {
        vtx_profile_method_t loaded;
        if (!read_method_profile(&r, &loaded)) goto fail;

        /* Merge loaded profile into global */
        vtx_profile_merge_method(global, &loaded);

        /* Free loaded arrays (merge copies the data) */
        free(loaded.call_sites);
        free(loaded.branches);
        free(loaded.field_accesses);
        free(loaded.loops);
    }

    /* Read call edges and merge */
    for (uint32_t i = 0; i < edge_count; i++) {
        uint32_t caller, callee;
        uint64_t freq;
        if (!reader_u32(&r, &caller)) goto fail;
        if (!reader_u32(&r, &callee)) goto fail;
        if (!reader_u64(&r, &freq))   goto fail;

        /* Add frequency to the call edge (or create it) */
        for (uint32_t j = 0; j < global->call_edge_count; j++) {
            if (global->call_edges[j].caller_method_id == caller &&
                global->call_edges[j].callee_method_id == callee) {
                uint64_t sum = global->call_edges[j].frequency + freq;
                global->call_edges[j].frequency =
                    (sum < global->call_edges[j].frequency) ? UINT64_MAX : sum;
                goto next_edge;
            }
        }
        /* New edge */
        if (global->call_edge_count < global->call_edge_capacity) {
            vtx_call_edge_t *e = &global->call_edges[global->call_edge_count++];
            e->caller_method_id = caller;
            e->callee_method_id = callee;
            e->frequency = freq;
        }
        next_edge:;
    }

    fclose(fp);
    return true;

fail:
    fclose(fp);
    return false;
}

/* ========================================================================== */
/* atexit handler                                                             */
/* ========================================================================== */

static vtx_profile_global_t *g_atexit_global = NULL;
static char                 *g_atexit_filename = NULL;

static void profile_atexit_handler(void)
{
    if (g_atexit_global && g_atexit_filename) {
        vtx_profile_save(g_atexit_global, g_atexit_filename);
    }
    free(g_atexit_filename);
    g_atexit_filename = NULL;
}

int vtx_profile_register_atexit(vtx_profile_global_t *global,
                                 const char *filename)
{
    if (!global || !filename) return -1;

    /* Free any previous filename */
    free(g_atexit_filename);

    g_atexit_global = global;
    g_atexit_filename = strdup(filename);
    if (!g_atexit_filename) return -1;

    if (atexit(profile_atexit_handler) != 0) {
        free(g_atexit_filename);
        g_atexit_filename = NULL;
        return -1;
    }

    return 0;
}
