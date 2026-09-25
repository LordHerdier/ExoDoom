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
 * This file currently covers the block-I/O seam, the volume layer
 * (superblock/format/mount), the FAT layer (block allocation, chain
 * traversal), the name area (variable-length names), directory entries and
 * path resolution. The directory and file operations built on them land in
 * later steps of SCRUM-189 and add their tests here.
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
#include "libos_fs/exofs_fat.h"
#include "libos_fs/exofs_name.h"
#include "libos_fs/exofs_dirent.h"
#include "libos_fs/exofs_path.h"
#include "libos_fs/exofs_dir.h"

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
    exofs_bdev_release();
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

    /* Exactly one block beyond the root is in use: the name block holding
     * the root's own "." and "..", allocated when format() bootstraps it.
     * Asserting the count rather than "nothing else" is the point — a
     * format that allocated two blocks, or none, is equally wrong. */
    CU_ASSERT_NOT_EQUAL(v->name_head, EXOFS_NO_BLOCK);
    CU_ASSERT_EQUAL(v->fat[v->name_head], EXOFS_BLOCK_EOC);

    uint32_t allocated = 0;
    for (uint32_t i = 1; i < v->total_blocks; i++) {
        if (v->fat[i] != EXOFS_BLOCK_FREE) allocated++;
    }
    CU_ASSERT_EQUAL(allocated, 1u);

    exofs_unmount();
}

/* ---- FAT layer: allocation and chains ----------------------------------- */

/* Every FAT test needs a mounted volume and the volume pointer. This folds
 * the two guards into one so the tests below read as their subject rather
 * than as setup. Returns NULL if the caller should bail. */
static exofs_volume_t *fat_test_volume(void)
{
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return NULL; }

    exofs_volume_t *v = exofs_vol();
    CU_ASSERT_PTR_NOT_NULL(v);
    return v;
}

static void test_alloc_and_free_one_block(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    uint32_t before = exofs_fat_free_count(v);

    uint32_t blk = 0;
    CU_ASSERT_EQUAL(exofs_fat_alloc(v, &blk), 0);
    CU_ASSERT_TRUE(blk > 0);                 /* never the root block */
    CU_ASSERT_TRUE(blk < v->total_blocks);

    uint32_t ent = 0;
    CU_ASSERT_EQUAL(exofs_fat_get(v, blk, &ent), 0);
    CU_ASSERT_EQUAL(ent, EXOFS_BLOCK_EOC);   /* a one-block chain */
    CU_ASSERT_EQUAL(exofs_fat_free_count(v), before - 1u);

    CU_ASSERT_EQUAL(exofs_fat_free(v, blk), 0);
    CU_ASSERT_EQUAL(exofs_fat_free_count(v), before);

    /* Freeing it twice is an error, not a no-op: it means a chain was
     * walked twice or a block was double-owned. */
    CU_ASSERT_EQUAL(exofs_fat_free(v, blk), -EXO_EINVAL);

    exofs_unmount();
}

/*
 * A newly allocated block must read back as zeros even when its previous
 * owner left data in it. For a directory block those stale bytes would be
 * read as live entries — see exofs_fat.h.
 */
static void test_alloc_zeroes_the_block(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    uint32_t blk = 0;
    CU_ASSERT_EQUAL(exofs_fat_alloc(v, &blk), 0);

    uint8_t *buf = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    CU_ASSERT_PTR_NOT_NULL(buf);
    if (buf == NULL) return;

    /* Dirty it thoroughly, then give it back. */
    for (uint32_t i = 0; i < EXOFS_BLOCK_SIZE; i++) buf[i] = (uint8_t)(i | 0x80u);
    CU_ASSERT_EQUAL(exofs_write_block(blk, buf), 0);
    CU_ASSERT_EQUAL(exofs_fat_free(v, blk), 0);

    /* The hint points back at the block just freed, so this reallocates the
     * same one — which is exactly the case that matters. */
    uint32_t again = 0;
    CU_ASSERT_EQUAL(exofs_fat_alloc(v, &again), 0);
    CU_ASSERT_EQUAL(again, blk);

    for (uint32_t i = 0; i < EXOFS_BLOCK_SIZE; i++) buf[i] = 0xFFu;
    CU_ASSERT_EQUAL(exofs_read_block(blk, buf), 0);

    for (uint32_t i = 0; i < EXOFS_BLOCK_SIZE; i++) {
        if (buf[i] != 0) { CU_ASSERT_EQUAL(buf[i], 0); break; }
    }

    libos_heap_free(buf);
    exofs_unmount();
}

static void test_chain_extend_walk_and_free(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    uint32_t before = exofs_fat_free_count(v);

    uint32_t head = 0;
    CU_ASSERT_EQUAL(exofs_fat_alloc(v, &head), 0);

    /* Grow to 5 blocks and remember them, so nth/last can be checked
     * against the real identities rather than against each other. */
    uint32_t want[5];
    want[0] = head;
    for (uint32_t i = 1; i < 5; i++) {
        CU_ASSERT_EQUAL(exofs_chain_extend(v, head, &want[i]), 0);
    }

    uint32_t len = 0;
    CU_ASSERT_EQUAL(exofs_chain_len(v, head, &len), 0);
    CU_ASSERT_EQUAL(len, 5u);

    uint32_t last = 0;
    CU_ASSERT_EQUAL(exofs_chain_last(v, head, &last), 0);
    CU_ASSERT_EQUAL(last, want[4]);

    for (uint32_t i = 0; i < 5; i++) {
        uint32_t nth = 0;
        CU_ASSERT_EQUAL(exofs_chain_nth(v, head, i, &nth), 0);
        CU_ASSERT_EQUAL(nth, want[i]);
    }

    /* Past the end is -EXO_EINVAL (a bad request), not -EXO_EIO (a bad
     * disk) — the distinction a file read past EOF depends on. */
    uint32_t past = 0;
    CU_ASSERT_EQUAL(exofs_chain_nth(v, head, 5u, &past), -EXO_EINVAL);

    CU_ASSERT_EQUAL(exofs_fat_free_count(v), before - 5u);
    CU_ASSERT_EQUAL(exofs_chain_free(v, head), 0);
    CU_ASSERT_EQUAL(exofs_fat_free_count(v), before);

    exofs_unmount();
}

/*
 * The hardening cfat does not have: a FAT with a loop in it must be
 * reported, not spun on. cfat's chain walks are unbounded
 * `while (FAT[i] != USHRT_MAX)` loops and hang forever on this input.
 *
 * The cycle is built by hand because nothing the allocator does can produce
 * one — which is the point: this is what a torn write or a corrupted sector
 * leaves behind, and the filesystem reads from a real disk now.
 */
static void test_chain_cycle_is_reported_not_hung(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    uint32_t a = 0, b = 0, c = 0;
    CU_ASSERT_EQUAL(exofs_fat_alloc(v, &a), 0);
    CU_ASSERT_EQUAL(exofs_chain_extend(v, a, &b), 0);
    CU_ASSERT_EQUAL(exofs_chain_extend(v, a, &c), 0);

    /* Close the loop: c now points back at a. */
    CU_ASSERT_EQUAL(exofs_fat_set(v, c, a), 0);

    uint32_t out = 0;
    CU_ASSERT_EQUAL(exofs_chain_last(v, a, &out), -EXO_EIO);
    CU_ASSERT_EQUAL(exofs_chain_len(v, a, &out), -EXO_EIO);
    CU_ASSERT_EQUAL(exofs_chain_free(v, a), -EXO_EIO);

    /* chain_free refused, so it must not have freed anything — the
     * corruption is left intact rather than turned into a half-freed
     * chain. */
    uint32_t ent = 0;
    CU_ASSERT_EQUAL(exofs_fat_get(v, a, &ent), 0);
    CU_ASSERT_NOT_EQUAL(ent, EXOFS_BLOCK_FREE);
    CU_ASSERT_EQUAL(exofs_fat_get(v, b, &ent), 0);
    CU_ASSERT_NOT_EQUAL(ent, EXOFS_BLOCK_FREE);

    exofs_unmount();
}

/* A chain link pointing at a free block is corruption too: the chain claims
 * a block the allocator thinks nobody owns. */
static void test_chain_into_free_block_is_reported(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    uint32_t a = 0, b = 0;
    CU_ASSERT_EQUAL(exofs_fat_alloc(v, &a), 0);
    CU_ASSERT_EQUAL(exofs_chain_extend(v, a, &b), 0);

    /* Free b behind the chain's back. */
    CU_ASSERT_EQUAL(exofs_fat_set(v, b, EXOFS_BLOCK_FREE), 0);

    uint32_t out = 0;
    CU_ASSERT_EQUAL(exofs_chain_last(v, a, &out), -EXO_EIO);
    CU_ASSERT_EQUAL(exofs_chain_len(v, a, &out), -EXO_EIO);

    exofs_unmount();
}

/* Out-of-range block indices must be refused rather than indexing past the
 * FAT array — the failure mode a corrupted link would otherwise cause. */
static void test_out_of_range_blocks_rejected(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    uint32_t out = 0;
    CU_ASSERT_EQUAL(exofs_fat_get(v, v->total_blocks, &out), -EXO_EINVAL);
    CU_ASSERT_EQUAL(exofs_fat_get(v, 0xFFFFFFFFu, &out), -EXO_EINVAL);
    CU_ASSERT_EQUAL(exofs_fat_set(v, v->total_blocks, 1u), -EXO_EINVAL);
    CU_ASSERT_EQUAL(exofs_fat_free(v, v->total_blocks), -EXO_EINVAL);
    CU_ASSERT_EQUAL(exofs_chain_last(v, v->total_blocks, &out), -EXO_EINVAL);

    /* The FAT array is sized to whole sectors, so indices between
     * total_blocks and the end of the last sector are real memory but not
     * real blocks. Those are exactly the ones a naive bounds check on the
     * allocation size would let through. */
    uint32_t past_end = v->fat_blocks * EXOFS_FAT_PER_BLOCK - 1u;
    if (past_end >= v->total_blocks) {
        CU_ASSERT_EQUAL(exofs_fat_get(v, past_end, &out), -EXO_EINVAL);
    }

    exofs_unmount();
}

/* Filling the volume must report -EXO_ENOSPC rather than calling exit(),
 * which is what cfat's findFreeBlock() did. */
