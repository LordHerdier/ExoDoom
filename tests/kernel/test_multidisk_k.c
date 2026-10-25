/*
 * test_multidisk_k.c — two disks at once (SCRUM-244 acceptance).
 *
 * The test boot attaches a second IDE drive (build/exodoom_disk2.img on the
 * primary slave, `index=1`), which ata_register_blkdevs() makes block device
 * 1. This suite drives the device-indexed syscalls through the real
 * dispatcher, and ExoFS through its EXO_KERNEL compile (test_exofs_k.c's top
 * comment has why that, and libos_heap_alloc() buffers, are required), to
 * show what SCRUM-233's `install` will need:
 *
 *   - one context holds both devices at once;
 *   - another context is refused a device that is held (-EXO_EBUSY) while a
 *     different device stays acquirable;
 *   - an ExoFS volume on each device, and a file copied between them;
 *   - reclaiming a context releases every device it held.
 *
 * Tests that need the second drive return early when device 0 or 1 is
 * missing, so
 * a boot without it (a developer's own QEMU line) still passes; the
 * argument-shape tests run regardless.
 *
 * LBA USE. Device 0: LBA 5120 for the raw round trip (clear of test_ata_k.c's
 * 2048, test_syscall_disk_k.c's 3072/4096 and test_exofs_k.c's 8192+), and
 * test_exofs_k.c's own EXOFS_TEST_BASE_LBA region as the copy's destination
 * volume, which that suite reformats on every test anyway. Device 1: all of
 * it -- the image exists only for this.
 */

#include "kunit.h"
#include "syscall.h"
#include "blkdev.h"
#include "disk_binding.h"
#include "revoke.h"
#include "exo_syscall.h"
#include "exo_errno.h"
#include "libos_heap.h"
#include "string.h"

#include "libos_fs/exofs.h"
#include "libos_fs/exofs_blockdev.h"

#include <stdint.h>
#include <stddef.h>

#define OTHER_LIBOS ((page_owner_t)(PAGE_OWNER_LIBOS + 1))

#define DEV_SYS   0u
#define DEV_STICK 1u

#define RAW_LBA        5120u
#define SYS_VOL_BASE   8192u   /* test_exofs_k.c's EXOFS_TEST_BASE_LBA */
#define SYS_VOL_SECTORS 8192u
#define STICK_VOL_BASE 0u

/* A file spanning several data blocks and ending mid-block, so the copy
 * exercises the FAT chain on both sides rather than one sector. */
#define COPY_LEN 5000u

static int two_disks(void)
{
    return blkdev_get(DEV_SYS) != NULL && blkdev_get(DEV_STICK) != NULL;
}

/* An index below EXO_DISK_MAX_DEVICES with nothing registered, or
 * EXO_DISK_MAX_DEVICES if every slot is filled. */
static uint32_t absent_index(void)
{
    for (uint32_t d = 0; d < EXO_DISK_MAX_DEVICES; d++)
        if (blkdev_get(d) == NULL)
            return d;
    return EXO_DISK_MAX_DEVICES;
}

static int64_t do_acquire(uint64_t dev, uint64_t base, uint64_t count)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_ACQUIRE, dev, base, count,
                                0, 0, 0);
}

static int64_t do_release(uint64_t dev)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_RELEASE, dev, 0, 0, 0, 0, 0);
}

static int64_t do_read(uint64_t dev, uint64_t lba, void *buf, uint64_t count)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_READ, dev, lba,
                                (uint64_t)(uintptr_t)buf, count, 0, 0);
}

static int64_t do_write(uint64_t dev, uint64_t lba, const void *buf,
                        uint64_t count)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_WRITE, dev, lba,
                                (uint64_t)(uintptr_t)buf, count, 0, 0);
}

static int64_t do_stat(uint64_t dev, exo_diskstat_t *out)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_STAT, dev,
                                (uint64_t)(uintptr_t)out, 0, 0, 0, 0);
}

/* Start and end with nothing held or mounted by anyone this suite touches,
 * whatever a failed test left behind. */
static void reset_bindings(void)
{
    exofs_unmount();
    for (uint32_t d = 0; d < EXO_DISK_MAX_DEVICES; d++)
        do_release(d);
    disk_binding_reclaim(OTHER_LIBOS);
}

int multidisk_suite_init(void)
{
    reset_bindings();
    return 0;
}

int multidisk_suite_cleanup(void)
{
    reset_bindings();
    return 0;
}

/* ---- Argument shape (no second drive needed) ----------------------------- */

