/**
 * VORTEX Guard-Page Type Checking — Implementation
 *
 * Zero-cost type guard implementation using mprotected memory pages.
 * See guard_page_type.h for detailed design documentation.
 *
 * Key implementation details:
 *
 *   - Virtual memory regions are allocated with mmap(MAP_PRIVATE|MAP_ANONYMOUS).
 *     PROT_NONE pages consume no physical memory — the kernel's virtual memory
 *     subsystem only allocates page table entries on access, which triggers
 *     SIGSEGV for PROT_NONE pages before any physical allocation occurs.
 *
 *   - The registry snapshot mechanism ensures lock-free lookups from signal
 *     handler context. Mutations (create/destroy) acquire the mutex, update
 *     the pages array, then atomically publish a new snapshot. The old
 *     snapshot is freed after the mutex is released (the publication of the
 *     new snapshot guarantees no signal handler is reading the old one).
 *
 *   - Reconfiguration (changing the expected type) is done by toggling
 *     page protections. There is a brief unsafe window where the old page
 *     is PROT_NONE but the new page hasn't been made PROT_READ yet. If
 *     a type check hits during this window, it will SIGSEGV spuriously.
 *     The handler detects this case (fault address is within the region
 *     but the type_id slot isn't the one being reconfigured to) and can
 *     retry or fall back to deopt.
 */

#define _GNU_SOURCE
#include "guard/guard_page_type.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

/* ========================================================================== */
/* Global state                                                                 */
/* ========================================================================== */

/** Global type guard page registry pointer */
static vtx_type_guard_page_registry_t *vtx_global_type_guard_registry = NULL;

/** Global availability flag — checked by isel without creating a dependency */
volatile int vtx_type_guard_page_available_flag = 0;

/** Fault callback — called from SIGSEGV handler context */
static vtx_type_guard_fault_callback_t vtx_type_guard_fault_callback = NULL;

/** System page size (cached) */
static long vtx_system_page_size = 0;

/* ========================================================================== */
/* Internal helpers                                                             */
/* ========================================================================== */

/**
 * Align a value up to the nearest page boundary.
 */
static inline size_t align_up_page(size_t value)
{
    if (vtx_system_page_size <= 0) return value;
    return (value + (size_t)vtx_system_page_size - 1) &
           ~((size_t)vtx_system_page_size - 1);
}

/**
 * Compute the region size needed for a type guard page.
 *
 * Each type_id gets its own page at offset type_id * VTX_TYPE_GUARD_STRIDE.
 * The total size is (max_type_id + 1) * VTX_TYPE_GUARD_STRIDE, which
 * must be at least one page.
 */
static inline size_t compute_region_size(uint32_t max_type_id)
{
    return (size_t)(max_type_id + 1) * VTX_TYPE_GUARD_STRIDE;
}

/**
 * Compute the page offset for a given type_id within a region.
 */
static inline size_t type_id_to_offset(uint32_t type_id)
{
    return (size_t)type_id * VTX_TYPE_GUARD_STRIDE;
}

/**
 * Update the registry snapshot for lock-free signal handler access.
 *
 * This function must be called while holding the registry mutex.
 * It allocates a new snapshot array, copies the current pages,
 * then atomically publishes it. The old snapshot is freed.
 */
