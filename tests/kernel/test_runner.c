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
void suite_stdio_tests   (CU_pSuite s);
void suite_kbd_ring_tests(CU_pSuite s);
void suite_ps2_decode_tests(CU_pSuite s);
void suite_exo_syscall_tests(CU_pSuite s);
void suite_exo_syscall_kview_tests(CU_pSuite s);
void suite_syscall_tests(CU_pSuite s);
void suite_syscall_mem_tests(CU_pSuite s);
void suite_ownership_tests(CU_pSuite s);
void suite_fb_binding_tests(CU_pSuite s);
void suite_vmm_tests(CU_pSuite s);
void suite_revoke_tests(CU_pSuite s);
void suite_page_map_tests(CU_pSuite s);
void suite_fault_tests(CU_pSuite s);
void suite_tss_tests(CU_pSuite s);
void suite_libos_launch_tests(CU_pSuite s);

/* Suite init/cleanup for the framebuffer binding suite: it swaps in a
 * synthetic framebuffer geometry and must put the real one back (SCRUM-154). */
int fb_binding_suite_init(void);
int fb_binding_suite_cleanup(void);

/* Same idea for the revocation suite: it borrows the framebuffer and allocates
 * pages under a second context id, and must leave neither behind (SCRUM-156). */
int revoke_suite_init(void);
int revoke_suite_cleanup(void);

/* The page-map suite borrows the framebuffer binding and a second context id
 * to prove what they refuse; it must leave neither behind (SCRUM-35). */
int page_map_suite_cleanup(void);

/* The fault suite installs a TESTING-only page-fault hook; leaving one
 * installed would make a later genuine fault resume into a stale label
 * instead of reporting (SCRUM-17). */
int fault_suite_cleanup(void);

/* Same idea for the TSS suite: it installs the fault hook and a SYS_ESCAPE
 * handler around its live ring-3 fault test (SCRUM-46). */
int tss_suite_cleanup(void);

/* Same idea for the libos_launch suite: it installs the fault hook, a
 * SYS_LIBOS_RETURN handler, and a real address space around its live
 * ring-3 launch test (SCRUM-47). */
int libos_launch_suite_cleanup(void);

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

    s = CU_add_suite("stdio",  NULL, NULL);
    suite_stdio_tests(s);

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

    s = CU_add_suite("syscall_mem", NULL, NULL);
    suite_syscall_mem_tests(s);

    s = CU_add_suite("ownership", NULL, NULL);
    suite_ownership_tests(s);

    s = CU_add_suite("fb_binding", fb_binding_suite_init,
                     fb_binding_suite_cleanup);
    suite_fb_binding_tests(s);

    s = CU_add_suite("vmm", NULL, NULL);
    suite_vmm_tests(s);

    s = CU_add_suite("revoke", revoke_suite_init, revoke_suite_cleanup);
    suite_revoke_tests(s);

    s = CU_add_suite("page_map", NULL, page_map_suite_cleanup);
    suite_page_map_tests(s);

    s = CU_add_suite("fault", NULL, fault_suite_cleanup);
    suite_fault_tests(s);

    s = CU_add_suite("tss", NULL, tss_suite_cleanup);
    suite_tss_tests(s);

    s = CU_add_suite("libos_launch", NULL, libos_launch_suite_cleanup);
    suite_libos_launch_tests(s);

    /* ADD NEW SUITES HERE: declare suite_*_tests above, then register it. */

    CU_run_all_tests();

    return CU_get_number_of_tests_failed() != 0 ? 1 : 0;
}
