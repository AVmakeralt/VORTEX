#ifndef VORTEX_TEST_FRAMEWORK_H
#define VORTEX_TEST_FRAMEWORK_H

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>

/**
 * VORTEX Minimal Test Framework
 *
 * Provides:
 *   - VTX_TEST(name) macro to define and auto-register test functions
 *   - Assertion macros with file:line reporting
 *   - VTX_ASSERT_THROWS for testing trap/abort conditions via setjmp/longjmp
 *   - Test runner that discovers registered tests and runs them in order
 *   - Pass/fail counts returned from vtx_test_run_all()
 */

/* ========================================================================== */
/* Test function type                                                           */
/* ========================================================================== */

typedef void (*vtx_test_fn)(void);

/* ========================================================================== */
/* Test registration                                                           */
/* ========================================================================== */

/**
 * Register a test function with a name.
 * Called automatically by VTX_TEST(); may also be called manually.
 */
void vtx_test_register(vtx_test_fn fn, const char *name);

/* ========================================================================== */
/* Test runner                                                                  */
/* ========================================================================== */

typedef struct {
    uint32_t pass_count;
    uint32_t fail_count;
    uint32_t total_count;
} vtx_test_result_t;

/**
 * Run all registered tests in registration order.
 * Returns pass/fail/total counts.
 */
vtx_test_result_t vtx_test_run_all(void);

/* ========================================================================== */
/* Internal assertion infrastructure                                            */
/* ========================================================================== */

/**
 * Record a test assertion failure at the given file:line with a message.
 * Does NOT abort the test — the test continues after recording the failure.
 */
void vtx_test_record_failure(const char *file, int line, const char *message);

/**
 * Get the number of failures recorded so far in the current test.
 */
uint32_t vtx_test_failure_count(void);

/**
 * Reset failure counter for a new test.
 */
void vtx_test_reset_failures(void);

/* ========================================================================== */
/* Assertion macros                                                             */
/* ========================================================================== */

#define VTX_ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            vtx_test_record_failure(__FILE__, __LINE__, #expr " expected true"); \
        } \
    } while (0)

#define VTX_ASSERT_FALSE(expr) \
    do { \
        if ((expr)) { \
            vtx_test_record_failure(__FILE__, __LINE__, #expr " expected false"); \
        } \
    } while (0)

#define VTX_ASSERT_EQUAL(a, b) \
    do { \
        if ((a) != (b)) { \
            vtx_test_record_failure(__FILE__, __LINE__, #a " != " #b); \
        } \
    } while (0)

#define VTX_ASSERT_NOT_EQUAL(a, b) \
    do { \
        if ((a) == (b)) { \
            vtx_test_record_failure(__FILE__, __LINE__, #a " == " #b); \
        } \
    } while (0)

#define VTX_ASSERT_NULL(ptr) \
    do { \
        if ((ptr) != NULL) { \
            vtx_test_record_failure(__FILE__, __LINE__, #ptr " expected NULL"); \
        } \
    } while (0)

#define VTX_ASSERT_NOT_NULL(ptr) \
    do { \
        if ((ptr) == NULL) { \
            vtx_test_record_failure(__FILE__, __LINE__, #ptr " expected non-NULL"); \
        } \
    } while (0)

/* ========================================================================== */
/* VTX_ASSERT_THROWS                                                           */
/* ========================================================================== */

/**
 * jmp_buf used by VTX_ASSERT_THROWS to catch calls to vtx_test_throw().
 * Declared extern so tests can link against it.
 */
extern jmp_buf vtx_test_throw_jmp;

/**
 * Flag: set to 1 when vtx_test_throw() is called.
 */
extern volatile int vtx_test_thrown_flag;

/**
 * Called by code under test to signal an exception.
 * longjmps back to the VTX_ASSERT_THROWS handler.
 */
void vtx_test_throw(void);

/**
 * Assert that calling (expr) causes vtx_test_throw() to be invoked.
 * Uses setjmp/longjmp to catch the throw without aborting the process.
 */
#define VTX_ASSERT_THROWS(expr) \
    do { \
        vtx_test_thrown_flag = 0; \
        if (setjmp(vtx_test_throw_jmp) == 0) { \
            (expr); \
            if (!vtx_test_thrown_flag) { \
                vtx_test_record_failure(__FILE__, __LINE__, \
                    #expr " expected to throw but did not"); \
            } \
        } \
    } while (0)

/* ========================================================================== */
/* Test definition macro                                                        */
/* ========================================================================== */

/**
 * Define a test function and auto-register it.
 * Usage:
 *   VTX_TEST(my_test) {
 *       VTX_ASSERT_TRUE(1 == 1);
 *   }
 */
#define VTX_TEST(name) \
    static void vtx_test_##name(void); \
    static void vtx_test_reg_##name(void) __attribute__((constructor)); \
    static void vtx_test_reg_##name(void) { \
        vtx_test_register(vtx_test_##name, #name); \
    } \
    static void vtx_test_##name(void)

#endif /* VORTEX_TEST_FRAMEWORK_H */
