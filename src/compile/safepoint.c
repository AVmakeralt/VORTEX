/**
 * VORTEX Safe Point Checks
 *
 * Application threads check for pending installations and invalidations
 * at safe points. The fast path is a single atomic load.
 *
 * Zero-cost deopt extensions:
 *   - Guard page polling: replaces CMP+JCC with a single MOV from a
 *     memory-mapped page. The page is normally readable; when a safepoint
 *     is needed, mprotect(PROT_NONE) triggers SIGSEGV → handler → deopt.
 *   - Guard-page type checking: eliminates type-check CMP+JCC branches
 *     using mprotected pages indexed by type_id. A load from the page
 *     at offset type_id*4096 succeeds for the expected type (readable)
 *     and SIGSEGVs for wrong types (PROT_NONE) → handler → deopt.
 *   - Implicit null checks: SIGSEGV from null deref → handler → deopt.
 *   - Predicated guard traps: INT3/UD2 from CMOVCC logic → SIGTRAP/SIGILL → handler → deopt.
 */

/* Enable GNU extensions for ucontext_t and SA_NODEFER on glibc */
#define _GNU_SOURCE
#include "compile/safepoint.h"
#include "guard/guard_page_type.h"
#include "interp/dispatch.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <errno.h>

/** Global safepoint manager pointer for signal handler access */
static vtx_safepoint_manager_t *vtx_global_safepoint_manager = NULL;

/** Global flag for guard page availability — now defined in vortex_common
 * (arena.c) to break the circular dependency between vortex_lower and
 * vortex_compile. This file only sets the flag, it does not define it. */

vtx_safepoint_manager_t *vtx_get_safepoint_manager(void)
{
    return vtx_global_safepoint_manager;
}

/* ========================================================================== */
/* Global safepoint flag                                                       */
/* ========================================================================== */

/**
 * Global safepoint flag, polled by JIT-compiled code at loop back-edges.
 * Set to non-zero by the GC/runtime when all threads must reach a safepoint.
 * Cleared when the safepoint operation is complete.
 */
volatile int vtx_safepoint_flag = 0;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_safepoint_init(vtx_safepoint_manager_t *manager,
                        vtx_method_registry_t *registry,
                        vtx_code_cache_t *code_cache)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");

    memset(manager, 0, sizeof(*manager));

    manager->state = VTX_SP_CLEAR;
    manager->registry = registry;
    manager->code_cache = code_cache;
    manager->install_head = NULL;
    manager->install_tail = NULL;
    manager->invalidate_head = NULL;
    manager->invalidate_tail = NULL;

    /* Save for signal handler access */
    vtx_global_safepoint_manager = manager;

    if (pthread_mutex_init(&manager->install_mutex, NULL) != 0) {
        return -1;
    }

    if (pthread_mutex_init(&manager->invalidate_mutex, NULL) != 0) {
        pthread_mutex_destroy(&manager->install_mutex);
        return -1;
    }

    return 0;
}

void vtx_safepoint_destroy(vtx_safepoint_manager_t *manager)
{
    if (!manager) return;

    /* Free pending installation requests */
    pthread_mutex_lock(&manager->install_mutex);
    vtx_sp_install_request_t *ireq = manager->install_head;
    while (ireq) {
        vtx_sp_install_request_t *next = ireq->next;
        free(ireq);
        ireq = next;
    }
    manager->install_head = NULL;
    manager->install_tail = NULL;
    pthread_mutex_unlock(&manager->install_mutex);

    /* Free pending invalidation requests */
    pthread_mutex_lock(&manager->invalidate_mutex);
    vtx_sp_invalidate_request_t *vreq = manager->invalidate_head;
    while (vreq) {
        vtx_sp_invalidate_request_t *next = vreq->next;
        free(vreq);
        vreq = next;
    }
    manager->invalidate_head = NULL;
    manager->invalidate_tail = NULL;
    pthread_mutex_unlock(&manager->invalidate_mutex);

    pthread_mutex_destroy(&manager->install_mutex);
    pthread_mutex_destroy(&manager->invalidate_mutex);
}

/* ========================================================================== */
/* Internal: process pending installations                                     */
/* ========================================================================== */