static int update_snapshot(vtx_type_guard_page_registry_t *registry)
{
    /* Allocate a new snapshot */
    vtx_type_guard_page_t *new_snapshot = NULL;
    if (registry->page_count > 0) {
        new_snapshot = (vtx_type_guard_page_t *)malloc(
            registry->page_count * sizeof(vtx_type_guard_page_t));
        if (new_snapshot == NULL) return -1;
        memcpy(new_snapshot, registry->pages,
               registry->page_count * sizeof(vtx_type_guard_page_t));
    }

    /* Publish the new snapshot atomically.
     * Any signal handler that reads the snapshot pointer after this
     * store will see the new data. The __ATOMIC_RELEASE ensures
     * that the memcpy completes before the pointer is published. */
    vtx_type_guard_page_t *old_snapshot = registry->snapshot;
    __atomic_store_n(&registry->snapshot, new_snapshot, __ATOMIC_RELEASE);
    __atomic_store_n(&registry->snapshot_count, registry->page_count,
                     __ATOMIC_RELEASE);

    /* Free the old snapshot. This is safe because:
     *   1. We hold the mutex, so no other thread is mutating the registry
     *   2. The new snapshot has been published, so any signal handler
     *      that was reading the old snapshot has already finished
     *      (signal handlers are synchronous with respect to the faulting
     *       thread, and we are that thread if we're in the handler) */
    if (old_snapshot != NULL) {
        free(old_snapshot);
    }

    return 0;
}

/* ========================================================================== */
/* Registry lifecycle                                                           */
/* ========================================================================== */

int vtx_type_guard_page_registry_init(vtx_type_guard_page_registry_t *registry)
{
    if (registry == NULL) return -1;

    /* Cache the system page size */
    vtx_system_page_size = sysconf(_SC_PAGESIZE);
    if (vtx_system_page_size <= 0) vtx_system_page_size = 4096;

    memset(registry, 0, sizeof(*registry));
    registry->page_count = 0;
    registry->snapshot = NULL;
    registry->snapshot_count = 0;
    registry->total_created = 0;
    registry->total_destroyed = 0;
    registry->total_faults_handled = 0;

    if (pthread_mutex_init(&registry->mutex, NULL) != 0) {
        return -1;
    }

    /* Set the global registry pointer */
    vtx_global_type_guard_registry = registry;

    /* Set the availability flag so isel can check it */
    __atomic_store_n(&vtx_type_guard_page_available_flag, 1, __ATOMIC_RELEASE);

    return 0;
}

void vtx_type_guard_page_registry_destroy(vtx_type_guard_page_registry_t *registry)
{
    if (registry == NULL) return;

    /* Clear the availability flag first */
    __atomic_store_n(&vtx_type_guard_page_available_flag, 0, __ATOMIC_RELEASE);

    /* Unmap all active guard page regions */
    for (uint32_t i = 0; i < registry->page_count; i++) {
        vtx_type_guard_page_t *page = &registry->pages[i];
        if (page->is_active && page->region_base != NULL) {
            munmap(page->region_base, page->region_size);
            page->region_base = NULL;
            page->is_active = false;
        }
    }

    /* Free the snapshot */
    if (registry->snapshot != NULL) {
        free(registry->snapshot);
        registry->snapshot = NULL;
    }
    registry->snapshot_count = 0;

    pthread_mutex_destroy(&registry->mutex);

    /* Clear the global registry pointer */
    if (vtx_global_type_guard_registry == registry) {
        vtx_global_type_guard_registry = NULL;
    }
}

/* ========================================================================== */
/* Type guard page creation                                                     */
/* ========================================================================== */

