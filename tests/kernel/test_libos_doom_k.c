/*
 * test_libos_doom_k.c — the real Doom LibOS blob builds (SCRUM-66).
 *
 * Same shape and same reasoning as test_libos_snake_k.c and
 * test_shell_libos_k.c: this does NOT launch libos_doom_main(). Doom's
 * D_DoomLoop() never returns, so driving it through libos_test_launch()
 * (tests/kernel/libos_test_common.h) would hang the suite forever waiting
 * for a libos_return() that is never coming.
 *
 * What it does verify automatically on every `make docker-test`/CI run is
 * the half that is worth guarding: the exact same
 * _binary_libos_doom_{code,data}_bin_* blobs src/syscall_launch.c embeds
 * link, still fit the LibOS-window budget, and build into a real address
 * space through libos_build_image() without error.
 *
 * That budget check is the point of this file. Doom is what forced
 * LIBOS_LAUNCH_MAX_{CODE,DATA}_PAGES from 8/16 up to 192/192
 * (src/libos_launch.h), and it sits at roughly half of each: ~97 pages of
 * code+rodata against 192, ~99 pages of data+bss against 192. Those are
 * numbers that move whenever the engine, the libc shim or the compiler
 * flags change, and the failure mode if they cross the cap is
 * libos_build_image() returning VMM_EINVAL from inside a syscall handler --
 * i.e. `doom: launch failed` on the shell's console and nothing else. This
 * suite turns that into a named test failure in CI instead.
 *
 * The other half -- that the image actually runs -- is what
 * `make docker-run-kernel` plus the shell's `doom` command is for, and what
 * SCRUM-77 (DG_DrawFrame) and SCRUM-79 (DG_GetKey) have to land before
 * there is anything to see.
 */

#include "kunit.h"
#include "libos_launch.h"
#include "vmm.h"
#include "page_alloc.h"

#include "libos_doom/libos_doom_layout.h"

#include <stdint.h>
#include <stddef.h>

/* PAGE_OWNER_LIBOS for the same reason test_libos_snake_k.c uses it: this
 * is a build-only rehearsal of the real sys_launch_doom() call, which runs
 * under a context_create()-allocated id. */
static uint64_t saved_libos_pml4;

int libos_doom_suite_init(void) {
    saved_libos_pml4 = vmm_address_space_for(PAGE_OWNER_LIBOS);
    return 0;
}

int libos_doom_suite_cleanup(void) {
    if (saved_libos_pml4 != 0)
        vmm_bind_address_space(PAGE_OWNER_LIBOS, saved_libos_pml4);
    return 0;
}

extern const uint8_t _binary_libos_doom_code_bin_start[];
extern const uint8_t _binary_libos_doom_code_bin_end[];
extern const uint8_t _binary_libos_doom_data_bin_start[];
extern const uint8_t _binary_libos_doom_data_bin_end[];

static void test_doom_image_builds_and_fits_budget(void)
{
    size_t code_len = (size_t)(_binary_libos_doom_code_bin_end -
                               _binary_libos_doom_code_bin_start);
    size_t data_len = (size_t)(_binary_libos_doom_data_bin_end -
                               _binary_libos_doom_data_bin_start);

    /* Much larger than any other target's, which is the whole reason the
     * caps moved -- assert it is genuinely the real engine and not an
     * empty or truncated blob. 256 KiB is well above the ~40 KiB the
     * largest previous target (the WAD viewer) produces and well below
     * Doom's own ~390 KiB, so this fails loudly if the blob ever collapses
     * to something that merely links. */
    CU_ASSERT_TRUE(code_len > 256 * 1024);
    CU_ASSERT_TRUE(code_len <= LIBOS_LAUNCH_MAX_CODE_PAGES * 0x1000);
    CU_ASSERT_TRUE(data_len > 0);
    CU_ASSERT_TRUE(data_len + LIBOS_DOOM_BSS_LEN <=
                   LIBOS_LAUNCH_MAX_DATA_PAGES * 0x1000);

    libos_image_t img;
    int build_rc = libos_build_image(PAGE_OWNER_LIBOS,
                                     _binary_libos_doom_code_bin_start, code_len,
                                     _binary_libos_doom_data_bin_start, data_len,
                                     LIBOS_DOOM_BSS_LEN, &img);
    CU_ASSERT_EQUAL(build_rc, VMM_OK);
    if (build_rc != VMM_OK) {
        return;
    }

    CU_ASSERT_EQUAL(img.entry_vaddr, LIBOS_LAUNCH_CODE_VADDR);
    CU_ASSERT_EQUAL(img.stack_top_vaddr, LIBOS_LAUNCH_STACK_TOP);

    /* The multi-page stack SCRUM-66 introduced: every page of it must have
     * been allocated and mapped, not just the first. A single-page stack
     * would still satisfy every assertion above. */
    CU_ASSERT_EQUAL(img.stack_pages, LIBOS_LAUNCH_STACK_PAGES);

    /* The image spans many pages in both regions -- again, the thing that
     * distinguishes this target from every earlier one. */
    CU_ASSERT_TRUE(img.code_pages > 1);
    CU_ASSERT_TRUE(img.data_pages > 1);

    libos_destroy_image(PAGE_OWNER_LIBOS, &img);
}

void suite_libos_doom_tests(CU_pSuite s)
{
    CU_add_test(s, "doom image builds and fits its LibOS-window budget",
               test_doom_image_builds_and_fits_budget);
}
