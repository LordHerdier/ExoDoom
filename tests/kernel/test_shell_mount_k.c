/*
 * test_shell_mount_k.c — the shell's mount table and path router
 * (SCRUM-266).
 *
 * src/shell/shell_mount.c is ring-3 shell code; docker/scripts/build.sh
 * compiles a second copy into the TESTING kernel so the routing rules can
 * be checked here without driving the shell's never-returning input loop
 * (see test_shell_libos_k.c). The handlers in shell_main.c that print and
 * prompt are not covered; every decision they make lives in this TU.
 */

#include "kunit.h"
#include "blkdev.h"
#include "exo_errno.h"
#include "libos_heap.h"
#include "shell/shell_install.h"
#include "shell/shell_mount.h"

#include "libos_fs/exofs.h"
#include "libos_fs/exofs_blockdev.h"
#include "libos_fs/exofs_layout.h"

#include <stddef.h>
#include <stdint.h>

/* The volume the tests in this file are working on (SCRUM-265): set by
 * whichever helper mounts, read by everything after. A slot is zeroed on
 * unmount, so a stale pointer reads as "not mounted". */
static exofs_vol_t *g_tv;
#define TV_MOUNTED() (g_tv != NULL && g_tv->mounted)

static const shell_volref_t ROOT_VOL = { 0, 8192, 0, 65536, 1 };
static const shell_volref_t STICK    = { 1, 0, 0, 0, 1 };
static const shell_volref_t SLOT3    = { 0, 262144 + 3 * 8192, 262144 + 3 * 8192, 8192, 3 };

static void norm_is(const char *in, const char *want)
{
    char out[SHELL_MOUNT_PATH_LEN];
    CU_ASSERT_EQUAL(shell_mount_normalize(in, out, sizeof(out)), 0);
    CU_ASSERT_STRING_EQUAL(out, want);
}

static void test_normalize(void)
{
    norm_is("/", "/");
    norm_is("//", "/");
    norm_is("/a", "/a");
    norm_is("/a/", "/a");
    norm_is("/a//b", "/a/b");
    norm_is("/a/./b", "/a/b");
    norm_is("/a/b/..", "/a");
    norm_is("/a/b/../c/", "/a/c");
    norm_is("/a/./b//../c", "/a/c");
    norm_is("/..", "/");
    norm_is("/../..", "/");
    norm_is("/a/../..", "/");
    norm_is("/a/b/../../..", "/");
    norm_is("/..a", "/..a");          /* a name that merely starts with dots */
    norm_is("/a/...", "/a/...");
}

static void test_normalize_rejects(void)
{
    char out[8];
    CU_ASSERT_EQUAL(shell_mount_normalize("a/b", out, sizeof(out)),
                    -EXO_EINVAL);                          /* not absolute */
    CU_ASSERT_EQUAL(shell_mount_normalize("", out, sizeof(out)), -EXO_EINVAL);
    CU_ASSERT_EQUAL(shell_mount_normalize(NULL, out, sizeof(out)),
                    -EXO_EINVAL);
    /* "/abcdefg" needs 9 bytes with its NUL: an 8-byte buffer must refuse
     * it rather than hand back "/abcdef", a different directory. */
    CU_ASSERT_EQUAL(shell_mount_normalize("/abcdefg", out, sizeof(out)),
                    -EXO_EINVAL);
    CU_ASSERT_EQUAL(shell_mount_normalize("/abcdef", out, sizeof(out)), 0);
    CU_ASSERT_EQUAL(shell_mount_normalize("/a", out, 1), -EXO_EINVAL);
}

static void test_path_within(void)
{
    CU_ASSERT_TRUE(shell_mount_path_within("/mnt/x", "/mnt/x"));
    CU_ASSERT_TRUE(shell_mount_path_within("/mnt/x/f", "/mnt/x"));
    CU_ASSERT_TRUE(shell_mount_path_within("/mnt/x/a/b", "/mnt"));
    CU_ASSERT_FALSE(shell_mount_path_within("/mnt/xy", "/mnt/x"));
    CU_ASSERT_FALSE(shell_mount_path_within("/mnt", "/mnt/x"));
    CU_ASSERT_FALSE(shell_mount_path_within("/other", "/mnt"));
    CU_ASSERT_TRUE(shell_mount_path_within("/anything", "/"));
    CU_ASSERT_TRUE(shell_mount_path_within("/", "/"));
}

