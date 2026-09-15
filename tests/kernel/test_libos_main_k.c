/*
 * test_libos_main_k.c -- the libos_main() entry framework (SCRUM-50).
 *
 * test_libos_launch_k.c already proved the launch mechanism itself: a real
 * address space, a real CPL-3 `iretq`, and a fault taken and handled without
 * a triple fault. What that suite does NOT prove is this ticket's actual
 * acceptance criterion -- that code landing at the entry point can call a
 * *real*, bound syscall and get a real answer back, with its data (not just
 * its code) placed by libos_build_image() (SCRUM-49) at a fixed, known
 * address it can reference without any relocation. This suite drives
 * libos_main_probe.s, which does exactly that: it calls exo_serial_write
 * (#8, bound by syscall_serial_init() -- see test_syscall_serial_k.c for
 * that handler's own contract) against the data region and hands the real
 * result back through libos_return(), rather than a fixed marker.
 *
 * Same non-goal as test_libos_launch_k.c: TESTING builds map the entire
 * kernel identity range VMM_USER, so this does not by itself prove a LibOS
 * cannot reach kernel memory (SCRUM-55/-56's job). What it proves is that
 * the whole chain -- ring 3 code, a real syscall, the dispatcher, the
 * handler, and the way back -- works end to end for a launched image built
 * with both code *and* data regions.
 *
 * libos_main_probe.s gets DATA_VADDR the same way this file does -- both are
 * preprocessed against / include the real LIBOS_LAUNCH_DATA_VADDR macro
 * (SCRUM-50, see that file and libos_launch.h) -- so there is nothing here
 * left to cross-check between a hardcoded literal and the header; if the
 * layout ever moves, both sides move with it by construction.
 */

#include "kunit.h"
#include "libos_launch.h"
#include "libos_test_common.h"
#include "vmm.h"

#include <stdint.h>

#define LIBOS_MAIN_TEST_OWNER TEST_OWNER_LIBOS_MAIN

extern void libos_main_probe(void);
extern void libos_main_probe_end(void);
extern const char libos_main_probe_data;
extern const char libos_main_probe_data_end;

static void test_libos_main_calls_real_syscall(void) {
    size_t code_len = (uintptr_t)&libos_main_probe_end -
                      (uintptr_t)&libos_main_probe;
    size_t data_len = (uintptr_t)&libos_main_probe_data_end -
                      (uintptr_t)&libos_main_probe_data;

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(LIBOS_MAIN_TEST_OWNER,
                                      (const void *)&libos_main_probe,
                                      code_len,
                                      (const void *)&libos_main_probe_data,
                                      data_len, 0,
                                      &img),
                   VMM_OK);

    libos_test_launch_result_t run = libos_test_launch(&img);
    CU_ASSERT_EQUAL(run.switch_in_status, VMM_OK);
    CU_ASSERT_EQUAL(run.switch_out_status, VMM_OK);

    /* No fault: the write landed in the mapped data region, not off the end
     * of it. */
    CU_ASSERT_EQUAL(run.fault_count, 0);

    /* The real exo_serial_write result, round-tripped through libos_return()
     * -- not a fixed marker. A launched context calling a real syscall and
     * getting the real answer back is this ticket's whole point. */
    CU_ASSERT_EQUAL(run.result, (uint64_t)data_len);

    libos_destroy_image(LIBOS_MAIN_TEST_OWNER, &img);
}

/* Leaves nothing behind even if an assertion above failed mid-test --
 * see libos_test_common.h. */
int libos_main_suite_cleanup(void) {
    libos_test_teardown_owner(LIBOS_MAIN_TEST_OWNER);
    return 0;
}

void suite_libos_main_tests(CU_pSuite s) {
    CU_add_test(s, "libos_main calls a real syscall and returns its result",
                test_libos_main_calls_real_syscall);
}
