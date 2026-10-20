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

#include <stdint.h>

#define TEST_LBA 2048

/* Multi-block round trip (SCRUM-190): 37 sectors at LBA 2112..2148, clear of
 * TEST_LBA/TEST_LBA+1 above, test_syscall_disk_k.c's 3072 and
 * test_exofs_k.c's 8192+. 37 is deliberately not a multiple of QEMU's
 * 16-sector multiple-mode block (16 + 16 + 5), so the short final DRQ
 * block is exercised in both directions. Static, not on the stack: 2 x 37
 * sectors is more than the 16 KiB kernel stack. */
#define MULTI_LBA   (TEST_LBA + 64)
#define MULTI_COUNT 37u
static uint8_t g_multi_write[MULTI_COUNT * 512u];
static uint8_t g_multi_read[MULTI_COUNT * 512u];

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

    CU_ASSERT_EQUAL(ata_write_sector(TEST_LBA, write_buf), ATA_OK);

    uint8_t read_buf[512];
    memset(read_buf, 0, sizeof(read_buf));
    CU_ASSERT_EQUAL(ata_read_sector(TEST_LBA, read_buf), ATA_OK);

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

/* ata_init() negotiates SET MULTIPLE MODE from IDENTIFY word 47, and a drive
 * that refuses it falls back to one sector per DRQ block -- correct, but
 * ~16x the VM exits per sector under KVM, which is what made the WAD load
 * take seconds. QEMU's IDE drive reports and accepts 16, so a 1 here means
 * the negotiation regressed silently rather than that the drive lacks it. */
static void test_multiple_mode_negotiated(void)
{
    if (ata_init() != ATA_OK) {
        return;
    }

    CU_ASSERT_TRUE(ata_multiple_block() > 1);
}

static void test_multi_block_round_trip_with_short_tail(void)
{
    if (ata_init() != ATA_OK) {
        return;
    }

    for (uint32_t i = 0; i < sizeof(g_multi_write); i++) {
        /* Varies per sector as well as per byte, so a block landing at the
         * wrong offset cannot compare equal by accident. */
        g_multi_write[i] = (uint8_t)(i * 7 + i / 512u * 13 + 1);
    }
    memset(g_multi_read, 0, sizeof(g_multi_read));

    CU_ASSERT_EQUAL(ata_write_sectors(MULTI_LBA, g_multi_write, MULTI_COUNT),
                    ATA_OK);
    CU_ASSERT_EQUAL(ata_read_sectors(MULTI_LBA, g_multi_read, MULTI_COUNT),
                    ATA_OK);
    CU_ASSERT_EQUAL(memcmp(g_multi_write, g_multi_read,
                           sizeof(g_multi_write)), 0);

    /* The tail sector read on its own agrees too: the run really landed
     * sector-for-sector at MULTI_LBA.., not merely self-consistently. */
    uint8_t one[512];
    CU_ASSERT_EQUAL(ata_read_sector(MULTI_LBA + MULTI_COUNT - 1, one), ATA_OK);
    CU_ASSERT_EQUAL(memcmp(one, g_multi_write + (MULTI_COUNT - 1) * 512u,
                           512), 0);
}

void suite_ata_tests(CU_pSuite s)
{
    CU_add_test(s, "ata_init detects drive or reports ENODEV",
               test_ata_init_detects_drive);
    CU_add_test(s, "write then read round-trips",
               test_write_then_read_round_trips);
    CU_add_test(s, "read of untouched sector does not fault",
               test_read_of_untouched_sector_does_not_fault);
    CU_add_test(s, "multiple mode negotiated",
               test_multiple_mode_negotiated);
    CU_add_test(s, "multi-block round trip with short tail",
               test_multi_block_round_trip_with_short_tail);
}