static void test_route_root(void)
{
    shell_mount_table_t t;
    shell_mount_init(&t);
    shell_volref_t v;
    char inner[SHELL_MOUNT_PATH_LEN];
    int idx = 99;

    CU_ASSERT_EQUAL(shell_mount_route(&t, &ROOT_VOL, "/a/b", &v, inner,
                                      sizeof(inner), &idx), 0);
    CU_ASSERT_EQUAL(idx, -1);
    CU_ASSERT_EQUAL(v.dev, 0);
    CU_ASSERT_EQUAL(v.base, 8192);
    CU_ASSERT_STRING_EQUAL(inner, "/a/b");

    CU_ASSERT_EQUAL(shell_mount_route(&t, &ROOT_VOL, "/", &v, inner,
                                      sizeof(inner), &idx), 0);
    CU_ASSERT_STRING_EQUAL(inner, "/");
}

static void test_route_mounted(void)
{
    shell_mount_table_t t;
    shell_mount_init(&t);
    CU_ASSERT_EQUAL(shell_mount_add(&t, "/mnt/x", &STICK), 0);

    shell_volref_t v;
    char inner[SHELL_MOUNT_PATH_LEN];
    int idx = -2;

    CU_ASSERT_EQUAL(shell_mount_route(&t, &ROOT_VOL, "/mnt/x/dir/f", &v,
                                      inner, sizeof(inner), &idx), 0);
    CU_ASSERT_EQUAL(idx, 0);
    CU_ASSERT_EQUAL(v.dev, 1);
    CU_ASSERT_STRING_EQUAL(inner, "/dir/f");

    /* The mount point itself is the volume's root -- and is how a caller
     * recognises "this path is a mount point". */
    CU_ASSERT_EQUAL(shell_mount_route(&t, &ROOT_VOL, "/mnt/x", &v, inner,
                                      sizeof(inner), &idx), 0);
    CU_ASSERT_EQUAL(idx, 0);
    CU_ASSERT_STRING_EQUAL(inner, "/");

    /* Component boundary: /mnt/xy is not under /mnt/x. */
    CU_ASSERT_EQUAL(shell_mount_route(&t, &ROOT_VOL, "/mnt/xy/f", &v, inner,
                                      sizeof(inner), &idx), 0);
    CU_ASSERT_EQUAL(idx, -1);
    CU_ASSERT_EQUAL(v.dev, 0);
    CU_ASSERT_STRING_EQUAL(inner, "/mnt/xy/f");

    /* The parent directory of a mount point stays on its own volume. */
    CU_ASSERT_EQUAL(shell_mount_route(&t, &ROOT_VOL, "/mnt", &v, inner,
                                      sizeof(inner), &idx), 0);
    CU_ASSERT_EQUAL(idx, -1);
}

static void test_route_longest_prefix(void)
{
    shell_mount_table_t t;
    shell_mount_init(&t);
    CU_ASSERT_EQUAL(shell_mount_add(&t, "/mnt", &STICK), 0);
    CU_ASSERT_EQUAL(shell_mount_add(&t, "/mnt/slot", &SLOT3), 0);

    shell_volref_t v;
    char inner[SHELL_MOUNT_PATH_LEN];
    int idx;

    CU_ASSERT_EQUAL(shell_mount_route(&t, &ROOT_VOL, "/mnt/slot/f", &v, inner,
                                      sizeof(inner), &idx), 0);
    CU_ASSERT_EQUAL(idx, 1);
    CU_ASSERT_EQUAL(v.base, SLOT3.base);
    CU_ASSERT_STRING_EQUAL(inner, "/f");

    CU_ASSERT_EQUAL(shell_mount_route(&t, &ROOT_VOL, "/mnt/other", &v, inner,
                                      sizeof(inner), &idx), 0);
    CU_ASSERT_EQUAL(idx, 0);
    CU_ASSERT_EQUAL(v.dev, 1);
    CU_ASSERT_STRING_EQUAL(inner, "/other");
}