static uint32_t process_installations(vtx_safepoint_manager_t *manager)
{
    uint32_t installed = 0;

    pthread_mutex_lock(&manager->install_mutex);

    /* Drain the installation queue */
    vtx_sp_install_request_t *req = manager->install_head;
    manager->install_head = NULL;
    manager->install_tail = NULL;

    pthread_mutex_unlock(&manager->install_mutex);

    while (req) {
        /* Install the compiled method */
        if (manager->registry) {
            vtx_compiled_method_t *existing =
                vtx_method_registry_get(manager->registry, req->method_id);

            if (existing) {
                /* Update the existing entry's code pointer atomically.
                 * The compiled_method's code_start is already set by
                 * the compilation thread. We just need to mark it as
                 * installed so future calls use it. */
                existing->is_installed = true;
                existing->code_start   = req->compiled_method->code_start;
                existing->code_size    = req->compiled_method->code_size;
                existing->is_valid     = true;

                installed++;
            } else {
                /* New method — add to registry */
                vtx_method_registry_add(manager->registry,
                                         req->compiled_method);
                req->compiled_method->is_installed = true;
                installed++;
            }
        }

        vtx_sp_install_request_t *next = req->next;
        free(req);
        req = next;
    }

    return installed;
}

/* ========================================================================== */
/* Internal: process pending invalidations                                     */
/* ========================================================================== */

static uint32_t process_invalidations(vtx_safepoint_manager_t *manager,
                                       uint32_t *first_invalidated_method_id)
{
    uint32_t invalidated = 0;
    if (first_invalidated_method_id) {
        *first_invalidated_method_id = UINT32_MAX;
    }

    pthread_mutex_lock(&manager->invalidate_mutex);

    /* Drain the invalidation queue */
    vtx_sp_invalidate_request_t *req = manager->invalidate_head;
    manager->invalidate_head = NULL;
    manager->invalidate_tail = NULL;

    pthread_mutex_unlock(&manager->invalidate_mutex);

    while (req) {
        if (manager->registry) {
            vtx_compiled_method_t *method =
                vtx_method_registry_get(manager->registry, req->method_id);

            if (method && method->is_installed) {
                /* Mark the method as invalid — future calls will go
                 * through the interpreter. The code in the cache is
                 * not freed immediately; it will be cleaned up by the
                 * eviction policy. */
                method->is_valid     = false;
                method->is_installed = false;
                if (first_invalidated_method_id &&
                    *first_invalidated_method_id == UINT32_MAX) {
                    *first_invalidated_method_id = req->method_id;
                }
                invalidated++;
            }
        }

        vtx_sp_invalidate_request_t *next = req->next;
        free(req);
        req = next;
    }

    return invalidated;
}

/* ========================================================================== */
/* Safe point check                                                            */
/* ========================================================================== */

