/*
 * test_libos_launch_k.c -- the real ring-3 launch mechanism (SCRUM-47).
 *
 * Builds a genuine, separate LibOS address space (not the kernel's own map
 * TESTING's RING3_PROBE trick lets tests/kernel/ring3_probe.s get away
 * with), switches CR3 to it, and iretq's into a mapped, user-accessible
 * code page. The probe then touches an address nothing mapped in its own
 * window and the fault is caught cleanly -- proving the launch mechanism
 * itself (real address space, real CPL 3 execution, a fault handled without
 * a triple fault, a clean way back).
 *
 * What this does NOT prove: that kernel memory specifically is off-limits.
 * TESTING builds map the *entire* kernel identity range VMM_USER
 * (src/vmm.c's KERNEL_MAP_USER) so ring3_probe.s and tss_fault_probe.s can
 * run against kernel .text/.bss -- so a kernel-memory read would not fault
 * under this same harness. vmm.c's own comment already names the fix:
 * SCRUM-55/56 tighten KERNEL_MAP_USER back to supervisor-only and assert
 * that wall for real, once nothing still needs the blanket exception. This
 * suite is what makes tightening it safe to attempt.
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
 * PAGE_OWNER_LIBOS + N): +1 (ownership/revoke/page_map/fb_binding), +3 and
 * +4 (vmm). */
#define LIBOS_LAUNCH_TEST_OWNER ((page_owner_t)(PAGE_OWNER_LIBOS + 5))

#define SYS_LIBOS_RETURN 20     /* EXO_SYS_EXIT, borrowed -- see the probe */
#define RESULT_MARKER    0x600DC0DEULL   /* must match libos_launch_probe.s */
#define FAULT_VADDR      (EXO_USER_VA_BASE + 0x20000ULL) /* ditto */

extern void libos_launch_probe(void);
extern void libos_launch_probe_resume(void);
extern void libos_launch_probe_end(void);

static volatile int      hook_calls;
static volatile uint64_t seen_err;
static volatile uint64_t seen_cs;
static volatile uint64_t resume_rip;

static int recording_hook(exception_frame_t *f, uint64_t cr2) {
    (void)cr2;
    hook_calls++;
    seen_err = f->error_code;
    seen_cs  = f->cs;
    f->rip = resume_rip;
    return 1;
}

static void test_ring3_launch_faults_are_caught(void) {
    size_t code_len = (uintptr_t)&libos_launch_probe_end -
                      (uintptr_t)&libos_launch_probe;
    uint64_t resume_offset = (uintptr_t)&libos_launch_probe_resume -
                             (uintptr_t)&libos_launch_probe;

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(LIBOS_LAUNCH_TEST_OWNER,
                                      (const void *)&libos_launch_probe,
                                      code_len, &img),
                   VMM_OK);

    /* The window is fresh (vmm_create_address_space leaves it entirely
     * unmapped) and FAULT_VADDR is well clear of the code/stack pages just
     * mapped, so this is guaranteed not-present regardless of build config
     * -- unlike touching kernel memory, this does not depend on
     * KERNEL_MAP_USER (see the file comment above). */
    CU_ASSERT_EQUAL(vmm_translate_in((uint64_t *)(uintptr_t)img.pml4_phys,
                                     FAULT_VADDR, 0, 0),
                   VMM_ENOENT);

    /* The copied blob's resume label sits at the same offset from its own
     * base as it does in this file's linked copy -- that offset is the one
     * thing that survives the relocation. */
    resume_rip = img.entry_vaddr + resume_offset;

    hook_calls = 0;
    seen_err = seen_cs = 0;
    fault_set_hook(recording_hook);
    exo_syscall_register(SYS_LIBOS_RETURN, libos_return);

    CU_ASSERT_EQUAL(vmm_switch_address_space(img.pml4_phys), VMM_OK);
    uint64_t result = libos_enter(img.entry_vaddr, img.stack_top_vaddr);
    CU_ASSERT_EQUAL(vmm_switch_address_space(vmm_kernel_pml4()), VMM_OK);

    exo_syscall_register(SYS_LIBOS_RETURN, 0);
    fault_set_hook(0);

    /* Reached the resume label and escaped cleanly -- the machine did not
     * triple-fault, and the launched code kept running (at CPL 3, on its
     * own address space) after the fault was caught. */
    CU_ASSERT_EQUAL(result, RESULT_MARKER);
    CU_ASSERT_EQUAL(hook_calls, 1);

    /* The fault really originated at CPL 3, on the address just proven
     * not-present above. */
    CU_ASSERT_EQUAL(seen_cs & 3, 3);
    CU_ASSERT_EQUAL(seen_err & PF_ERR_PRESENT, 0);
    CU_ASSERT_NOT_EQUAL(seen_err & PF_ERR_USER, 0);

    libos_destroy_image(LIBOS_LAUNCH_TEST_OWNER, &img);
}

static void test_build_image_rejects_oversized_code(void) {
    unsigned char big[VMM_PAGE_SIZE + 1] = {0};
    libos_image_t img;

    CU_ASSERT_EQUAL(libos_build_image(LIBOS_LAUNCH_TEST_OWNER, big,
                                      sizeof(big), &img),
                   VMM_EINVAL);

    /* Rejected before anything was created -- nothing bound to clean up. */
    CU_ASSERT_EQUAL(vmm_address_space_for(LIBOS_LAUNCH_TEST_OWNER), 0);
}

/* Leaves nothing behind even if an assertion above failed mid-test and
 * skipped its own cleanup -- same reasoning as the fault/tss suites'
 * cleanups. Unlike the normal path (libos_destroy_image, which knows exactly
 * which two pages to free), `img` is out of scope here, so this falls back
 * to page_reclaim_all() to sweep up anything the test's owner id still
 * holds before tearing down the address space. */
int libos_launch_suite_cleanup(void) {
    fault_set_hook(0);
    exo_syscall_register(SYS_LIBOS_RETURN, 0);
    if (vmm_address_space_for(LIBOS_LAUNCH_TEST_OWNER) != 0) {
        vmm_switch_address_space(vmm_kernel_pml4());
        page_reclaim_all(LIBOS_LAUNCH_TEST_OWNER);
        vmm_destroy_address_space(LIBOS_LAUNCH_TEST_OWNER);
    }
    return 0;
}

void suite_libos_launch_tests(CU_pSuite s) {
    CU_add_test(s, "ring3 launch faults are caught",
                test_ring3_launch_faults_are_caught);
    CU_add_test(s, "build_image rejects oversized code",
                test_build_image_rejects_oversized_code);
}