/* The reason paths are normalized before routing: ".." out of a mounted
 * root must land on the volume the mount point lives on. */
static void test_dotdot_across_mount(void)
{
    shell_mount_table_t t;
    shell_mount_init(&t);
    CU_ASSERT_EQUAL(shell_mount_add(&t, "/mnt/x", &STICK), 0);

    char norm[SHELL_MOUNT_PATH_LEN];
    shell_volref_t v;
    char inner[SHELL_MOUNT_PATH_LEN];
    int idx;

    CU_ASSERT_EQUAL(shell_mount_normalize("/mnt/x/..", norm, sizeof(norm)), 0);
    CU_ASSERT_EQUAL(shell_mount_route(&t, &ROOT_VOL, norm, &v, inner,
                                      sizeof(inner), &idx), 0);
    CU_ASSERT_EQUAL(idx, -1);
    CU_ASSERT_EQUAL(v.dev, 0);
    CU_ASSERT_STRING_EQUAL(inner, "/mnt");

    /* And ".." that stays inside the mount stays on it. */
    CU_ASSERT_EQUAL(shell_mount_normalize("/mnt/x/a/../b", norm,
                                          sizeof(norm)), 0);
    CU_ASSERT_EQUAL(shell_mount_route(&t, &ROOT_VOL, norm, &v, inner,
                                      sizeof(inner), &idx), 0);
    CU_ASSERT_EQUAL(idx, 0);
    CU_ASSERT_STRING_EQUAL(inner, "/b");
}

static void test_route_inner_too_small(void)
{
    shell_mount_table_t t;
    shell_mount_init(&t);
    shell_volref_t v;
    char inner[4];
    CU_ASSERT_EQUAL(shell_mount_route(&t, &ROOT_VOL, "/abcdef", &v, inner,
                                      sizeof(inner), NULL), -EXO_EINVAL);
    CU_ASSERT_EQUAL(shell_mount_route(&t, &ROOT_VOL, "relative", &v, inner,
                                      sizeof(inner), NULL), -EXO_EINVAL);
}

static void test_add_rules(void)
{
    shell_mount_table_t t;
    shell_mount_init(&t);

    CU_ASSERT_EQUAL(shell_mount_add(&t, "/", &STICK), -EXO_EINVAL);
    CU_ASSERT_EQUAL(shell_mount_add(&t, "mnt", &STICK), -EXO_EINVAL);
    CU_ASSERT_EQUAL(shell_mount_add(&t, "/mnt/x", &STICK), 0);
    CU_ASSERT_EQUAL(shell_mount_add(&t, "/mnt/x", &SLOT3), -EXO_EBUSY);
    CU_ASSERT_EQUAL(shell_mount_find(&t, "/mnt/x"), 0);
    CU_ASSERT_EQUAL(shell_mount_find(&t, "/mnt"), -1);
    CU_ASSERT_EQUAL(shell_mount_find(&t, "/mnt/x/y"), -1);
}

static void test_table_full(void)
{
    shell_mount_table_t t;
    shell_mount_init(&t);
    char p[] = "/m0";
    for (unsigned i = 0; i < SHELL_MOUNT_MAX; i++) {
        p[2] = (char)('0' + i);
        CU_ASSERT_EQUAL(shell_mount_add(&t, p, &STICK), 0);
    }
    CU_ASSERT_EQUAL(shell_mount_add(&t, "/extra", &STICK), -EXO_ENOSPC);

    /* A freed slot is reusable. */
    CU_ASSERT_EQUAL(shell_mount_remove(&t, "/m3", "/"), 0);
    CU_ASSERT_EQUAL(shell_mount_add(&t, "/extra", &STICK), 0);
}

