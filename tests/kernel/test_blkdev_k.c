/*
 * test_blkdev_k.c — the kernel block-device registry (SCRUM-244).
 *
 * Two halves. The registry itself (src/blkdev.c) is pure bookkeeping, so it
 * is tested with fake RAM-backed devices that register, answer and are then
 * truncated away again -- blkdev_truncate() back to the boot count leaves the
 * real ATA registrations exactly as kernel_main made them, which every disk
 * suite after this one depends on. The second half checks what
 * ata_register_blkdevs() registered at boot against what src/ata.c probed:
 * one device per present IDE position, in position order, CD-ROM excluded.
 */

#include "kunit.h"
#include "blkdev.h"
#include "ata.h"
#include "string.h"

#include <stdint.h>
#include <stddef.h>

/* ---- A fake device ------------------------------------------------------ */

#define FAKE_SECTORS 4u

typedef struct {
    uint8_t  data[FAKE_SECTORS * 512u];
    uint32_t reads, writes;
} fake_disk_t;

static fake_disk_t fake_a, fake_b;

static int fake_read(void *ctx, uint32_t lba, uint8_t *buf, uint32_t count)
{
    fake_disk_t *d = ctx;
    if (lba + count > FAKE_SECTORS)
        return BLKDEV_EIO;
    memcpy(buf, d->data + lba * 512u, count * 512u);
    d->reads++;
    return BLKDEV_OK;
}

static int fake_write(void *ctx, uint32_t lba, const uint8_t *buf,
                      uint32_t count)
{
    fake_disk_t *d = ctx;
    if (lba + count > FAKE_SECTORS)
        return BLKDEV_EIO;
    memcpy(d->data + lba * 512u, buf, count * 512u);
    d->writes++;
    return BLKDEV_OK;
}

static blkdev_t fake_dev(fake_disk_t *d)
{
    blkdev_t dev = {
        .sectors = FAKE_SECTORS,
        .read    = fake_read,
        .write   = fake_write,
        .ctx     = d,
    };
    return dev;
}

/* ---- Registry ----------------------------------------------------------- */

static void test_get_past_end_is_null(void)
{
    CU_ASSERT_PTR_NULL(blkdev_get(blkdev_count()));
    CU_ASSERT_PTR_NULL(blkdev_get(BLKDEV_MAX));
    CU_ASSERT_PTR_NULL(blkdev_get(UINT32_MAX));
}

static void test_register_appends_in_order(void)
{
    uint32_t n0 = blkdev_count();
    if (n0 + 2 > BLKDEV_MAX)
        return; /* no room for two fakes on a fully populated machine */

    blkdev_t a = fake_dev(&fake_a);
    blkdev_t b = fake_dev(&fake_b);
    CU_ASSERT_EQUAL(blkdev_register(&a), (int)n0);
    CU_ASSERT_EQUAL(blkdev_register(&b), (int)n0 + 1);
    CU_ASSERT_EQUAL(blkdev_count(), n0 + 2);

    const blkdev_t *ga = blkdev_get(n0);
    const blkdev_t *gb = blkdev_get(n0 + 1);
    CU_ASSERT_PTR_NOT_NULL(ga);
    CU_ASSERT_PTR_NOT_NULL(gb);
    if (ga && gb) {
        CU_ASSERT_EQUAL(ga->sectors, FAKE_SECTORS);
        CU_ASSERT(ga->ctx == &fake_a);
        CU_ASSERT(gb->ctx == &fake_b);
    }

    blkdev_truncate(n0);
    CU_ASSERT_EQUAL(blkdev_count(), n0);
}

/* The ops reach the device they were registered with, and only that one --
 * the property syscall_disk.c relies on when it dispatches by index. */
static void test_ops_reach_their_own_device(void)
{
    uint32_t n0 = blkdev_count();
    if (n0 + 2 > BLKDEV_MAX)
        return;

    memset(&fake_a, 0, sizeof(fake_a));
    memset(&fake_b, 0, sizeof(fake_b));
    blkdev_t a = fake_dev(&fake_a);
    blkdev_t b = fake_dev(&fake_b);
    blkdev_register(&a);
    blkdev_register(&b);

    static uint8_t sector[512];
    memset(sector, 0xA5, sizeof(sector));
    const blkdev_t *gb = blkdev_get(n0 + 1);
    CU_ASSERT_EQUAL(gb->write(gb->ctx, 2, sector, 1), BLKDEV_OK);
    CU_ASSERT_EQUAL(fake_b.writes, 1);
    CU_ASSERT_EQUAL(fake_a.writes, 0);
    CU_ASSERT_EQUAL(fake_b.data[2 * 512u], 0xA5);
    CU_ASSERT_EQUAL(fake_a.data[2 * 512u], 0x00);

    memset(sector, 0, sizeof(sector));
    const blkdev_t *ga = blkdev_get(n0);
    CU_ASSERT_EQUAL(ga->read(ga->ctx, 2, sector, 1), BLKDEV_OK);
    CU_ASSERT_EQUAL(sector[0], 0x00);
    CU_ASSERT_EQUAL(gb->read(gb->ctx, 2, sector, 1), BLKDEV_OK);
    CU_ASSERT_EQUAL(sector[0], 0xA5);

    CU_ASSERT_EQUAL(ga->read(ga->ctx, FAKE_SECTORS, sector, 1), BLKDEV_EIO);

    blkdev_truncate(n0);
}