int vtx_safepoint_check(vtx_safepoint_manager_t *manager, void *interp)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");

    /* Fast path: check if anything is pending */
    vtx_safepoint_state_t state =
        __atomic_load_n(&manager->state, __ATOMIC_ACQUIRE);

    if (state == VTX_SP_CLEAR) {
        __atomic_fetch_add(&manager->total_checks, 1, __ATOMIC_RELAXED);
        return 0;
    }

    /* Slow path: process pending work.
     *
     * Atomically exchange the state to VTX_SP_CLEAR before processing.
     * This ensures that any new requests that arrive during processing
     * will set the flag again and will not be lost. If we cleared
     * unconditionally after processing, requests arriving between
     * the initial load and the clear would be discarded. */
    vtx_safepoint_state_t orig_state =
        __atomic_exchange_n(&manager->state, VTX_SP_CLEAR, __ATOMIC_ACQ_REL);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int result = 0;

    /* Process installations */
    if (orig_state & VTX_SP_INSTALL_PENDING) {
        uint32_t installed = process_installations(manager);
        manager->total_installations += installed;
        if (installed > 0) {
            result = 1;
        }
    }

    /* Process invalidations */
    if (orig_state & VTX_SP_INVALIDATE_PENDING) {
        uint32_t first_invalidated_id = UINT32_MAX;
        uint32_t invalidated = process_invalidations(manager, &first_invalidated_id);
        manager->total_invalidations += invalidated;
        if (invalidated > 0) {
            result = -1; /* signal that deopt may be needed */

            /* Trigger deoptimization: if the caller provided an interpreter
             * state and a method was invalidated, set the deopt_pending flag
             * so the dispatch loop triggers deoptimization at the next safe
             * point. */
            if (interp != NULL) {
                vtx_interp_t *interp_state = (vtx_interp_t *)interp;
                interp_state->deopt_pending = true;
                interp_state->deopt_method_id = first_invalidated_id;
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t elapsed_ns =
        (uint64_t)(end.tv_sec - start.tv_sec) * 1000000000ULL +
        (uint64_t)(end.tv_nsec - start.tv_nsec);
    __atomic_fetch_add(&manager->total_time_ns, elapsed_ns, __ATOMIC_RELAXED);

    __atomic_fetch_add(&manager->total_checks, 1, __ATOMIC_RELAXED);

    return result;
}

/* ========================================================================== */
/* Installation requests                                                       */
/* ========================================================================== */

int vtx_safepoint_request_install(vtx_safepoint_manager_t *manager,
                                   uint32_t method_id,
                                   vtx_compiled_method_t *compiled_method)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");
    VTX_ASSERT(compiled_method != NULL, "compiled_method must not be NULL");

    /* Allocate request */
    vtx_sp_install_request_t *req = malloc(sizeof(vtx_sp_install_request_t));
    if (!req) return -1;

    req->method_id = method_id;
    req->compiled_method = compiled_method;
    req->next = NULL;

    /* Add to the installation queue */
    pthread_mutex_lock(&manager->install_mutex);
    if (manager->install_tail) {
        manager->install_tail->next = req;
    } else {
        manager->install_head = req;
    }
    manager->install_tail = req;
    pthread_mutex_unlock(&manager->install_mutex);

    /* Set the global flag to trigger safe point processing */
    vtx_safepoint_state_t old_state =
        __atomic_load_n(&manager->state, __ATOMIC_RELAXED);
    vtx_safepoint_state_t new_state;

    do {
        new_state = (vtx_safepoint_state_t)(
            (unsigned)old_state | VTX_SP_INSTALL_PENDING);
    } while (!__atomic_compare_exchange_n(&manager->state, &old_state,
                                           new_state, false,
                                           __ATOMIC_RELEASE,
                                           __ATOMIC_RELAXED));

    return 0;
}

/* ========================================================================== */
/* Invalidation requests                                                       */
/* ========================================================================== */

int vtx_safepoint_request_invalidate(vtx_safepoint_manager_t *manager,
                                      uint32_t method_id)
{
    VTX_ASSERT(manager != NULL, "manager must not be NULL");

    /* Allocate request */
    vtx_sp_invalidate_request_t *req = malloc(sizeof(vtx_sp_invalidate_request_t));
    if (!req) return -1;

    req->method_id = method_id;
    req->next = NULL;

    /* Add to the invalidation queue */
    pthread_mutex_lock(&manager->invalidate_mutex);
    if (manager->invalidate_tail) {
        manager->invalidate_tail->next = req;
    } else {
        manager->invalidate_head = req;
    }
    manager->invalidate_tail = req;
    pthread_mutex_unlock(&manager->invalidate_mutex);

    /* Set the global flag to trigger safe point processing */
    vtx_safepoint_state_t old_state =
        __atomic_load_n(&manager->state, __ATOMIC_RELAXED);
    vtx_safepoint_state_t new_state;

    do {
        new_state = (vtx_safepoint_state_t)(
            (unsigned)old_state | VTX_SP_INVALIDATE_PENDING);
    } while (!__atomic_compare_exchange_n(&manager->state, &old_state,
                                           new_state, false,
                                           __ATOMIC_RELEASE,
                                           __ATOMIC_RELAXED));

    return 0;
}

/* ========================================================================== */
/* Guard page safepoint polling (zero-cost deopt)                              */
/* ========================================================================== */

/**
 * Guard page for zero-cost safepoint polls.
 *
 * When armed (safepoint requested): page is PROT_NONE, any read triggers SIGSEGV.
 * When disarmed (normal execution): page is PROT_READ, reads succeed silently.
 *
 * The page also contains a magic value at offset 0 that the SIGSEGV handler
 * can check to distinguish guard page faults from null pointer dereferences.
 */
static uint8_t *vtx_guard_page_mem = NULL;
static long     vtx_guard_page_size = 0;

/** Saved signal handlers for restoration on destroy */
static struct sigaction vtx_old_sigsegv_action;
static struct sigaction vtx_old_sigtrap_action;
static struct sigaction vtx_old_sigill_action;
static bool vtx_signal_handlers_installed = false;

/** Deopt table: maps code ranges to deopt metadata for SIGSEGV handler */
static vtx_guard_page_deopt_entry_t vtx_deopt_table[VTX_GUARD_PAGE_DEOPT_TABLE_SIZE];
static uint32_t vtx_deopt_table_count = 0;
static pthread_mutex_t vtx_deopt_table_mutex = PTHREAD_MUTEX_INITIALIZER;

/** Magic value at the start of the guard page (for fault disambiguation) */
#define VTX_GUARD_PAGE_MAGIC 0xDEADBEEFCAFEBABEULL

/* Forward declaration of the signal handler */
static void vtx_guard_page_sigsegv_handler(int sig, siginfo_t *info, void *ucontext);
static void vtx_guard_page_sigtrap_handler(int sig, siginfo_t *info, void *ucontext);

/* ========================================================================== */
/* SIGSEGV handler — guard page faults + implicit null checks                  */
/* ========================================================================== */

/**
 * SIGSEGV handler for zero-cost deopt.
 *
 * This handler is invoked when:
 *   1. A guard page poll triggers (page is PROT_NONE) -> safepoint deopt
 *   2. An implicit null check fails (null deref) -> null check deopt
 *   3. Any other SIGSEGV in JIT code -> forward to original handler
 *
 * The handler distinguishes between these cases by checking:
 *   - Is the fault address within the guard page? -> safepoint
 *   - Is the fault address < VTX_NULL_PAGE_LIMIT? -> implicit null check
 *   - Is the faulting RIP within a registered code range? -> deopt
 *   - Otherwise -> chain to the previous handler (crash)
 */
static void vtx_guard_page_sigsegv_handler(int sig, siginfo_t *info, void *ucontext)
{
    (void)sig;

    uintptr_t fault_addr = (uintptr_t)info->si_addr;

    /* Case 1: Guard page fault — safepoint requested.
     * The fault address falls within the guard page's address range. */
    if (vtx_guard_page_mem != NULL &&
        fault_addr >= (uintptr_t)vtx_guard_page_mem &&
        fault_addr < (uintptr_t)(vtx_guard_page_mem + vtx_guard_page_size)) {
        /* Disarm the guard page immediately to prevent re-triggering.
         * The safepoint handler will re-arm if needed. */
        mprotect(vtx_guard_page_mem, (size_t)vtx_guard_page_size, PROT_READ);

        /* Perform the safepoint check. This processes pending
         * installations and invalidations. */
        vtx_safepoint_manager_t *mgr = vtx_get_safepoint_manager();
        if (mgr) {
            vtx_safepoint_check(mgr, NULL);
        }

        /* Return from signal handler — the faulting instruction will be
         * re-executed. Since we disarmed the page, the MOV will succeed
         * this time. The loaded value is ignored by the JIT code. */
        return;
    }

    /* Case 2: Type guard page fault — guard-page type checking.
     * The fault address falls within a registered type guard page
     * region. This means a type check failed: the loaded type_id
     * indexed into a PROT_NONE page instead of the readable page
     * for the expected type.
     *
     * This check is performed BEFORE the null-page check because
     * type guard page regions are allocated with mmap and will
     * never be in the low address range. The lookup is lock-free
     * and safe in signal handler context. */
    {
        vtx_type_guard_page_registry_t *tg_registry =
            vtx_type_guard_page_get_registry();
        if (tg_registry != NULL) {
            vtx_type_guard_fault_info_t fault_info;
            if (vtx_type_guard_page_registry_lookup(tg_registry, fault_addr,
                                                     &fault_info)) {
                /* Found: the fault is from a type guard page.
                 * Invoke the registered callback or perform deopt. */
                vtx_type_guard_fault_callback_t callback =
                    vtx_type_guard_page_get_fault_callback();
                if (callback != NULL) {
                    callback(&fault_info, ucontext);
                    return;
                }

                /* No callback registered — use the standard deopt path.
                 * Find the compiled method from the faulting RIP and
                 * perform deoptimization. */
                ucontext_t *uc = (ucontext_t *)ucontext;
                uintptr_t faulting_rip =
                    (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];

                pthread_mutex_lock(&vtx_deopt_table_mutex);
                for (uint32_t i = 0; i < vtx_deopt_table_count; i++) {
                    const vtx_guard_page_deopt_entry_t *entry =
                        &vtx_deopt_table[i];
                    if (faulting_rip >= (uintptr_t)entry->code_start &&
                        faulting_rip < (uintptr_t)(entry->code_start +
                                                    entry->code_size)) {
                        pthread_mutex_unlock(&vtx_deopt_table_mutex);

                        extern void vtx_deopt_handler_stub(uint32_t, uint32_t);
                        uint32_t native_pc = (uint32_t)(faulting_rip -
                            (uintptr_t)entry->code_start);
                        vtx_deopt_handler_stub(entry->side_table_index,
                                                native_pc);
                        return;
                    }
                }
                pthread_mutex_unlock(&vtx_deopt_table_mutex);

                /* Method not found in deopt table — fall through.
                 * This shouldn't happen in normal operation; it means
                 * the type guard page was faulted but the method isn't
                 * registered for deopt. Chain to the original handler. */
            }
        }
    }

    /* Case 3: Implicit null check — fault in the low address range.
     * This occurs when JIT code dereferences a null pointer without
     * an explicit test+branch guard. The MMU catches the fault. */
    if (fault_addr < VTX_NULL_PAGE_LIMIT) {
        /* Find the compiled method that contains the faulting RIP.
         * The RIP is in the ucontext's register state. */
        ucontext_t *uc = (ucontext_t *)ucontext;
        uintptr_t faulting_rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];

        pthread_mutex_lock(&vtx_deopt_table_mutex);
        for (uint32_t i = 0; i < vtx_deopt_table_count; i++) {
            const vtx_guard_page_deopt_entry_t *entry = &vtx_deopt_table[i];
            if (faulting_rip >= (uintptr_t)entry->code_start &&
                faulting_rip < (uintptr_t)(entry->code_start + entry->code_size)) {
                /* Found the method — perform deopt.
                 * Look up the side table for the faulting PC to get
                 * the frame state index, then call the deopt handler. */
                pthread_mutex_unlock(&vtx_deopt_table_mutex);

                /* Call the deopt runtime to transition to the interpreter.
                 * The deopt handler will reconstruct the interpreter frame
                 * from the side table entry at the faulting PC. */
                extern void vtx_deopt_handler_stub(uint32_t, uint32_t);
                uint32_t native_pc = (uint32_t)(faulting_rip -
                    (uintptr_t)entry->code_start);
                vtx_deopt_handler_stub(entry->side_table_index, native_pc);

                /* vtx_deopt_handler_stub should not return — it transfers
                 * to the interpreter. If it does return, the signal handler
                 * returns and the faulting instruction is re-executed,
                 * which will SIGSEGV again and fall through to the
                 * original handler below. */
                return;
            }
        }
        pthread_mutex_unlock(&vtx_deopt_table_mutex);

        /* Code range not found — could be a genuine null deref in
         * non-JIT code. Fall through to the original handler. */
    }

    /* Case 4: Not a VORTEX-related fault — chain to the original handler.
     * This preserves normal crash behavior for bugs in non-JIT code. */
    if (vtx_old_sigsegv_action.sa_flags & SA_SIGINFO) {
        if (vtx_old_sigsegv_action.sa_sigaction) {
            vtx_old_sigsegv_action.sa_sigaction(sig, info, ucontext);
            return;
        }
    } else if (vtx_old_sigsegv_action.sa_handler != SIG_DFL &&
               vtx_old_sigsegv_action.sa_handler != SIG_IGN) {
        vtx_old_sigsegv_action.sa_handler(sig);
        return;
    }

    /* No previous handler — re-raise with default handler (crash) */
    signal(SIGSEGV, SIG_DFL);
    raise(SIGSEGV);
}

