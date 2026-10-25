/*
 * test_shell_install_k.c — the shell's `install` copy (SCRUM-233).
 *
 * The shell itself never returns and cannot be launched under test (see
 * tests/kernel/test_shell_libos_k.c), so this drives the two halves it
 * delegates to src/shell/shell_install.c: name resolution, and the
 * volume-to-volume copy between the install disk (block device 1, the
 * second IDE drive) and a volume on the system disk. ExoFS runs through its
 * EXO_KERNEL compile with libos_heap_alloc() buffers, as in
 * test_multidisk_k.c, whose two_disks() guard and LBA use this copies.
 *
 * LBA USE. Device 0: test_exofs_k.c's EXOFS_TEST_BASE_LBA region (8192) as
 * the destination volume, which that suite reformats on every test anyway.
 * Device 1: all of it, from LBA 0 -- the build writes the real install
 * volume there and this suite overwrites it (build.sh rewrites it on the
 * next build).
 */

#include "kunit.h"
#include "blkdev.h"
#include "exo_errno.h"
#include "libos_heap.h"
#include "string.h"

#include "libos_fs/exofs.h"
#include "libos_fs/exofs_blockdev.h"
#include "shell/shell_install.h"

#include <stdint.h>
#include <stddef.h>

#define DEV_SYS   0u
#define DEV_STICK 1u

#define SYS_VOL_BASE    8192u   /* test_exofs_k.c's EXOFS_TEST_BASE_LBA */
#define SYS_VOL_SECTORS 8192u
#define STICK_VOL_BASE  0u
#define STICK_VOL_SECTORS 8192u

/* Smaller than the file, and not a divisor of it, so a copy takes several
 * chunks and ends on a partial one. */
#define CHUNK    1024u
#define FILE_LEN 5000u

static const shell_vol_t stick = { DEV_STICK, STICK_VOL_BASE };
static const shell_vol_t sysvol = { DEV_SYS, SYS_VOL_BASE };

static int two_disks(void)
{
    return blkdev_get(DEV_SYS) != NULL && blkdev_get(DEV_STICK) != NULL;
}

static void fill(uint8_t *p, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0; i < len; i++)
        p[i] = (uint8_t)(seed + i * 7u);
}

/* Write `len` bytes of `src` to `path` on volume `v` (mount, write,
 * unmount). */
static int put_file(const shell_vol_t *v, const char *path,
                    const uint8_t *src, uint32_t len)
{
    int rc = exofs_mount(v->dev, v->base);
    if (rc != 0)
        return rc;
    exofs_file_t f;
    rc = exofs_open(path, EXOFS_O_WRITE | EXOFS_O_CREATE | EXOFS_O_TRUNC, &f);
    if (rc == 0) {
        int64_t n = len ? exofs_write(&f, src, len) : 0;
        if (n != (int64_t)len)
            rc = -EXO_EIO;
        exofs_close(&f);
    }
    exofs_unmount();
    return rc;
}

/* Read `path` on volume `v` into `dst` (room for `cap`) from a fresh
 * mount; returns its size or a negative error. */
static int64_t get_file(const shell_vol_t *v, const char *path, uint8_t *dst,
                        uint32_t cap)
{
    int rc = exofs_mount(v->dev, v->base);
    if (rc != 0)
        return rc;
    exofs_file_t f;
    int64_t n = exofs_open(path, EXOFS_O_READ, &f);
    if (n == 0) {
        n = exofs_read(&f, dst, cap);
        exofs_close(&f);
    }
    exofs_unmount();
    return n;
}

static int stat_on(const shell_vol_t *v, const char *path)
{
    int rc = exofs_mount(v->dev, v->base);
    if (rc != 0)
        return rc;
    exofs_stat_t st;
    rc = exofs_stat(path, &st);
    exofs_unmount();
    return rc;
}

/* Both disks bound, a fresh volume on each. Returns 0 if the second drive
 * is missing or the setup failed (asserted), so the test can just return. */
