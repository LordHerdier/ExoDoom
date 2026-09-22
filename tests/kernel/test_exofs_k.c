/*
 * test_exofs_k.c — ExoFS, the ported FAT-like LibOS filesystem (SCRUM-189).
 *
 * Drives src/libos_fs/ directly from ring 0. That works because those
 * sources are compiled into the kernel image with -DEXO_KERNEL under
 * TESTING=1 (docker/scripts/build.sh step [3b4/7]), which selects
 * exofs_blockdev.c's exo_syscall_dispatch() form of disk I/O rather than the
 * inline `syscall` stubs — the same dual-compile arrangement
 * src/libos_page_alloc.c uses, and for the same reason: a stub's `sysretq`
 * would force CPL 3 on return and drop the rest of the test run to ring 3.
 * See exofs_blockdev.h's top comment.
 *
 * This file currently covers the block-I/O seam and the volume layer
 * (superblock/format/mount). The layers above it (FAT, names, directories,
 * files) land in later steps of SCRUM-189 and add their tests here.
 *
 * DISK BINDING. exo_disk_read/exo_disk_write answer -EXO_EBUSY to a caller
 * that does not hold the binding (SCRUM-188), so suite_init acquires it and
 * suite_cleanup releases it — exactly what test_syscall_disk_k.c does. A
 * headless build has nothing to acquire, which is fine: every test that
 * touches hardware guards on drive_present() first, same as test_ata_k.c and
 * test_syscall_disk_k.c.
 *
 * LBA RANGE. All I/O here is at EXOFS_TEST_BASE_LBA (8192) and above — the
 * upper half of docker-test's 8 MiB build/ata_scratch.img, clear of
 * test_ata_k.c's LBA 2048/2049 and test_syscall_disk_k.c's LBA 3072. Later
 * steps format a whole volume starting there, which is why the base is
 * reserved wholesale rather than a sector at a time.
 *
 * BUFFERS. libos_heap_alloc(), never malloc(). Under EXO_KERNEL malloc() is
 * kmalloc(), which returns a kernel bump address — outside
 * [EXO_USER_VA_BASE, EXO_USER_VA_END), so every transfer would fail
 * -EXO_EFAULT. libos_heap_alloc() sits on libos_page_alloc() and hands out
 * window addresses under both compiles. This is the single most likely thing
 * to go wrong when adding a test here, hence test_heap_buffer_is_in_window()
 * below asserting the property directly rather than leaving it implied by
 * whichever transfer test happens to fail first.
 */

#include "kunit.h"
#include "syscall.h"
#include "disk_binding.h"
#include "exo_syscall.h"
#include "exo_errno.h"
#include "syscall_disk.h"
#include "ata.h"
#include "libos_heap.h"

#include "libos_fs/exofs_layout.h"
#include "libos_fs/exofs_blockdev.h"
#include "libos_fs/exofs.h"
#include "libos_fs/exofs_internal.h"

#include <stdint.h>
#include <stddef.h>

/* Base of the region SCRUM-189 owns on the scratch image. See the LBA RANGE
 * note above before moving it. */
#define EXOFS_TEST_BASE_LBA 8192u

/* A sector inside that region for the raw seam tests, far enough in that a
 * later exofs_format() at the base will not be testing against bytes these
 * tests left behind. */
#define SEAM_LBA (EXOFS_TEST_BASE_LBA + 64u)

/*
 * Sectors handed to exofs_format() in the volume tests. The scratch image is
 * 8 MiB = 16384 sectors and this region starts at 8192, so 8192 is the whole
 * rest of it. Deliberately the real remainder rather than a round number: a
 * geometry bug that only shows up when total_blocks is not a multiple of
 * EXOFS_FAT_PER_BLOCK would hide behind a convenient size.
 */
#define VOL_SECTORS 8192u

static int drive_present(void)
{
    return ata_init() == ATA_OK;
}

int exofs_suite_init(void)
{
    exofs_bdev_acquire();
    return 0;
}

int exofs_suite_cleanup(void)
{
    /* Leave nothing mounted for the next suite, even if a test failed
     * partway through with a volume up. */
    exofs_unmount();
    disk_binding_release(syscall_current_context());
    return 0;
}

/* Format + mount, or bail out of the calling test. Every volume test starts
 * from a freshly formatted region rather than inheriting whatever the last
 * one left, so a failure names its own cause. */