static void test_absent_device_is_enodev(void)
{
    uint32_t none = absent_index();
    uint8_t *buf = libos_heap_alloc(512);
    CU_ASSERT_PTR_NOT_NULL(buf);

    CU_ASSERT_EQUAL(do_acquire(none, 0, 0), -EXO_ENODEV);
    CU_ASSERT_EQUAL(do_acquire(1ULL << 40, 0, 0), -EXO_ENODEV);
    CU_ASSERT_EQUAL(do_read(none, 0, buf, 1), -EXO_ENODEV);
    CU_ASSERT_EQUAL(do_write(none, 0, buf, 1), -EXO_ENODEV);
    /* #27's unconditional-cleanup rule survives the device argument. */
    CU_ASSERT_EQUAL(do_release(none), 0);
    CU_ASSERT_EQUAL(do_release(1ULL << 40), 0);

    libos_heap_free(buf);
}

static void test_diskstat_enumerates(void)
{
    exo_diskstat_t *st = libos_heap_alloc(sizeof(*st));
    CU_ASSERT_PTR_NOT_NULL(st);

    for (uint32_t d = 0; d < EXO_DISK_MAX_DEVICES; d++) {
        CU_ASSERT_EQUAL(do_stat(d, st), 0);
        const blkdev_t *bd = blkdev_get(d);
        CU_ASSERT_EQUAL(st->present, bd != NULL ? 1 : 0);
        CU_ASSERT_EQUAL(st->total_sectors, bd != NULL ? bd->sectors : 0);
    }
    CU_ASSERT_EQUAL(do_stat(EXO_DISK_MAX_DEVICES, st), -EXO_EINVAL);

    libos_heap_free(st);
}

/* ---- Two drives ---------------------------------------------------------- */

static void test_second_drive_attached(void)
{
    /* Not a skip: the CI boot attaches it, and every test below silently
     * passing because it went missing would hide exactly the regression
     * this suite exists for. */
    CU_ASSERT_TRUE(two_disks());
    if (!two_disks())
        return;
    CU_ASSERT(blkdev_get(DEV_STICK)->sectors > 0);
}

static void test_one_context_holds_both(void)
{
    if (!two_disks())
        return;
    page_owner_t me = syscall_current_context();
    uint32_t base, count;

    CU_ASSERT_EQUAL(do_acquire(DEV_SYS, SYS_VOL_BASE, SYS_VOL_SECTORS), 0);
    CU_ASSERT_EQUAL(do_acquire(DEV_STICK, 0, 0), 0);

    CU_ASSERT_EQUAL(disk_binding_extent_of(me, DEV_SYS, &base, &count),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(base, SYS_VOL_BASE);
    CU_ASSERT_EQUAL(disk_binding_extent_of(me, DEV_STICK, &base, &count),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(base, 0);

    /* Releasing one device leaves the other held. */
    CU_ASSERT_EQUAL(do_release(DEV_STICK), 0);
    CU_ASSERT_EQUAL(disk_binding_extent_of(me, DEV_STICK, &base, &count),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_extent_of(me, DEV_SYS, &base, &count),
                    DISK_BIND_OK);

    reset_bindings();
}

/* Jira acceptance: a second context acquiring a bound device is refused,
 * while a different device stays acquirable. */
static void test_held_device_busy_other_free(void)
{
    if (!two_disks())
        return;

    CU_ASSERT_EQUAL(do_acquire(DEV_STICK, 0, 0), 0);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS, DEV_STICK),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS, DEV_SYS), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_owner(DEV_SYS), OTHER_LIBOS);
    CU_ASSERT_EQUAL(disk_binding_owner(DEV_STICK), syscall_current_context());

    reset_bindings();
}

/* The same LBAs on two devices are two different sectors, so equal ranges
 * held by different owners do not conflict. */
static void test_same_range_on_two_devices(void)
{
    if (!two_disks())
        return;

    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, DEV_SYS, 0, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(do_acquire(DEV_STICK, 0, 100), 0);
    CU_ASSERT_EQUAL(do_acquire(DEV_SYS, 0, 100), -EXO_EBUSY);

    reset_bindings();
}

/* Holding device 0 grants nothing on device 1. */
static void test_binding_is_per_device(void)
{
    if (!two_disks())
        return;
    uint8_t *buf = libos_heap_alloc(512);
    CU_ASSERT_PTR_NOT_NULL(buf);

    CU_ASSERT_EQUAL(do_acquire(DEV_SYS, RAW_LBA, 1), 0);
    CU_ASSERT_EQUAL(do_read(DEV_STICK, RAW_LBA, buf, 1), -EXO_EBUSY);
    CU_ASSERT_EQUAL(do_read(DEV_SYS, RAW_LBA, buf, 1), 1);

    libos_heap_free(buf);
    reset_bindings();
}

static void fill(uint8_t *p, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0; i < len; i++)
        p[i] = (uint8_t)(seed + i * 7u);
}

/* The registry really routes to two drives: the same LBA written with two
 * different patterns reads back as two different patterns. */