static int setup(uint32_t sys_sectors)
{
    if (!two_disks())
        return 0;
    exofs_unmount();
    exofs_bdev_release(DEV_STICK);
    exofs_bdev_release(DEV_SYS);
    int ok = exofs_bdev_acquire(DEV_STICK) == 0 &&
             exofs_bdev_acquire_extent(DEV_SYS, SYS_VOL_BASE,
                                       SYS_VOL_SECTORS) == 0 &&
             exofs_format(DEV_STICK, STICK_VOL_BASE, STICK_VOL_SECTORS) == 0 &&
             exofs_format(DEV_SYS, SYS_VOL_BASE, sys_sectors) == 0;
    CU_ASSERT(ok);
    return ok;
}

static void teardown(void)
{
    exofs_unmount();
    exofs_bdev_release(DEV_STICK);
    exofs_bdev_release(DEV_SYS);
}

int shell_install_suite_init(void)
{
    teardown();
    return 0;
}

int shell_install_suite_cleanup(void)
{
    teardown();
    return 0;
}

/* ---- Name resolution (no disk) ------------------------------------------- */

static void test_resolve_bare_name_gets_exo(void)
{
    char out[32];
    CU_ASSERT_EQUAL(shell_install_resolve("doom", out, sizeof(out)), 0);
    CU_ASSERT_STRING_EQUAL(out, "/doom.exo");
}

static void test_resolve_keeps_extension(void)
{
    char out[32];
    CU_ASSERT_EQUAL(shell_install_resolve("freedoom2.wad", out, sizeof(out)),
                    0);
    CU_ASSERT_STRING_EQUAL(out, "/freedoom2.wad");
    CU_ASSERT_EQUAL(shell_install_resolve("snake.exo", out, sizeof(out)), 0);
    CU_ASSERT_STRING_EQUAL(out, "/snake.exo");
}

static void test_resolve_leading_slash_once(void)
{
    char out[32];
    CU_ASSERT_EQUAL(shell_install_resolve("/clock", out, sizeof(out)), 0);
    CU_ASSERT_STRING_EQUAL(out, "/clock.exo");
}

/* Only the last component decides: a dot in a directory name does not
 * count as the file's extension. */
static void test_resolve_dotted_directory(void)
{
    char out[32];
    CU_ASSERT_EQUAL(shell_install_resolve("v1.2/doom", out, sizeof(out)), 0);
    CU_ASSERT_STRING_EQUAL(out, "/v1.2/doom.exo");
    CU_ASSERT_EQUAL(shell_install_resolve("v1.2/a.wad", out, sizeof(out)), 0);
    CU_ASSERT_STRING_EQUAL(out, "/v1.2/a.wad");
}

/* A name that only fits once ".exo" is dropped is refused, not cut short:
 * a truncated name is a different file. */
static void test_resolve_refuses_overlong(void)
{
    char out[10];
    CU_ASSERT_EQUAL(shell_install_resolve("abcd", out, sizeof(out)), 0);
    CU_ASSERT_STRING_EQUAL(out, "/abcd.exo");          /* exactly 9 + NUL */
    CU_ASSERT_EQUAL(shell_install_resolve("abcde", out, sizeof(out)),
                    -EXO_EINVAL);
    CU_ASSERT_EQUAL(shell_install_resolve("abcdefghij.x", out, sizeof(out)),
                    -EXO_EINVAL);
}

static void test_resolve_refuses_empty(void)
{
    char out[16];
    CU_ASSERT_EQUAL(shell_install_resolve("", out, sizeof(out)), -EXO_EINVAL);
    CU_ASSERT_EQUAL(shell_install_resolve("/", out, sizeof(out)), -EXO_EINVAL);
}

/* ---- The copy (needs both drives) ---------------------------------------- */

/* Jira acceptance shape: a package on the install disk lands intact on the
 * system volume, over several chunks, and the source is left alone. */