static int fresh_volume(void)
{
    exofs_unmount();
    if (exofs_format(EXOFS_TEST_BASE_LBA, VOL_SECTORS) != 0) return 0;
    if (exofs_mount(EXOFS_TEST_BASE_LBA) != 0) return 0;
    return 1;
}

/* ---- The buffer rule ----------------------------------------------------
 *
 * Asserted on its own rather than inferred from a transfer failure: if this
 * breaks, every other test in this file breaks with -EXO_EFAULT and none of
 * them says why.
 */
static void test_heap_buffer_is_in_window(void)
{
    void *buf = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    CU_ASSERT_PTR_NOT_NULL(buf);
    if (buf == NULL) return;

    uintptr_t lo = (uintptr_t)buf;
    uintptr_t hi = lo + EXOFS_BLOCK_SIZE;

    CU_ASSERT_TRUE(lo >= EXO_USER_VA_BASE);
    CU_ASSERT_TRUE(hi <= EXO_USER_VA_END);

    libos_heap_free(buf);
}

/* ---- Layout constants ---------------------------------------------------
 *
 * exofs_layout.h's _Static_asserts already pin the struct sizes at compile
 * time. What they cannot pin is the relationship to the *syscall's* sector
 * size, which lives in a different header and could drift independently.
 */
static void test_block_size_matches_sector_size(void)
{
    /* exo_disk_read/exo_disk_write transfer 512-byte sectors, and ExoFS
     * turns a block index into an LBA with a single addition on the
     * strength of these being equal (exofs_layout.h). */
    CU_ASSERT_EQUAL(EXOFS_BLOCK_SIZE, 512u);
    CU_ASSERT_EQUAL(EXOFS_ENTS_PER_BLOCK, 16u);
    CU_ASSERT_EQUAL(EXOFS_FAT_PER_BLOCK, 128u);
    CU_ASSERT_EQUAL(sizeof(exofs_dirent_t), 32u);
    CU_ASSERT_EQUAL(sizeof(exofs_super_t), EXOFS_BLOCK_SIZE);
}

/* ---- Error pass-through -------------------------------------------------
 *
 * The seam must not invent or swallow error codes: a caller needs to be able
 * to tell -EXO_EBUSY (no binding) from -EXO_EFAULT (bad buffer), because the
 * first is recoverable by acquiring and the second never is.
 */
static void test_null_buffer_rejected(void)
{
    /* Rejected locally, before any syscall: count > 0 with no buffer is a
     * caller bug, and answering -EXO_EFAULT matches what the syscall itself
     * would have said. */
    CU_ASSERT_EQUAL(exofs_bdev_read(SEAM_LBA, NULL, 1), -EXO_EFAULT);
    CU_ASSERT_EQUAL(exofs_bdev_write(SEAM_LBA, NULL, 1), -EXO_EFAULT);
}

static void test_zero_count_is_a_noop(void)
{
    /* Nothing to transfer, so the loop never runs and NULL is not a problem
     * — mirroring the syscall's own count == 0 contract
     * (docs/syscall_spec.md §3.2 #27). Runs with or without a drive. */
    CU_ASSERT_EQUAL(exofs_bdev_read(SEAM_LBA, NULL, 0), 0);
    CU_ASSERT_EQUAL(exofs_bdev_write(SEAM_LBA, NULL, 0), 0);
}

static void test_kernel_buffer_rejected(void)
{
    if (!drive_present()) return;

    /* A stack address is a kernel address: outside the LibOS window, so the
     * handler must refuse it. This is the failure mode the BUFFERS note
     * above exists to prevent, demonstrated deliberately. */
    uint8_t stack_buf[EXOFS_BLOCK_SIZE];

    CU_ASSERT_EQUAL(exofs_bdev_read(SEAM_LBA, stack_buf, 1), -EXO_EFAULT);
}

/* ---- Round trip ---------------------------------------------------------
 *
 * The point of the whole seam: bytes written through exo_disk_write come
 * back through exo_disk_read.
 */
static void test_single_sector_round_trips(void)
{
    if (!drive_present()) return;

    uint8_t *w = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    uint8_t *r = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    CU_ASSERT_PTR_NOT_NULL(w);
    CU_ASSERT_PTR_NOT_NULL(r);
    if (w == NULL || r == NULL) goto out;

    for (uint32_t i = 0; i < EXOFS_BLOCK_SIZE; i++) {
        w[i] = (uint8_t)(i * 7u + 3u);
        r[i] = 0;
    }

    CU_ASSERT_EQUAL(exofs_bdev_write(SEAM_LBA, w, 1), 0);
    CU_ASSERT_EQUAL(exofs_bdev_read(SEAM_LBA, r, 1), 0);

    for (uint32_t i = 0; i < EXOFS_BLOCK_SIZE; i++) {
        if (r[i] != w[i]) {
            CU_ASSERT_EQUAL(r[i], w[i]);
            break;      /* one failure is the signal; 512 is noise */
        }
    }

out:
    libos_heap_free(w);
    libos_heap_free(r);
}

