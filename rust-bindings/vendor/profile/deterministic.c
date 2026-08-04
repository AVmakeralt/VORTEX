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
 * VORTEX Deterministic Mode (Sprint 1.4) — Implementation
 *
 * Caches the VORTEX_DETERMINISTIC env var at startup so the hot path
 * is a single boolean check.
 *
 * BUGFIX P22: The old code had a race on lazy init — g_initialized and
 * g_enabled were non-atomic plain bools, and getenv() is not thread-safe.
 * Two threads calling vtx_deterministic_enabled() concurrently on first
 * access could both read g_initialized=false, both call getenv(), and
 * race on writing g_enabled. Fix: use pthread_once for thread-safe
 * one-time initialization.
 */

#include "profile/deterministic.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ========================================================================== */
/* Module state                                                                */
/* ========================================================================== */

static bool     g_enabled     = false;
static pthread_once_t g_once  = PTHREAD_ONCE_INIT;

/* ========================================================================== */
/* Initialization (thread-safe via pthread_once)                               */
/* ========================================================================== */

static void deterministic_init_once(void)
{
    /* getenv() is not thread-safe in general, but pthread_once ensures
     * this function runs exactly once before any concurrent access.
     * After pthread_once returns, g_enabled is stable and can be read
     * without synchronization (it's written before the once completes,
     * and pthread_once provides a happens-before relationship). */
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
}

void vtx_deterministic_init(void)
{
    pthread_once(&g_once, deterministic_init_once);
}

/* ========================================================================== */
/* Query                                                                       */
/* ========================================================================== */

bool vtx_deterministic_enabled(void)
{
    pthread_once(&g_once, deterministic_init_once);
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