static void test_copy_multi_chunk(void)
{
    if (!setup(SYS_VOL_SECTORS))
        return;
    uint8_t *src = libos_heap_alloc(FILE_LEN);
    uint8_t *got = libos_heap_alloc(FILE_LEN);
    uint8_t *buf = libos_heap_alloc(CHUNK);
    CU_ASSERT(src && got && buf);
    if (!src || !got || !buf)
        goto out;
    fill(src, FILE_LEN, 0x21);
    CU_ASSERT_EQUAL(put_file(&stick, "/pkg.exo", src, FILE_LEN), 0);

    uint32_t copied = 0;
    CU_ASSERT_EQUAL(shell_install_copy(&stick, &sysvol, "/pkg.exo", buf, CHUNK,
                                       &copied), 0);
    CU_ASSERT_EQUAL(copied, FILE_LEN);
    CU_ASSERT_FALSE(exofs_is_mounted());

    memset(got, 0, FILE_LEN);
    CU_ASSERT_EQUAL(get_file(&sysvol, "/pkg.exo", got, FILE_LEN),
                    (int64_t)FILE_LEN);
    CU_ASSERT_EQUAL(memcmp(got, src, FILE_LEN), 0);

    memset(got, 0, FILE_LEN);
    CU_ASSERT_EQUAL(get_file(&stick, "/pkg.exo", got, FILE_LEN),
                    (int64_t)FILE_LEN);
    CU_ASSERT_EQUAL(memcmp(got, src, FILE_LEN), 0);
out:
    libos_heap_free(src);
    libos_heap_free(got);
    libos_heap_free(buf);
    teardown();
}

/* A file that is an exact number of chunks, and an empty one: the loop's
 * two edges. */
static void test_copy_exact_chunks_and_empty(void)
{
    if (!setup(SYS_VOL_SECTORS))
        return;
    uint8_t *src = libos_heap_alloc(CHUNK * 3u);
    uint8_t *got = libos_heap_alloc(CHUNK * 3u);
    uint8_t *buf = libos_heap_alloc(CHUNK);
    CU_ASSERT(src && got && buf);
    if (!src || !got || !buf)
        goto out;
    fill(src, CHUNK * 3u, 0x55);
    CU_ASSERT_EQUAL(put_file(&stick, "/three.bin", src, CHUNK * 3u), 0);
    CU_ASSERT_EQUAL(put_file(&stick, "/empty.bin", src, 0), 0);

    uint32_t copied = 1;
    CU_ASSERT_EQUAL(shell_install_copy(&stick, &sysvol, "/three.bin", buf,
                                       CHUNK, &copied), 0);
    CU_ASSERT_EQUAL(copied, CHUNK * 3u);
    CU_ASSERT_EQUAL(get_file(&sysvol, "/three.bin", got, CHUNK * 3u),
                    (int64_t)(CHUNK * 3u));
    CU_ASSERT_EQUAL(memcmp(got, src, CHUNK * 3u), 0);

    copied = 1;
    CU_ASSERT_EQUAL(shell_install_copy(&stick, &sysvol, "/empty.bin", buf,
                                       CHUNK, &copied), 0);
    CU_ASSERT_EQUAL(copied, 0);
    CU_ASSERT_EQUAL(get_file(&sysvol, "/empty.bin", got, CHUNK), 0);
out:
    libos_heap_free(src);
    libos_heap_free(got);
    libos_heap_free(buf);
    teardown();
}

/* Reinstalling replaces: a shorter new version leaves no stale tail. */
static void test_reinstall_replaces(void)
{
    if (!setup(SYS_VOL_SECTORS))
        return;
    uint8_t *src = libos_heap_alloc(FILE_LEN);
    uint8_t *got = libos_heap_alloc(FILE_LEN);
    uint8_t *buf = libos_heap_alloc(CHUNK);
    CU_ASSERT(src && got && buf);
    if (!src || !got || !buf)
        goto out;
    fill(src, FILE_LEN, 0x10);
    CU_ASSERT_EQUAL(put_file(&sysvol, "/pkg.exo", src, FILE_LEN), 0);
    fill(src, 1500u, 0x99);
    CU_ASSERT_EQUAL(put_file(&stick, "/pkg.exo", src, 1500u), 0);

    CU_ASSERT_EQUAL(shell_install_copy(&stick, &sysvol, "/pkg.exo", buf, CHUNK,
                                       NULL), 0);
    CU_ASSERT_EQUAL(get_file(&sysvol, "/pkg.exo", got, FILE_LEN),
                    (int64_t)1500);
    CU_ASSERT_EQUAL(memcmp(got, src, 1500u), 0);
out:
    libos_heap_free(src);
    libos_heap_free(got);
    libos_heap_free(buf);
    teardown();
}

/* A name the install disk does not have is reported before the system
 * volume is touched: an installed package of that name survives. */
