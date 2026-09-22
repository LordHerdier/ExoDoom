/*
 * test_ata_k.c — ATA PIO driver (SCRUM-102).
 *
 * Drives the real ata_init()/ata_read_sector()/ata_write_sector() against
 * whatever is attached to the primary bus. docker-test/docker-ci attach a
 * throwaway scratch raw image via `-drive ...,if=ide` (see Makefile);
 * docker-run/docker-run-kernel attach nothing, so a build run outside the
 * test harness would see ATA_ENODEV here and skip straight to the
 * round-trip test's own guard -- CI is the only place these assertions are
 * meant to actually exercise hardware.
 *
 * Round-trip tests use LBA 2048, far from LBA 0 so a future FAT superblock
 * (SCRUM-189/190, reusing this same scratch image) is never at risk even
 * after this suite has written to it.
 */

#include "kunit.h"
#include "ata.h"
#include "string.h"
#include "serial.h"

#include <stdint.h>

#define TEST_LBA 2048

static void test_ata_init_detects_drive(void)
{
    int rc = ata_init();
    CU_ASSERT(rc == ATA_OK || rc == ATA_ENODEV);
}

static void test_write_then_read_round_trips(void)
{
    if (ata_init() != ATA_OK) {
        /* No drive attached (e.g. a non-CI run) -- nothing to round-trip. */
        return;
    }

    uint8_t write_buf[512];
    for (int i = 0; i < 512; i++) {
        write_buf[i] = (uint8_t)(i * 3 + 7);
    }

    /* SCRUM-102 (PR #122): CI reproduces a write failure that never shows
     * up locally. Diagnostic-only -- prints the real rc so the CI serial
     * log says ETIMEOUT vs EIO vs something else, instead of just "not
     * ATA_OK". Remove once the root cause is confirmed and fixed. */
    int write_rc = ata_write_sector(TEST_LBA, write_buf);
    if (write_rc != ATA_OK) {
        serial_print("DEBUG ata_write_sector rc=");
        serial_print_hex64((uint64_t)(int64_t)write_rc);
        serial_print("\n");
    }
    CU_ASSERT_EQUAL(write_rc, ATA_OK);

    uint8_t read_buf[512];
    memset(read_buf, 0, sizeof(read_buf));
    int read_rc = ata_read_sector(TEST_LBA, read_buf);
    if (read_rc != ATA_OK) {
        serial_print("DEBUG ata_read_sector rc=");
        serial_print_hex64((uint64_t)(int64_t)read_rc);
        serial_print("\n");
    }
    CU_ASSERT_EQUAL(read_rc, ATA_OK);

    CU_ASSERT_EQUAL(memcmp(write_buf, read_buf, 512), 0);
}

static void test_read_of_untouched_sector_does_not_fault(void)
{
    if (ata_init() != ATA_OK) {
        return;
    }

    uint8_t buf[512];
    /* LBA far enough from TEST_LBA above to be untouched by that test
     * regardless of run order within the suite. */
    CU_ASSERT_EQUAL(ata_read_sector(TEST_LBA + 1, buf), ATA_OK);
}

void suite_ata_tests(CU_pSuite s)
{
    CU_add_test(s, "ata_init detects drive or reports ENODEV",
               test_ata_init_detects_drive);
    CU_add_test(s, "write then read round-trips",
               test_write_then_read_round_trips);
    CU_add_test(s, "read of untouched sector does not fault",
               test_read_of_untouched_sector_does_not_fault);
}