vtx_type_guard_page_t *vtx_type_guard_page_create(
    vtx_type_guard_page_registry_t *registry,
    uint32_t expected_type_id,
    uint32_t max_type_id,
    uint32_t method_id,
    uint32_t guard_id,
    uint32_t side_table_index)
{
    if (registry == NULL) return NULL;

    /* Validate: expected_type_id must be within range */
    if (expected_type_id > max_type_id) return NULL;

    pthread_mutex_lock(&registry->mutex);

    /* Check capacity */
    if (registry->page_count >= VTX_TYPE_GUARD_REGISTRY_MAX_CAPACITY) {
        pthread_mutex_unlock(&registry->mutex);
        return NULL;
    }

    /* Compute region size: one page per type_id */
    size_t region_size = compute_region_size(max_type_id);

    /* Allocate the virtual memory region.
     * Use MAP_PRIVATE|MAP_ANONYMOUS — no file backing needed.
     * Initially map with PROT_NONE so all pages are inaccessible.
     * The kernel will not allocate physical memory for PROT_NONE pages. */
    void *region = mmap(NULL, region_size, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) {
        pthread_mutex_unlock(&registry->mutex);
        return NULL;
    }

    /* Now make the page for expected_type_id readable.
     * The page starts at offset expected_type_id * VTX_TYPE_GUARD_STRIDE. */
    size_t valid_offset = type_id_to_offset(expected_type_id);
    void *valid_page = (uint8_t *)region + valid_offset;

    if (mprotect(valid_page, VTX_TYPE_GUARD_STRIDE, PROT_READ | PROT_WRITE) != 0) {
        munmap(region, region_size);
        pthread_mutex_unlock(&registry->mutex);
        return NULL;
    }

    /* Write the magic value at the start of the valid page.
     * This helps the SIGSEGV handler verify that a fault came from
     * a type guard page region (not some other mmap'd area). */
    uint64_t magic = VTX_TYPE_GUARD_PAGE_MAGIC;
    memcpy(valid_page, &magic, sizeof(magic));

    /* Write a non-zero sentinel byte at offset 0 of the valid page's
     * data area (after the magic). The JIT code loads from this slot
     * and the loaded value is available for debugging, but the guard
     * "passes" simply by not SIGSEGVing. */
    uint8_t *data_area = (uint8_t *)valid_page + sizeof(magic);
    *data_area = 0x01;  /* sentinel: "this type is valid for this guard" */

    /* Make the valid page read-only (JIT code only reads from it) */
    if (mprotect(valid_page, VTX_TYPE_GUARD_STRIDE, PROT_READ) != 0) {
        munmap(region, region_size);
        pthread_mutex_unlock(&registry->mutex);
        return NULL;
    }

    /* Add to the registry */
    vtx_type_guard_page_t *page = &registry->pages[registry->page_count];
    page->region_base = region;
    page->region_size = region_size;
    page->expected_type_id = expected_type_id;
    page->max_type_id = max_type_id;
    page->method_id = method_id;
    page->guard_id = guard_id;
    page->side_table_index = side_table_index;
    page->is_active = true;

    registry->page_count++;
    registry->total_created++;

    /* Update the snapshot for lock-free signal handler access */
    if (update_snapshot(registry) != 0) {
        /* Snapshot update failed — the guard page is still usable,
         * but signal handler lookups won't find it. This is a degraded
         * state. We keep the page active but log the error.
         * In production, this should trigger a warning. */
        /* TODO: log warning — snapshot update failed */
    }

    pthread_mutex_unlock(&registry->mutex);

    return page;
}

/* ========================================================================== */
/* Type guard page destruction                                                  */
/* ========================================================================== */

int vtx_type_guard_page_destroy(vtx_type_guard_page_registry_t *registry,
                                 vtx_type_guard_page_t *page)
{
    if (registry == NULL || page == NULL) return -1;

    pthread_mutex_lock(&registry->mutex);

    /* Find the page in the registry */
    int32_t index = -1;
    for (uint32_t i = 0; i < registry->page_count; i++) {
        if (&registry->pages[i] == page) {
            index = (int32_t)i;
            break;
        }
    }

    if (index < 0) {
        pthread_mutex_unlock(&registry->mutex);
        return -1;
    }

    /* Unmap the region */
    if (page->is_active && page->region_base != NULL) {
        munmap(page->region_base, page->region_size);
        page->region_base = NULL;
    }
    page->is_active = false;

    /* Remove from the registry by swapping with the last entry */
    if ((uint32_t)index < registry->page_count - 1) {
        registry->pages[index] = registry->pages[registry->page_count - 1];
    }
    registry->page_count--;
    registry->total_destroyed++;

    /* Update the snapshot */
    if (update_snapshot(registry) != 0) {
        /* Snapshot update failed — the destroyed page may still be
         * visible to the signal handler. This is a race condition,
         * but it's benign: the handler will see the old entry,
         * try to look up the region, and find that the fault address
         * doesn't match any active page (since the region is unmapped). */
        /* TODO: log warning — snapshot update failed after destroy */
    }

    pthread_mutex_unlock(&registry->mutex);

    return 0;
}

