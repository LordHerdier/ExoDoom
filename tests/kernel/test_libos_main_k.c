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
 */

#include "kunit.h"
#include "libos_launch.h"
#include "vmm.h"
#include "fault.h"
#include "exo_syscall.h"
#include "syscall.h"
#include "page_alloc.h"

#include <stdint.h>

/* Distinct from every other suite's scratch owner id (page_alloc.h /
 * PAGE_OWNER_LIBOS + N): +5 is test_libos_launch_k.c's. */
#define LIBOS_MAIN_TEST_OWNER ((page_owner_t)(PAGE_OWNER_LIBOS + 6))

#define SYS_LIBOS_RETURN 20     /* EXO_SYS_EXIT, borrowed -- see the probe */

extern void libos_main_probe(void);
extern void libos_main_probe_end(void);
extern const char libos_main_probe_data;
extern const char libos_main_probe_data_end;

static volatile int hook_calls;

/* Not expected to fire in the happy path -- registered anyway so a
 * regression (the probe touching something unmapped) reports as a failed
 * assertion instead of a triple fault taking the whole suite down with it. */
static int recording_hook(exception_frame_t *f, uint64_t cr2) {
    (void)f; (void)cr2;
    hook_calls++;
    return 0;
}

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

    /* The probe's fixed DATA_VADDR immediate must agree with what
     * libos_build_image() actually used -- if libos_launch.h's layout ever
     * moves, this catches it here rather than as a mysterious -EXO_EFAULT
     * from inside the probe. */
    CU_ASSERT_EQUAL((uint64_t)LIBOS_LAUNCH_DATA_VADDR,
                   0x400000000000ULL + 0x5000ULL);

    hook_calls = 0;
    fault_set_hook(recording_hook);
    exo_syscall_register(SYS_LIBOS_RETURN, libos_return);

    CU_ASSERT_EQUAL(vmm_switch_address_space(img.pml4_phys), VMM_OK);
    uint64_t result = libos_enter(img.entry_vaddr, img.stack_top_vaddr);
    CU_ASSERT_EQUAL(vmm_switch_address_space(vmm_kernel_pml4()), VMM_OK);

    exo_syscall_register(SYS_LIBOS_RETURN, 0);
    fault_set_hook(0);

    /* No fault: the write landed in the mapped data region, not off the end
     * of it. */
    CU_ASSERT_EQUAL(hook_calls, 0);

    /* The real exo_serial_write result, round-tripped through libos_return()
     * -- not a fixed marker. A launched context calling a real syscall and
     * getting the real answer back is this ticket's whole point. */
    CU_ASSERT_EQUAL(result, (uint64_t)data_len);

    libos_destroy_image(LIBOS_MAIN_TEST_OWNER, &img);
}

/* Leaves nothing behind even if an assertion above failed mid-test --
 * same reasoning as test_libos_launch_k.c's cleanup. */
int libos_main_suite_cleanup(void) {
    fault_set_hook(0);
    exo_syscall_register(SYS_LIBOS_RETURN, 0);
    if (vmm_address_space_for(LIBOS_MAIN_TEST_OWNER) != 0) {
        vmm_switch_address_space(vmm_kernel_pml4());
        page_reclaim_all(LIBOS_MAIN_TEST_OWNER);
        vmm_destroy_address_space(LIBOS_MAIN_TEST_OWNER);
    }
    return 0;
}

void suite_libos_main_tests(CU_pSuite s) {
    CU_add_test(s, "libos_main calls a real syscall and returns its result",
                test_libos_main_calls_real_syscall);
}