static void test_exhaustion_reports_enospc(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    /*
     * Allocating every block of a 4 MiB volume means ~8000 zeroing writes,
     * which is minutes of polled PIO and well past docs/testing.md's CI
     * budget. So the FAT is filled directly and only the last block is
     * allocated for real — the allocator's search and its full-volume
     * answer are what is under test, not the disk.
     */
    uint32_t last_free = 0;
    uint32_t free_seen = 0;
    for (uint32_t i = 1; i < v->total_blocks; i++) {
        if (v->fat[i] == EXOFS_BLOCK_FREE) {
            last_free = i;
            free_seen++;
        }
    }
    CU_ASSERT_TRUE(free_seen > 1u);

    for (uint32_t i = 1; i < v->total_blocks; i++) {
        if (i != last_free) v->fat[i] = EXOFS_BLOCK_EOC;
    }
    CU_ASSERT_EQUAL(exofs_fat_free_count(v), 1u);

    /* One block left: it must be found wherever the hint happens to be,
     * which is what the wrap in the search exists for. */
    v->next_free_hint = v->total_blocks - 1u;
    uint32_t blk = 0;
    CU_ASSERT_EQUAL(exofs_fat_alloc(v, &blk), 0);
    CU_ASSERT_EQUAL(blk, last_free);

    /* And now there are none. */
    CU_ASSERT_EQUAL(exofs_fat_free_count(v), 0u);
    CU_ASSERT_EQUAL(exofs_fat_alloc(v, &blk), -EXO_ENOSPC);
    CU_ASSERT_EQUAL(exofs_chain_extend(v, last_free, &blk), -EXO_ENOSPC);

    /* A failed extend must not have linked anything: last_free is still a
     * one-block chain. */
    uint32_t ent = 0;
    CU_ASSERT_EQUAL(exofs_fat_get(v, last_free, &ent), 0);
    CU_ASSERT_EQUAL(ent, EXOFS_BLOCK_EOC);

    exofs_unmount();
}

/* Chain changes are FAT changes, so they have to survive a sync/remount the
 * same way a hand-dirtied entry does. */
static void test_chain_survives_remount(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    uint32_t head = 0;
    CU_ASSERT_EQUAL(exofs_fat_alloc(v, &head), 0);

    uint32_t want[4];
    want[0] = head;
    for (uint32_t i = 1; i < 4; i++) {
        CU_ASSERT_EQUAL(exofs_chain_extend(v, head, &want[i]), 0);
    }

    CU_ASSERT_EQUAL(exofs_sync(), 0);
    exofs_unmount();
    CU_ASSERT_EQUAL(exofs_mount(EXOFS_TEST_BASE_LBA), 0);

    v = exofs_vol();
    CU_ASSERT_PTR_NOT_NULL(v);
    if (v == NULL) return;

    uint32_t len = 0;
    CU_ASSERT_EQUAL(exofs_chain_len(v, head, &len), 0);
    CU_ASSERT_EQUAL(len, 4u);

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t nth = 0;
        CU_ASSERT_EQUAL(exofs_chain_nth(v, head, i, &nth), 0);
        CU_ASSERT_EQUAL(nth, want[i]);
    }

    exofs_unmount();
}

/* ---- Name area: variable-length names -----------------------------------
 *
 * The ticket's headline fix — cfat capped names at 11 bytes, inline in the
 * directory entry — so this gets the heaviest coverage in the file.
 */

/* Fill `buf` with `len` bytes of a pattern that differs at every position,
 * plus a NUL. Used to build long names whose content is checkable rather
 * than a run of one character, so a read that returns the right length from
 * the wrong offset still fails. */
static void make_name(char *buf, uint32_t len, uint32_t seed)
{
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = alphabet[(i * 7u + seed) % (sizeof(alphabet) - 1u)];
    }
    buf[len] = '\0';
}

static void test_name_store_and_read_back(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    char name[EXOFS_MAX_NAME + 1];
    char back[EXOFS_MAX_NAME + 1];

    /* 1 byte, something ordinary, and the full 255 — the last being the
     * whole point of the ticket. */
    const uint32_t lens[] = { 1u, 11u, 12u, 64u, EXOFS_MAX_NAME };

    for (uint32_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        uint32_t len = lens[i];
        make_name(name, len, i);

        uint32_t blk = 0; uint16_t off = 0;
        CU_ASSERT_EQUAL(exofs_name_alloc(v, name, len, &blk, &off), 0);

        CU_ASSERT_EQUAL(exofs_name_read(v, blk, off, len, back), 0);
        CU_ASSERT_STRING_EQUAL(back, name);
        CU_ASSERT_EQUAL(exofs_name_equals(v, blk, off, len, name), 1);
    }

    exofs_unmount();
}

/* 12 bytes is one past cfat's MAXFILENAME. Called out on its own because
 * "longer than 11" is the acceptance criterion, not an incidental case. */
static void test_name_longer_than_cfat_limit(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    const char *name = "a-name-much-longer-than-eleven-characters.txt";
    uint32_t len = (uint32_t)strlen(name);
    CU_ASSERT_TRUE(len > 11u);

    uint32_t blk = 0; uint16_t off = 0;
    CU_ASSERT_EQUAL(exofs_name_alloc(v, name, len, &blk, &off), 0);

    char back[EXOFS_MAX_NAME + 1];
    CU_ASSERT_EQUAL(exofs_name_read(v, blk, off, len, back), 0);
    CU_ASSERT_STRING_EQUAL(back, name);

    exofs_unmount();
}

static void test_name_length_limits(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    char name[EXOFS_MAX_NAME + 2];
    uint32_t blk = 0; uint16_t off = 0;

    make_name(name, EXOFS_MAX_NAME + 1u, 0);
    CU_ASSERT_EQUAL(exofs_name_alloc(v, name, EXOFS_MAX_NAME + 1u, &blk, &off),
                    -EXO_EINVAL);

    make_name(name, 1u, 0);
    CU_ASSERT_EQUAL(exofs_name_alloc(v, name, 0, &blk, &off), -EXO_EINVAL);

    exofs_unmount();
}

/*
 * Two names that agree for their first 200 bytes must not be confused. The
 * case a length-capped or prefix-comparing implementation gets wrong, and
 * the reason make_name() builds content rather than padding.
 */
static void test_long_names_with_common_prefix_are_distinct(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    char a[EXOFS_MAX_NAME + 1];
    char b[EXOFS_MAX_NAME + 1];

    make_name(a, 250u, 0);
    make_name(b, 250u, 0);
    b[249] = (a[249] == 'x') ? 'y' : 'x';   /* differ only in the last byte */

    uint32_t ablk = 0, bblk = 0; uint16_t aoff = 0, boff = 0;
    CU_ASSERT_EQUAL(exofs_name_alloc(v, a, 250u, &ablk, &aoff), 0);
    CU_ASSERT_EQUAL(exofs_name_alloc(v, b, 250u, &bblk, &boff), 0);

    CU_ASSERT_EQUAL(exofs_name_equals(v, ablk, aoff, 250u, a), 1);
    CU_ASSERT_EQUAL(exofs_name_equals(v, ablk, aoff, 250u, b), 0);
    CU_ASSERT_EQUAL(exofs_name_equals(v, bblk, boff, 250u, b), 1);
    CU_ASSERT_EQUAL(exofs_name_equals(v, bblk, boff, 250u, a), 0);

    /* And a name that is a strict prefix of another is not equal to it. */
    char shorter[EXOFS_MAX_NAME + 1];
    memcpy(shorter, a, 100u);
    shorter[100] = '\0';
    CU_ASSERT_EQUAL(exofs_name_equals(v, ablk, aoff, 250u, shorter), 0);

    exofs_unmount();
}

/*
 * Reuse, not growth. Create and release the same-sized name repeatedly and
 * assert the name chain does not grow — the property that keeps SCRUM-104's
 * save-file rotation from consuming the volume.
 */
static void test_name_records_are_reused(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    char name[EXOFS_MAX_NAME + 1];
    make_name(name, 200u, 3);

    uint32_t blk = 0; uint16_t off = 0;
    CU_ASSERT_EQUAL(exofs_name_alloc(v, name, 200u, &blk, &off), 0);

    uint32_t chain_after_first = 0;
    CU_ASSERT_EQUAL(exofs_name_chain_len(v, &chain_after_first), 0);
    CU_ASSERT_EQUAL(chain_after_first, 1u);

    uint32_t free_blocks_before = exofs_fat_free_count(v);

    /* 50 cycles at 200 bytes each: without reuse this would need ~20 blocks
     * and the chain would grow well past 1. */
    for (uint32_t i = 0; i < 50u; i++) {
        CU_ASSERT_EQUAL(exofs_name_free(v, blk, off), 0);
        CU_ASSERT_EQUAL(exofs_name_alloc(v, name, 200u, &blk, &off), 0);
    }

    uint32_t chain_after_churn = 0;
    CU_ASSERT_EQUAL(exofs_name_chain_len(v, &chain_after_churn), 0);
    CU_ASSERT_EQUAL(chain_after_churn, 1u);
    CU_ASSERT_EQUAL(exofs_fat_free_count(v), free_blocks_before);

    /* The name still reads back correctly after all that churn. */
    char back[EXOFS_MAX_NAME + 1];
    CU_ASSERT_EQUAL(exofs_name_read(v, blk, off, 200u, back), 0);
    CU_ASSERT_STRING_EQUAL(back, name);

    exofs_unmount();
}

/*
 * Coalescing. Freeing several adjacent small records must produce one big
 * free record, not a row of unusable holes — otherwise a block fragments
 * until nothing fits while its tail is exhausted.
 */
static void test_freed_records_coalesce(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    char small[EXOFS_MAX_NAME + 1];
    char big[EXOFS_MAX_NAME + 1];
    make_name(small, 20u, 1);
    make_name(big, 200u, 2);

    /* Ten 20-byte names: 10 * 22 = 220 bytes of the block. */
    uint32_t blks[10]; uint16_t offs[10];
    for (uint32_t i = 0; i < 10u; i++) {
        CU_ASSERT_EQUAL(exofs_name_alloc(v, small, 20u, &blks[i], &offs[i]), 0);
        CU_ASSERT_EQUAL(blks[i], blks[0]);   /* all in the first block */
    }

    /* Free them all. Coalescing should hand the space back as one run. */
    for (uint32_t i = 0; i < 10u; i++) {
        CU_ASSERT_EQUAL(exofs_name_free(v, blks[i], offs[i]), 0);
    }

    uint32_t chain_before = 0;
    CU_ASSERT_EQUAL(exofs_name_chain_len(v, &chain_before), 0);

    /* A 200-byte name now has to fit in the space those ten vacated. It
     * only can if the holes merged: the largest single freed record was 20
     * bytes. */
    uint32_t bblk = 0; uint16_t boff = 0;
    CU_ASSERT_EQUAL(exofs_name_alloc(v, big, 200u, &bblk, &boff), 0);
    CU_ASSERT_EQUAL(bblk, blks[0]);

    uint32_t chain_after = 0;
    CU_ASSERT_EQUAL(exofs_name_chain_len(v, &chain_after), 0);
    CU_ASSERT_EQUAL(chain_after, chain_before);

    char back[EXOFS_MAX_NAME + 1];
    CU_ASSERT_EQUAL(exofs_name_read(v, bblk, boff, 200u, back), 0);
    CU_ASSERT_STRING_EQUAL(back, big);

    exofs_unmount();
}

