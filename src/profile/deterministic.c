/**
 * VORTEX Deterministic Mode (Sprint 1.4) — Implementation
 *
 * Caches the VORTEX_DETERMINISTIC env var at startup so the hot path
 * is a single boolean check.
 */

#include "profile/deterministic.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Module state                                                                */
/* ========================================================================== */

static bool     g_initialized = false;
static bool     g_enabled     = false;

/* ========================================================================== */
/* Initialization                                                              */
/* ========================================================================== */

void vtx_deterministic_init(void)
{
    if (g_initialized) return;

    const char *val = getenv("VORTEX_DETERMINISTIC");
    if (val == NULL) {
        g_enabled = false;
    } else if (val[0] == '\0') {
        g_enabled = false;          /* empty string = explicitly disabled */
    } else if (strcmp(val, "0") == 0) {
        g_enabled = false;          /* "0" = explicitly disabled */
    } else {
        /* "1", "true", "yes", anything non-empty non-"0" → enabled. */
        g_enabled = true;
    }

    g_initialized = true;
}

/* ========================================================================== */
/* Query                                                                       */
/* ========================================================================== */

bool vtx_deterministic_enabled(void)
{
    if (!g_initialized) vtx_deterministic_init();
    return g_enabled;
}

uint32_t vtx_deterministic_threads(void)
{
    if (!vtx_deterministic_enabled()) return 0;
    return 1;  /* single worker = deterministic ordering */
}

uint32_t vtx_deterministic_check_interval_ms(void)
{
    if (!vtx_deterministic_enabled()) return 0;
    return 100;  /* fixed 100ms, no jitter */
}

bool vtx_deterministic_disable_persistence(void)
{
    return vtx_deterministic_enabled();
}

bool vtx_deterministic_freeze_guard_ewma(void)
{
    return vtx_deterministic_enabled();
}