/* ========================================================================== */
/* SIGTRAP handler — predicated guard traps (CMOVCC + INT3/UD2)               */
/* ========================================================================== */

/**
 * SIGTRAP handler for predicated guard traps.
 *
 * When a PredicatedCheck guard fails, the CMOVCC logic places an INT3
 * (0xCC) instruction at the guard point, which triggers SIGTRAP.
 * This handler translates the trap into a deopt.
 */
static void vtx_guard_page_sigtrap_handler(int sig, siginfo_t *info, void *ucontext)
{
    (void)sig;
    (void)info;

    ucontext_t *uc = (ucontext_t *)ucontext;
    uintptr_t faulting_rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];

    /* The RIP points to the instruction AFTER the INT3 (0xCC).
     * The INT3 is a 1-byte instruction, so the trapping instruction
     * is at faulting_rip - 1. But the kernel sets RIP to the
     * instruction following the INT3, so faulting_rip is actually
     * correct for looking up the side table. */

    /* Look up the code range in the deopt table */
    pthread_mutex_lock(&vtx_deopt_table_mutex);
    for (uint32_t i = 0; i < vtx_deopt_table_count; i++) {
        const vtx_guard_page_deopt_entry_t *entry = &vtx_deopt_table[i];
        if (faulting_rip >= (uintptr_t)entry->code_start &&
            faulting_rip < (uintptr_t)(entry->code_start + entry->code_size)) {
            pthread_mutex_unlock(&vtx_deopt_table_mutex);

            extern void vtx_deopt_handler_stub(uint32_t, uint32_t);
            uint32_t native_pc = (uint32_t)(faulting_rip -
                (uintptr_t)entry->code_start);
            vtx_deopt_handler_stub(entry->side_table_index, native_pc);
            return;
        }
    }
    pthread_mutex_unlock(&vtx_deopt_table_mutex);

    /* Not a VORTEX guard trap — chain to original handler */
    if (vtx_old_sigtrap_action.sa_flags & SA_SIGINFO) {
        if (vtx_old_sigtrap_action.sa_sigaction) {
            vtx_old_sigtrap_action.sa_sigaction(sig, info, ucontext);
            return;
        }
    } else if (vtx_old_sigtrap_action.sa_handler != SIG_DFL &&
               vtx_old_sigtrap_action.sa_handler != SIG_IGN) {
        vtx_old_sigtrap_action.sa_handler(sig);
        return;
    }

    signal(SIGTRAP, SIG_DFL);
    raise(SIGTRAP);
}