/* An oversized free record is split, so a short name reusing a long one's
 * slot does not strand the remainder. */
static void test_oversized_record_is_split(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    char big[EXOFS_MAX_NAME + 1];
    char tiny[EXOFS_MAX_NAME + 1];
    make_name(big, 250u, 4);
    make_name(tiny, 4u, 5);

    uint32_t bblk = 0; uint16_t boff = 0;
    CU_ASSERT_EQUAL(exofs_name_alloc(v, big, 250u, &bblk, &boff), 0);
    CU_ASSERT_EQUAL(exofs_name_free(v, bblk, boff), 0);

    /* Reusing the 250-byte hole for a 4-byte name must leave the remainder
     * available: two more 100-byte names have to fit in the same block. */
    uint32_t t1 = 0, t2 = 0, t3 = 0; uint16_t o1 = 0, o2 = 0, o3 = 0;
    char mid[EXOFS_MAX_NAME + 1];
    make_name(mid, 100u, 6);

    CU_ASSERT_EQUAL(exofs_name_alloc(v, tiny, 4u, &t1, &o1), 0);
    CU_ASSERT_EQUAL(exofs_name_alloc(v, mid, 100u, &t2, &o2), 0);
    CU_ASSERT_EQUAL(exofs_name_alloc(v, mid, 100u, &t3, &o3), 0);

    CU_ASSERT_EQUAL(t1, bblk);
    CU_ASSERT_EQUAL(t2, bblk);
    CU_ASSERT_EQUAL(t3, bblk);

    /* All three read back intact — the split did not overlap them. */
    char back[EXOFS_MAX_NAME + 1];
    CU_ASSERT_EQUAL(exofs_name_read(v, t1, o1, 4u, back), 0);
    CU_ASSERT_STRING_EQUAL(back, tiny);
    CU_ASSERT_EQUAL(exofs_name_read(v, t2, o2, 100u, back), 0);
    CU_ASSERT_STRING_EQUAL(back, mid);
    CU_ASSERT_EQUAL(exofs_name_read(v, t3, o3, 100u, back), 0);
    CU_ASSERT_STRING_EQUAL(back, mid);

    exofs_unmount();
}

/* More names than one block holds must grow the chain, and every one of
 * them must still be readable afterwards. */
static void test_name_area_spans_multiple_blocks(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    /* 40 names of 100 bytes = ~4080 bytes, so at least 8 blocks. */
    enum { N = 40 };
    uint32_t blks[N]; uint16_t offs[N];
    char name[EXOFS_MAX_NAME + 1];
    char back[EXOFS_MAX_NAME + 1];

    for (uint32_t i = 0; i < N; i++) {
        make_name(name, 100u, i);
        CU_ASSERT_EQUAL(exofs_name_alloc(v, name, 100u, &blks[i], &offs[i]), 0);
    }

    uint32_t chain = 0;
    CU_ASSERT_EQUAL(exofs_name_chain_len(v, &chain), 0);
    CU_ASSERT_TRUE(chain > 1u);

    for (uint32_t i = 0; i < N; i++) {
        make_name(name, 100u, i);
        CU_ASSERT_EQUAL(exofs_name_read(v, blks[i], offs[i], 100u, back), 0);
        if (strcmp(back, name) != 0) {
            CU_ASSERT_STRING_EQUAL(back, name);
            break;
        }
    }

    exofs_unmount();
}

/* Names and their chain head must survive a remount — the head lives in the
 * superblock precisely so it can. */
static void test_names_survive_remount(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    char name[EXOFS_MAX_NAME + 1];
    make_name(name, 180u, 9);

    uint32_t blk = 0; uint16_t off = 0;
    CU_ASSERT_EQUAL(exofs_name_alloc(v, name, 180u, &blk, &off), 0);
    CU_ASSERT_EQUAL(exofs_sync(), 0);

    uint32_t head_before = v->name_head;
    CU_ASSERT_NOT_EQUAL(head_before, EXOFS_NO_BLOCK);

    exofs_unmount();
    CU_ASSERT_EQUAL(exofs_mount(EXOFS_TEST_BASE_LBA), 0);

    v = exofs_vol();
    CU_ASSERT_PTR_NOT_NULL(v);
    if (v == NULL) return;

    CU_ASSERT_EQUAL(v->name_head, head_before);

    char back[EXOFS_MAX_NAME + 1];
    CU_ASSERT_EQUAL(exofs_name_read(v, blk, off, 180u, back), 0);
    CU_ASSERT_STRING_EQUAL(back, name);

    exofs_unmount();
}

/* A dirent carrying a stale or corrupt reference must be refused, not
 * followed into the middle of another record or past the block. */
static void test_bad_name_references_rejected(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    char name[EXOFS_MAX_NAME + 1];
    char back[EXOFS_MAX_NAME + 1];
    make_name(name, 32u, 11);

    uint32_t blk = 0; uint16_t off = 0;
    CU_ASSERT_EQUAL(exofs_name_alloc(v, name, 32u, &blk, &off), 0);

    /* An offset with no header before it. */
    CU_ASSERT_EQUAL(exofs_name_read(v, blk, 0, 32u, back), -EXO_EINVAL);

    /* An offset past the end of the block. */
    CU_ASSERT_EQUAL(exofs_name_read(v, blk, EXOFS_BLOCK_SIZE, 32u, back),
                    -EXO_EINVAL);

    /* A length longer than the record actually reserved. */
    CU_ASSERT_EQUAL(exofs_name_read(v, blk, off, 200u, back), -EXO_EINVAL);

    /* A block index off the volume. */
    CU_ASSERT_EQUAL(exofs_name_read(v, v->total_blocks, off, 32u, back),
                    -EXO_EINVAL);

    /* Reading a record after it has been freed. */
    CU_ASSERT_EQUAL(exofs_name_free(v, blk, off), 0);
    CU_ASSERT_EQUAL(exofs_name_read(v, blk, off, 32u, back), -EXO_EINVAL);

    /* And freeing it twice. */
    CU_ASSERT_EQUAL(exofs_name_free(v, blk, off), -EXO_EINVAL);

    exofs_unmount();
}

/* ---- Directory entries --------------------------------------------------
 *
 * These drive exofs_dir_add/lookup/remove against the ROOT block directly.
 * The root has no `.`/`..` yet — exofs_dir.c writes those in the next step —
 * so a freshly formatted root is an empty directory, which is exactly the
 * clean starting point these need.
 */

/* Count the live entries in a directory, via the iterator. Also serves as
 * the iterator's own smoke test in every case that uses it. */
static int count_entries(exofs_volume_t *v, uint32_t head, uint32_t *out)
{
    exofs_dir_iter_t it;
    exofs_dir_iter_init(&it, head);

    uint32_t n = 0;
    for (;;) {
        int rc = exofs_dir_iter_next(v, &it, NULL, NULL);
        if (rc < 0) return rc;
        if (rc == 0) break;
        n++;
    }
    *out = n;
    return 0;
}

/* Live entries excluding "." and "..". Every directory has those two from
 * the moment it exists (exofs_dir.h), so this is what "how many things are
 * in here" means to a test. */
static int count_real_entries(exofs_volume_t *v, uint32_t head, uint32_t *out)
{
    exofs_dir_iter_t it;
    exofs_dir_iter_init(&it, head);

    uint32_t n = 0;
    for (;;) {
        exofs_dirent_t e;
        int rc = exofs_dir_iter_next(v, &it, NULL, &e);
        if (rc < 0) return rc;
        if (rc == 0) break;
        if (!exofs_dir_is_dot(v, &e)) n++;
    }
    *out = n;
    return 0;
}

static void test_dirent_add_lookup_remove(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    const uint32_t root = v->root_block;
    uint32_t n = 0;

    CU_ASSERT_EQUAL(count_real_entries(v, root, &n), 0);
    CU_ASSERT_EQUAL(n, 0u);

    exofs_entry_ref_t ref;
    CU_ASSERT_EQUAL(exofs_dir_add(v, root, "hello.txt", EXOFS_ATTR_FILE,
                                  EXOFS_NO_BLOCK, 0, &ref), 0);

    CU_ASSERT_EQUAL(count_real_entries(v, root, &n), 0);
    CU_ASSERT_EQUAL(n, 1u);

    exofs_dirent_t e;
    CU_ASSERT_EQUAL(exofs_dir_lookup(v, root, "hello.txt", NULL, &e), 0);
    CU_ASSERT_EQUAL(e.attributes, EXOFS_ATTR_FILE);
    CU_ASSERT_EQUAL(e.name_len, 9u);
    CU_ASSERT_TRUE(exofs_dirent_is_live(&e));

    /* The name really went through the name area. */
    char back[EXOFS_MAX_NAME + 1];
    CU_ASSERT_EQUAL(exofs_name_read(v, e.name_block, e.name_off, e.name_len,
                                    back), 0);
    CU_ASSERT_STRING_EQUAL(back, "hello.txt");

    /* A name that is not there. */
    CU_ASSERT_EQUAL(exofs_dir_lookup(v, root, "nope.txt", NULL, NULL),
                    -EXO_ENOENT);

    /* Duplicates refused, and the refusal costs nothing: still one entry. */
    CU_ASSERT_EQUAL(exofs_dir_add(v, root, "hello.txt", EXOFS_ATTR_FILE,
                                  EXOFS_NO_BLOCK, 0, NULL), -EXO_EEXIST);
    CU_ASSERT_EQUAL(count_real_entries(v, root, &n), 0);
    CU_ASSERT_EQUAL(n, 1u);

    CU_ASSERT_EQUAL(exofs_dir_remove(v, &ref), 0);
    CU_ASSERT_EQUAL(exofs_dir_lookup(v, root, "hello.txt", NULL, NULL),
                    -EXO_ENOENT);
    CU_ASSERT_EQUAL(count_real_entries(v, root, &n), 0);
    CU_ASSERT_EQUAL(n, 0u);

    exofs_unmount();
}

