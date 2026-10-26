/*
 * test_exofs_multivol_k.c — several ExoFS volumes mounted at once (SCRUM-265).
 *
 * Until SCRUM-265 the mounted volume was a file-static, so a second
 * exofs_mount() was -EXO_EBUSY and copying between volumes meant mounting one
 * at a time. This suite holds up to EXOFS_MAX_VOLUMES together and checks the
 * things that can only go wrong when there is more than one: a write landing
 * on the wrong volume, a handle outliving its mount, the pool filling, and
 * the same volume being mounted twice.
 *
 * LAYOUT. Volume A is the whole second drive (block device 1, the install
 * disk's slot -- test_multidisk_k.c's STICK_VOL_BASE) and B and C are two
 * halves of test_exofs_k.c's LBA region on device 0. Tests return early
 * without the second drive, like test_multidisk_k.c, so a boot on a
 * developer's own QEMU line still passes.
 */

#include "kunit.h"
#include "blkdev.h"
#include "exo_errno.h"
#include "libos_heap.h"
#include "string.h"

#include "libos_fs/exofs.h"
#include "libos_fs/exofs_internal.h"
#include "libos_fs/exofs_blockdev.h"
#include "shell/shell_install.h"

#include <stdint.h>
#include <stddef.h>

#define DEV_SYS   0u
#define DEV_STICK 1u

#define A_BASE  0u
#define A_SECTORS 8192u
#define B_BASE  8192u      /* test_exofs_k.c's EXOFS_TEST_BASE_LBA */
#define C_BASE  12288u
#define BC_SECTORS 4096u

#define FILE_LEN 5000u
#define CHUNK    1024u

static int two_disks(void)
{
    return blkdev_get(DEV_SYS) != NULL && blkdev_get(DEV_STICK) != NULL;
}

int exofs_multivol_suite_init(void)
{
    exofs_unmount_all();
    return 0;
}

int exofs_multivol_suite_cleanup(void)
{
    exofs_unmount_all();
    return 0;
}

static void fill(uint8_t *p, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0; i < len; i++)
        p[i] = (uint8_t)(seed + i * 7u);
}

/* Bind both devices and lay down a fresh volume on each of A, B and C. */
static int setup(void)
{
    exofs_unmount_all();
    if (exofs_bdev_acquire(DEV_STICK) != 0)
        return 0;
    if (exofs_bdev_acquire_extent(DEV_SYS, B_BASE, 2u * BC_SECTORS) != 0) {
        exofs_bdev_release(DEV_STICK);
        return 0;
    }
    if (exofs_format(DEV_STICK, A_BASE, A_SECTORS) != 0 ||
        exofs_format(DEV_SYS, B_BASE, BC_SECTORS) != 0 ||
        exofs_format(DEV_SYS, C_BASE, BC_SECTORS) != 0) {
        exofs_bdev_release(DEV_SYS);
        exofs_bdev_release(DEV_STICK);
        return 0;
    }
    return 1;
}

static void teardown(void)
{
    exofs_unmount_all();
    exofs_bdev_release(DEV_SYS);
    exofs_bdev_release(DEV_STICK);
}

static int put(exofs_vol_t *v, const char *path, const uint8_t *buf,
               uint32_t len)
{
    exofs_file_t f;
    int rc = exofs_open(v, path, EXOFS_O_WRITE | EXOFS_O_CREATE, &f);
    if (rc != 0)
        return rc;
    int64_t n = exofs_write(&f, buf, len);
    int crc = exofs_close(&f);
    if (n != (int64_t)len)
        return n < 0 ? (int)n : -EXO_ENOSPC;
    return crc;
}

/* Exactly `len` bytes of `path`, or a negative error. */
static int64_t get(exofs_vol_t *v, const char *path, uint8_t *buf,
                   uint32_t len)
{
    exofs_file_t f;
    int rc = exofs_open(v, path, EXOFS_O_READ, &f);
    if (rc != 0)
        return rc;
    int64_t n = exofs_read(&f, buf, len);
    exofs_close(&f);
    return n;
}