/* ========================================================================== */
/* Guard page lifecycle                                                        */
/* ========================================================================== */

int vtx_guard_page_init(void)
{
    /* Get page size */
    vtx_guard_page_size = sysconf(_SC_PAGESIZE);
    if (vtx_guard_page_size <= 0) vtx_guard_page_size = 4096;

    /* Allocate a single guard page with mmap.
     * Initially readable (PROT_READ) — JIT code can load from it freely. */
    vtx_guard_page_mem = (uint8_t *)mmap(NULL, (size_t)vtx_guard_page_size,
                                          PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vtx_guard_page_mem == MAP_FAILED) {
        vtx_guard_page_mem = NULL;
        return -1;
    }

    /* Write a magic value at the start of the page for disambiguation.
     * This value is visible to JIT code when they load from the guard page,
     * but the loaded value is always ignored. */
    memset(vtx_guard_page_mem, 0, (size_t)vtx_guard_page_size);
    uint64_t magic = VTX_GUARD_PAGE_MAGIC;
    memcpy(vtx_guard_page_mem, &magic, sizeof(magic));

    /* Now make the page read-only (JIT code only reads from it) */
    if (mprotect(vtx_guard_page_mem, (size_t)vtx_guard_page_size, PROT_READ) != 0) {
        munmap(vtx_guard_page_mem, (size_t)vtx_guard_page_size);
        vtx_guard_page_mem = NULL;
        return -1;
    }

    /* Install signal handlers for SIGSEGV (guard page + null checks)
     * and SIGTRAP (predicated guard traps with INT3). */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = vtx_guard_page_sigsegv_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGSEGV, &sa, &vtx_old_sigsegv_action) != 0) {
        munmap(vtx_guard_page_mem, (size_t)vtx_guard_page_size);
        vtx_guard_page_mem = NULL;
        return -1;
    }

    /* Install SIGTRAP handler for predicated guard INT3 traps */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = vtx_guard_page_sigtrap_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGTRAP, &sa, &vtx_old_sigtrap_action) != 0) {
        /* Restore SIGSEGV handler on failure */
        sigaction(SIGSEGV, &vtx_old_sigsegv_action, NULL);
        munmap(vtx_guard_page_mem, (size_t)vtx_guard_page_size);
        vtx_guard_page_mem = NULL;
        return -1;
    }

    /* Save the old SIGILL handler too (for UD2 traps if used) */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = vtx_guard_page_sigtrap_handler; /* reuse same handler */
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, &vtx_old_sigill_action);
    /* SIGILL install failure is non-fatal — UD2 is optional */

    vtx_signal_handlers_installed = true;
    vtx_deopt_table_count = 0;

    /* Set the global availability flag so isel can use guard page polls */
    __atomic_store_n(&vtx_guard_page_available_flag, 1, __ATOMIC_RELEASE);

    return 0;
}