/* Removing an entry must release its name record too, or every create/delete
 * cycle leaks name bytes nothing can reach again. */
static void test_remove_releases_the_name(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    const uint32_t root = v->root_block;
    char name[EXOFS_MAX_NAME + 1];
    make_name(name, 200u, 21);

    exofs_entry_ref_t ref;
    CU_ASSERT_EQUAL(exofs_dir_add(v, root, name, EXOFS_ATTR_FILE,
                                  EXOFS_NO_BLOCK, 0, &ref), 0);

    exofs_dirent_t e;
    CU_ASSERT_EQUAL(exofs_dirent_read(v, &ref, &e), 0);
    uint32_t nblk = e.name_block; uint16_t noff = e.name_off;

    CU_ASSERT_EQUAL(exofs_dir_remove(v, &ref), 0);

    /* The record is gone, so reading it is refused rather than returning
     * stale bytes. */
    char back[EXOFS_MAX_NAME + 1];
    CU_ASSERT_EQUAL(exofs_name_read(v, nblk, noff, 200u, back), -EXO_EINVAL);

    /* And 50 add/remove cycles do not grow the name chain — the entry layer
     * has to be returning records, not just forgetting them. */
    uint32_t chain_before = 0;
    CU_ASSERT_EQUAL(exofs_name_chain_len(v, &chain_before), 0);

    for (uint32_t i = 0; i < 50u; i++) {
        CU_ASSERT_EQUAL(exofs_dir_add(v, root, name, EXOFS_ATTR_FILE,
                                      EXOFS_NO_BLOCK, 0, &ref), 0);
        CU_ASSERT_EQUAL(exofs_dir_remove(v, &ref), 0);
    }

    uint32_t chain_after = 0;
    CU_ASSERT_EQUAL(exofs_name_chain_len(v, &chain_after), 0);
    CU_ASSERT_EQUAL(chain_after, chain_before);

    exofs_unmount();
}

/* A removed slot must be reused before the directory is extended — cfat
 * never reclaimed one, so create/delete/create grew a directory forever. */
static void test_freed_slots_are_reused(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    const uint32_t root = v->root_block;

    /* Fill the root's single block exactly. "." and ".." already hold two
     * of its slots, so it takes EXOFS_ENTS_PER_BLOCK - 2 more. */
    enum { FILL = EXOFS_ENTS_PER_BLOCK - 2 };
    exofs_entry_ref_t refs[FILL];
    char name[32];
    for (uint32_t i = 0; i < FILL; i++) {
        make_name(name, 8u, i);
        CU_ASSERT_EQUAL(exofs_dir_add(v, root, name, EXOFS_ATTR_FILE,
                                      EXOFS_NO_BLOCK, 0, &refs[i]), 0);
        CU_ASSERT_EQUAL(refs[i].block, root);
    }

    uint32_t len_full = 0;
    CU_ASSERT_EQUAL(exofs_chain_len(v, root, &len_full), 0);
    CU_ASSERT_EQUAL(len_full, 1u);

    /* Free one in the middle and add another: it must land in the hole, not
     * in a newly allocated block. */
    const uint32_t victim = 7u;
    CU_ASSERT_EQUAL(exofs_dir_remove(v, &refs[victim]), 0);

    exofs_entry_ref_t fresh;
    CU_ASSERT_EQUAL(exofs_dir_add(v, root, "reused", EXOFS_ATTR_FILE,
                                  EXOFS_NO_BLOCK, 0, &fresh), 0);
    CU_ASSERT_EQUAL(fresh.block, refs[victim].block);
    CU_ASSERT_EQUAL(fresh.index, refs[victim].index);

    uint32_t len_after = 0;
    CU_ASSERT_EQUAL(exofs_chain_len(v, root, &len_after), 0);
    CU_ASSERT_EQUAL(len_after, 1u);

    exofs_unmount();
}

/* Past 16 entries the directory has to grow, and every entry must still be
 * findable across the block boundary. */
static void test_directory_spans_multiple_blocks(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    const uint32_t root = v->root_block;
    enum { N = 40 };   /* > 2 blocks' worth */
    char name[64];

    for (uint32_t i = 0; i < N; i++) {
        make_name(name, 20u, i);
        CU_ASSERT_EQUAL(exofs_dir_add(v, root, name, EXOFS_ATTR_FILE,
                                      EXOFS_NO_BLOCK, i, NULL), 0);
    }

    uint32_t chain = 0;
    CU_ASSERT_EQUAL(exofs_chain_len(v, root, &chain), 0);
    CU_ASSERT_TRUE(chain > 1u);

    uint32_t n = 0;
    CU_ASSERT_EQUAL(count_real_entries(v, root, &n), 0);
    CU_ASSERT_EQUAL(n, (uint32_t)N);

    /* Each one still resolves, and to the right entry — `size` carries the
     * index so a lookup returning the wrong entry is caught. */
    for (uint32_t i = 0; i < N; i++) {
        make_name(name, 20u, i);
        exofs_dirent_t e;
        int rc = exofs_dir_lookup(v, root, name, NULL, &e);
        if (rc != 0 || e.size != i) {
            CU_ASSERT_EQUAL(rc, 0);
            CU_ASSERT_EQUAL(e.size, i);
            break;
        }
    }

    exofs_unmount();
}

/*
 * Two entries whose names differ only past byte 11 — the case cfat could not
 * represent at all, since its names stopped at 11 bytes. Also the case its
 * strcmp-based iteration got wrong.
 */
static void test_entries_with_long_similar_names(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    const uint32_t root = v->root_block;
    const char *a = "same-prefix-aaaa.txt";
    const char *b = "same-prefix-bbbb.txt";

    CU_ASSERT_EQUAL(exofs_dir_add(v, root, a, EXOFS_ATTR_FILE,
                                  EXOFS_NO_BLOCK, 111u, NULL), 0);
    CU_ASSERT_EQUAL(exofs_dir_add(v, root, b, EXOFS_ATTR_FILE,
                                  EXOFS_NO_BLOCK, 222u, NULL), 0);

    exofs_dirent_t e;
    CU_ASSERT_EQUAL(exofs_dir_lookup(v, root, a, NULL, &e), 0);
    CU_ASSERT_EQUAL(e.size, 111u);
    CU_ASSERT_EQUAL(exofs_dir_lookup(v, root, b, NULL, &e), 0);
    CU_ASSERT_EQUAL(e.size, 222u);

    exofs_unmount();
}

/* Entries survive a remount: the whole point of writing them to disk. */
static void test_entries_survive_remount(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    const uint32_t root = v->root_block;
    char name[EXOFS_MAX_NAME + 1];
    make_name(name, 255u, 33);

    CU_ASSERT_EQUAL(exofs_dir_add(v, root, name, EXOFS_ATTR_DIRECTORY,
                                  42u, 7u, NULL), 0);
    CU_ASSERT_EQUAL(exofs_sync(), 0);

    exofs_unmount();
    CU_ASSERT_EQUAL(exofs_mount(EXOFS_TEST_BASE_LBA), 0);
    v = exofs_vol();
    CU_ASSERT_PTR_NOT_NULL(v);
    if (v == NULL) return;

    exofs_dirent_t e;
    CU_ASSERT_EQUAL(exofs_dir_lookup(v, v->root_block, name, NULL, &e), 0);
    CU_ASSERT_EQUAL(e.attributes, EXOFS_ATTR_DIRECTORY);
    CU_ASSERT_EQUAL(e.first_block, 42u);
    CU_ASSERT_EQUAL(e.size, 7u);
    CU_ASSERT_EQUAL(e.name_len, 255u);

    exofs_unmount();
}

/* ---- Path parsing and resolution ---------------------------------------- */

static void test_path_iterator(void)
{
    exofs_path_iter_t it;
    char comp[EXOFS_MAX_NAME + 1];

    /* Repeated and trailing separators are skipped, not turned into empty
     * components. */
    exofs_path_iter_init(&it, "//a///bb//ccc/");
    CU_ASSERT_EQUAL(exofs_path_iter_next(&it, comp), 1);
    CU_ASSERT_STRING_EQUAL(comp, "a");
    CU_ASSERT_EQUAL(exofs_path_iter_next(&it, comp), 1);
    CU_ASSERT_STRING_EQUAL(comp, "bb");
    CU_ASSERT_EQUAL(exofs_path_iter_next(&it, comp), 1);
    CU_ASSERT_STRING_EQUAL(comp, "ccc");
    CU_ASSERT_EQUAL(exofs_path_iter_next(&it, comp), 0);

    /* "/" has no components at all. */
    exofs_path_iter_init(&it, "/");
    CU_ASSERT_EQUAL(exofs_path_iter_next(&it, comp), 0);

    /*
     * Two iterators interleaved. This is the case cfat could not do: its
     * path walks used strtok(), whose state is a single static, so a nested
     * or interleaved walk silently destroyed the outer one.
     */
    exofs_path_iter_t a, b;
    char ca[EXOFS_MAX_NAME + 1], cb[EXOFS_MAX_NAME + 1];
    exofs_path_iter_init(&a, "/one/two");
    exofs_path_iter_init(&b, "/red/blue");

    CU_ASSERT_EQUAL(exofs_path_iter_next(&a, ca), 1);
    CU_ASSERT_STRING_EQUAL(ca, "one");
    CU_ASSERT_EQUAL(exofs_path_iter_next(&b, cb), 1);
    CU_ASSERT_STRING_EQUAL(cb, "red");
    CU_ASSERT_EQUAL(exofs_path_iter_next(&a, ca), 1);
    CU_ASSERT_STRING_EQUAL(ca, "two");
    CU_ASSERT_EQUAL(exofs_path_iter_next(&b, cb), 1);
    CU_ASSERT_STRING_EQUAL(cb, "blue");
    CU_ASSERT_EQUAL(exofs_path_iter_next(&a, ca), 0);
    CU_ASSERT_EQUAL(exofs_path_iter_next(&b, cb), 0);
}