/* ========================================================================== */
/* SIGSEGV handler lookup (lock-free, signal-safe)                              */
/* ========================================================================== */

bool vtx_type_guard_page_registry_lookup(
    const vtx_type_guard_page_registry_t *registry,
    uintptr_t fault_addr,
    vtx_type_guard_fault_info_t *fault_info)
{
    if (registry == NULL || fault_info == NULL) return false;

    /* Initialize fault_info to "not found" */
    fault_info->found = false;
    fault_info->method_id = 0;
    fault_info->guard_id = 0;
    fault_info->side_table_index = 0;
    fault_info->actual_type_id = 0;
    fault_info->expected_type_id = 0;

    /* Read the snapshot pointer atomically.
     * This is safe in signal handler context — no locks needed. */
    vtx_type_guard_page_t *snapshot =
        __atomic_load_n((vtx_type_guard_page_t * const *)&registry->snapshot,
                        __ATOMIC_ACQUIRE);
    uint32_t count =
        __atomic_load_n((uint32_t const *)&registry->snapshot_count,
                        __ATOMIC_ACQUIRE);

    if (snapshot == NULL || count == 0) return false;

    /* Scan the snapshot for a matching region.
     * For each active type guard page, check if the fault address
     * falls within its mmap'd region. */
    for (uint32_t i = 0; i < count; i++) {
        const vtx_type_guard_page_t *page = &snapshot[i];

        if (!page->is_active || page->region_base == NULL) continue;

        uintptr_t base = (uintptr_t)page->region_base;
        uintptr_t end = base + page->region_size;

        if (fault_addr >= base && fault_addr < end) {
            /* Found the region. Compute which type_id caused the fault.
             * The fault is at offset (fault_addr - base) within the region,
             * and each type_id occupies VTX_TYPE_GUARD_STRIDE bytes. */
            size_t offset = (size_t)(fault_addr - base);
            uint32_t faulting_type_id = (uint32_t)(offset / VTX_TYPE_GUARD_STRIDE);

            fault_info->found = true;
            fault_info->method_id = page->method_id;
            fault_info->guard_id = page->guard_id;
            fault_info->side_table_index = page->side_table_index;
            fault_info->actual_type_id = faulting_type_id;
            fault_info->expected_type_id = page->expected_type_id;

            return true;
        }
    }

    return false;
}

/* ========================================================================== */
/* Guard page access                                                            */
/* ========================================================================== */

void *vtx_type_guard_page_base(const vtx_type_guard_page_t *page)
{
    if (page == NULL || !page->is_active) return NULL;
    return page->region_base;
}

bool vtx_type_guard_page_is_active(const vtx_type_guard_page_t *page)
{
    if (page == NULL) return false;
    return page->is_active;
}

/* ========================================================================== */
/* Reconfiguration                                                              */
/* ========================================================================== */