static void test_missing_source_touches_nothing(void)
{
    if (!setup(SYS_VOL_SECTORS))
        return;
    uint8_t *src = libos_heap_alloc(FILE_LEN);
    uint8_t *got = libos_heap_alloc(FILE_LEN);
    uint8_t *buf = libos_heap_alloc(CHUNK);
    CU_ASSERT(src && got && buf);
    if (!src || !got || !buf)
        goto out;
    fill(src, FILE_LEN, 0x42);
    CU_ASSERT_EQUAL(put_file(&sysvol, "/kept.exo", src, FILE_LEN), 0);

    uint32_t copied = 7;
    CU_ASSERT_EQUAL(shell_install_copy(&stick, &sysvol, "/kept.exo", buf,
                                       CHUNK, &copied), -EXO_ENOENT);
    CU_ASSERT_EQUAL(copied, 0);
    CU_ASSERT_EQUAL(shell_install_copy(&stick, &sysvol, "/nope.exo", buf,
                                       CHUNK, NULL), -EXO_ENOENT);
    CU_ASSERT_FALSE(exofs_is_mounted());
    CU_ASSERT_EQUAL(stat_on(&sysvol, "/nope.exo"), -EXO_ENOENT);
    CU_ASSERT_EQUAL(get_file(&sysvol, "/kept.exo", got, FILE_LEN),
                    (int64_t)FILE_LEN);
    CU_ASSERT_EQUAL(memcmp(got, src, FILE_LEN), 0);
out:
    libos_heap_free(src);
    libos_heap_free(got);
    libos_heap_free(buf);
    teardown();
}

/* A system volume too small for the file: -ENOSPC, and the partial copy is
 * removed again -- its blocks come back, so a smaller install then fits. */
static void test_enospc_removes_partial(void)
{
    if (!setup(16u))               /* a handful of 512-byte data blocks */
        return;
    uint8_t *src = libos_heap_alloc(FILE_LEN * 4u);
    uint8_t *buf = libos_heap_alloc(CHUNK);
    CU_ASSERT(src && buf);
    if (!src || !buf)
        goto out;
    fill(src, FILE_LEN * 4u, 0x07);
    CU_ASSERT_EQUAL(put_file(&stick, "/big.exo", src, FILE_LEN * 4u), 0);
    CU_ASSERT_EQUAL(put_file(&stick, "/small.exo", src, 600u), 0);

    CU_ASSERT_EQUAL(shell_install_copy(&stick, &sysvol, "/big.exo", buf, CHUNK,
                                       NULL), -EXO_ENOSPC);
    CU_ASSERT_FALSE(exofs_is_mounted());
    CU_ASSERT_EQUAL(stat_on(&sysvol, "/big.exo"), -EXO_ENOENT);

    uint32_t copied = 0;
    CU_ASSERT_EQUAL(shell_install_copy(&stick, &sysvol, "/small.exo", buf,
                                       CHUNK, &copied), 0);
    CU_ASSERT_EQUAL(copied, 600u);
out:
    libos_heap_free(src);
    libos_heap_free(buf);
    teardown();
}

/* A directory is not something to install. */
static void test_directory_refused(void)
{
    if (!setup(SYS_VOL_SECTORS))
        return;
    uint8_t *buf = libos_heap_alloc(CHUNK);
    CU_ASSERT(buf != NULL);
    if (buf == NULL)
        goto out;
    CU_ASSERT_EQUAL(exofs_mount(DEV_STICK, STICK_VOL_BASE), 0);
    CU_ASSERT_EQUAL(exofs_mkdir("/games"), 0);
    exofs_unmount();

    CU_ASSERT_EQUAL(shell_install_copy(&stick, &sysvol, "/games", buf, CHUNK,
                                       NULL), -EXO_EISDIR);
    CU_ASSERT_EQUAL(stat_on(&sysvol, "/games"), -EXO_ENOENT);
out:
    libos_heap_free(buf);
    teardown();
}

/* ---- Patterns (`install *`) --------------------------------------------- */