static void test_path_validation(void)
{
    CU_ASSERT_EQUAL(exofs_path_validate("/"), 0);
    CU_ASSERT_EQUAL(exofs_path_validate("/a/b/c"), 0);

    CU_ASSERT_EQUAL(exofs_path_validate(NULL), -EXO_EINVAL);
    CU_ASSERT_EQUAL(exofs_path_validate(""), -EXO_EINVAL);
    CU_ASSERT_EQUAL(exofs_path_validate("relative/path"), -EXO_EINVAL);

    /* An over-long component is reported rather than truncated: a truncated
     * component would resolve to a different file. */
    char big[EXOFS_MAX_NAME + 8];
    big[0] = '/';
    for (uint32_t i = 1; i < EXOFS_MAX_NAME + 3u; i++) big[i] = 'x';
    big[EXOFS_MAX_NAME + 3u] = '\0';
    CU_ASSERT_EQUAL(exofs_path_validate(big), -EXO_EINVAL);
}

static void test_path_resolve(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    const uint32_t root = v->root_block;

    /* Build /dir -> /dir/file.txt by hand: exofs_dir.c does not exist yet,
     * so the subdirectory gets a chain of its own the same way mkdir will. */
    uint32_t dirblk = 0;
    CU_ASSERT_EQUAL(exofs_fat_alloc(v, &dirblk), 0);
    CU_ASSERT_EQUAL(exofs_dir_add(v, root, "dir", EXOFS_ATTR_DIRECTORY,
                                  dirblk, 0, NULL), 0);
    CU_ASSERT_EQUAL(exofs_dir_add(v, dirblk, "file.txt", EXOFS_ATTR_FILE,
                                  EXOFS_NO_BLOCK, 99u, NULL), 0);

    /* "/" is the synthetic root, with no slot to write back to. */
    exofs_entry_ref_t ref;
    exofs_dirent_t e;
    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/", &ref, &e), 0);
    CU_ASSERT_EQUAL(ref.block, EXOFS_NO_BLOCK);
    CU_ASSERT_EQUAL(e.first_block, root);
    CU_ASSERT_EQUAL(e.attributes, EXOFS_ATTR_DIRECTORY);

    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/dir", NULL, &e), 0);
    CU_ASSERT_EQUAL(e.first_block, dirblk);

    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/dir/file.txt", NULL, &e), 0);
    CU_ASSERT_EQUAL(e.size, 99u);

    /* Redundant separators resolve the same way. */
    CU_ASSERT_EQUAL(exofs_path_resolve(v, "//dir//file.txt", NULL, &e), 0);
    CU_ASSERT_EQUAL(e.size, 99u);

    /* Missing components, and descending through a file. The second is
     * -EXO_ENOTDIR rather than -EXO_ENOENT: "your path is wrong" is a
     * different answer from "it isn't there". */
    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/nope", NULL, NULL), -EXO_ENOENT);
    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/dir/nope", NULL, NULL),
                    -EXO_ENOENT);
    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/dir/file.txt/x", NULL, NULL),
                    -EXO_ENOTDIR);
    CU_ASSERT_EQUAL(exofs_path_resolve(v, "relative", NULL, NULL),
                    -EXO_EINVAL);

    exofs_unmount();
}

static void test_path_resolve_parent(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    const uint32_t root = v->root_block;

    uint32_t dirblk = 0;
    CU_ASSERT_EQUAL(exofs_fat_alloc(v, &dirblk), 0);
    CU_ASSERT_EQUAL(exofs_dir_add(v, root, "dir", EXOFS_ATTR_DIRECTORY,
                                  dirblk, 0, NULL), 0);

    exofs_dirent_t parent;
    char leaf[EXOFS_MAX_NAME + 1];

    /* A leaf directly under the root. */
    CU_ASSERT_EQUAL(exofs_path_resolve_parent(v, "/newfile.txt", &parent,
                                              leaf), 0);
    CU_ASSERT_EQUAL(parent.first_block, root);
    CU_ASSERT_STRING_EQUAL(leaf, "newfile.txt");

    /* A leaf one level down — the parent must be the subdirectory, which is
     * the off-by-one this function exists to get right. */
    CU_ASSERT_EQUAL(exofs_path_resolve_parent(v, "/dir/newfile.txt", &parent,
                                              leaf), 0);
    CU_ASSERT_EQUAL(parent.first_block, dirblk);
    CU_ASSERT_STRING_EQUAL(leaf, "newfile.txt");

    /* The leaf need not exist — that is the point, for create. */
    CU_ASSERT_EQUAL(exofs_dir_lookup(v, parent.first_block, leaf, NULL, NULL),
                    -EXO_ENOENT);

    /* But intermediate components must. */
    CU_ASSERT_EQUAL(exofs_path_resolve_parent(v, "/nope/x", &parent, leaf),
                    -EXO_ENOENT);

    /* The root has no parent, and saying so beats quietly returning the root
     * and letting a caller create an entry named "". */
    CU_ASSERT_EQUAL(exofs_path_resolve_parent(v, "/", &parent, leaf),
                    -EXO_EINVAL);

    exofs_unmount();
}

/* A directory chain with a cycle must be reported by the iterator, not spun
 * on — the same hardening the FAT layer has, at the level above it. */
static void test_directory_cycle_is_reported(void)
{
    if (!drive_present()) return;
    exofs_volume_t *v = fat_test_volume();
    if (v == NULL) return;

    const uint32_t root = v->root_block;

    /* Fill the root and force it to grow, then loop the second block back
     * to the first. */
    char name[32];
    for (uint32_t i = 0; i < EXOFS_ENTS_PER_BLOCK + 1u; i++) {
        make_name(name, 8u, i);
        CU_ASSERT_EQUAL(exofs_dir_add(v, root, name, EXOFS_ATTR_FILE,
                                      EXOFS_NO_BLOCK, 0, NULL), 0);
    }

    uint32_t second = 0;
    CU_ASSERT_EQUAL(exofs_fat_get(v, root, &second), 0);
    CU_ASSERT_NOT_EQUAL(second, EXOFS_BLOCK_EOC);
    CU_ASSERT_EQUAL(exofs_fat_set(v, second, root), 0);

    uint32_t n = 0;
    CU_ASSERT_EQUAL(count_entries(v, root, &n), -EXO_EIO);

    exofs_unmount();
}

/* ---- Directory operations ----------------------------------------------- */

/* Whether `name` appears in the directory `path`, via the public readdir. */
static int readdir_contains(const char *path, const char *name, int *found)
{
    exofs_dir_t d;
    int rc = exofs_opendir(path, &d);
    if (rc < 0) return rc;

    *found = 0;
    for (;;) {
        exofs_dirinfo_t info;
        rc = exofs_readdir(&d, &info);
        if (rc < 0) { exofs_closedir(&d); return rc; }
        if (rc == 0) break;
        if (strcmp(info.name, name) == 0) *found = 1;
    }

    return exofs_closedir(&d);
}

/* A freshly formatted root already has "." and "..", written by format()
 * itself -- which is why format has to mount the volume it just laid down. */
static void test_fresh_root_has_dot_entries(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    exofs_volume_t *v = exofs_vol();
    CU_ASSERT_PTR_NOT_NULL(v);
    if (v == NULL) return;

    exofs_dirent_t dot, dotdot;
    CU_ASSERT_EQUAL(exofs_dir_lookup(v, v->root_block, ".", NULL, &dot), 0);
    CU_ASSERT_EQUAL(exofs_dir_lookup(v, v->root_block, "..", NULL, &dotdot), 0);

    CU_ASSERT_EQUAL(dot.attributes, EXOFS_ATTR_DIRECTORY);
    CU_ASSERT_EQUAL(dot.first_block, v->root_block);

    /* The root's ".." points at the root: there is nowhere above it, and a
     * self-loop makes "/.." resolve to "/" with no special case in the path
     * code. */
    CU_ASSERT_EQUAL(dotdot.first_block, v->root_block);

    uint32_t real = 0;
    CU_ASSERT_EQUAL(count_real_entries(v, v->root_block, &real), 0);
    CU_ASSERT_EQUAL(real, 0u);

    exofs_unmount();
}

static void test_mkdir_and_readdir(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    CU_ASSERT_EQUAL(exofs_mkdir("/docs"), 0);
    CU_ASSERT_EQUAL(exofs_mkdir("/docs/notes"), 0);

    /* A long name, since that is what this ticket is for. */
    char longname[EXOFS_MAX_NAME + 1];
    make_name(longname, 200u, 44);
    char longpath[EXOFS_MAX_NAME + 16];
    longpath[0] = '/';
    memcpy(longpath + 1, longname, 201u);
    CU_ASSERT_EQUAL(exofs_mkdir(longpath), 0);

    int found = 0;
    CU_ASSERT_EQUAL(readdir_contains("/", "docs", &found), 0);
    CU_ASSERT_TRUE(found);
    CU_ASSERT_EQUAL(readdir_contains("/", longname, &found), 0);
    CU_ASSERT_TRUE(found);
    CU_ASSERT_EQUAL(readdir_contains("/docs", "notes", &found), 0);
    CU_ASSERT_TRUE(found);

    /* readdir reports "." and "..", the way POSIX does. */
    CU_ASSERT_EQUAL(readdir_contains("/docs", ".", &found), 0);
    CU_ASSERT_TRUE(found);
    CU_ASSERT_EQUAL(readdir_contains("/docs", "..", &found), 0);
    CU_ASSERT_TRUE(found);

    /* And the new directory is reachable by path. */
    exofs_volume_t *v = exofs_vol();
    exofs_dirent_t e;
    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/docs/notes", NULL, &e), 0);
    CU_ASSERT_EQUAL(e.attributes, EXOFS_ATTR_DIRECTORY);

    exofs_unmount();
}

static void test_mkdir_errors(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    CU_ASSERT_EQUAL(exofs_mkdir("/a"), 0);

    CU_ASSERT_EQUAL(exofs_mkdir("/a"), -EXO_EEXIST);
    CU_ASSERT_EQUAL(exofs_mkdir("/missing/b"), -EXO_ENOENT);
    CU_ASSERT_EQUAL(exofs_mkdir("relative"), -EXO_EINVAL);
    CU_ASSERT_EQUAL(exofs_mkdir("/"), -EXO_EINVAL);

    /* Descending through a non-directory. Built by hand because there are
     * no files yet -- exofs_file.c is the next step. */
    exofs_volume_t *v = exofs_vol();
    CU_ASSERT_EQUAL(exofs_dir_add(v, v->root_block, "afile", EXOFS_ATTR_FILE,
                                  EXOFS_NO_BLOCK, 0, NULL), 0);
    CU_ASSERT_EQUAL(exofs_mkdir("/afile/b"), -EXO_ENOTDIR);

    exofs_unmount();
}