void vtx_guard_page_destroy(void)
{
    /* Clear the availability flag first */
    __atomic_store_n(&vtx_guard_page_available_flag, 0, __ATOMIC_RELEASE);

    if (vtx_guard_page_mem != NULL) {
        munmap(vtx_guard_page_mem, (size_t)vtx_guard_page_size);
        vtx_guard_page_mem = NULL;
    }

    /* Restore original signal handlers */
    if (vtx_signal_handlers_installed) {
        sigaction(SIGSEGV, &vtx_old_sigsegv_action, NULL);
        sigaction(SIGTRAP, &vtx_old_sigtrap_action, NULL);
        sigaction(SIGILL, &vtx_old_sigill_action, NULL);
        vtx_signal_handlers_installed = false;
    }

    vtx_deopt_table_count = 0;
}

void *vtx_guard_page_address(void)
{
    return (void *)vtx_guard_page_mem;
}

int vtx_guard_page_arm(void)
{
    if (vtx_guard_page_mem == NULL) return -1;

    if (mprotect(vtx_guard_page_mem, (size_t)vtx_guard_page_size, PROT_NONE) != 0) {
        return -1;
    }
    return 0;
}

int vtx_guard_page_disarm(void)
{
    if (vtx_guard_page_mem == NULL) return -1;

    if (mprotect(vtx_guard_page_mem, (size_t)vtx_guard_page_size, PROT_READ) != 0) {
        return -1;
    }
    return 0;
}