static void test_same_lba_two_devices_distinct(void)
{
    if (!two_disks())
        return;
    uint8_t *a = libos_heap_alloc(512);
    uint8_t *b = libos_heap_alloc(512);
    uint8_t *r = libos_heap_alloc(512);
    CU_ASSERT(a && b && r);

    fill(a, 512, 0x11);
    fill(b, 512, 0x80);

    CU_ASSERT_EQUAL(do_acquire(DEV_SYS, RAW_LBA, 1), 0);
    CU_ASSERT_EQUAL(do_acquire(DEV_STICK, RAW_LBA, 1), 0);
    CU_ASSERT_EQUAL(do_write(DEV_SYS, RAW_LBA, a, 1), 1);
    CU_ASSERT_EQUAL(do_write(DEV_STICK, RAW_LBA, b, 1), 1);

    CU_ASSERT_EQUAL(do_read(DEV_SYS, RAW_LBA, r, 1), 1);
    CU_ASSERT_EQUAL(memcmp(r, a, 512), 0);
    CU_ASSERT_EQUAL(do_read(DEV_STICK, RAW_LBA, r, 1), 1);
    CU_ASSERT_EQUAL(memcmp(r, b, 512), 0);

    libos_heap_free(a);
    libos_heap_free(b);
    libos_heap_free(r);
    reset_bindings();
}

/* Write `len` bytes of `src` to `path` on the mounted volume. */
static int put_file(const char *path, const uint8_t *src, uint32_t len)
{
    exofs_file_t f;
    int rc = exofs_open(path, EXOFS_O_WRITE | EXOFS_O_CREATE | EXOFS_O_TRUNC,
                        &f);
    if (rc != 0)
        return rc;
    int64_t n = exofs_write(&f, src, len);
    exofs_close(&f);
    return n == (int64_t)len ? 0 : -EXO_EIO;
}

/* Read all of `path` on the mounted volume into `dst` (room for `cap`);
 * returns its size or a negative error. */
static int64_t get_file(const char *path, uint8_t *dst, uint32_t cap)
{
    exofs_file_t f;
    int rc = exofs_open(path, EXOFS_O_READ, &f);
    if (rc != 0)
        return rc;
    int64_t n = exofs_read(&f, dst, cap);
    exofs_close(&f);
    return n;
}

/* Jira acceptance: acquire both, a volume on each, copy a file between them.
 * One volume is mounted at a time (exofs.h), so the copy goes through a
 * buffer -- the shape `install` (SCRUM-233) takes. Both devices stay bound
 * for the whole copy. */
static void test_copy_file_between_devices(void)
{
    if (!two_disks())
        return;
    uint8_t *src = libos_heap_alloc(COPY_LEN);
    uint8_t *buf = libos_heap_alloc(COPY_LEN);
    CU_ASSERT(src && buf);
    if (!src || !buf)
        goto out;
    fill(src, COPY_LEN, 0x3C);

    uint32_t stick_sectors = blkdev_get(DEV_STICK)->sectors;
    CU_ASSERT_EQUAL(exofs_bdev_acquire(DEV_STICK), 0);
    CU_ASSERT_EQUAL(exofs_bdev_acquire_extent(DEV_SYS, SYS_VOL_BASE,
                                              SYS_VOL_SECTORS), 0);

    /* Stage the source: a volume on the stick holding the file. */
    CU_ASSERT_EQUAL(exofs_format(DEV_STICK, STICK_VOL_BASE, stick_sectors), 0);
    CU_ASSERT_EQUAL(exofs_mount(DEV_STICK, STICK_VOL_BASE), 0);
    CU_ASSERT_EQUAL(put_file("/pkg.exo", src, COPY_LEN), 0);
    exofs_unmount();

    /* Destination volume on the system disk. */
    CU_ASSERT_EQUAL(exofs_format(DEV_SYS, SYS_VOL_BASE, SYS_VOL_SECTORS), 0);

    /* The copy: source volume -> buffer -> destination volume. */
    CU_ASSERT_EQUAL(exofs_mount(DEV_STICK, STICK_VOL_BASE), 0);
    memset(buf, 0, COPY_LEN);
    CU_ASSERT_EQUAL(get_file("/pkg.exo", buf, COPY_LEN), (int64_t)COPY_LEN);
    exofs_unmount();

    CU_ASSERT_EQUAL(exofs_mount(DEV_SYS, SYS_VOL_BASE), 0);
    CU_ASSERT_EQUAL(put_file("/pkg.exo", buf, COPY_LEN), 0);
    exofs_unmount();

    /* Verify from a fresh mount, not from anything cached. */
    CU_ASSERT_EQUAL(exofs_mount(DEV_SYS, SYS_VOL_BASE), 0);
    memset(buf, 0, COPY_LEN);
    CU_ASSERT_EQUAL(get_file("/pkg.exo", buf, COPY_LEN), (int64_t)COPY_LEN);
    CU_ASSERT_EQUAL(memcmp(buf, src, COPY_LEN), 0);
    exofs_unmount();

    /* And the source is still intact on the stick. */
    CU_ASSERT_EQUAL(exofs_mount(DEV_STICK, STICK_VOL_BASE), 0);
    memset(buf, 0, COPY_LEN);
    CU_ASSERT_EQUAL(get_file("/pkg.exo", buf, COPY_LEN), (int64_t)COPY_LEN);
    CU_ASSERT_EQUAL(memcmp(buf, src, COPY_LEN), 0);
    exofs_unmount();

out:
    libos_heap_free(src);
    libos_heap_free(buf);
    exofs_bdev_release(DEV_STICK);
    exofs_bdev_release(DEV_SYS);
    reset_bindings();
}