static void test_remove_rules(void)
{
    shell_mount_table_t t;
    shell_mount_init(&t);
    CU_ASSERT_EQUAL(shell_mount_add(&t, "/mnt", &STICK), 0);
    CU_ASSERT_EQUAL(shell_mount_add(&t, "/mnt/slot", &SLOT3), 0);

    /* Not a mount point (a plain directory, or something under a mount). */
    CU_ASSERT_EQUAL(shell_mount_remove(&t, "/tmp", "/"), -EXO_EINVAL);
    CU_ASSERT_EQUAL(shell_mount_remove(&t, "/mnt/dir", "/"), -EXO_EINVAL);

    /* Another mount lies beneath it. */
    CU_ASSERT_EQUAL(shell_mount_has_below(&t, "/mnt"), 1);
    CU_ASSERT_EQUAL(shell_mount_has_below(&t, "/mnt/slot"), 0);
    CU_ASSERT_EQUAL(shell_mount_remove(&t, "/mnt", "/"), -EXO_EBUSY);

    /* The shell is standing in it, directly or deeper. */
    CU_ASSERT_EQUAL(shell_mount_remove(&t, "/mnt/slot", "/mnt/slot"),
                    -EXO_EBUSY);
    CU_ASSERT_EQUAL(shell_mount_remove(&t, "/mnt/slot", "/mnt/slot/a/b"),
                    -EXO_EBUSY);

    /* Standing elsewhere -- including the parent mount -- is fine. */
    CU_ASSERT_EQUAL(shell_mount_remove(&t, "/mnt/slot", "/mnt"), 0);
    CU_ASSERT_EQUAL(shell_mount_find(&t, "/mnt/slot"), -1);
    CU_ASSERT_EQUAL(shell_mount_remove(&t, "/mnt", "/"), 0);

    /* Gone is gone. */
    CU_ASSERT_EQUAL(shell_mount_remove(&t, "/mnt", "/"), -EXO_EINVAL);
}

static void test_parse_volname(void)
{
    uint32_t d = 77, v = 77;

    CU_ASSERT_EQUAL(shell_mount_parse_volname("disk1:vol2", &d, &v), 0);
    CU_ASSERT_EQUAL(d, 1);
    CU_ASSERT_EQUAL(v, 2);
    CU_ASSERT_EQUAL(shell_mount_parse_volname("disk0:vol1", &d, &v), 0);
    CU_ASSERT_EQUAL(d, 0);
    CU_ASSERT_EQUAL(v, 1);
    CU_ASSERT_EQUAL(shell_mount_parse_volname("disk12:vol10", &d, &v), 0);
    CU_ASSERT_EQUAL(d, 12);
    CU_ASSERT_EQUAL(v, 10);

    /* Refusals leave the outputs alone. */
    d = 77;
    v = 77;
    static const char *const bad[] = {
        "", "disk", "disk1", "disk1:", "disk1:vol", "disk1:vol0",
        "disk:vol1", "disk1vol1", "Disk1:vol1", "disk1:Vol1", "disk-1:vol1",
        "disk1:vol1x", "disk1:vol1 ", " disk1:vol1", "disk1:vol-1",
        "vol1", "disk1:vol1:vol2", "disk1:vol99999", "disk99999:vol1",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CU_ASSERT_EQUAL(shell_mount_parse_volname(bad[i], &d, &v),
                        -EXO_EINVAL);
    }
    CU_ASSERT_EQUAL(shell_mount_parse_volname(NULL, &d, &v), -EXO_EINVAL);
    CU_ASSERT_EQUAL(d, 77);
    CU_ASSERT_EQUAL(v, 77);
}

static void fill_sb(uint8_t *s, uint32_t magic, uint16_t version, uint16_t bs)
{
    for (int i = 0; i < 512; i++) {
        s[i] = 0;
    }
    s[0] = (uint8_t)magic;
    s[1] = (uint8_t)(magic >> 8);
    s[2] = (uint8_t)(magic >> 16);
    s[3] = (uint8_t)(magic >> 24);
    s[4] = (uint8_t)version;
    s[5] = (uint8_t)(version >> 8);
    s[6] = (uint8_t)bs;
    s[7] = (uint8_t)(bs >> 8);
}

