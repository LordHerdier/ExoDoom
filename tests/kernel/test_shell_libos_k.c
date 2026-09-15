/*
 * test_shell_libos_k.c — the real shell LibOS blob builds (SCRUM-110).
 *
 * This does NOT launch shell_main(): unlike every other ring-3 suite in this
 * tree, shell_main() never calls libos_return() -- it is a real interactive
 * idle loop (exo_kbd_poll()/exo_yield() forever), by design, since it is what
 * kernel_main() hands off to for the rest of a normal boot. Driving it
 * through libos_test_launch() (tests/kernel/libos_test_common.h) would hang
 * this suite forever waiting for a return that never comes. What this DOES
 * verify automatically, on every `make docker-test`/CI run: the exact same
 * _binary_shell_{code,data}_bin_* blobs kernel_main() embeds
 * (docker/scripts/build.sh's unconditional build_ring3_link_target step)
 * link, fit the LibOS window budget, and build into a real address space via
 * libos_build_image() without error -- catching a build-time regression
 * (an oversized blob, a missing symbol, a linker-script mismatch) the same
 * way test_libc_shim_probe_k.c catches one for its own target, just without
 * the run. The other, genuinely interactive half (typing, backspace editing,
 * command dispatch, on-screen output) is verified manually via
 * `make docker-run-kernel` -- see the SCRUM-110 plan for why an automated
 * interactive test was judged not worth the complexity of inventing a
 * TESTING-only return seam for what is deliberately a never-returning loop.
 */

#include "kunit.h"
#include "libos_launch.h"
#include "vmm.h"
#include "page_alloc.h"

#include "shell/shell_layout.h"

#include <stdint.h>
#include <stddef.h>

/*
 * PAGE_OWNER_LIBOS, not a TEST_OWNER_* sentinel -- see
 * tests/kernel/libos_test_common.h's comment on why test_libc_shim_probe_k.c
 * is the one other exception to that rule. This suite doesn't launch the
 * image (so syscall_current_context() never actually matters here), but
 * kernel_main() itself builds the real shell under PAGE_OWNER_LIBOS
 * (src/kernel.c, SCRUM-110), and matching that is what makes this test a
 * faithful rehearsal of the real boot-time call rather than a different one
 * that happens to also succeed.
 */
static uint64_t saved_libos_pml4;

int shell_libos_suite_init(void) {
    saved_libos_pml4 = vmm_address_space_for(PAGE_OWNER_LIBOS);
    return 0;
}

int shell_libos_suite_cleanup(void) {
    if (saved_libos_pml4 != 0)
        vmm_bind_address_space(PAGE_OWNER_LIBOS, saved_libos_pml4);
    return 0;
}

extern const uint8_t _binary_shell_code_bin_start[];
extern const uint8_t _binary_shell_code_bin_end[];
extern const uint8_t _binary_shell_data_bin_start[];
extern const uint8_t _binary_shell_data_bin_end[];

static void test_shell_image_builds_and_fits_budget(void)
{
    size_t code_len = (size_t)(_binary_shell_code_bin_end -
                               _binary_shell_code_bin_start);
    size_t data_len = (size_t)(_binary_shell_data_bin_end -
                               _binary_shell_data_bin_start);

    CU_ASSERT_TRUE(code_len > 0);
    CU_ASSERT_TRUE(code_len <= LIBOS_LAUNCH_MAX_CODE_PAGES * 0x1000);
    CU_ASSERT_TRUE(data_len + SHELL_BSS_LEN <=
                   LIBOS_LAUNCH_MAX_DATA_PAGES * 0x1000);

    libos_image_t img;
    int build_rc = libos_build_image(PAGE_OWNER_LIBOS,
                                     _binary_shell_code_bin_start, code_len,
                                     _binary_shell_data_bin_start, data_len,
                                     SHELL_BSS_LEN, &img);
    CU_ASSERT_EQUAL(build_rc, VMM_OK);
    if (build_rc != VMM_OK) {
        return;
    }

    CU_ASSERT_EQUAL(img.entry_vaddr, LIBOS_LAUNCH_CODE_VADDR);
    CU_ASSERT_EQUAL(img.stack_top_vaddr, LIBOS_LAUNCH_STACK_VADDR + 0x1000);

    libos_destroy_image(PAGE_OWNER_LIBOS, &img);
}

void suite_shell_libos_tests(CU_pSuite s)
{
    CU_add_test(s, "shell image builds and fits its LibOS-window budget",
               test_shell_image_builds_and_fits_budget);
}