static void test_glob_match(void)
{
    CU_ASSERT_TRUE(shell_install_glob_match("*.exo", "doom.exo"));
    CU_ASSERT_TRUE(shell_install_glob_match("*.exo", ".exo"));
    CU_ASSERT_FALSE(shell_install_glob_match("*.exo", "freedoom2.wad"));
    CU_ASSERT_FALSE(shell_install_glob_match("*.exo", "doom.exo.bak"));
    CU_ASSERT_TRUE(shell_install_glob_match("*", "anything"));
    CU_ASSERT_TRUE(shell_install_glob_match("*", ""));
    CU_ASSERT_TRUE(shell_install_glob_match("*.*", "freedoom2.wad"));
    CU_ASSERT_FALSE(shell_install_glob_match("*.*", "README"));
    CU_ASSERT_TRUE(shell_install_glob_match("d*m.exo", "doom.exo"));
    CU_ASSERT_TRUE(shell_install_glob_match("d*m.exo", "dm.exo"));
    /* Backtracking: the first 'o' tried is not the one that matches. */
    CU_ASSERT_TRUE(shell_install_glob_match("*o.exo", "voldemoo.exo"));
    CU_ASSERT_TRUE(shell_install_glob_match("**.exo", "snake.exo"));
    CU_ASSERT_FALSE(shell_install_glob_match("Doom.exo", "doom.exo"));
    CU_ASSERT_TRUE(shell_install_glob_match("doom.exo", "doom.exo"));
}

/* `install *` is "every package": the no-extension rule turns "*" into
 * "*.exo", and that is what makes it a pattern. */
static void test_star_resolves_to_packages(void)
{
    char out[32];
    CU_ASSERT_EQUAL(shell_install_resolve("*", out, sizeof(out)), 0);
    CU_ASSERT_STRING_EQUAL(out, "/*.exo");
    CU_ASSERT_TRUE(shell_install_is_glob(out));
    CU_ASSERT_EQUAL(shell_install_resolve("*.wad", out, sizeof(out)), 0);
    CU_ASSERT_STRING_EQUAL(out, "/*.wad");
    CU_ASSERT_FALSE(shell_install_is_glob("/doom.exo"));
}

#define STRIDE 32u

static void test_matches_lists_files(void)
{
    if (!setup(SYS_VOL_SECTORS))
        return;
    static const uint8_t byte = 0x5A;
    CU_ASSERT_EQUAL(put_file(&stick, "/a.exo", &byte, 1), 0);
    CU_ASSERT_EQUAL(put_file(&stick, "/b.exo", &byte, 1), 0);
    CU_ASSERT_EQUAL(put_file(&stick, "/c.wad", &byte, 1), 0);
    CU_ASSERT_EQUAL(exofs_mount(DEV_STICK, STICK_VOL_BASE), 0);
    CU_ASSERT_EQUAL(exofs_mkdir("/dir.exo"), 0);   /* never a match */
    exofs_unmount();

    char *names = libos_heap_alloc(8u * STRIDE);
    CU_ASSERT(names != NULL);
    if (names == NULL)
        goto out;

    uint32_t count = 99;
    CU_ASSERT_EQUAL(shell_install_matches(&stick, "/*.exo", names, STRIDE, 8u,
                                          &count), 0);
    CU_ASSERT_EQUAL(count, 2u);
    CU_ASSERT_STRING_EQUAL(names, "/a.exo");
    CU_ASSERT_STRING_EQUAL(names + STRIDE, "/b.exo");
    CU_ASSERT_FALSE(exofs_is_mounted());

    CU_ASSERT_EQUAL(shell_install_matches(&stick, "/*.*", names, STRIDE, 8u,
                                          &count), 0);
    CU_ASSERT_EQUAL(count, 3u);

    CU_ASSERT_EQUAL(shell_install_matches(&stick, "/z*", names, STRIDE, 8u,
                                          &count), 0);
    CU_ASSERT_EQUAL(count, 0u);
out:
    libos_heap_free(names);
    teardown();
}

/* More matches than the caller has room for is refused whole, and a pattern
 * below the top level is refused outright. */
static void test_matches_limits(void)
{
    if (!setup(SYS_VOL_SECTORS))
        return;
    static const uint8_t byte = 0x5A;
    CU_ASSERT_EQUAL(put_file(&stick, "/a.exo", &byte, 1), 0);
    CU_ASSERT_EQUAL(put_file(&stick, "/b.exo", &byte, 1), 0);
    CU_ASSERT_EQUAL(put_file(&stick, "/c.exo", &byte, 1), 0);

    char *names = libos_heap_alloc(8u * STRIDE);
    CU_ASSERT(names != NULL);
    if (names == NULL)
        goto out;
    uint32_t count = 0;
    CU_ASSERT_EQUAL(shell_install_matches(&stick, "/*.exo", names, STRIDE, 2u,
                                          &count), -EXO_ENOSPC);
    CU_ASSERT_FALSE(exofs_is_mounted());
    CU_ASSERT_EQUAL(shell_install_matches(&stick, "/games/*.exo", names,
                                          STRIDE, 8u, &count), -EXO_EINVAL);
    CU_ASSERT_EQUAL(count, 0u);
out:
    libos_heap_free(names);
    teardown();
}