static void test_two_volumes_interleaved(void)
{
    if (!two_disks()) return;
    if (!setup()) { CU_ASSERT_TRUE(0); return; }

    uint8_t *wa = libos_heap_alloc(FILE_LEN);
    uint8_t *wb = libos_heap_alloc(FILE_LEN);
    uint8_t *rd = libos_heap_alloc(FILE_LEN);
    CU_ASSERT_TRUE(wa && wb && rd);
    if (!wa || !wb || !rd) goto out;
    fill(wa, FILE_LEN, 0x11);
    fill(wb, FILE_LEN, 0x77);

    exofs_vol_t *a = NULL, *b = NULL;
    CU_ASSERT_EQUAL(exofs_mount(DEV_STICK, A_BASE, &a), 0);
    CU_ASSERT_EQUAL(exofs_mount(DEV_SYS, B_BASE, &b), 0);
    CU_ASSERT_PTR_NOT_NULL(a);
    CU_ASSERT_PTR_NOT_NULL(b);
    CU_ASSERT_TRUE(a != b);
    if (!a || !b) goto out;

    /* Write A, B, then read A, B: each file is on its own volume only. */
    CU_ASSERT_EQUAL(put(a, "/only_a.bin", wa, FILE_LEN), 0);
    CU_ASSERT_EQUAL(put(b, "/only_b.bin", wb, FILE_LEN), 0);

    memset(rd, 0, FILE_LEN);
    CU_ASSERT_EQUAL(get(a, "/only_a.bin", rd, FILE_LEN), (int64_t)FILE_LEN);
    CU_ASSERT_EQUAL(memcmp(rd, wa, FILE_LEN), 0);
    memset(rd, 0, FILE_LEN);
    CU_ASSERT_EQUAL(get(b, "/only_b.bin", rd, FILE_LEN), (int64_t)FILE_LEN);
    CU_ASSERT_EQUAL(memcmp(rd, wb, FILE_LEN), 0);

    exofs_stat_t st;
    CU_ASSERT_EQUAL(exofs_stat(a, "/only_b.bin", &st), -EXO_ENOENT);
    CU_ASSERT_EQUAL(exofs_stat(b, "/only_a.bin", &st), -EXO_ENOENT);

    /* Interleaved on open handles: two files open at once, one per volume,
     * written a chunk at a time in alternation. */
    exofs_file_t fa, fb;
    CU_ASSERT_EQUAL(exofs_open(a, "/mix.bin",
                               EXOFS_O_WRITE | EXOFS_O_CREATE, &fa), 0);
    CU_ASSERT_EQUAL(exofs_open(b, "/mix.bin",
                               EXOFS_O_WRITE | EXOFS_O_CREATE, &fb), 0);
    for (uint32_t off = 0; off < FILE_LEN; off += CHUNK) {
        uint32_t n = FILE_LEN - off < CHUNK ? FILE_LEN - off : CHUNK;
        CU_ASSERT_EQUAL(exofs_write(&fa, wa + off, n), (int64_t)n);
        CU_ASSERT_EQUAL(exofs_write(&fb, wb + off, n), (int64_t)n);
    }
    CU_ASSERT_EQUAL(exofs_close(&fa), 0);
    CU_ASSERT_EQUAL(exofs_close(&fb), 0);

    memset(rd, 0, FILE_LEN);
    CU_ASSERT_EQUAL(get(a, "/mix.bin", rd, FILE_LEN), (int64_t)FILE_LEN);
    CU_ASSERT_EQUAL(memcmp(rd, wa, FILE_LEN), 0);
    memset(rd, 0, FILE_LEN);
    CU_ASSERT_EQUAL(get(b, "/mix.bin", rd, FILE_LEN), (int64_t)FILE_LEN);
    CU_ASSERT_EQUAL(memcmp(rd, wb, FILE_LEN), 0);

    /* Unmounting one leaves the other working. */
    exofs_unmount(a);
    memset(rd, 0, FILE_LEN);
    CU_ASSERT_EQUAL(get(b, "/only_b.bin", rd, FILE_LEN), (int64_t)FILE_LEN);
    CU_ASSERT_EQUAL(memcmp(rd, wb, FILE_LEN), 0);
    CU_ASSERT_EQUAL(exofs_stat(a, "/only_a.bin", &st), -EXO_EINVAL);

out:
    libos_heap_free(wa);
    libos_heap_free(wb);
    libos_heap_free(rd);
    teardown();
}