static void test_sb_valid(void)
{
    uint8_t s[512];

    fill_sb(s, EXOFS_MAGIC, EXOFS_VERSION, EXOFS_BLOCK_SIZE);
    CU_ASSERT_TRUE(shell_mount_sb_valid(s));

    fill_sb(s, EXOFS_MAGIC ^ 1u, EXOFS_VERSION, EXOFS_BLOCK_SIZE);
    CU_ASSERT_FALSE(shell_mount_sb_valid(s));
    fill_sb(s, EXOFS_MAGIC, EXOFS_VERSION + 1, EXOFS_BLOCK_SIZE);
    CU_ASSERT_FALSE(shell_mount_sb_valid(s));
    fill_sb(s, EXOFS_MAGIC, EXOFS_VERSION, 1024);
    CU_ASSERT_FALSE(shell_mount_sb_valid(s));

    fill_sb(s, 0, 0, 0);               /* a blank sector */
    CU_ASSERT_FALSE(shell_mount_sb_valid(s));
    CU_ASSERT_FALSE(shell_mount_sb_valid(NULL));
}

/* Must agree with the real superblock layout, since the probe reads the
 * fields by offset. */
static void test_sb_layout_matches(void)
{
    exofs_super_t sb;
    uint8_t *p = (uint8_t *)&sb;
    for (size_t i = 0; i < sizeof(sb); i++) {
        p[i] = 0;
    }
    sb.magic = EXOFS_MAGIC;
    sb.version = EXOFS_VERSION;
    sb.block_size = EXOFS_BLOCK_SIZE;
    CU_ASSERT_TRUE(shell_mount_sb_valid(&sb));
}

/* ---- Across two disks ----------------------------------------------------
 *
 * The same USB stick test_shell_install_k.c and test_multidisk_k.c
 * use (block device 1): every test below formats from LBA 0 of it, and
 * device 0's 8192-sector region at 8192 (test_exofs_k.c's, reformatted by
 * that suite on every test anyway).
 */

#define DEV_SYS     0u
#define DEV_STICK   1u
#define SYS_BASE    8192u
#define SYS_SECTORS 8192u
#define STICK_SECTORS 8192u

static int two_disks(void)
{
    return blkdev_get(DEV_SYS) != NULL && blkdev_get(DEV_STICK) != NULL;
}

static void teardown(void)
{
    exofs_unmount(g_tv);
    exofs_bdev_release(DEV_STICK);
    exofs_bdev_release(DEV_SYS);
}

int shell_mount_suite_init(void)
{
    teardown();
    return 0;
}

int shell_mount_suite_cleanup(void)
{
    teardown();
    return 0;
}

/* Both disks bound and a fresh volume on each. 0 if there is no second
 * drive, or setup failed (asserted), so the test can just return. */
static int setup(void)
{
    if (!two_disks())
        return 0;
    teardown();
    int ok = exofs_bdev_acquire(DEV_STICK) == 0 &&
             exofs_bdev_acquire_extent(DEV_SYS, SYS_BASE, SYS_SECTORS) == 0 &&
             exofs_format(DEV_STICK, 0, STICK_SECTORS) == 0 &&
             exofs_format(DEV_SYS, SYS_BASE, SYS_SECTORS) == 0;
    CU_ASSERT(ok);
    return ok;
}

static int put(uint32_t dev, uint32_t base, const char *path, const char *text)
{
    int rc = exofs_mount(dev, base, &g_tv);
    if (rc != 0)
        return rc;
    exofs_file_t f;
    rc = exofs_open(g_tv, path, EXOFS_O_WRITE | EXOFS_O_CREATE | EXOFS_O_TRUNC, &f);
    if (rc == 0) {
        uint32_t n = 0;
        while (text[n] != '\0')
            n++;
        if (exofs_write(&f, text, n) != (int64_t)n)
            rc = -EXO_EIO;
        exofs_close(&f);
    }
    exofs_unmount(g_tv);
    return rc;
}

