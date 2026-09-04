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
void suite_smoke_tests   (CU_pSuite s);
void suite_string_tests  (CU_pSuite s);
void suite_ctype_tests   (CU_pSuite s);
void suite_kbd_ring_tests(CU_pSuite s);
void suite_ps2_decode_tests(CU_pSuite s);
void suite_exo_syscall_tests(CU_pSuite s);
void suite_exo_syscall_kview_tests(CU_pSuite s);
void suite_syscall_tests(CU_pSuite s);
void suite_sys_time_tests(CU_pSuite s);

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

    s = CU_add_suite("kbd_ring", NULL, NULL);
    suite_kbd_ring_tests(s);

    s = CU_add_suite("ps2_decode", NULL, NULL);
    suite_ps2_decode_tests(s);

    s = CU_add_suite("exo_syscall", NULL, NULL);
    suite_exo_syscall_tests(s);

    s = CU_add_suite("exo_syscall_kview", NULL, NULL);
    suite_exo_syscall_kview_tests(s);

    s = CU_add_suite("syscall", NULL, NULL);
    suite_syscall_tests(s);

    /* Keep sys_time last: its final test starts the PIT and enables
     * interrupts, and ring-3 code cannot survive an interrupt until SCRUM-46
     * adds a TSS.  Any suite registered after this one must not enter ring 3. */
    s = CU_add_suite("sys_time", NULL, NULL);
    suite_sys_time_tests(s);

    /* ADD NEW SUITES HERE: declare suite_*_tests above, then register it. */

    CU_run_all_tests();

    return CU_get_number_of_tests_failed() != 0 ? 1 : 0;
}
