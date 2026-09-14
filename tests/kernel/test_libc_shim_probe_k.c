/*
 * test_libc_shim_probe_k.c — the libc shim works from ring 3 (SCRUM-51).
 *
 * test_libos_c_probe_k.c (SCRUM-173) already proved a compiled function can
 * call one real, bound syscall from ring 3 through the launch mechanism.
 * This suite drives the same mechanism against tests/kernel/libc_shim_probe/,
 * which links in the real src/stdlib.c + src/stdio.c (+ their string.c/
 * ctype.c/libos_heap.c/libos_page_alloc.c dependencies) compiled without
 * -DEXO_KERNEL, and asserts on the bitmask libc_shim_probe_main() hands back
 * through libos_return() -- one bit per acceptance criterion (malloc, printf,
 * timer), not just "it didn't fault".
 *
 * _binary_libc_shim_probe_{code,data}_bin_{start,end} are the objcopy-
 * embedded blobs build.sh's dedicated step produces from the separately-
 * linked libc_shim_probe.elf; LIBC_SHIM_PROBE_BSS_LEN (generated into
 * build/../tests/kernel/libc_shim_probe/libc_shim_probe_layout.h by that same
 * step) is its .bss size.
 */

#include "kunit.h"
#include "libos_launch.h"
#include "libos_test_common.h"
#include "vmm.h"
#include "sleep.h"
#include "pit.h"
#include "libc_shim_probe/libc_shim_probe_layout.h"
#include "libc_shim_probe/libc_shim_probe_result.h"

#include <stdint.h>
#include <stddef.h>

/*
 * PAGE_OWNER_LIBOS, not a TEST_OWNER_* sentinel -- see libos_test_common.h's
 * comment on why this suite is the exception. exo_page_alloc/exo_page_map,
 * called for real from ring 3 by this probe's malloc(), route to "the
 * caller's address space" via syscall_current_context(), which v1's kernel
 * hardcodes to PAGE_OWNER_LIBOS; launching under any other id would make
 * those syscalls map into the kernel's own page tables instead of the one
 * actually loaded in CR3.
 */
#define LIBC_SHIM_PROBE_TEST_OWNER PAGE_OWNER_LIBOS

extern const uint8_t _binary_libc_shim_probe_code_bin_start[];
extern const uint8_t _binary_libc_shim_probe_code_bin_end[];
extern const uint8_t _binary_libc_shim_probe_data_bin_start[];
extern const uint8_t _binary_libc_shim_probe_data_bin_end[];

/*
 * kernel_main binds PAGE_OWNER_LIBOS to vmm_kernel_pml4() ahead of the
 * TESTING branch (src/kernel.c) so exo_page_map/-unmap have something to
 * resolve during every OTHER suite's run. vmm_bind_address_space() rebinds
 * in place rather than refusing a second bind for the same owner, so
 * libos_build_image(PAGE_OWNER_LIBOS, ...) below succeeds -- but it leaves
 * that boot-time binding overwritten, and libos_destroy_image() unbinds it
 * entirely rather than restoring it. Saved here (suite init, before the
 * test runs) and restored in suite cleanup so suites that run after this
 * one (libos_page_alloc, libos_heap, libos_heap_stress -- see
 * test_runner.c) still find PAGE_OWNER_LIBOS resolving to the kernel's map,
 * exactly as they would have if this suite did not exist.
 */
static uint64_t saved_libos_pml4;

int libc_shim_probe_suite_init(void) {
    saved_libos_pml4 = vmm_address_space_for(PAGE_OWNER_LIBOS);
    return 0;
}

static void test_libc_shim_works_from_ring3(void) {
    /*
     * The probe's ticks>0 check needs a real tick to have already landed --
     * ambient IF is not assumed anywhere in this suite (see
     * test_syscall_pit_k.c's file header for why kernel_main leaves it
     * clear by default), and ring-3 code cannot execute `sti`/`hlt` itself
     * (both are CPL-0-only, so the probe would take a GPF instead of
     * sleeping). Enabling interrupts and waiting here, in ring 0, before the
     * launch, is what test_ticks_advance_over_time
     * (tests/kernel/test_syscall_pit_k.c) does for the same reason.
     */
    __asm__ volatile ("sti");
    kernel_sleep_ms(5);
    __asm__ volatile ("cli");
    CU_ASSERT(kernel_get_ticks_ms() > 0);

    size_t code_len = (size_t)(_binary_libc_shim_probe_code_bin_end -
                               _binary_libc_shim_probe_code_bin_start);
    size_t data_len = (size_t)(_binary_libc_shim_probe_data_bin_end -
                               _binary_libc_shim_probe_data_bin_start);

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(LIBC_SHIM_PROBE_TEST_OWNER,
                                      _binary_libc_shim_probe_code_bin_start,
                                      code_len,
                                      _binary_libc_shim_probe_data_bin_start,
                                      data_len, LIBC_SHIM_PROBE_BSS_LEN,
                                      &img),
                   VMM_OK);

    libos_test_launch_result_t run = libos_test_launch(&img);
    CU_ASSERT_EQUAL(run.switch_in_status, VMM_OK);
    CU_ASSERT_EQUAL(run.switch_out_status, VMM_OK);
    CU_ASSERT_EQUAL(run.fault_count, 0);

    CU_ASSERT_EQUAL(run.result & LIBC_SHIM_OK_MALLOC, LIBC_SHIM_OK_MALLOC);
    CU_ASSERT_EQUAL(run.result & LIBC_SHIM_OK_PRINTF, LIBC_SHIM_OK_PRINTF);
    CU_ASSERT_EQUAL(run.result & LIBC_SHIM_OK_TICKS,  LIBC_SHIM_OK_TICKS);
    CU_ASSERT_EQUAL(run.result, (uint64_t)LIBC_SHIM_OK_ALL);

    libos_destroy_image(LIBC_SHIM_PROBE_TEST_OWNER, &img);
}

/*
 * Leaves nothing behind even if an assertion above failed mid-test -- see
 * libos_test_common.h -- and additionally restores PAGE_OWNER_LIBOS's
 * boot-time binding (see the comment above libc_shim_probe_suite_init),
 * which the shared libos_test_teardown_owner() helper has no way to know
 * about: every other suite's TEST_OWNER_* sentinel has no "before" state to
 * put back.
 */
int libc_shim_probe_suite_cleanup(void) {
    libos_test_teardown_owner(LIBC_SHIM_PROBE_TEST_OWNER);
    if (saved_libos_pml4 != 0)
        vmm_bind_address_space(PAGE_OWNER_LIBOS, saved_libos_pml4);
    return 0;
}

void suite_libc_shim_probe_tests(CU_pSuite s) {
    CU_add_test(s, "malloc/printf/timer all work from ring 3 via syscall",
               test_libc_shim_works_from_ring3);
}