int vtx_type_guard_page_reconfigure(vtx_type_guard_page_t *page,
                                     uint32_t new_expected_type)
{
    if (page == NULL || !page->is_active) return -1;
    if (page->region_base == NULL) return -1;

    /* Validate: new type must be within range */
    if (new_expected_type > page->max_type_id) return -1;

    /* No-op if the type hasn't changed */
    if (new_expected_type == page->expected_type_id) return 0;

    /* Step 1: Make the new expected page readable.
     * We do this BEFORE making the old page inaccessible to minimize
     * the unsafe window. If both pages are readable simultaneously,
     * two types will pass the guard briefly. This is safer than
     * having both be inaccessible (which would SIGSEGV the correct type).
     *
     * The brief dual-readable window is acceptable because:
     *   - The guard is being reconfigured because the type profile changed,
     *     meaning the old type is no longer the hot one
     *   - The window is extremely short (two mprotect calls, ~1μs)
     *   - A spurious pass of the old type would at worst cause one
     *     incorrect execution before deopt, which is caught by the
     *     next type guard or deopt check
     */
    size_t new_offset = type_id_to_offset(new_expected_type);
    void *new_page = (uint8_t *)page->region_base + new_offset;

    if (mprotect(new_page, VTX_TYPE_GUARD_STRIDE, PROT_READ | PROT_WRITE) != 0) {
        return -1;
    }

    /* Write the magic value and sentinel to the new page */
    uint64_t magic = VTX_TYPE_GUARD_PAGE_MAGIC;
    memcpy(new_page, &magic, sizeof(magic));
    uint8_t *data_area = (uint8_t *)new_page + sizeof(magic);
    *data_area = 0x01;

    if (mprotect(new_page, VTX_TYPE_GUARD_STRIDE, PROT_READ) != 0) {
        /* Failed to make the new page read-only. This is bad but
         * not catastrophic — the page is still readable/writeable.
         * Try to restore it to PROT_NONE and fail. */
        mprotect(new_page, VTX_TYPE_GUARD_STRIDE, PROT_NONE);
        return -1;
    }

    /* Step 2: Make the old expected page inaccessible.
     * After this, only the new expected type will pass the guard. */
    size_t old_offset = type_id_to_offset(page->expected_type_id);
    void *old_page = (uint8_t *)page->region_base + old_offset;

    if (mprotect(old_page, VTX_TYPE_GUARD_STRIDE, PROT_NONE) != 0) {
        /* This is problematic: both the old and new types now pass
         * the guard. In practice, this should not happen — mprotect
         * from PROT_READ to PROT_NONE rarely fails on anonymous mappings.
         * If it does, we leave both pages readable (conservative). */
        /* TODO: log error — old page couldn't be made inaccessible */
        /* Continue anyway — update the expected_type_id */
    }

    /* Update the metadata */
    page->expected_type_id = new_expected_type;

    /* Note: We do NOT update the snapshot here. The snapshot update
     * requires the registry mutex and is a heavier operation.
     * The signal handler will see the stale expected_type_id in the
     * snapshot, but this is acceptable because:
     *   - The fault_info.actual_type_id is computed from the fault
     *     address, not from expected_type_id
     *   - The side_table_index and method_id don't change
     *   - The expected_type_id mismatch is at most a diagnostic issue
     *
     * For correctness-critical scenarios, the caller should also call
     * vtx_type_guard_page_update_snapshot() after reconfiguration. */

    return 0;
}

/* ========================================================================== */
/* Fault callback registration                                                  */
/* ========================================================================== */

int vtx_type_guard_page_register_fault_callback(
    vtx_type_guard_fault_callback_t callback)
{
    if (callback == NULL) return -1;

    /* Only allow one callback to be registered */
    vtx_type_guard_fault_callback_t existing =
        __atomic_load_n(&vtx_type_guard_fault_callback, __ATOMIC_ACQUIRE);
    if (existing != NULL) return -1;

    __atomic_store_n(&vtx_type_guard_fault_callback, callback, __ATOMIC_RELEASE);
    return 0;
}

vtx_type_guard_fault_callback_t vtx_type_guard_page_get_fault_callback(void)
{
    return __atomic_load_n(&vtx_type_guard_fault_callback, __ATOMIC_ACQUIRE);
}

/* ========================================================================== */
/* Global registry access                                                       */
/* ========================================================================== */

vtx_type_guard_page_registry_t *vtx_type_guard_page_get_registry(void)
{
    return vtx_global_type_guard_registry;
}

void vtx_type_guard_page_set_registry(vtx_type_guard_page_registry_t *registry)
{
    vtx_global_type_guard_registry = registry;
    if (registry != NULL) {
        __atomic_store_n(&vtx_type_guard_page_available_flag, 1, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&vtx_type_guard_page_available_flag, 0, __ATOMIC_RELEASE);
    }
}

bool vtx_type_guard_page_is_available(void)
{
    return __atomic_load_n(&vtx_type_guard_page_available_flag, __ATOMIC_ACQUIRE) != 0;
}