/* The whole shell path for `cat /mnt/x/dir/hello`: normalize, route, mount
 * what the route says, read. */
static void test_route_and_read_second_disk(void)
{
    if (!setup())
        return;

    CU_ASSERT_EQUAL(exofs_mount(DEV_STICK, 0, &g_tv), 0);
    CU_ASSERT_EQUAL(exofs_mkdir(g_tv, "/dir"), 0);
    exofs_unmount(g_tv);
    CU_ASSERT_EQUAL(exofs_mount(DEV_SYS, SYS_BASE, &g_tv), 0);
    CU_ASSERT_EQUAL(exofs_mkdir(g_tv, "/dir"), 0);
    exofs_unmount(g_tv);
    CU_ASSERT_EQUAL(put(DEV_STICK, 0, "/dir/hello", "from the stick"), 0);
    CU_ASSERT_EQUAL(put(DEV_SYS, SYS_BASE, "/dir/hello", "from the system"), 0);

    shell_mount_table_t t;
    shell_mount_init(&t);
    shell_volref_t root = { DEV_SYS, SYS_BASE, 0, SYS_BASE + SYS_SECTORS, 1 };
    shell_volref_t stick = { DEV_STICK, 0, 0, 0, 1 };
    CU_ASSERT_EQUAL(shell_mount_add(&t, "/mnt/x", &stick), 0);

    static const struct { const char *path; const char *want; } cases[] = {
        { "/mnt/x/dir/hello",          "from the stick" },
        { "/mnt/x/dir/../dir/./hello", "from the stick" },
        { "/dir/hello",                "from the system" },
        { "/mnt/x/../../dir/hello",    "from the system" },  /* .. leaves the mount */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char norm[SHELL_MOUNT_PATH_LEN], inner[SHELL_MOUNT_PATH_LEN];
        shell_volref_t v;
        CU_ASSERT_EQUAL(shell_mount_normalize(cases[i].path, norm,
                                              sizeof(norm)), 0);
        CU_ASSERT_EQUAL(shell_mount_route(&t, &root, norm, &v, inner,
                                          sizeof(inner), NULL), 0);

        char buf[32] = { 0 };
        CU_ASSERT_EQUAL(exofs_mount(v.dev, v.base, &g_tv), 0);
        exofs_file_t f;
        CU_ASSERT_EQUAL(exofs_open(g_tv, inner, EXOFS_O_READ, &f), 0);
        int64_t n = exofs_read(&f, buf, sizeof(buf) - 1);
        exofs_close(&f);
        exofs_unmount(g_tv);
        CU_ASSERT(n > 0);
        CU_ASSERT_STRING_EQUAL(buf, cases[i].want);
    }
    teardown();
}

/* The probe `disk`/`mount` use: a formatted volume's first sector passes,
 * a blanked one does not (and does not mount either). */
static void test_probe_formatted_vs_blank(void)
{
    if (!setup())
        return;

    uint8_t *sec = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    CU_ASSERT_PTR_NOT_NULL(sec);
    if (sec == NULL) {
        teardown();
        return;
    }

    CU_ASSERT_EQUAL(exofs_bdev_read(DEV_STICK, 0, sec, 1), 0);
    CU_ASSERT_TRUE(shell_mount_sb_valid(sec));
    CU_ASSERT_EQUAL(exofs_bdev_read(DEV_SYS, SYS_BASE, sec, 1), 0);
    CU_ASSERT_TRUE(shell_mount_sb_valid(sec));

    for (unsigned i = 0; i < EXOFS_BLOCK_SIZE; i++)
        sec[i] = 0;
    CU_ASSERT_EQUAL(exofs_bdev_write(DEV_STICK, 0, sec, 1), 0);
    CU_ASSERT_EQUAL(exofs_bdev_read(DEV_STICK, 0, sec, 1), 0);
    CU_ASSERT_FALSE(shell_mount_sb_valid(sec));
    CU_ASSERT_EQUAL(exofs_mount(DEV_STICK, 0, &g_tv), -EXO_EINVAL);

    libos_heap_free(sec);
    teardown();
}

