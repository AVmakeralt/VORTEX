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
 * VORTEX Phase-Aware Profile Persistence (Sprint 2.2) — Implementation
 *
 * See phase_persist.h for design rationale.
 */

#include "profile/phase_persist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ========================================================================== */
/* Filename helpers                                                            */
/* ========================================================================== */

int vtx_phase_partition_filename(const char *dir,
                                   const char *hash_hex,
                                   uint32_t phase_id,
                                   char *out,
                                   size_t out_size)
{
    if (dir == NULL || hash_hex == NULL || out == NULL) return -1;

    const char *label;
    char numbuf[16];

    if (phase_id == VTX_PHASE_NONE) {
        label = "default";
    } else {
        snprintf(numbuf, sizeof(numbuf), "%u", phase_id);
        label = numbuf;
    }

    int n = snprintf(out, out_size, "%s/%s.%s.prof", dir, hash_hex, label);
    if (n < 0 || (size_t)n >= out_size) return -1;
    return 0;
}

/* ========================================================================== */
/* Save all phases                                                             */
/* ========================================================================== */

int vtx_phase_partition_save_all(vtx_phase_partition_t *part,
                                   const char *dir,
                                   const char *hash_hex,
                                   const uint8_t bytecode_hash[VTX_PROFILE_HASH_SIZE])
{
    if (part == NULL || dir == NULL || hash_hex == NULL) return -1;

    int saved = 0;
    for (uint32_t i = 0; i < part->entry_count; i++) {
        if (!part->entries[i].valid) continue;

        char filename[600];
        if (vtx_phase_partition_filename(dir, hash_hex,
                                           part->entries[i].phase_id,
                                           filename, sizeof(filename)) != 0) {
            continue;
        }

        if (vtx_profile_save(&part->entries[i].profile, filename, bytecode_hash)) {
            saved++;
        } else {
            fprintf(stderr, "[pgo] warning: failed to save phase %u profile to %s\n",
                    part->entries[i].phase_id, filename);
        }
    }
    return saved;
}

/* ========================================================================== */
/* Load all phases                                                             */
/* ========================================================================== */

/* Parse a phase label ("default" or a decimal number) into a phase_id.
 * Returns true on success. */
static bool parse_phase_label(const char *label, uint32_t *out_phase_id)
{
    if (label == NULL || out_phase_id == NULL) return false;
    if (strcmp(label, "default") == 0) {
        *out_phase_id = VTX_PHASE_NONE;
        return true;
    }
    /* Try parsing as decimal. */
    char *endp = NULL;
    unsigned long val = strtoul(label, &endp, 10);
    if (endp == label || *endp != '\0') return false;  /* not a number */
    if (val == VTX_PHASE_NONE) return false;            /* collision with default */
    *out_phase_id = (uint32_t)val;
    return true;
}

/* Extract the phase label from a filename of the form
 * "<hash_hex>.<label>.prof". Returns true on success, with label_out
 * pointing into a static buffer (valid until next call). */
static bool extract_phase_label(const char *filename,
                                  const char *hash_hex,
                                  char *label_out,
                                  size_t label_out_size)
{
    /* filename is "<hash_hex>.<label>.prof" — skip the hash and the dot. */
    size_t hlen = strlen(hash_hex);
    if (strncmp(filename, hash_hex, hlen) != 0) return false;
    if (filename[hlen] != '.') return false;

    const char *start = filename + hlen + 1;

    /* Find ".prof" suffix. */
    const char *suffix = strstr(start, ".prof");
    if (suffix == NULL) return false;

    size_t label_len = (size_t)(suffix - start);
    if (label_len == 0 || label_len >= label_out_size) return false;

    memcpy(label_out, start, label_len);
    label_out[label_len] = '\0';
    return true;
}

int vtx_phase_partition_load_all(vtx_phase_partition_t *part,
                                   const char *dir,
                                   const char *hash_hex,
                                   const uint8_t expected_hash[VTX_PROFILE_HASH_SIZE])
{
    if (part == NULL || dir == NULL || hash_hex == NULL) return -1;

    DIR *d = opendir(dir);
    if (d == NULL) return 0;  /* no directory = no phases to load */

    int loaded = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* Skip "." and ".." and hidden files. */
        if (ent->d_name[0] == '.') continue;

        /* Check if this file matches our hash prefix. */
        char label[32];
        if (!extract_phase_label(ent->d_name, hash_hex, label, sizeof(label))) {
            continue;
        }

        uint32_t phase_id;
        if (!parse_phase_label(label, &phase_id)) {
            continue;
        }

        /* Build the full path. */
        char path[600];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;

        /* Get or create the phase entry, then load into it. */
        vtx_profile_global_t *phase_profile =
            vtx_phase_partition_get_or_create(part, phase_id);
        if (phase_profile == NULL) continue;

        if (vtx_profile_load(phase_profile, path, expected_hash)) {
            loaded++;
        } else {
            /* Load failed — remove the empty phase entry to keep the
             * partition clean. We do this by destroying and invalidating. */
            /* Find the entry and invalidate it. */
            for (uint32_t i = 0; i < part->entry_count; i++) {
                if (part->entries[i].valid &&
                    part->entries[i].phase_id == phase_id) {
                    vtx_profile_global_destroy(&part->entries[i].profile);
                    part->entries[i].valid = false;
                    break;
                }
            }
        }
    }

    closedir(d);

    /* Make sure the default phase exists even if no file was loaded. */
    if (vtx_phase_partition_get_phase(part, VTX_PHASE_NONE) == NULL) {
        (void)vtx_phase_partition_get_or_create(part, VTX_PHASE_NONE);
    }

    return loaded;
}