/*
 * ".." really navigates upwards, with no special case in the path code --
 * the payoff for storing it as a real entry.
 */
static void test_dotdot_navigates_upward(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    CU_ASSERT_EQUAL(exofs_mkdir("/x"), 0);
    CU_ASSERT_EQUAL(exofs_mkdir("/x/y"), 0);

    exofs_volume_t *v = exofs_vol();
    exofs_dirent_t root_e, up_e, back_e;

    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/", NULL, &root_e), 0);
    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/x/y/..", NULL, &up_e), 0);
    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/x", NULL, &back_e), 0);
    CU_ASSERT_EQUAL(up_e.first_block, back_e.first_block);

    /* Two levels up lands on the root. */
    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/x/y/../..", NULL, &up_e), 0);
    CU_ASSERT_EQUAL(up_e.first_block, root_e.first_block);

    /* "/.." is the root, because the root's ".." points at itself. */
    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/..", NULL, &up_e), 0);
    CU_ASSERT_EQUAL(up_e.first_block, root_e.first_block);

    /* And "." stays put. */
    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/x/./y", NULL, &up_e), 0);
    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/x/y", NULL, &back_e), 0);
    CU_ASSERT_EQUAL(up_e.first_block, back_e.first_block);

    exofs_unmount();
}

static void test_rmdir(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    exofs_volume_t *v = exofs_vol();
    uint32_t free_before = exofs_fat_free_count(v);

    CU_ASSERT_EQUAL(exofs_mkdir("/tmp"), 0);
    CU_ASSERT_EQUAL(exofs_mkdir("/tmp/inner"), 0);

    /* A directory with something in it is refused. */
    CU_ASSERT_EQUAL(exofs_rmdir("/tmp"), -EXO_ENOTEMPTY);

    /* The root cannot be removed: it has no parent to be removed from. */
    CU_ASSERT_EQUAL(exofs_rmdir("/"), -EXO_EBUSY);

    /* Not there. */
    CU_ASSERT_EQUAL(exofs_rmdir("/nope"), -EXO_ENOENT);

    CU_ASSERT_EQUAL(exofs_rmdir("/tmp/inner"), 0);
    CU_ASSERT_EQUAL(exofs_rmdir("/tmp"), 0);

    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/tmp", NULL, NULL), -EXO_ENOENT);

    /* Everything both directories held is back: their blocks AND their
     * name records. A rmdir that forgot the "."/".." names would leak two
     * records per directory, which this catches because the name area
     * would have had to grow. */
    CU_ASSERT_EQUAL(exofs_fat_free_count(v), free_before);

    exofs_unmount();
}

/*
 * mkdir/rmdir in a loop must return everything it took, every time. The
 * cycle that would expose a leaked name record, a leaked block or an
 * un-reused entry slot -- all three of which cfat had.
 */
static void test_mkdir_rmdir_cycle_leaks_nothing(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    exofs_volume_t *v = exofs_vol();

    /* One cycle first, so the name area has reached its steady size before
     * the baseline is taken. */
    CU_ASSERT_EQUAL(exofs_mkdir("/cycle"), 0);
    CU_ASSERT_EQUAL(exofs_rmdir("/cycle"), 0);

    uint32_t free_before = exofs_fat_free_count(v);
    uint32_t names_before = 0;
    CU_ASSERT_EQUAL(exofs_name_chain_len(v, &names_before), 0);

    for (uint32_t i = 0; i < 30u; i++) {
        CU_ASSERT_EQUAL(exofs_mkdir("/cycle"), 0);
        CU_ASSERT_EQUAL(exofs_rmdir("/cycle"), 0);
    }

    CU_ASSERT_EQUAL(exofs_fat_free_count(v), free_before);

    uint32_t names_after = 0;
    CU_ASSERT_EQUAL(exofs_name_chain_len(v, &names_after), 0);
    CU_ASSERT_EQUAL(names_after, names_before);

    uint32_t real = 0;
    CU_ASSERT_EQUAL(count_real_entries(v, v->root_block, &real), 0);
    CU_ASSERT_EQUAL(real, 0u);

    exofs_unmount();
}

/* A directory tree has to survive a remount -- this is the acceptance
 * criterion's "list a root-directory entry" half, through the public API. */
static void test_directories_survive_remount(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    char longname[EXOFS_MAX_NAME + 1];
    make_name(longname, 255u, 77);
    char longpath[EXOFS_MAX_NAME + 16];
    longpath[0] = '/';
    memcpy(longpath + 1, longname, 256u);

    CU_ASSERT_EQUAL(exofs_mkdir("/keep"), 0);
    CU_ASSERT_EQUAL(exofs_mkdir("/keep/deeper"), 0);
    CU_ASSERT_EQUAL(exofs_mkdir(longpath), 0);
    CU_ASSERT_EQUAL(exofs_sync(), 0);

    exofs_unmount();
    CU_ASSERT_EQUAL(exofs_mount(EXOFS_TEST_BASE_LBA), 0);

    int found = 0;
    CU_ASSERT_EQUAL(readdir_contains("/", "keep", &found), 0);
    CU_ASSERT_TRUE(found);
    CU_ASSERT_EQUAL(readdir_contains("/", longname, &found), 0);
    CU_ASSERT_TRUE(found);
    CU_ASSERT_EQUAL(readdir_contains("/keep", "deeper", &found), 0);
    CU_ASSERT_TRUE(found);

    exofs_volume_t *v = exofs_vol();
    exofs_dirent_t e;
    CU_ASSERT_EQUAL(exofs_path_resolve(v, "/keep/deeper", NULL, &e), 0);
    CU_ASSERT_EQUAL(e.attributes, EXOFS_ATTR_DIRECTORY);

    exofs_unmount();
}

static void test_opendir_errors(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    exofs_dir_t d;
    CU_ASSERT_EQUAL(exofs_opendir("/nope", &d), -EXO_ENOENT);

    exofs_volume_t *v = exofs_vol();
    CU_ASSERT_EQUAL(exofs_dir_add(v, v->root_block, "plainfile",
                                  EXOFS_ATTR_FILE, EXOFS_NO_BLOCK, 0, NULL),
                    0);
    CU_ASSERT_EQUAL(exofs_opendir("/plainfile", &d), -EXO_ENOTDIR);

    /* Reading a closed directory is a bad handle, not a crash. */
    CU_ASSERT_EQUAL(exofs_opendir("/", &d), 0);
    CU_ASSERT_EQUAL(exofs_closedir(&d), 0);

    exofs_dirinfo_t info;
    CU_ASSERT_EQUAL(exofs_readdir(&d, &info), -EXO_EBADF);

    exofs_unmount();
}

/* Formatting under a live mount would leave that mount describing a volume
 * that no longer exists. */
static void test_format_under_a_mount_refused(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    CU_ASSERT_TRUE(exofs_is_mounted());
    CU_ASSERT_EQUAL(exofs_format(EXOFS_TEST_BASE_LBA, VOL_SECTORS),
                    -EXO_EBUSY);
    CU_ASSERT_TRUE(exofs_is_mounted());

    exofs_unmount();
}

/* ---- File operations ---------------------------------------------------- */

/* Fill `buf` with `n` bytes of a per-offset pattern. Deliberately varies
 * with the absolute offset, so data written at the right length to the wrong
 * offset still fails. */
static void fill_pattern(uint8_t *buf, uint32_t n, uint32_t base, uint32_t seed)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = base + i;
        buf[i] = (uint8_t)((o * 31u) ^ (o >> 8) ^ seed);
    }
}

static int check_pattern(const uint8_t *buf, uint32_t n, uint32_t base,
                         uint32_t seed)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = base + i;
        if (buf[i] != (uint8_t)((o * 31u) ^ (o >> 8) ^ seed)) return 0;
    }
    return 1;
}

static void test_file_create_write_read(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    exofs_file_t f;
    CU_ASSERT_EQUAL(exofs_open("/hello.txt",
                               EXOFS_O_WRITE | EXOFS_O_CREATE, &f), 0);

    const char *msg = "hello, exofs";
    CU_ASSERT_EQUAL(exofs_write(&f, msg, 12u), 12);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);

    exofs_stat_t st;
    CU_ASSERT_EQUAL(exofs_stat("/hello.txt", &st), 0);
    CU_ASSERT_EQUAL(st.size, 12u);
    CU_ASSERT_EQUAL(st.attributes, EXOFS_ATTR_FILE);

    char back[32];
    memset(back, 0, sizeof(back));
    CU_ASSERT_EQUAL(exofs_open("/hello.txt", EXOFS_O_READ, &f), 0);
    CU_ASSERT_EQUAL(exofs_read(&f, back, 32u), 12);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);
    CU_ASSERT_STRING_EQUAL(back, msg);

    exofs_unmount();
}

/* An empty file costs no data blocks: first_block stays EXOFS_NO_BLOCK
 * until something is actually written. */
static void test_empty_file_uses_no_blocks(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    exofs_volume_t *v = exofs_vol();
    uint32_t free_before = exofs_fat_free_count(v);

    exofs_file_t f;
    CU_ASSERT_EQUAL(exofs_open("/empty", EXOFS_O_WRITE | EXOFS_O_CREATE, &f),
                    0);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);

    /* The entry and its name fit in blocks that already existed, so nothing
     * new was allocated at all. */
    CU_ASSERT_EQUAL(exofs_fat_free_count(v), free_before);

    exofs_dirent_t e;
    CU_ASSERT_EQUAL(exofs_dir_lookup(v, v->root_block, "empty", NULL, &e), 0);
    CU_ASSERT_EQUAL(e.first_block, EXOFS_NO_BLOCK);
    CU_ASSERT_EQUAL(e.size, 0u);

    /* Reading it returns EOF rather than failing. */
    char buf[8];
    CU_ASSERT_EQUAL(exofs_open("/empty", EXOFS_O_READ, &f), 0);
    CU_ASSERT_EQUAL(exofs_read(&f, buf, 8u), 0);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);

    exofs_unmount();
}

/* A write spanning several blocks, read back byte-exact. The case that
 * exercises chain extension, partial first/last blocks and the position
 * cache all at once. */