static void test_table_full_is_refused(void)
{
    uint32_t n0 = blkdev_count();
    blkdev_t a = fake_dev(&fake_a);

    while (blkdev_count() < BLKDEV_MAX)
        CU_ASSERT(blkdev_register(&a) >= 0);
    CU_ASSERT_EQUAL(blkdev_register(&a), BLKDEV_EFULL);
    CU_ASSERT_EQUAL(blkdev_count(), BLKDEV_MAX);

    blkdev_truncate(n0);
    CU_ASSERT_EQUAL(blkdev_count(), n0);
}

static void test_truncate_past_end_is_noop(void)
{
    uint32_t n0 = blkdev_count();
    blkdev_truncate(n0 + 1);
    blkdev_truncate(BLKDEV_MAX);
    CU_ASSERT_EQUAL(blkdev_count(), n0);
}

/* ---- What ATA registered at boot ---------------------------------------- */

/* One device per present position, in position order. Re-probes first:
 * ata_init() is idempotent and other suites call it too. */
static void test_ata_positions_registered_in_order(void)
{
    ata_init();

    uint32_t dev = 0;
    for (unsigned pos = 0; pos < ATA_MAX_DRIVES; pos++) {
        if (!ata_drive_present(pos))
            continue;
        const blkdev_t *bd = blkdev_get(dev);
        CU_ASSERT_PTR_NOT_NULL(bd);
        if (bd)
            CU_ASSERT_EQUAL(bd->sectors, ata_drive_sector_count(pos));
        dev++;
    }
    CU_ASSERT_EQUAL(blkdev_count(), dev);
}

/* The system disk is device 0 whenever the primary master is there, which
 * is what lets every fixed LBA layout constant keep its meaning. */
static void test_primary_master_is_device_zero(void)
{
    if (ata_init() != ATA_OK)
        return;

    const blkdev_t *bd = blkdev_get(0);
    CU_ASSERT_PTR_NOT_NULL(bd);
    if (bd)
        CU_ASSERT_EQUAL(bd->sectors, ata_get_sector_count());
}

/* The test boot attaches the ISO with -cdrom, which QEMU puts on the
 * secondary master. It is a packet device and must not be registered as a
 * disk. */
static void test_cdrom_is_not_a_disk(void)
{
    ata_init();
    CU_ASSERT_FALSE(ata_drive_present(2));
    CU_ASSERT_EQUAL(ata_drive_sector_count(2), 0);
}

static void test_out_of_range_position_absent(void)
{
    CU_ASSERT_FALSE(ata_drive_present(ATA_MAX_DRIVES));
    CU_ASSERT_EQUAL(ata_read_sectors_on(ATA_MAX_DRIVES, 0, NULL, 1),
                    ATA_ENODEV);
}

void suite_blkdev_tests(CU_pSuite s)
{
    CU_add_test(s, "blkdev_get past the end is NULL",
                test_get_past_end_is_null);
    CU_add_test(s, "blkdev_register appends in order",
                test_register_appends_in_order);
    CU_add_test(s, "ops reach their own device only",
                test_ops_reach_their_own_device);
    CU_add_test(s, "full table refuses a fifth device",
                test_table_full_is_refused);
    CU_add_test(s, "blkdev_truncate past the end is a no-op",
                test_truncate_past_end_is_noop);
    CU_add_test(s, "every present ATA position registered, in order",
                test_ata_positions_registered_in_order);
    CU_add_test(s, "primary master is device 0",
                test_primary_master_is_device_zero);
    CU_add_test(s, "ATAPI CD-ROM is not registered",
                test_cdrom_is_not_a_disk);
    CU_add_test(s, "out-of-range ATA position is absent",
                test_out_of_range_position_absent);
}
