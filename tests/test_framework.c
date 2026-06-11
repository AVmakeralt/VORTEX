#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

/* ========================================================================== */
/* Test registry                                                               */
/* ========================================================================== */

#define VTX_TEST_MAX_REGISTRATIONS 256

typedef struct {
    vtx_test_fn fn;
    const char *name;
} vtx_test_entry_t;

static vtx_test_entry_t vtx_test_registry[VTX_TEST_MAX_REGISTRATIONS];
static uint32_t vtx_test_registry_count = 0;

/* ========================================================================== */
/* Throw infrastructure                                                        */
/* ========================================================================== */

jmp_buf vtx_test_throw_jmp;
volatile int vtx_test_thrown_flag = 0;

void vtx_test_throw(void)
{
    vtx_test_thrown_flag = 1;
    longjmp(vtx_test_throw_jmp, 1);
}

/* ========================================================================== */
/* Failure tracking                                                            */
/* ========================================================================== */

static uint32_t vtx_test_current_failures = 0;

void vtx_test_record_failure(const char *file, int line, const char *message)
{
    vtx_test_current_failures++;
    fprintf(stderr, "  FAIL: %s:%d: %s\n", file, line, message);
}

uint32_t vtx_test_failure_count(void)
{
    return vtx_test_current_failures;
}

void vtx_test_reset_failures(void)
{
    vtx_test_current_failures = 0;
}

/* ========================================================================== */
/* Registration                                                                */
/* ========================================================================== */

void vtx_test_register(vtx_test_fn fn, const char *name)
{
    if (vtx_test_registry_count >= VTX_TEST_MAX_REGISTRATIONS) {
        fprintf(stderr, "vtx_test_register: too many tests (max %d)\n",
                VTX_TEST_MAX_REGISTRATIONS);
        abort();
    }
    vtx_test_registry[vtx_test_registry_count].fn   = fn;
    vtx_test_registry[vtx_test_registry_count].name = name;
    vtx_test_registry_count++;
}

/* ========================================================================== */
/* Test runner                                                                 */
/* ========================================================================== */

vtx_test_result_t vtx_test_run_all(void)
{
    vtx_test_result_t result = {0, 0, 0};

    printf("Running %u tests...\n\n", vtx_test_registry_count);

    for (uint32_t i = 0; i < vtx_test_registry_count; i++) {
        vtx_test_reset_failures();

        printf("  [%3u/%3u] %-50s ", i + 1, vtx_test_registry_count,
               vtx_test_registry[i].name);
        fflush(stdout);

        vtx_test_registry[i].fn();

        if (vtx_test_failure_count() == 0) {
            printf("PASS\n");
            result.pass_count++;
        } else {
            printf("FAIL (%u assertions)\n", vtx_test_failure_count());
            result.fail_count++;
        }
        result.total_count++;
    }

    printf("\n========================================\n");
    printf("Results: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    printf("========================================\n");

    return result;
}