static void test_multiblock_write_and_read(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    /* Deliberately not a multiple of the block size, and not starting on a
     * block boundary once the partial writes below are done. */
    const uint32_t N = 5000u;

    uint8_t *w = libos_heap_alloc(N);
    uint8_t *r = libos_heap_alloc(N);
    CU_ASSERT_PTR_NOT_NULL(w);
    CU_ASSERT_PTR_NOT_NULL(r);
    if (w == NULL || r == NULL) return;

    fill_pattern(w, N, 0, 5);

    exofs_file_t f;
    CU_ASSERT_EQUAL(exofs_open("/big.bin", EXOFS_O_WRITE | EXOFS_O_CREATE, &f),
                    0);

    /* Written in odd-sized pieces so no write starts block-aligned. */
    uint32_t done = 0;
    while (done < N) {
        uint32_t chunk = 300u;
        if (chunk > N - done) chunk = N - done;
        CU_ASSERT_EQUAL(exofs_write(&f, w + done, chunk), (int64_t)chunk);
        done += chunk;
    }
    CU_ASSERT_EQUAL(exofs_close(&f), 0);

    exofs_stat_t st;
    CU_ASSERT_EQUAL(exofs_stat("/big.bin", &st), 0);
    CU_ASSERT_EQUAL(st.size, N);

    memset(r, 0, N);
    CU_ASSERT_EQUAL(exofs_open("/big.bin", EXOFS_O_READ, &f), 0);

    done = 0;
    while (done < N) {
        uint32_t chunk = 700u;
        if (chunk > N - done) chunk = N - done;
        CU_ASSERT_EQUAL(exofs_read(&f, r + done, chunk), (int64_t)chunk);
        done += chunk;
    }
    /* And nothing past the end. */
    CU_ASSERT_EQUAL(exofs_read(&f, r, 16u), 0);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);

    CU_ASSERT_TRUE(check_pattern(r, N, 0, 5));

    libos_heap_free(w);
    libos_heap_free(r);
    exofs_unmount();
}

static void test_seek_and_overwrite(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    const uint32_t N = 2000u;
    uint8_t *w = libos_heap_alloc(N);
    uint8_t *r = libos_heap_alloc(N);
    CU_ASSERT_PTR_NOT_NULL(w);
    CU_ASSERT_PTR_NOT_NULL(r);
    if (w == NULL || r == NULL) return;

    fill_pattern(w, N, 0, 9);

    exofs_file_t f;
    CU_ASSERT_EQUAL(exofs_open("/seek.bin",
                               EXOFS_O_READ | EXOFS_O_WRITE | EXOFS_O_CREATE,
                               &f), 0);
    CU_ASSERT_EQUAL(exofs_write(&f, w, N), (int64_t)N);

    /* Seek back into the middle -- across a block boundary -- and overwrite
     * in place. The file must not grow. */
    CU_ASSERT_EQUAL(exofs_seek(&f, 700, EXOFS_SEEK_SET), 700);
    uint8_t patch[100];
    fill_pattern(patch, 100u, 700u, 77);
    CU_ASSERT_EQUAL(exofs_write(&f, patch, 100u), 100);
    CU_ASSERT_EQUAL(f.size, N);

    /* SEEK_CUR and SEEK_END. */
    CU_ASSERT_EQUAL(exofs_seek(&f, -50, EXOFS_SEEK_CUR), 750);
    CU_ASSERT_EQUAL(exofs_seek(&f, 0, EXOFS_SEEK_END), (int64_t)N);
    CU_ASSERT_EQUAL(exofs_seek(&f, -1, EXOFS_SEEK_SET), -EXO_EINVAL);

    CU_ASSERT_EQUAL(exofs_seek(&f, 0, EXOFS_SEEK_SET), 0);
    memset(r, 0, N);
    CU_ASSERT_EQUAL(exofs_read(&f, r, N), (int64_t)N);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);

    /* Everything outside [700, 800) is the original pattern; inside it is
     * the patch. */
    CU_ASSERT_TRUE(check_pattern(r, 700u, 0, 9));
    CU_ASSERT_TRUE(check_pattern(r + 700, 100u, 700u, 77));
    CU_ASSERT_TRUE(check_pattern(r + 800, N - 800u, 800u, 9));

    libos_heap_free(w);
    libos_heap_free(r);
    exofs_unmount();
}

/* Seeking past the end and writing leaves a hole that reads as zeros --
 * for free, because every block is zeroed when allocated. */
static void test_hole_reads_as_zeros(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    exofs_file_t f;
    CU_ASSERT_EQUAL(exofs_open("/hole.bin",
                               EXOFS_O_READ | EXOFS_O_WRITE | EXOFS_O_CREATE,
                               &f), 0);

    CU_ASSERT_EQUAL(exofs_write(&f, "AB", 2u), 2);
    CU_ASSERT_EQUAL(exofs_seek(&f, 3000, EXOFS_SEEK_SET), 3000);
    CU_ASSERT_EQUAL(exofs_write(&f, "CD", 2u), 2);
    CU_ASSERT_EQUAL(f.size, 3002u);

    uint8_t *r = libos_heap_alloc(3002u);
    CU_ASSERT_PTR_NOT_NULL(r);
    if (r == NULL) return;
    memset(r, 0xFF, 3002u);

    CU_ASSERT_EQUAL(exofs_seek(&f, 0, EXOFS_SEEK_SET), 0);
    CU_ASSERT_EQUAL(exofs_read(&f, r, 3002u), 3002);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);

    CU_ASSERT_EQUAL(r[0], 'A');
    CU_ASSERT_EQUAL(r[1], 'B');
    CU_ASSERT_EQUAL(r[3000], 'C');
    CU_ASSERT_EQUAL(r[3001], 'D');

    uint32_t nonzero = 0;
    for (uint32_t i = 2; i < 3000u; i++) if (r[i] != 0) nonzero++;
    CU_ASSERT_EQUAL(nonzero, 0u);

    libos_heap_free(r);
    exofs_unmount();
}

static void test_append_and_truncate(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    exofs_file_t f;
    CU_ASSERT_EQUAL(exofs_open("/log.txt", EXOFS_O_WRITE | EXOFS_O_CREATE, &f),
                    0);
    CU_ASSERT_EQUAL(exofs_write(&f, "one", 3u), 3);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);

    /* APPEND starts at the end and stays there. */
    CU_ASSERT_EQUAL(exofs_open("/log.txt", EXOFS_O_WRITE | EXOFS_O_APPEND, &f),
                    0);
    CU_ASSERT_EQUAL(exofs_seek(&f, 0, EXOFS_SEEK_SET), 0);
    CU_ASSERT_EQUAL(exofs_write(&f, "two", 3u), 3);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);

    char back[16];
    memset(back, 0, sizeof(back));
    CU_ASSERT_EQUAL(exofs_open("/log.txt", EXOFS_O_READ, &f), 0);
    CU_ASSERT_EQUAL(exofs_read(&f, back, 16u), 6);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);
    CU_ASSERT_STRING_EQUAL(back, "onetwo");

    /* TRUNC discards the contents and gives the blocks back. */
    exofs_volume_t *v = exofs_vol();
    uint32_t free_before = exofs_fat_free_count(v);

    CU_ASSERT_EQUAL(exofs_open("/log.txt", EXOFS_O_WRITE | EXOFS_O_TRUNC, &f),
                    0);
    CU_ASSERT_EQUAL(f.size, 0u);
    CU_ASSERT_EQUAL(f.first_block, EXOFS_NO_BLOCK);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);

    CU_ASSERT_EQUAL(exofs_fat_free_count(v), free_before + 1u);

    exofs_stat_t st;
    CU_ASSERT_EQUAL(exofs_stat("/log.txt", &st), 0);
    CU_ASSERT_EQUAL(st.size, 0u);

    exofs_unmount();
}

static void test_open_errors(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    exofs_file_t f;

    /* Absent without O_CREATE. */
    CU_ASSERT_EQUAL(exofs_open("/nope", EXOFS_O_READ, &f), -EXO_ENOENT);

    /* Neither read nor write. */
    CU_ASSERT_EQUAL(exofs_open("/nope", EXOFS_O_CREATE, &f), -EXO_EINVAL);

    /* A directory is not a file, including the root. */
    CU_ASSERT_EQUAL(exofs_mkdir("/adir"), 0);
    CU_ASSERT_EQUAL(exofs_open("/adir", EXOFS_O_READ, &f), -EXO_EISDIR);
    CU_ASSERT_EQUAL(exofs_open("/", EXOFS_O_READ, &f), -EXO_EISDIR);

    /* Missing parent, malformed path. */
    CU_ASSERT_EQUAL(exofs_open("/missing/x",
                               EXOFS_O_WRITE | EXOFS_O_CREATE, &f),
                    -EXO_ENOENT);
    CU_ASSERT_EQUAL(exofs_open("relative", EXOFS_O_READ, &f), -EXO_EINVAL);

    /* Access is enforced per handle. */
    CU_ASSERT_EQUAL(exofs_open("/ro", EXOFS_O_WRITE | EXOFS_O_CREATE, &f), 0);
    CU_ASSERT_EQUAL(exofs_read(&f, (void *)&f, 1u), -EXO_EACCES);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);

    CU_ASSERT_EQUAL(exofs_open("/ro", EXOFS_O_READ, &f), 0);
    CU_ASSERT_EQUAL(exofs_write(&f, "x", 1u), -EXO_EACCES);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);

    /* A closed handle is a bad descriptor, not a crash. */
    CU_ASSERT_EQUAL(exofs_read(&f, (void *)&f, 1u), -EXO_EBADF);
    CU_ASSERT_EQUAL(exofs_write(&f, "x", 1u), -EXO_EBADF);
    CU_ASSERT_EQUAL(exofs_seek(&f, 0, EXOFS_SEEK_SET), -EXO_EBADF);

    exofs_unmount();
}

