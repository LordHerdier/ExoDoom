/*
 * test_syscall_disk_k.c — exo_disk_read/exo_disk_write (SCRUM-103).
 *
 * Drives the real dispatch path, exo_syscall_dispatch(EXO_SYS_DISK_READ/
 * WRITE, ...). The handlers are bound in kernel_main by syscall_disk_init()
 * before run_tests(), with drive presence latched from ata_init()'s own
 * result at that point -- see docker-test/docker-ci's scratch -drive, the
 * same one test_ata_k.c exercises.
 *
 * Round-trip assertions use LBA 3072, well clear of test_ata_k.c's LBA
 * 2048/2049 range on the same scratch image. A non-CI run with no drive
 * attached sees -EXO_ENODEV from every call below, same as test_ata_k.c's
 * own guard -- the bounds/validation checks that don't touch hardware
 * (zero count, oversized count, out-of-window/unmapped buffers, LBA
 * overflow) still run unconditionally, since they reject before ever
 * reaching ata.c.
 */

#include "kunit.h"
#include "syscall.h"
#include "syscall_disk.h"
#include "exo_syscall.h"
#include "ata.h"

#include <stdint.h>

/* Scratch virtual address in the LibOS window, apart from other suites'
 * ranges (test_syscall_serial_k.c uses +0x28000000). */
#define SCRATCH (EXO_USER_VA_BASE + 0x29000000ULL)

/* A second scratch address in this suite's own range that no test here ever
 * maps -- the in-window-but-unmapped pointer SCRUM-186 reproduces the crash
 * with (same pattern as every other syscall suite). */
#define UNMAPPED (SCRATCH + 0x1000ULL)

#define TEST_LBA 3072

static int drive_present(void)
{
    return ata_init() == ATA_OK;
}

static int64_t do_disk_read(uint64_t lba, uint64_t buf, uint64_t count)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_READ, lba, buf, count, 0, 0, 0);
}

static int64_t do_disk_write(uint64_t lba, uint64_t buf, uint64_t count)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_WRITE, lba, buf, count, 0, 0, 0);
}

static int64_t do_alloc(void)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
}

static int64_t do_free(uint64_t paddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_FREE, paddr, 0, 0, 0, 0, 0);
}

static int64_t do_map(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_MAP, vaddr, paddr, flags,
                                0, 0, 0);
}

static int64_t do_unmap(uint64_t vaddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, vaddr, 0, 0, 0, 0, 0);
}

/* The boot path must have bound these numbers; without this the rest of the
 * suite would only be re-proving the dispatcher's -EXO_ENOSYS fallback. */
static void test_handlers_are_bound(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_DISK_READ));
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_DISK_WRITE));
}

static void test_zero_count_is_a_noop(void)
{
    /* count == 0 needs no valid buf at all -- NULL is fine, nothing to
     * transfer. Runs regardless of drive presence: the check is ahead of
     * both the ENODEV check and the hardware. */
    CU_ASSERT_EQUAL(do_disk_read(0, 0, 0), 0);
    CU_ASSERT_EQUAL(do_disk_write(0, 0, 0), 0);
}

static void test_oversized_count_rejected(void)
{
    CU_ASSERT_EQUAL(do_disk_read(0, 0, EXO_DISK_MAX_SECTORS + 1), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_disk_write(0, 0, EXO_DISK_MAX_SECTORS + 1),
                   -EXO_EINVAL);
}

static void test_lba_overflow_rejected(void)
{
    /* lba + count - 1 runs past the 28-bit LBA ceiling. */
    CU_ASSERT_EQUAL(do_disk_read(0x0FFFFFFFu, 0, 2), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_disk_write(0x0FFFFFFFu, 0, 2), -EXO_EINVAL);
}

static void test_null_buffer_rejected(void)
{
    CU_ASSERT_EQUAL(do_disk_read(TEST_LBA, 0, 1), -EXO_EFAULT);
    CU_ASSERT_EQUAL(do_disk_write(TEST_LBA, 0, 1), -EXO_EFAULT);
}

static void test_buffer_below_window_rejected(void)
{
    CU_ASSERT_EQUAL(do_disk_read(TEST_LBA, EXO_USER_VA_BASE - 1, 1),
                   -EXO_EFAULT);
    CU_ASSERT_EQUAL(do_disk_write(TEST_LBA, EXO_USER_VA_BASE - 1, 1),
                   -EXO_EFAULT);
}

static void test_unmapped_in_window_buffer_rejected(void)
{
    CU_ASSERT_EQUAL(do_disk_read(TEST_LBA, UNMAPPED, 1), -EXO_EFAULT);
    CU_ASSERT_EQUAL(do_disk_write(TEST_LBA, UNMAPPED, 1), -EXO_EFAULT);
}

static void test_write_then_read_round_trips(void)
{
    if (!drive_present()) {
        /* No drive attached (e.g. a non-CI run) -- nothing to round-trip,
         * same guard test_ata_k.c uses. */
        return;
    }

    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)p,
                          EXO_PAGE_WRITE | EXO_PAGE_USER), 0);

    uint8_t *buf = (uint8_t *)(uintptr_t)SCRATCH;
    for (int i = 0; i < 512; i++)
        buf[i] = (uint8_t)(i * 5 + 11);

    CU_ASSERT_EQUAL(do_disk_write(TEST_LBA, SCRATCH, 1), 1);

    for (int i = 0; i < 512; i++)
        buf[i] = 0;

    CU_ASSERT_EQUAL(do_disk_read(TEST_LBA, SCRATCH, 1), 1);

    int ok = 1;
    for (int i = 0; i < 512; i++) {
        if (buf[i] != (uint8_t)(i * 5 + 11)) {
            ok = 0;
            break;
        }
    }
    CU_ASSERT(ok);

    CU_ASSERT_EQUAL(do_unmap(SCRATCH), 0);
    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
}

static void test_no_drive_reports_enodev(void)
{
    if (drive_present()) {
        /* This build has a drive attached -- the -EXO_ENODEV path is not
         * reachable here (SCRUM-103 latches presence once at boot, and this
         * suite cannot un-attach the scratch drive), so there is nothing to
         * assert. Covered by CI runs with no -drive attached instead. */
        return;
    }

    CU_ASSERT_EQUAL(do_disk_read(TEST_LBA, 0, 1), -EXO_ENODEV);
    CU_ASSERT_EQUAL(do_disk_write(TEST_LBA, 0, 1), -EXO_ENODEV);
}

void suite_syscall_disk_tests(CU_pSuite s)
{
    CU_add_test(s, "handlers are bound", test_handlers_are_bound);
    CU_add_test(s, "zero count is a no-op", test_zero_count_is_a_noop);
    CU_add_test(s, "oversized count rejected", test_oversized_count_rejected);
    CU_add_test(s, "LBA overflow rejected", test_lba_overflow_rejected);
    CU_add_test(s, "NULL buffer rejected", test_null_buffer_rejected);
    CU_add_test(s, "buffer below window rejected",
               test_buffer_below_window_rejected);
    CU_add_test(s, "unmapped in-window buffer rejected",
               test_unmapped_in_window_buffer_rejected);
    CU_add_test(s, "write then read round-trips",
               test_write_then_read_round_trips);
    CU_add_test(s, "no drive reports ENODEV", test_no_drive_reports_enodev);
}
