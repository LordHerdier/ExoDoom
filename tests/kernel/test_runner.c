/*
 * test_runner.c — Top-level test entry point for kernel test builds.
 *
 * Registers all test suites and runs them via KUnit.  Called from
 * kernel_main when the kernel is compiled with -DTESTING.
 *
 * Returns 0 if all tests pass, 1 if any test fails.
 */

#include "kunit.h"

/* Suite registration functions defined in their respective test files. */
void suite_smoke_tests (CU_pSuite s);
void suite_string_tests(CU_pSuite s);
void suite_ctype_tests (CU_pSuite s);
void suite_memory_tests(CU_pSuite s);
void suite_timer_tests (CU_pSuite s);

int run_tests(void)
{
    CU_pSuite s;

    CU_initialize_registry();

    s = CU_add_suite("smoke",  NULL, NULL);
    suite_smoke_tests(s);

    s = CU_add_suite("string", NULL, NULL);
    suite_string_tests(s);

    s = CU_add_suite("ctype",  NULL, NULL);
    suite_ctype_tests(s);

    s = CU_add_suite("memory", NULL, NULL);
    suite_memory_tests(s);

    s = CU_add_suite("timer",  NULL, NULL);
    suite_timer_tests(s);

    /* ADD NEW SUITES HERE: declare suite_*_tests above, then register it. */

    CU_run_all_tests();

    return CU_get_number_of_tests_failed() != 0 ? 1 : 0;
}