static void test_unlink(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    exofs_volume_t *v = exofs_vol();

    /* One cycle first so the name area has reached its steady size. */
    exofs_file_t f;
    CU_ASSERT_EQUAL(exofs_open("/gone", EXOFS_O_WRITE | EXOFS_O_CREATE, &f), 0);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);
    CU_ASSERT_EQUAL(exofs_unlink("/gone"), 0);

    uint32_t free_before = exofs_fat_free_count(v);

    uint8_t *w = libos_heap_alloc(4000u);
    CU_ASSERT_PTR_NOT_NULL(w);
    if (w == NULL) return;
    fill_pattern(w, 4000u, 0, 3);

    CU_ASSERT_EQUAL(exofs_open("/gone", EXOFS_O_WRITE | EXOFS_O_CREATE, &f), 0);
    CU_ASSERT_EQUAL(exofs_write(&f, w, 4000u), 4000);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);

    CU_ASSERT_TRUE(exofs_fat_free_count(v) < free_before);

    CU_ASSERT_EQUAL(exofs_unlink("/gone"), 0);
    CU_ASSERT_EQUAL(exofs_stat("/gone", NULL), -EXO_EINVAL);

    exofs_stat_t st;
    CU_ASSERT_EQUAL(exofs_stat("/gone", &st), -EXO_ENOENT);

    /* Every block AND the name record came back. */
    CU_ASSERT_EQUAL(exofs_fat_free_count(v), free_before);

    /* A directory is rmdir's job, not unlink's. */
    CU_ASSERT_EQUAL(exofs_mkdir("/adir"), 0);
    CU_ASSERT_EQUAL(exofs_unlink("/adir"), -EXO_EISDIR);
    CU_ASSERT_EQUAL(exofs_unlink("/"), -EXO_EISDIR);
    CU_ASSERT_EQUAL(exofs_unlink("/nothing"), -EXO_ENOENT);

    libos_heap_free(w);
    exofs_unmount();
}

/* Create/write/unlink in a loop returns everything, every time -- the file
 * analogue of the mkdir/rmdir cycle. */
static void test_file_cycle_leaks_nothing(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    exofs_volume_t *v = exofs_vol();
    uint8_t *w = libos_heap_alloc(1500u);
    CU_ASSERT_PTR_NOT_NULL(w);
    if (w == NULL) return;
    fill_pattern(w, 1500u, 0, 1);

    exofs_file_t f;
    CU_ASSERT_EQUAL(exofs_open("/churn", EXOFS_O_WRITE | EXOFS_O_CREATE, &f), 0);
    CU_ASSERT_EQUAL(exofs_write(&f, w, 1500u), 1500);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);
    CU_ASSERT_EQUAL(exofs_unlink("/churn"), 0);

    uint32_t free_before = exofs_fat_free_count(v);
    uint32_t names_before = 0;
    CU_ASSERT_EQUAL(exofs_name_chain_len(v, &names_before), 0);

    for (uint32_t i = 0; i < 20u; i++) {
        CU_ASSERT_EQUAL(exofs_open("/churn", EXOFS_O_WRITE | EXOFS_O_CREATE,
                                   &f), 0);
        CU_ASSERT_EQUAL(exofs_write(&f, w, 1500u), 1500);
        CU_ASSERT_EQUAL(exofs_close(&f), 0);
        CU_ASSERT_EQUAL(exofs_unlink("/churn"), 0);
    }

    CU_ASSERT_EQUAL(exofs_fat_free_count(v), free_before);

    uint32_t names_after = 0;
    CU_ASSERT_EQUAL(exofs_name_chain_len(v, &names_after), 0);
    CU_ASSERT_EQUAL(names_after, names_before);

    libos_heap_free(w);
    exofs_unmount();
}

/*
 * SCRUM-189's acceptance criteria, end to end and through the public API
 * only: format a disk, put a file on it with a name cfat could not have
 * stored, unmount, mount fresh, list the root directory, and read the file's
 * contents back correctly -- all over exo_disk_read/exo_disk_write, with no
 * blob table anywhere in sight.
 */
static void test_acceptance_round_trip(void)
{
    if (!drive_present()) return;

    const char *fname = "a-readme-with-a-long-name.txt";
    const uint32_t N = 3333u;

    uint8_t *w = libos_heap_alloc(N);
    uint8_t *r = libos_heap_alloc(N);
    CU_ASSERT_PTR_NOT_NULL(w);
    CU_ASSERT_PTR_NOT_NULL(r);
    if (w == NULL || r == NULL) return;
    fill_pattern(w, N, 0, 189);

    /* 1. Format and populate. */
    exofs_unmount();
    CU_ASSERT_EQUAL(exofs_format(EXOFS_TEST_BASE_LBA, VOL_SECTORS), 0);
    CU_ASSERT_EQUAL(exofs_mount(EXOFS_TEST_BASE_LBA), 0);

    CU_ASSERT_EQUAL(exofs_mkdir("/subdir"), 0);

    char path[EXOFS_MAX_NAME + 8];
    path[0] = '/';
    memcpy(path + 1, fname, strlen(fname) + 1u);

    exofs_file_t f;
    CU_ASSERT_EQUAL(exofs_open(path, EXOFS_O_WRITE | EXOFS_O_CREATE, &f), 0);
    CU_ASSERT_EQUAL(exofs_write(&f, w, N), (int64_t)N);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);

    /* 2. Unmount -- everything now has to come off the disk. */
    exofs_unmount();
    CU_ASSERT_FALSE(exofs_is_mounted());

    /* 3. Mount fresh and list the root. */
    CU_ASSERT_EQUAL(exofs_mount(EXOFS_TEST_BASE_LBA), 0);

    int saw_file = 0, saw_dir = 0;
    exofs_dir_t d;
    CU_ASSERT_EQUAL(exofs_opendir("/", &d), 0);
    for (;;) {
        exofs_dirinfo_t info;
        int rc = exofs_readdir(&d, &info);
        CU_ASSERT_TRUE(rc >= 0);
        if (rc <= 0) break;

        if (strcmp(info.name, fname) == 0) {
            saw_file = 1;
            CU_ASSERT_EQUAL(info.size, N);
            CU_ASSERT_EQUAL(info.attributes, EXOFS_ATTR_FILE);
        }
        if (strcmp(info.name, "subdir") == 0) {
            saw_dir = 1;
            CU_ASSERT_EQUAL(info.attributes, EXOFS_ATTR_DIRECTORY);
        }
    }
    CU_ASSERT_EQUAL(exofs_closedir(&d), 0);
    CU_ASSERT_TRUE(saw_file);
    CU_ASSERT_TRUE(saw_dir);

    /* 4. Read the contents back and compare. */
    memset(r, 0, N);
    CU_ASSERT_EQUAL(exofs_open(path, EXOFS_O_READ, &f), 0);
    CU_ASSERT_EQUAL(exofs_read(&f, r, N), (int64_t)N);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);
    CU_ASSERT_TRUE(check_pattern(r, N, 0, 189));

    libos_heap_free(w);
    libos_heap_free(r);
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

    CU_add_test(s, "allocate and free one block",
                test_alloc_and_free_one_block);
    CU_add_test(s, "allocation zeroes the block",
                test_alloc_zeroes_the_block);
    CU_add_test(s, "chain extend, walk and free",
                test_chain_extend_walk_and_free);
    CU_add_test(s, "a FAT cycle is reported, not hung",
                test_chain_cycle_is_reported_not_hung);
    CU_add_test(s, "a chain into a free block is reported",
                test_chain_into_free_block_is_reported);
    CU_add_test(s, "out-of-range blocks rejected",
                test_out_of_range_blocks_rejected);
    CU_add_test(s, "exhaustion reports ENOSPC",
                test_exhaustion_reports_enospc);
    CU_add_test(s, "a chain survives a remount",
                test_chain_survives_remount);

    CU_add_test(s, "names store and read back",
                test_name_store_and_read_back);
    CU_add_test(s, "a name longer than cfat's 11-byte cap",
                test_name_longer_than_cfat_limit);
    CU_add_test(s, "name length limits enforced", test_name_length_limits);
    CU_add_test(s, "long names with a common prefix stay distinct",
                test_long_names_with_common_prefix_are_distinct);
    CU_add_test(s, "freed name records are reused",
                test_name_records_are_reused);
    CU_add_test(s, "freed name records coalesce",
                test_freed_records_coalesce);
    CU_add_test(s, "an oversized name record is split",
                test_oversized_record_is_split);
    CU_add_test(s, "the name area spans multiple blocks",
                test_name_area_spans_multiple_blocks);
    CU_add_test(s, "names survive a remount", test_names_survive_remount);
    CU_add_test(s, "bad name references rejected",
                test_bad_name_references_rejected);

    CU_add_test(s, "entry add, lookup and remove",
                test_dirent_add_lookup_remove);
    CU_add_test(s, "removing an entry releases its name",
                test_remove_releases_the_name);
    CU_add_test(s, "freed entry slots are reused",
                test_freed_slots_are_reused);
    CU_add_test(s, "a directory spans multiple blocks",
                test_directory_spans_multiple_blocks);
    CU_add_test(s, "entries with long, similar names",
                test_entries_with_long_similar_names);
    CU_add_test(s, "entries survive a remount",
                test_entries_survive_remount);
    CU_add_test(s, "path component iterator", test_path_iterator);
    CU_add_test(s, "path validation", test_path_validation);
    CU_add_test(s, "path resolution", test_path_resolve);
    CU_add_test(s, "parent path resolution", test_path_resolve_parent);
    CU_add_test(s, "a directory cycle is reported",
                test_directory_cycle_is_reported);

    CU_add_test(s, "a fresh root has . and ..",
                test_fresh_root_has_dot_entries);
    CU_add_test(s, "mkdir and readdir", test_mkdir_and_readdir);
    CU_add_test(s, "mkdir error cases", test_mkdir_errors);
    CU_add_test(s, ".. navigates upward", test_dotdot_navigates_upward);
    CU_add_test(s, "rmdir", test_rmdir);
    CU_add_test(s, "mkdir/rmdir cycles leak nothing",
                test_mkdir_rmdir_cycle_leaks_nothing);
    CU_add_test(s, "directories survive a remount",
                test_directories_survive_remount);
    CU_add_test(s, "opendir error cases", test_opendir_errors);
    CU_add_test(s, "format under a live mount is refused",
                test_format_under_a_mount_refused);

    CU_add_test(s, "file create, write and read",
                test_file_create_write_read);
    CU_add_test(s, "an empty file uses no blocks",
                test_empty_file_uses_no_blocks);
    CU_add_test(s, "multi-block write and read",
                test_multiblock_write_and_read);
    CU_add_test(s, "seek and overwrite in place",
                test_seek_and_overwrite);
    CU_add_test(s, "a hole reads as zeros", test_hole_reads_as_zeros);
    CU_add_test(s, "append and truncate", test_append_and_truncate);
    CU_add_test(s, "open error cases", test_open_errors);
    CU_add_test(s, "unlink", test_unlink);
    CU_add_test(s, "file cycles leak nothing",
                test_file_cycle_leaks_nothing);
    CU_add_test(s, "ACCEPTANCE: format, write, remount, list, read back",
                test_acceptance_round_trip);
}
