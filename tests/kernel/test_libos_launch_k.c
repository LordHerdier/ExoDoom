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
#include "libos_test_common.h"
#include "vmm.h"
#include "fault.h"
#include "exo_syscall.h"
#include "syscall.h"
#include "page_alloc.h"
#include "string.h"

#include <stdint.h>

#define LIBOS_LAUNCH_TEST_OWNER TEST_OWNER_LIBOS_LAUNCH

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
                                      code_len, 0, 0, 0, &img),
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
    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, libos_return);

    CU_ASSERT_EQUAL(vmm_switch_address_space(img.pml4_phys), VMM_OK);
    uint64_t result = libos_enter(img.entry_vaddr, img.stack_top_vaddr);
    CU_ASSERT_EQUAL(vmm_switch_address_space(vmm_kernel_pml4()), VMM_OK);

    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, 0);
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

/* Static, not a stack local: this is now up to
 * LIBOS_LAUNCH_MAX_CODE_PAGES * VMM_PAGE_SIZE + 1 bytes (16 KiB+), too big to
 * carry safely on the 16 KiB kernel test stack. */
static unsigned char oversized_code[LIBOS_LAUNCH_MAX_CODE_PAGES * VMM_PAGE_SIZE + 1];

static void test_build_image_rejects_oversized_code(void) {
    libos_image_t img;

    CU_ASSERT_EQUAL(libos_build_image(LIBOS_LAUNCH_TEST_OWNER, oversized_code,
                                      sizeof(oversized_code), 0, 0, 0, &img),
                   VMM_EINVAL);

    /* Rejected before anything was created -- nothing bound to clean up. */
    CU_ASSERT_EQUAL(vmm_address_space_for(LIBOS_LAUNCH_TEST_OWNER), 0);
}

static void test_build_image_rejects_zero_code_len(void) {
    libos_image_t img;

    /* code_len == 0 would otherwise leave entry_vaddr pointing at a page
     * this call never mapped -- libos_enter() would iretq straight into
     * an unmapped page. */
    CU_ASSERT_EQUAL(libos_build_image(LIBOS_LAUNCH_TEST_OWNER,
                                      (const void *)&libos_launch_probe, 0,
                                      0, 0, 0, &img),
                   VMM_EINVAL);

    CU_ASSERT_EQUAL(vmm_address_space_for(LIBOS_LAUNCH_TEST_OWNER), 0);
}

/* Static for the same reason as oversized_code above. */
static unsigned char oversized_data[LIBOS_LAUNCH_MAX_DATA_PAGES * VMM_PAGE_SIZE + 1];

static void test_build_image_rejects_oversized_data(void) {
    size_t code_len = (uintptr_t)&libos_launch_probe_end -
                      (uintptr_t)&libos_launch_probe;
    libos_image_t img;

    CU_ASSERT_EQUAL(libos_build_image(LIBOS_LAUNCH_TEST_OWNER,
                                      (const void *)&libos_launch_probe,
                                      code_len,
                                      oversized_data, sizeof(oversized_data),
                                      0, &img),
                   VMM_EINVAL);

    /* Rejected before anything was created -- nothing bound to clean up. */
    CU_ASSERT_EQUAL(vmm_address_space_for(LIBOS_LAUNCH_TEST_OWNER), 0);
}

/* SCRUM-49: code and data now land at their own defined, fixed virtual
 * addresses with distinct permissions -- prove placement, permissions and
 * that the copy (and the bss zero-fill after it) actually landed, all from
 * ring 0 via the identity map rather than by executing anything. */
static void test_build_image_places_code_and_data(void) {
    static const unsigned char sample_data[] = "SCRUM-49 data segment";
    size_t code_len = (uintptr_t)&libos_launch_probe_end -
                      (uintptr_t)&libos_launch_probe;

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(LIBOS_LAUNCH_TEST_OWNER,
                                      (const void *)&libos_launch_probe,
                                      code_len,
                                      sample_data, sizeof(sample_data),
                                      64, &img),
                   VMM_OK);

    uint64_t *pml4 = (uint64_t *)(uintptr_t)img.pml4_phys;
    uint64_t paddr, flags;

    CU_ASSERT_EQUAL(vmm_translate_in(pml4, LIBOS_LAUNCH_CODE_VADDR,
                                     &paddr, &flags), VMM_OK);
    CU_ASSERT_EQUAL(paddr, img.code_paddrs[0]);
    CU_ASSERT_EQUAL(flags & VMM_WRITE, 0);

    CU_ASSERT_EQUAL(vmm_translate_in(pml4, LIBOS_LAUNCH_DATA_VADDR,
                                     &paddr, &flags), VMM_OK);
    CU_ASSERT_EQUAL(paddr, img.data_paddrs[0]);
    CU_ASSERT_NOT_EQUAL(flags & VMM_WRITE, 0);

    /* Still identity-mapped for the kernel, so the physical address doubles
     * as a valid ring-0 pointer to check what actually landed there. */
    unsigned char *data_page = (unsigned char *)(uintptr_t)paddr;
    CU_ASSERT_EQUAL(memcmp(data_page, sample_data, sizeof(sample_data)), 0);
    CU_ASSERT_EQUAL(data_page[sizeof(sample_data)], 0);   /* bss tail */

    libos_destroy_image(LIBOS_LAUNCH_TEST_OWNER, &img);
}