/* Revocation is per context: an extent taken while a revoke is pending is
 * part of what was asked for, so it starts marked. Otherwise a context could
 * pick up a second device, release the first, and look as if it had
 * complied while still holding a disk. */
static void test_new_extent_inherits_revoke_mark(void)
{
    if (!two_disks())
        return;

    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, DEV_SYS,
                                                RAW_LBA, 1), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_revoke_mark(OTHER_LIBOS), DISK_REVOKE_OK);

    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS, DEV_STICK),
                    DISK_BIND_OK);
    disk_binding_release(OTHER_LIBOS, DEV_SYS);
    CU_ASSERT_TRUE(disk_binding_revoke_pending(OTHER_LIBOS));

    /* Clearing the mark clears it on every device. */
    CU_ASSERT_EQUAL(disk_binding_revoke_clear(OTHER_LIBOS), DISK_REVOKE_OK);
    CU_ASSERT_FALSE(disk_binding_revoke_pending(OTHER_LIBOS));

    reset_bindings();
}

/* With no mark pending, a new extent starts unmarked. */
static void test_new_extent_unmarked_without_pending(void)
{
    if (!two_disks())
        return;

    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS, DEV_STICK),
                    DISK_BIND_OK);
    CU_ASSERT_FALSE(disk_binding_revoke_pending(OTHER_LIBOS));

    reset_bindings();
}

/* Reclaiming a context gives back every device it held, not just one. */
static void test_reclaim_releases_every_device(void)
{
    if (!two_disks())
        return;
    uint32_t base, count;

    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, DEV_SYS,
                                                RAW_LBA, 1), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS, DEV_STICK),
                    DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_revoke_mark(OTHER_LIBOS), DISK_REVOKE_OK);
    CU_ASSERT_TRUE(disk_binding_revoke_pending(OTHER_LIBOS));

    revoke_all(OTHER_LIBOS);
    CU_ASSERT_EQUAL(disk_binding_extent_of(OTHER_LIBOS, DEV_SYS, &base,
                                           &count), DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_extent_of(OTHER_LIBOS, DEV_STICK, &base,
                                           &count), DISK_BIND_EBUSY);
    CU_ASSERT_FALSE(disk_binding_revoke_pending(OTHER_LIBOS));

    /* Both are free for someone else afterwards. */
    CU_ASSERT_EQUAL(do_acquire(DEV_SYS, RAW_LBA, 1), 0);
    CU_ASSERT_EQUAL(do_acquire(DEV_STICK, 0, 0), 0);

    reset_bindings();
}

void suite_multidisk_tests(CU_pSuite s)
{
    CU_add_test(s, "absent device: -ENODEV, release still 0",
                test_absent_device_is_enodev);
    CU_add_test(s, "exo_diskstat enumerates every device",
                test_diskstat_enumerates);
    CU_add_test(s, "second drive is attached as device 1",
                test_second_drive_attached);
    CU_add_test(s, "one context holds both devices",
                test_one_context_holds_both);
    CU_add_test(s, "held device -EBUSY, other device acquirable",
                test_held_device_busy_other_free);
    CU_add_test(s, "same LBA range on two devices does not conflict",
                test_same_range_on_two_devices);
    CU_add_test(s, "binding on device 0 grants nothing on device 1",
                test_binding_is_per_device);
    CU_add_test(s, "same LBA on two devices holds two sectors",
                test_same_lba_two_devices_distinct);
    CU_add_test(s, "ExoFS file copied between devices",
                test_copy_file_between_devices);
    CU_add_test(s, "extent taken during a pending revoke starts marked",
                test_new_extent_inherits_revoke_mark);
    CU_add_test(s, "extent taken with no revoke pending starts unmarked",
                test_new_extent_unmarked_without_pending);
    CU_add_test(s, "reclaim releases every device",
                test_reclaim_releases_every_device);
}