/* `install *` end to end: every listed package lands on the system volume. */
static void test_install_every_match(void)
{
    if (!setup(SYS_VOL_SECTORS))
        return;
    uint8_t *src = libos_heap_alloc(FILE_LEN);
    uint8_t *got = libos_heap_alloc(FILE_LEN);
    uint8_t *buf = libos_heap_alloc(CHUNK);
    char *names = libos_heap_alloc(8u * STRIDE);
    CU_ASSERT(src && got && buf && names);
    if (!src || !got || !buf || !names)
        goto out;
    fill(src, FILE_LEN, 0x31);
    CU_ASSERT_EQUAL(put_file(&stick, "/one.exo", src, FILE_LEN), 0);
    CU_ASSERT_EQUAL(put_file(&stick, "/two.exo", src, 700u), 0);
    CU_ASSERT_EQUAL(put_file(&stick, "/data.wad", src, 10u), 0);

    char pat[16];
    uint32_t count = 0;
    CU_ASSERT_EQUAL(shell_install_resolve("*", pat, sizeof(pat)), 0);
    CU_ASSERT_EQUAL(shell_install_matches(&stick, pat, names, STRIDE, 8u,
                                          &count), 0);
    CU_ASSERT_EQUAL(count, 2u);
    for (uint32_t i = 0; i < count; i++)
        CU_ASSERT_EQUAL(shell_install_copy(&stick, &sysvol, names + i * STRIDE,
                                           buf, CHUNK, NULL), 0);

    CU_ASSERT_EQUAL(get_file(&sysvol, "/one.exo", got, FILE_LEN),
                    (int64_t)FILE_LEN);
    CU_ASSERT_EQUAL(memcmp(got, src, FILE_LEN), 0);
    CU_ASSERT_EQUAL(get_file(&sysvol, "/two.exo", got, FILE_LEN), 700);
    CU_ASSERT_EQUAL(stat_on(&sysvol, "/data.wad"), -EXO_ENOENT);
out:
    libos_heap_free(src);
    libos_heap_free(got);
    libos_heap_free(buf);
    libos_heap_free(names);
    teardown();
}

void suite_shell_install_tests(CU_pSuite s)
{
    CU_add_test(s, "resolve: bare name gets .exo",
                test_resolve_bare_name_gets_exo);
    CU_add_test(s, "resolve: an extension is kept",
                test_resolve_keeps_extension);
    CU_add_test(s, "resolve: leading slash not doubled",
                test_resolve_leading_slash_once);
    CU_add_test(s, "resolve: a dotted directory is not an extension",
                test_resolve_dotted_directory);
    CU_add_test(s, "resolve: overlong name refused, not truncated",
                test_resolve_refuses_overlong);
    CU_add_test(s, "resolve: empty name refused",
                test_resolve_refuses_empty);
    CU_add_test(s, "copy across devices in several chunks",
                test_copy_multi_chunk);
    CU_add_test(s, "copy: exact chunk multiple and empty file",
                test_copy_exact_chunks_and_empty);
    CU_add_test(s, "reinstall replaces, no stale tail",
                test_reinstall_replaces);
    CU_add_test(s, "missing source: -ENOENT, system volume untouched",
                test_missing_source_touches_nothing);
    CU_add_test(s, "-ENOSPC removes the partial copy",
                test_enospc_removes_partial);
    CU_add_test(s, "a directory is refused",
                test_directory_refused);
    CU_add_test(s, "glob: * matches any run of characters",
                test_glob_match);
    CU_add_test(s, "glob: install * means every .exo",
                test_star_resolves_to_packages);
    CU_add_test(s, "glob: matching files listed, directories skipped",
                test_matches_lists_files);
    CU_add_test(s, "glob: too many matches or a subdirectory refused",
                test_matches_limits);
    CU_add_test(s, "glob: install * copies every package",
                test_install_every_match);
}