/*
 * Chunking is the one piece of logic this file adds on top of the syscall,
 * so it gets a transfer that genuinely crosses the cap rather than one that
 * merely could. EXO_DISK_MAX_SECTORS + 1 is the smallest count that forces a
 * second iteration, and it is exactly the count the raw syscall rejects with
 * -EXO_EINVAL — so this also proves the split happens rather than the count
 * being passed through.
 */
static void test_transfer_larger_than_cap_is_chunked(void)
{
    if (!drive_present()) return;

    const uint32_t count = EXO_DISK_MAX_SECTORS + 1u;
    const size_t   bytes = (size_t)count * EXOFS_BLOCK_SIZE;

    uint8_t *w = libos_heap_alloc(bytes);
    uint8_t *r = libos_heap_alloc(bytes);
    CU_ASSERT_PTR_NOT_NULL(w);
    CU_ASSERT_PTR_NOT_NULL(r);
    if (w == NULL || r == NULL) goto out;

    /* Vary per sector, not just per byte: a chunking bug that reads the
     * right number of sectors from the wrong LBA would survive a pattern
     * that repeats every 512 bytes. */
    for (size_t i = 0; i < bytes; i++) {
        w[i] = (uint8_t)((i / EXOFS_BLOCK_SIZE) * 31u + (i % 251u));
        r[i] = 0;
    }

    CU_ASSERT_EQUAL(exofs_bdev_write(SEAM_LBA, w, count), 0);
    CU_ASSERT_EQUAL(exofs_bdev_read(SEAM_LBA, r, count), 0);

    for (size_t i = 0; i < bytes; i++) {
        if (r[i] != w[i]) {
            CU_ASSERT_EQUAL(r[i], w[i]);
            break;
        }
    }

    /* And the sector past the transfer must be untouched — a chunk loop
     * that over-runs by one would otherwise go unnoticed. */
    uint8_t *guard = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (guard != NULL) {
        for (uint32_t i = 0; i < EXOFS_BLOCK_SIZE; i++) guard[i] = 0xA5;
        CU_ASSERT_EQUAL(exofs_bdev_write(SEAM_LBA + count, guard, 1), 0);

        for (uint32_t i = 0; i < EXOFS_BLOCK_SIZE; i++) guard[i] = 0;
        CU_ASSERT_EQUAL(exofs_bdev_write(SEAM_LBA, w, count), 0);
        CU_ASSERT_EQUAL(exofs_bdev_read(SEAM_LBA + count, guard, 1), 0);
        CU_ASSERT_EQUAL(guard[0], 0xA5);
        libos_heap_free(guard);
    }

out:
    libos_heap_free(w);
    libos_heap_free(r);
}

/* ---- Binding ------------------------------------------------------------ */

static void test_acquire_is_idempotent(void)
{
    if (!drive_present()) return;

    /* The suite already holds it; re-acquiring must not be punished, so a
     * caller can acquire unconditionally rather than tracking whether it
     * already did (SCRUM-188, docs/syscall_spec.md §3.5a). */
    CU_ASSERT_EQUAL(exofs_bdev_acquire(), 0);
    CU_ASSERT_EQUAL(exofs_bdev_acquire(), 0);
    CU_ASSERT_EQUAL(disk_binding_owner(), syscall_current_context());
}

