/*
 * test_clock_libos_k.c — the real clock LibOS blob builds (SCRUM-168).
 *
 * Same shape as test_shell_libos_k.c (SCRUM-110): this does NOT launch
 * clock_main() -- like shell_main(), it is a real never-returning loop
 * (a tight poll on exo_get_ticks(), redrawing on a second boundary), by
 * design, and driving it through libos_test_launch()
 * (tests/kernel/libos_test_common.h) would hang this suite forever waiting
 * for a return that never comes. What this DOES verify automatically, on
 * every `make docker-test`/CI run: the exact same
 * _binary_libos_clock_{code,data}_bin_* blobs src/syscall_launch.c embeds
 * (docker/scripts/build.sh's unconditional build_ring3_link_target step)
 * link, fit the LibOS window budget, and build into a real address space via
 * libos_build_image() without error -- catching a build-time regression the
 * same way test_shell_libos_k.c catches one for the shell's own target, just
 * without the run. The interactive half (rendering, timing) is verified
 * manually via `make docker-run-kernel` -- see the SCRUM-168 plan.
 */

#include "kunit.h"
#include "libos_launch.h"
#include "vmm.h"
#include "page_alloc.h"

#include "libos_clock/libos_clock_layout.h"

#include <stdint.h>
#include <stddef.h>

/* PAGE_OWNER_LIBOS, not a TEST_OWNER_* sentinel -- same reasoning as
 * test_shell_libos_k.c's own comment: this suite doesn't launch the image
 * (so syscall_current_context() never actually matters here), but matching
 * the id kernel_main()/src/syscall_launch.c would actually use keeps this a
 * faithful rehearsal rather than a different call that happens to also
 * succeed. */
static uint64_t saved_libos_pml4;

int clock_libos_suite_init(void) {
    saved_libos_pml4 = vmm_address_space_for(PAGE_OWNER_LIBOS);
    return 0;
}

int clock_libos_suite_cleanup(void) {
    if (saved_libos_pml4 != 0)
        vmm_bind_address_space(PAGE_OWNER_LIBOS, saved_libos_pml4);
    return 0;
}

extern const uint8_t _binary_libos_clock_code_bin_start[];
extern const uint8_t _binary_libos_clock_code_bin_end[];
extern const uint8_t _binary_libos_clock_data_bin_start[];
extern const uint8_t _binary_libos_clock_data_bin_end[];

static void test_clock_image_builds_and_fits_budget(void)
{
    size_t code_len = (size_t)(_binary_libos_clock_code_bin_end -
                               _binary_libos_clock_code_bin_start);
    size_t data_len = (size_t)(_binary_libos_clock_data_bin_end -
                               _binary_libos_clock_data_bin_start);

    CU_ASSERT_TRUE(code_len > 0);
    CU_ASSERT_TRUE(code_len <= LIBOS_LAUNCH_MAX_CODE_PAGES * 0x1000);
    CU_ASSERT_TRUE(data_len + LIBOS_CLOCK_BSS_LEN <=
                   LIBOS_LAUNCH_MAX_DATA_PAGES * 0x1000);

    libos_image_t img;
    int build_rc = libos_build_image(PAGE_OWNER_LIBOS,
                                     _binary_libos_clock_code_bin_start, code_len,
                                     _binary_libos_clock_data_bin_start, data_len,
                                     LIBOS_CLOCK_BSS_LEN, &img);
    CU_ASSERT_EQUAL(build_rc, VMM_OK);
    if (build_rc != VMM_OK) {
        return;
    }

    CU_ASSERT_EQUAL(img.entry_vaddr, LIBOS_LAUNCH_CODE_VADDR);
    CU_ASSERT_EQUAL(img.stack_top_vaddr, LIBOS_LAUNCH_STACK_VADDR + 0x1000);

    libos_destroy_image(PAGE_OWNER_LIBOS, &img);
}

void suite_clock_libos_tests(CU_pSuite s)
{
    CU_add_test(s, "clock image builds and fits its LibOS-window budget",
               test_clock_image_builds_and_fits_budget);
}
