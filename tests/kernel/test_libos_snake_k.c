/*
 * test_libos_snake_k.c — the real Snake LibOS blob builds (SCRUM-182).
 *
 * Same shape and same reasoning as test_shell_libos_k.c: this does NOT
 * launch libos_snake_main() -- like the shell, Snake is a real interactive
 * loop (exo_kbd_poll()/exo_get_ticks() driven, exiting only via
 * EXO_SYS_EXIT), and driving it through libos_test_launch()
 * (tests/kernel/libos_test_common.h) would hang this suite forever waiting
 * for a return that never comes. What this DOES verify automatically, on
 * every `make docker-test`/CI run: the exact same
 * _binary_libos_snake_{code,data}_bin_* blobs src/syscall_launch.c embeds
 * (docker/scripts/build.sh's unconditional build_ring3_link_target step)
 * link, fit the LibOS window budget, and build into a real address space via
 * libos_build_image() without error -- catching a build-time regression
 * (an oversized blob, a missing symbol, a linker-script mismatch) the same
 * way test_shell_libos_k.c / test_libc_shim_probe_k.c catch one for their
 * own targets, just without the run. The interactive half (movement,
 * collision, score, quitting) is verified manually via
 * `make docker-run-kernel`.
 */

#include "kunit.h"
#include "libos_launch.h"
#include "vmm.h"
#include "page_alloc.h"

#include "libos_snake/libos_snake_layout.h"

#include <stdint.h>
#include <stddef.h>

/*
 * PAGE_OWNER_LIBOS, not a TEST_OWNER_* sentinel -- see
 * tests/kernel/libos_test_common.h's comment on why test_libc_shim_probe_k.c
 * is the one other exception to that rule. This suite doesn't launch the
 * image (so syscall_current_context() never actually matters here), but
 * src/syscall_launch.c's sys_launch_snake() builds the real Snake image
 * under a context_create()-allocated id, and PAGE_OWNER_LIBOS is what
 * test_shell_libos_k.c already establishes as this tree's stand-in owner id
 * for a build-only rehearsal of that call.
 */
static uint64_t saved_libos_pml4;

int libos_snake_suite_init(void) {
    saved_libos_pml4 = vmm_address_space_for(PAGE_OWNER_LIBOS);
    return 0;
}

int libos_snake_suite_cleanup(void) {
    if (saved_libos_pml4 != 0)
        vmm_bind_address_space(PAGE_OWNER_LIBOS, saved_libos_pml4);
    return 0;
}

extern const uint8_t _binary_libos_snake_code_bin_start[];
extern const uint8_t _binary_libos_snake_code_bin_end[];
extern const uint8_t _binary_libos_snake_data_bin_start[];
extern const uint8_t _binary_libos_snake_data_bin_end[];

static void test_snake_image_builds_and_fits_budget(void)
{
    size_t code_len = (size_t)(_binary_libos_snake_code_bin_end -
                               _binary_libos_snake_code_bin_start);
    size_t data_len = (size_t)(_binary_libos_snake_data_bin_end -
                               _binary_libos_snake_data_bin_start);

    CU_ASSERT_TRUE(code_len > 0);
    CU_ASSERT_TRUE(code_len <= LIBOS_LAUNCH_MAX_CODE_PAGES * 0x1000);
    CU_ASSERT_TRUE(data_len + LIBOS_SNAKE_BSS_LEN <=
                   LIBOS_LAUNCH_MAX_DATA_PAGES * 0x1000);

    libos_image_t img;
    int build_rc = libos_build_image(PAGE_OWNER_LIBOS,
                                     _binary_libos_snake_code_bin_start, code_len,
                                     _binary_libos_snake_data_bin_start, data_len,
                                     LIBOS_SNAKE_BSS_LEN, &img);
    CU_ASSERT_EQUAL(build_rc, VMM_OK);
    if (build_rc != VMM_OK) {
        return;
    }

    CU_ASSERT_EQUAL(img.entry_vaddr, LIBOS_LAUNCH_CODE_VADDR);
    CU_ASSERT_EQUAL(img.stack_top_vaddr, LIBOS_LAUNCH_STACK_TOP);

    libos_destroy_image(PAGE_OWNER_LIBOS, &img);
}

void suite_libos_snake_tests(CU_pSuite s)
{
    CU_add_test(s, "snake image builds and fits its LibOS-window budget",
               test_snake_image_builds_and_fits_budget);
}