static void test_foreign_owner_blocks_transfers(void)
{
    if (!drive_present()) return;

    page_owner_t me = syscall_current_context();
    uint8_t *buf = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    CU_ASSERT_PTR_NOT_NULL(buf);
    if (buf == NULL) return;

    /* Hand the disk to another context and confirm the error surfaces
     * through the seam unchanged — -EXO_EBUSY, not a local code. Same
     * "another context" convention test_disk_binding_k.c and
     * test_syscall_disk_k.c use. */
    disk_binding_release(me);
    CU_ASSERT_EQUAL(disk_binding_acquire((page_owner_t)(PAGE_OWNER_LIBOS + 1)),
                    DISK_BIND_OK);

    CU_ASSERT_EQUAL(exofs_bdev_read(SEAM_LBA, buf, 1), -EXO_EBUSY);
    CU_ASSERT_EQUAL(exofs_bdev_write(SEAM_LBA, buf, 1), -EXO_EBUSY);

    /* Restore, so later tests in this suite still hold the binding. */
    disk_binding_release((page_owner_t)(PAGE_OWNER_LIBOS + 1));
    CU_ASSERT_EQUAL(exofs_bdev_acquire(), 0);
    CU_ASSERT_EQUAL(disk_binding_owner(), me);

    libos_heap_free(buf);
}

/* ---- Volume layer: format, superblock, mount ---------------------------- */

static void test_format_then_mount(void)
{
    if (!drive_present()) return;

    CU_ASSERT_EQUAL(exofs_format(EXOFS_TEST_BASE_LBA, VOL_SECTORS), 0);
    CU_ASSERT_EQUAL(exofs_mount(EXOFS_TEST_BASE_LBA), 0);
    CU_ASSERT_TRUE(exofs_is_mounted());

    exofs_geometry_t g;
    CU_ASSERT_EQUAL(exofs_geometry(&g), 0);

    CU_ASSERT_EQUAL(g.base_lba, EXOFS_TEST_BASE_LBA);
    CU_ASSERT_EQUAL(g.root_block, EXOFS_ROOT_BLOCK);

    /* The three geometry invariants format() is supposed to establish, each
     * checked against the others rather than against a hardcoded number —
     * the point is that they agree, not that they equal what this test
     * guessed. */
    CU_ASSERT_EQUAL(g.fat_blocks,
                    (g.total_blocks + EXOFS_FAT_PER_BLOCK - 1u)
                        / EXOFS_FAT_PER_BLOCK);
    CU_ASSERT_EQUAL(g.data_lba, EXOFS_TEST_BASE_LBA + 1u + g.fat_blocks);
    CU_ASSERT_TRUE(1u + g.fat_blocks + g.total_blocks <= VOL_SECTORS);

    /* And it should not have given away space it did not have to: one more
     * data block would have to overflow the region. */
    CU_ASSERT_TRUE(1u + ((g.total_blocks + 1u + EXOFS_FAT_PER_BLOCK - 1u)
                            / EXOFS_FAT_PER_BLOCK)
                      + (g.total_blocks + 1u) > VOL_SECTORS);

    exofs_unmount();
    CU_ASSERT_FALSE(exofs_is_mounted());
}

static void test_mount_survives_remount(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    exofs_geometry_t first;
    CU_ASSERT_EQUAL(exofs_geometry(&first), 0);
    exofs_unmount();

    /* Mounting again reads the same superblock off the disk rather than
     * anything left in memory — which is the property SCRUM-190's
     * prebuilt image depends on entirely. */
    CU_ASSERT_EQUAL(exofs_mount(EXOFS_TEST_BASE_LBA), 0);

    exofs_geometry_t second;
    CU_ASSERT_EQUAL(exofs_geometry(&second), 0);
    CU_ASSERT_EQUAL(first.total_blocks, second.total_blocks);
    CU_ASSERT_EQUAL(first.fat_blocks,   second.fat_blocks);
    CU_ASSERT_EQUAL(first.data_lba,     second.data_lba);

    exofs_unmount();
}

/*
 * The superblock's entire reason for existing: an unformatted region must be
 * refused, not interpreted. cfat had no such check and would read a blank
 * disk as a FAT full of zeros.
 */
static void test_mount_unformatted_region_rejected(void)
{
    if (!drive_present()) return;

    /* Zero a sector somewhere else in the scratch image and try to mount
     * there. Well clear of the formatted volume and of the seam tests. */
    const uint32_t blank_lba = EXOFS_TEST_BASE_LBA + 4096u;

    uint8_t *zero = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    CU_ASSERT_PTR_NOT_NULL(zero);
    if (zero == NULL) return;
    for (uint32_t i = 0; i < EXOFS_BLOCK_SIZE; i++) zero[i] = 0;

    exofs_unmount();
    CU_ASSERT_EQUAL(exofs_bdev_write(blank_lba, zero, 1), 0);
    CU_ASSERT_EQUAL(exofs_mount(blank_lba), -EXO_EINVAL);
    CU_ASSERT_FALSE(exofs_is_mounted());

    /* And garbage that is not merely zero, in case "all zeros" were being
     * special-cased somewhere rather than the magic actually being read. */
    for (uint32_t i = 0; i < EXOFS_BLOCK_SIZE; i++) zero[i] = (uint8_t)(i | 1u);
    CU_ASSERT_EQUAL(exofs_bdev_write(blank_lba, zero, 1), 0);
    CU_ASSERT_EQUAL(exofs_mount(blank_lba), -EXO_EINVAL);
    CU_ASSERT_FALSE(exofs_is_mounted());

    libos_heap_free(zero);
}