static void test_pool_fills_and_frees(void)
{
    if (!two_disks()) return;
    if (!setup()) { CU_ASSERT_TRUE(0); return; }

    exofs_vol_t *a = NULL, *b = NULL, *c = NULL, *d = NULL;
    CU_ASSERT_EQUAL(exofs_mount(DEV_STICK, A_BASE, &a), 0);
    CU_ASSERT_EQUAL(exofs_mount(DEV_SYS, B_BASE, &b), 0);
    CU_ASSERT_EQUAL(exofs_mount(DEV_SYS, C_BASE, &c), 0);

    /* All EXOFS_MAX_VOLUMES slots taken: a further volume is refused before
     * anything is read, and the refusal leaves `d` alone. */
    CU_ASSERT_EQUAL(EXOFS_MAX_VOLUMES, 3u);
    CU_ASSERT_EQUAL(exofs_mount(DEV_SYS, 20000u, &d), -EXO_ENFILE);
    CU_ASSERT_PTR_NULL(d);

    /* A slot comes back when its volume is unmounted. */
    exofs_unmount(c);
    CU_ASSERT_EQUAL(exofs_mount(DEV_SYS, C_BASE, &d), 0);
    CU_ASSERT_PTR_NOT_NULL(d);

    teardown();
}

static void test_same_volume_twice_is_ebusy(void)
{
    if (!two_disks()) return;
    if (!setup()) { CU_ASSERT_TRUE(0); return; }

    exofs_vol_t *a = NULL, *again = NULL;
    CU_ASSERT_EQUAL(exofs_mount(DEV_STICK, A_BASE, &a), 0);
    CU_ASSERT_EQUAL(exofs_mount(DEV_STICK, A_BASE, &again), -EXO_EBUSY);
    CU_ASSERT_PTR_NULL(again);

    /* The first mount is undisturbed. */
    exofs_geometry_t g;
    CU_ASSERT_EQUAL(exofs_geometry(a, &g), 0);
    CU_ASSERT_EQUAL(g.base_lba, A_BASE);

    teardown();
}

static void test_handle_stale_after_unmount(void)
{
    if (!two_disks()) return;
    if (!setup()) { CU_ASSERT_TRUE(0); return; }

    uint8_t data[64];
    fill(data, sizeof(data), 0x42);

    exofs_vol_t *b = NULL;
    CU_ASSERT_EQUAL(exofs_mount(DEV_SYS, B_BASE, &b), 0);
    CU_ASSERT_EQUAL(put(b, "/f.bin", data, sizeof(data)), 0);

    exofs_file_t f;
    exofs_dir_t dir;
    CU_ASSERT_EQUAL(exofs_open(b, "/f.bin", EXOFS_O_READ, &f), 0);
    CU_ASSERT_EQUAL(exofs_opendir(b, "/", &dir), 0);

    exofs_unmount(b);

    /* The slot is free again, and the very next mount takes it -- of a
     * different volume, so the old file must not read from it. */
    exofs_vol_t *c = NULL;
    CU_ASSERT_EQUAL(exofs_mount(DEV_SYS, C_BASE, &c), 0);

    uint8_t rd[64];
    exofs_dirinfo_t info;
    CU_ASSERT_EQUAL(exofs_read(&f, rd, sizeof(rd)), -EXO_EBADF);
    CU_ASSERT_EQUAL(exofs_seek(&f, 0, EXOFS_SEEK_SET), -EXO_EBADF);
    CU_ASSERT_EQUAL(exofs_readdir(&dir, &info), -EXO_EBADF);
    CU_ASSERT_EQUAL(exofs_close(&f), -EXO_EBADF);

    /* The stale handle did not disturb the volume now in its slot. */
    exofs_stat_t st;
    CU_ASSERT_EQUAL(exofs_stat(c, "/", &st), 0);

    teardown();
}

static void test_format_other_volume_while_mounted(void)
{
    if (!two_disks()) return;
    if (!setup()) { CU_ASSERT_TRUE(0); return; }

    uint8_t data[64];
    uint8_t rd[64];
    fill(data, sizeof(data), 0x5A);

    exofs_vol_t *b = NULL;
    CU_ASSERT_EQUAL(exofs_mount(DEV_SYS, B_BASE, &b), 0);
    CU_ASSERT_EQUAL(put(b, "/keep.bin", data, sizeof(data)), 0);

    /* C is somebody else's volume: reformatting it leaves B alone. */
    CU_ASSERT_EQUAL(exofs_format(DEV_SYS, C_BASE, BC_SECTORS), 0);
    memset(rd, 0, sizeof(rd));
    CU_ASSERT_EQUAL(get(b, "/keep.bin", rd, sizeof(rd)), (int64_t)sizeof(rd));
    CU_ASSERT_EQUAL(memcmp(rd, data, sizeof(rd)), 0);

    /* B itself is mounted, so formatting it is still refused. */
    CU_ASSERT_EQUAL(exofs_format(DEV_SYS, B_BASE, BC_SECTORS), -EXO_EBUSY);

    teardown();
}