/* SCRUM-175: libos_launch_patch_params() overwrites the first `len` bytes of
 * a built image's .data region in place -- the generalized replacement for a
 * per-app side-channel params page. Prove it lands exactly at offset 0 of
 * img.data_paddrs[0], and that bytes past `len` (the rest of the original
 * `data` blob) are left untouched. */
static void test_patch_params_overwrites_data_front(void) {
    static const unsigned char sample_data[] = "0123456789ABCDEF";
    static const uint32_t patch = 0xDEADBEEFu;
    size_t code_len = (uintptr_t)&libos_launch_probe_end -
                      (uintptr_t)&libos_launch_probe;

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(LIBOS_LAUNCH_TEST_OWNER,
                                      (const void *)&libos_launch_probe,
                                      code_len,
                                      sample_data, sizeof(sample_data),
                                      0, &img),
                   VMM_OK);

    CU_ASSERT_EQUAL(libos_launch_patch_params(&img, &patch, sizeof(patch)),
                   VMM_OK);

    unsigned char *data_page = (unsigned char *)(uintptr_t)img.data_paddrs[0];
    CU_ASSERT_EQUAL(memcmp(data_page, &patch, sizeof(patch)), 0);
    CU_ASSERT_EQUAL(memcmp(data_page + sizeof(patch),
                          sample_data + sizeof(patch),
                          sizeof(sample_data) - sizeof(patch)), 0);

    libos_destroy_image(LIBOS_LAUNCH_TEST_OWNER, &img);
}

/* An image built with no data/bss at all (data_pages == 0) has nothing at
 * LIBOS_LAUNCH_DATA_VADDR to patch. */
static void test_patch_params_rejects_no_data_region(void) {
    size_t code_len = (uintptr_t)&libos_launch_probe_end -
                      (uintptr_t)&libos_launch_probe;
    static const uint32_t patch = 0xDEADBEEFu;

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(LIBOS_LAUNCH_TEST_OWNER,
                                      (const void *)&libos_launch_probe,
                                      code_len, 0, 0, 0, &img),
                   VMM_OK);

    CU_ASSERT_EQUAL(libos_launch_patch_params(&img, &patch, sizeof(patch)),
                   VMM_EINVAL);

    libos_destroy_image(LIBOS_LAUNCH_TEST_OWNER, &img);
}

/* A patch longer than one page can never fit at img.data_paddrs[0] -- there
 * is no second page to spill into, and libos_launch_patch_params() only ever
 * touches the first data page. */
static void test_patch_params_rejects_oversized_len(void) {
    static const unsigned char sample_data[] = "0123456789ABCDEF";
    size_t code_len = (uintptr_t)&libos_launch_probe_end -
                      (uintptr_t)&libos_launch_probe;

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(LIBOS_LAUNCH_TEST_OWNER,
                                      (const void *)&libos_launch_probe,
                                      code_len,
                                      sample_data, sizeof(sample_data),
                                      0, &img),
                   VMM_OK);

    unsigned char oversized[VMM_PAGE_SIZE + 1] = {0};
    CU_ASSERT_EQUAL(libos_launch_patch_params(&img, oversized,
                                              sizeof(oversized)),
                   VMM_EINVAL);

    libos_destroy_image(LIBOS_LAUNCH_TEST_OWNER, &img);
}

/* Leaves nothing behind even if an assertion above failed mid-test and
 * skipped its own cleanup -- see libos_test_common.h. Unlike the normal path
 * (libos_destroy_image, which knows exactly which pages to free), `img` is
 * out of scope here, so the shared teardown falls back to page_reclaim_all()
 * to sweep up anything the test's owner id still holds. */
int libos_launch_suite_cleanup(void) {
    libos_test_teardown_owner(LIBOS_LAUNCH_TEST_OWNER);
    return 0;
}

void suite_libos_launch_tests(CU_pSuite s) {
    CU_add_test(s, "ring3 launch faults are caught",
                test_ring3_launch_faults_are_caught);
    CU_add_test(s, "build_image rejects oversized code",
                test_build_image_rejects_oversized_code);
    CU_add_test(s, "build_image rejects zero code_len",
                test_build_image_rejects_zero_code_len);
    CU_add_test(s, "build_image rejects oversized data",
                test_build_image_rejects_oversized_data);
    CU_add_test(s, "build_image places code and data at fixed addresses",
                test_build_image_places_code_and_data);
    CU_add_test(s, "patch_params overwrites data front",
                test_patch_params_overwrites_data_front);
    CU_add_test(s, "patch_params rejects image with no data region",
                test_patch_params_rejects_no_data_region);
    CU_add_test(s, "patch_params rejects oversized len",
                test_patch_params_rejects_oversized_len);
}