/*
 * A superblock with the right magic but inconsistent geometry must also be
 * refused. This is the case a corrupted sector produces, and the one where
 * "it has our magic, trust it" would hand every layer above a data_lba
 * pointing at nothing.
 */
static void test_mount_inconsistent_geometry_rejected(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    exofs_geometry_t g;
    CU_ASSERT_EQUAL(exofs_geometry(&g), 0);
    exofs_unmount();

    uint8_t *buf = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    CU_ASSERT_PTR_NOT_NULL(buf);
    if (buf == NULL) return;

    CU_ASSERT_EQUAL(exofs_bdev_read(EXOFS_TEST_BASE_LBA, buf, 1), 0);
    exofs_super_t *sb = (exofs_super_t *)buf;
    CU_ASSERT_EQUAL(sb->magic, EXOFS_MAGIC);

    const uint32_t good_fat  = sb->fat_blocks;
    const uint32_t good_data = sb->data_lba;
    const uint16_t good_ver  = sb->version;

    /* fat_blocks that does not match total_blocks. */
    sb->fat_blocks = good_fat + 1u;
    CU_ASSERT_EQUAL(exofs_bdev_write(EXOFS_TEST_BASE_LBA, sb, 1), 0);
    CU_ASSERT_EQUAL(exofs_mount(EXOFS_TEST_BASE_LBA), -EXO_EINVAL);
    sb->fat_blocks = good_fat;

    /* data_lba that does not follow the FAT. */
    sb->data_lba = good_data + 1u;
    CU_ASSERT_EQUAL(exofs_bdev_write(EXOFS_TEST_BASE_LBA, sb, 1), 0);
    CU_ASSERT_EQUAL(exofs_mount(EXOFS_TEST_BASE_LBA), -EXO_EINVAL);
    sb->data_lba = good_data;

    /* A version this build does not know. */
    sb->version = (uint16_t)(good_ver + 1u);
    CU_ASSERT_EQUAL(exofs_bdev_write(EXOFS_TEST_BASE_LBA, sb, 1), 0);
    CU_ASSERT_EQUAL(exofs_mount(EXOFS_TEST_BASE_LBA), -EXO_EINVAL);
    sb->version = good_ver;

    /* Restored, the same sector mounts again — proving the rejections above
     * were about the fields changed and not about the sector being touched. */
    CU_ASSERT_EQUAL(exofs_bdev_write(EXOFS_TEST_BASE_LBA, sb, 1), 0);
    CU_ASSERT_EQUAL(exofs_mount(EXOFS_TEST_BASE_LBA), 0);
    exofs_unmount();

    libos_heap_free(buf);
}

static void test_double_mount_rejected(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    /* One volume at a time (exofs.h). A second mount must not silently
     * leak the first volume's FAT allocation. */
    CU_ASSERT_EQUAL(exofs_mount(EXOFS_TEST_BASE_LBA), -EXO_EBUSY);
    CU_ASSERT_TRUE(exofs_is_mounted());

    exofs_unmount();
}

static void test_format_rejects_tiny_volume(void)
{
    /* No drive needed: the size check is ahead of any transfer. */
    CU_ASSERT_EQUAL(exofs_format(EXOFS_TEST_BASE_LBA, 0), -EXO_EINVAL);
    CU_ASSERT_EQUAL(exofs_format(EXOFS_TEST_BASE_LBA, 1), -EXO_EINVAL);
    CU_ASSERT_EQUAL(exofs_format(EXOFS_TEST_BASE_LBA,
                                 EXOFS_MIN_SECTORS - 1u), -EXO_EINVAL);
}

static void test_unmounted_calls_rejected(void)
{
    exofs_unmount();
    CU_ASSERT_FALSE(exofs_is_mounted());

    exofs_geometry_t g;
    CU_ASSERT_EQUAL(exofs_geometry(&g), -EXO_EINVAL);
    CU_ASSERT_EQUAL(exofs_sync(), -EXO_EINVAL);

    /* And unmounting nothing is a no-op rather than a fault — cleanup paths
     * call it unconditionally. */
    exofs_unmount();
}