static void test_copy_between_mounted_volumes(void)
{
    if (!two_disks()) return;
    if (!setup()) { CU_ASSERT_TRUE(0); return; }

    uint8_t *src = libos_heap_alloc(FILE_LEN);
    uint8_t *rd = libos_heap_alloc(FILE_LEN);
    uint8_t *buf = libos_heap_alloc(CHUNK);
    CU_ASSERT_TRUE(src && rd && buf);
    if (!src || !rd || !buf) goto out;
    fill(src, FILE_LEN, 0x3C);

    exofs_vol_t *a = NULL, *b = NULL;
    CU_ASSERT_EQUAL(exofs_mount(DEV_STICK, A_BASE, &a), 0);
    CU_ASSERT_EQUAL(exofs_mount(DEV_SYS, B_BASE, &b), 0);
    if (!a || !b) goto out;
    CU_ASSERT_EQUAL(put(a, "/pkg.exo", src, FILE_LEN), 0);

    /* Several chunks, ending on a partial one, with neither volume
     * unmounted at any point. */
    uint32_t copied = 0;
    CU_ASSERT_EQUAL(shell_copy_vols(a, "/pkg.exo", b, "/pkg.exo", buf, CHUNK,
                                    &copied), 0);
    CU_ASSERT_EQUAL(copied, FILE_LEN);
    CU_ASSERT_TRUE(a->mounted);
    CU_ASSERT_TRUE(b->mounted);

    memset(rd, 0, FILE_LEN);
    CU_ASSERT_EQUAL(get(b, "/pkg.exo", rd, FILE_LEN), (int64_t)FILE_LEN);
    CU_ASSERT_EQUAL(memcmp(rd, src, FILE_LEN), 0);

    /* The source is intact, and a copy inside one volume works too. */
    CU_ASSERT_EQUAL(shell_copy_vols(b, "/pkg.exo", b, "/dup.exo", buf, CHUNK,
                                    &copied), 0);
    memset(rd, 0, FILE_LEN);
    CU_ASSERT_EQUAL(get(b, "/dup.exo", rd, FILE_LEN), (int64_t)FILE_LEN);
    CU_ASSERT_EQUAL(memcmp(rd, src, FILE_LEN), 0);

    /* A missing source leaves the destination volume untouched. */
    exofs_stat_t st;
    CU_ASSERT_EQUAL(shell_copy_vols(a, "/nope.exo", b, "/nope.exo", buf,
                                    CHUNK, NULL), -EXO_ENOENT);
    CU_ASSERT_EQUAL(exofs_stat(b, "/nope.exo", &st), -EXO_ENOENT);

    /* The same pair through the mounting wrapper, then a remount to prove it
     * reached the disk and not only the resident FAT. */
    exofs_unmount(b);
    exofs_unmount(a);
    const shell_vol_t sa = { DEV_STICK, A_BASE };
    const shell_vol_t sb = { DEV_SYS, B_BASE };
    CU_ASSERT_EQUAL(shell_copy_between(&sa, "/pkg.exo", &sb, "/via.exo", buf,
                                       CHUNK, &copied), 0);
    CU_ASSERT_EQUAL(exofs_mount(DEV_SYS, B_BASE, &b), 0);
    memset(rd, 0, FILE_LEN);
    CU_ASSERT_EQUAL(get(b, "/via.exo", rd, FILE_LEN), (int64_t)FILE_LEN);
    CU_ASSERT_EQUAL(memcmp(rd, src, FILE_LEN), 0);

out:
    libos_heap_free(src);
    libos_heap_free(rd);
    libos_heap_free(buf);
    teardown();
}

void suite_exofs_multivol_tests(CU_pSuite s)
{
    CU_add_test(s, "two volumes mounted at once, interleaved I/O",
                test_two_volumes_interleaved);
    CU_add_test(s, "pool: EXOFS_MAX_VOLUMES mounts, then -ENFILE, slot reused",
                test_pool_fills_and_frees);
    CU_add_test(s, "same volume mounted twice is -EBUSY",
                test_same_volume_twice_is_ebusy);
    CU_add_test(s, "files and dirs go stale (-EBADF) when their volume is unmounted",
                test_handle_stale_after_unmount);
    CU_add_test(s, "format of another volume leaves a mounted one alone",
                test_format_other_volume_while_mounted);
    CU_add_test(s, "copy between two mounted volumes unmounts neither",
                test_copy_between_mounted_volumes);
}