bool vtx_guard_page_is_available(void)
{
    return vtx_guard_page_mem != NULL;
}

int vtx_guard_page_register_code(const uint8_t *code_start, uint32_t code_size,
                                   uint32_t method_id, uint32_t side_table_index)
{
    pthread_mutex_lock(&vtx_deopt_table_mutex);

    if (vtx_deopt_table_count >= VTX_GUARD_PAGE_DEOPT_TABLE_SIZE) {
        pthread_mutex_unlock(&vtx_deopt_table_mutex);
        return -1;
    }

    vtx_guard_page_deopt_entry_t *entry =
        &vtx_deopt_table[vtx_deopt_table_count];
    entry->code_start = code_start;
    entry->code_size = code_size;
    entry->method_id = method_id;
    entry->side_table_index = side_table_index;
    vtx_deopt_table_count++;

    pthread_mutex_unlock(&vtx_deopt_table_mutex);
    return 0;
}

int vtx_guard_page_unregister_code(const uint8_t *code_start)
{
    pthread_mutex_lock(&vtx_deopt_table_mutex);

    for (uint32_t i = 0; i < vtx_deopt_table_count; i++) {
        if (vtx_deopt_table[i].code_start == code_start) {
            /* Swap with last entry and shrink */
            vtx_deopt_table[i] = vtx_deopt_table[vtx_deopt_table_count - 1];
            vtx_deopt_table_count--;
            pthread_mutex_unlock(&vtx_deopt_table_mutex);
            return 0;
        }
    }

    pthread_mutex_unlock(&vtx_deopt_table_mutex);
    return -1;
}