/*
 * The FAT cache's writeback path, exercised without the FAT layer that will
 * normally drive it: dirty an entry by hand, sync, remount, and confirm the
 * change reached the disk. Remounting is what makes this a test of
 * writeback rather than of the in-memory array.
 */
static void test_fat_writeback_survives_remount(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    exofs_volume_t *v = exofs_vol();
    CU_ASSERT_PTR_NOT_NULL(v);
    if (v == NULL) return;

    /* Two entries in different FAT sectors, so the flush has to handle a
     * non-contiguous dirty set rather than one run. */
    const uint32_t a = 5u;
    const uint32_t b = EXOFS_FAT_PER_BLOCK + 9u;
    CU_ASSERT_TRUE(b < v->total_blocks);

    v->fat[a] = EXOFS_BLOCK_EOC;
    exofs_fat_mark_dirty(v, a);
    v->fat[b] = 1234u;
    exofs_fat_mark_dirty(v, b);

    CU_ASSERT_EQUAL(exofs_sync(), 0);
    exofs_unmount();

    CU_ASSERT_EQUAL(exofs_mount(EXOFS_TEST_BASE_LBA), 0);
    v = exofs_vol();
    CU_ASSERT_PTR_NOT_NULL(v);
    if (v == NULL) return;

    CU_ASSERT_EQUAL(v->fat[a], EXOFS_BLOCK_EOC);
    CU_ASSERT_EQUAL(v->fat[b], 1234u);

    /* An entry nobody touched must still be free — a flush that wrote back
     * more than it was asked to would show up here. */
    CU_ASSERT_EQUAL(v->fat[b + 1u], EXOFS_BLOCK_FREE);

    exofs_unmount();
}

/* A fresh format leaves the root's chain terminated and every other block
 * free. mount() already refuses a volume whose root entry is free, so this
 * checks the other half: that format did not mark anything else. */
static void test_fresh_volume_fat_is_empty(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    exofs_volume_t *v = exofs_vol();
    CU_ASSERT_PTR_NOT_NULL(v);
    if (v == NULL) return;

    CU_ASSERT_EQUAL(v->fat[EXOFS_ROOT_BLOCK], EXOFS_BLOCK_EOC);

    uint32_t allocated = 0;
    for (uint32_t i = 1; i < v->total_blocks; i++) {
        if (v->fat[i] != EXOFS_BLOCK_FREE) allocated++;
    }
    CU_ASSERT_EQUAL(allocated, 0u);

    exofs_unmount();
}

void suite_exofs_tests(CU_pSuite s)
{
    CU_add_test(s, "heap buffer lies in the LibOS window",
                test_heap_buffer_is_in_window);
    CU_add_test(s, "layout constants match the sector size",
                test_block_size_matches_sector_size);
    CU_add_test(s, "NULL buffer rejected", test_null_buffer_rejected);
    CU_add_test(s, "zero count is a no-op", test_zero_count_is_a_noop);
    CU_add_test(s, "kernel buffer rejected", test_kernel_buffer_rejected);
    CU_add_test(s, "single sector round-trips",
                test_single_sector_round_trips);
    CU_add_test(s, "transfer larger than the cap is chunked",
                test_transfer_larger_than_cap_is_chunked);
    CU_add_test(s, "acquire is idempotent", test_acquire_is_idempotent);
    CU_add_test(s, "foreign owner blocks transfers",
                test_foreign_owner_blocks_transfers);

    CU_add_test(s, "format then mount", test_format_then_mount);
    CU_add_test(s, "mount survives remount", test_mount_survives_remount);
    CU_add_test(s, "mount rejects an unformatted region",
                test_mount_unformatted_region_rejected);
    CU_add_test(s, "mount rejects inconsistent geometry",
                test_mount_inconsistent_geometry_rejected);
    CU_add_test(s, "double mount rejected", test_double_mount_rejected);
    CU_add_test(s, "format rejects a tiny volume",
                test_format_rejects_tiny_volume);
    CU_add_test(s, "calls without a mount rejected",
                test_unmounted_calls_rejected);
    CU_add_test(s, "FAT writeback survives remount",
                test_fat_writeback_survives_remount);
    CU_add_test(s, "a fresh volume's FAT is empty",
                test_fresh_volume_fat_is_empty);
}
