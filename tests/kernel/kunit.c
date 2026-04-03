/*
 * kunit.c — Bare-metal CUnit-compatible test framework implementation.
 *
 * Uses static storage only (no heap allocation) and routes all output through
 * the COM1 serial driver.  The entire registry lives in BSS and is zeroed by
 * the multiboot loader before kernel_main is called.
 */

#include "kunit.h"
#include "serial.h"
#include "string.h"

/* ---- Internal struct definitions --------------------------------------- */

struct _CU_Test {
    char        name[KUNIT_NAME_LEN];
    CU_TestFunc func;
    unsigned    assertions;
    unsigned    failures;
};

struct _CU_Suite {
    char              name[KUNIT_NAME_LEN];
    CU_InitializeFunc init;
    CU_CleanupFunc    cleanup;
    CU_Test           tests[KUNIT_MAX_TESTS_PER_SUITE];
    unsigned          num_tests;
};

typedef struct {
    CU_Suite suites[KUNIT_MAX_SUITES];
    unsigned num_suites;
    int      initialized;
} CU_Registry;

/* ---- Global state (all in .bss, zero-initialized at boot) -------------- */

static CU_Registry  g_registry;
static CU_Test     *g_current_test = NULL;
static CU_ErrorCode g_last_error   = CUE_SUCCESS;
static unsigned     g_total_assertions = 0;
static unsigned     g_total_failures   = 0;
static unsigned     g_tests_run        = 0;
static unsigned     g_tests_failed     = 0;

/* ---- Helpers ----------------------------------------------------------- */

/* Print an unsigned integer to COM1 serial. */
static void print_uint(unsigned n)
{
    char buf[12];
    int i = 11;

    buf[i] = '\0';
    if (n == 0) {
        serial_putc('0');
        return;
    }
    while (n > 0) {
        buf[--i] = (char)('0' + (n % 10));
        n /= 10;
    }
    serial_print(buf + i);
}

/* Copy src into dst, writing at most (max-1) bytes and always NUL-terminating. */
static void kunit_strncpy(char *dst, const char *src, unsigned max)
{
    unsigned i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ---- Public API -------------------------------------------------------- */

void CU_initialize_registry(void)
{
    g_registry.num_suites  = 0;
    g_registry.initialized = 1;
    g_total_assertions = 0;
    g_total_failures   = 0;
    g_tests_run        = 0;
    g_tests_failed     = 0;
    g_last_error       = CUE_SUCCESS;
    g_current_test     = NULL;
}

CU_ErrorCode CU_get_error(void) { return g_last_error; }

CU_pSuite CU_add_suite(const char *name,
                       CU_InitializeFunc init,
                       CU_CleanupFunc cleanup)
{
    CU_Suite *s;

    if (!g_registry.initialized ||
        g_registry.num_suites >= KUNIT_MAX_SUITES) {
        g_last_error = CUE_NOMEMORY;
        return NULL;
    }
    s = &g_registry.suites[g_registry.num_suites++];
    kunit_strncpy(s->name, name, KUNIT_NAME_LEN);
    s->init      = init;
    s->cleanup   = cleanup;
    s->num_tests = 0;
    return s;
}

CU_pTest CU_add_test(CU_pSuite suite,
                     const char *name,
                     CU_TestFunc func)
{
    CU_Test *t;

    if (!suite || suite->num_tests >= KUNIT_MAX_TESTS_PER_SUITE) {
        g_last_error = CUE_NOMEMORY;
        return NULL;
    }
    t = &suite->tests[suite->num_tests++];
    kunit_strncpy(t->name, name, KUNIT_NAME_LEN);
    t->func       = func;
    t->assertions = 0;
    t->failures   = 0;
    return t;
}

void _kunit_assert(int pass, const char *expr,
                   const char *file, int line)
{
    if (!g_current_test) return;

    g_current_test->assertions++;
    g_total_assertions++;

    if (!pass) {
        g_current_test->failures++;
        g_total_failures++;
        serial_print("    ASSERT FAILED: ");
        serial_print(expr);
        serial_print(" (");
        serial_print(file);
        serial_print(":");
        print_uint((unsigned)line);
        serial_print(")\n");
    }
}

CU_ErrorCode CU_run_all_tests(void)
{
    unsigned si, ti;

    if (!g_registry.initialized) {
        g_last_error = CUE_NOREGISTRY;
        return CUE_NOREGISTRY;
    }

    serial_print("\n=== KUnit Test Runner ===\n\n");

    for (si = 0; si < g_registry.num_suites; si++) {
        CU_Suite *suite = &g_registry.suites[si];

        serial_print("Suite: ");
        serial_print(suite->name);
        serial_print("\n");

        if (suite->init && suite->init() != 0) {
            serial_print("  [ERROR] Suite init failed, skipping\n");
            g_last_error = CUE_SUITE_INIT_FAILED;
            continue;
        }

        for (ti = 0; ti < suite->num_tests; ti++) {
            CU_Test *test = &suite->tests[ti];

            test->assertions = 0;
            test->failures   = 0;
            g_current_test   = test;

            test->func();

            g_tests_run++;
            if (test->failures > 0) {
                g_tests_failed++;
                serial_print("  [FAIL] ");
            } else {
                serial_print("  [PASS] ");
            }
            serial_print(test->name);
            serial_print("\n");
        }

        if (suite->cleanup)
            suite->cleanup();
    }

    g_current_test = NULL;

    /* Summary */
    serial_print("\n=== Summary ===\n");
    serial_print("Suites:     "); print_uint(g_registry.num_suites); serial_print("\n");
    serial_print("Tests run:  "); print_uint(g_tests_run);           serial_print("\n");
    serial_print("Assertions: "); print_uint(g_total_assertions);    serial_print("\n");
    serial_print("Failures:   "); print_uint(g_total_failures);      serial_print("\n");
    serial_print("\n");

    if (g_tests_failed == 0) {
        serial_print("ALL TESTS PASSED\n");
    } else {
        serial_print("TESTS FAILED: ");
        print_uint(g_tests_failed);
        serial_print(" test(s) failed\n");
    }

    return CUE_SUCCESS;
}

unsigned CU_get_number_of_tests_run(void)
{
    return g_tests_run;
}

unsigned CU_get_number_of_tests_failed(void)
{
    return g_tests_failed;
}

unsigned CU_get_number_of_assertions(void)
{
    return g_total_assertions;
}

unsigned CU_get_number_of_assertion_failures(void)
{
    return g_total_failures;
}