/* `cp /mnt/x/a /b` and `mv`: the two-path copy under different names, and a
 * missing source leaving the destination alone. */
static void test_copy_between_different_names(void)
{
    if (!setup())
        return;

    CU_ASSERT_EQUAL(put(DEV_STICK, 0, "/a.txt", "payload"), 0);
    CU_ASSERT_EQUAL(put(DEV_SYS, SYS_BASE, "/keep", "untouched"), 0);

    shell_vol_t stick = { DEV_STICK, 0 };
    shell_vol_t sys = { DEV_SYS, SYS_BASE };
    uint8_t *buf = libos_heap_alloc(256);
    CU_ASSERT_PTR_NOT_NULL(buf);
    if (buf == NULL) {
        teardown();
        return;
    }
    uint32_t copied = 0;

    CU_ASSERT_EQUAL(shell_copy_between(&stick, "/a.txt", &sys, "/b.txt", buf,
                                       256, &copied), 0);
    CU_ASSERT_EQUAL(copied, 7);

    char text[16] = { 0 };
    exofs_file_t f;
    CU_ASSERT_EQUAL(exofs_mount(DEV_SYS, SYS_BASE, &g_tv), 0);
    CU_ASSERT_EQUAL(exofs_open(g_tv, "/b.txt", EXOFS_O_READ, &f), 0);
    CU_ASSERT_EQUAL(exofs_read(&f, text, sizeof(text) - 1), 7);
    exofs_close(&f);
    exofs_stat_t st;
    CU_ASSERT_EQUAL(exofs_stat(g_tv, "/a.txt", &st), -EXO_ENOENT); /* not on sys */
    exofs_unmount(g_tv);
    CU_ASSERT_STRING_EQUAL(text, "payload");

    /* A source that is not there fails before the destination is touched. */
    CU_ASSERT_EQUAL(shell_copy_between(&stick, "/nope", &sys, "/keep", buf,
                                       256, &copied), -EXO_ENOENT);
    CU_ASSERT_EQUAL(copied, 0);
    CU_ASSERT_EQUAL(exofs_mount(DEV_SYS, SYS_BASE, &g_tv), 0);
    CU_ASSERT_EQUAL(exofs_stat(g_tv, "/keep", &st), 0);
    CU_ASSERT_EQUAL(st.size, 9);
    exofs_unmount(g_tv);

    libos_heap_free(buf);
    teardown();
}

void suite_shell_mount_tests(CU_pSuite s)
{
    CU_add_test(s, "normalize", test_normalize);
    CU_add_test(s, "normalize: rejects", test_normalize_rejects);
    CU_add_test(s, "path within", test_path_within);
    CU_add_test(s, "route: root volume", test_route_root);
    CU_add_test(s, "route: mounted volume", test_route_mounted);
    CU_add_test(s, "route: longest prefix", test_route_longest_prefix);
    CU_add_test(s, "route: .. across a mount", test_dotdot_across_mount);
    CU_add_test(s, "route: inner buffer too small", test_route_inner_too_small);
    CU_add_test(s, "table: add rules", test_add_rules);
    CU_add_test(s, "table: full", test_table_full);
    CU_add_test(s, "table: remove rules", test_remove_rules);
    CU_add_test(s, "volume name parse", test_parse_volname);
    CU_add_test(s, "superblock probe", test_sb_valid);
    CU_add_test(s, "superblock probe matches exofs_super_t",
                test_sb_layout_matches);
    CU_add_test(s, "second disk: route, mount and read",
                test_route_and_read_second_disk);
    CU_add_test(s, "second disk: probe formatted vs blank",
                test_probe_formatted_vs_blank);
    CU_add_test(s, "second disk: two-path copy", test_copy_between_different_names);
}
