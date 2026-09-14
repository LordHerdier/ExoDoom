/*
 * test_libos_c_probe_k.c — compiled C running at ring 3 (SCRUM-173).
 *
 * test_libos_main_k.c already proved the launch mechanism can carry a real
 * syscall round trip for hand-written assembly. This suite proves the thing
 * SCRUM-173 actually adds: a *compiled* C function, built and linked by
 * docker/scripts/build.sh's own dedicated step (see libos_c_probe.c and
 * libos_c_probe.ld.in) rather than hand-assembled, launched the same way and
 * driving the same real, bound syscall (exo_serial_write, #8) with its
 * result round-tripped through libos_return() -- not a fixed marker.
 *
 * _binary_libos_c_probe_{code,data}_bin_{start,end} are the objcopy-embedded
 * blobs build.sh produces from the separately-linked libos_c_probe.elf;
 * LIBOS_C_PROBE_BSS_LEN (generated into build/libos_c_probe_layout.h by that
 * same step) is libos_c_probe.c's .bss size, which has no file bytes for
 * objcopy to embed.
 */

#include "kunit.h"
#include "libos_launch.h"
#include "libos_test_common.h"
#include "vmm.h"
#include "libos_c_probe/libos_c_probe_layout.h"

#include <stdint.h>
#include <stddef.h>

#define LIBOS_C_PROBE_TEST_OWNER TEST_OWNER_LIBOS_C_PROBE

extern const uint8_t _binary_libos_c_probe_code_bin_start[];
extern const uint8_t _binary_libos_c_probe_code_bin_end[];
extern const uint8_t _binary_libos_c_probe_data_bin_start[];
extern const uint8_t _binary_libos_c_probe_data_bin_end[];

static void test_compiled_c_calls_real_syscall(void) {
    size_t code_len = (size_t)(_binary_libos_c_probe_code_bin_end -
                               _binary_libos_c_probe_code_bin_start);
    size_t data_len = (size_t)(_binary_libos_c_probe_data_bin_end -
                               _binary_libos_c_probe_data_bin_start);

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(LIBOS_C_PROBE_TEST_OWNER,
                                      _binary_libos_c_probe_code_bin_start,
                                      code_len,
                                      _binary_libos_c_probe_data_bin_start,
                                      data_len, LIBOS_C_PROBE_BSS_LEN,
                                      &img),
                   VMM_OK);

    libos_test_launch_result_t run = libos_test_launch(&img);
    CU_ASSERT_EQUAL(run.switch_in_status, VMM_OK);
    CU_ASSERT_EQUAL(run.switch_out_status, VMM_OK);

    /* No fault: the compiled code's own .data reference (libos_c_probe_msg)
     * resolved correctly at LIBOS_LAUNCH_DATA_VADDR. */
    CU_ASSERT_EQUAL(run.fault_count, 0);

    /* The real exo_serial_write result, round-tripped through
     * libos_return() -- proves the syscall actually ran, not just that
     * libos_enter() didn't fault. libos_c_probe_msg is data_len bytes
     * (including its compiler-inserted NUL); the probe writes all but that
     * trailing NUL, so the real result is one less than data_len. */
    CU_ASSERT_EQUAL(run.result, (uint64_t)(data_len - 1));

    libos_destroy_image(LIBOS_C_PROBE_TEST_OWNER, &img);
}

/* Leaves nothing behind even if an assertion above failed mid-test -- see
 * libos_test_common.h. */
int libos_c_probe_suite_cleanup(void) {
    libos_test_teardown_owner(LIBOS_C_PROBE_TEST_OWNER);
    return 0;
}

void suite_libos_c_probe_tests(CU_pSuite s) {
    CU_add_test(s, "compiled C calls a real syscall and returns its result",
               test_compiled_c_calls_real_syscall);
}
