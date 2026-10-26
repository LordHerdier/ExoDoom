/*
 * test_exo_file_k.c — exo_file_*, the descriptor API over ExoFS (SCRUM-44).
 *
 * Drives src/libos_fs/exo_file.c from ring 0, the same way test_exofs_k.c
 * drives ExoFS itself: the kernel-view (-DEXO_KERNEL) compile of
 * src/libos_fs/ routes disk I/O through exo_syscall_dispatch() rather than
 * the `syscall` stubs. See that file's header for the full reasoning; the
 * rules it sets out apply here unchanged:
 *
 *   - DISK BINDING. suite_init acquires it, suite_cleanup releases it, and
 *     every test touching hardware guards on drive_present().
 *   - LBA RANGE. The same EXOFS_TEST_BASE_LBA region the exofs suite owns.
 *     Suites run one after another and each formats the region fresh, so
 *     sharing it is safe; it keeps clear of the IWAD volume past 16384.
 *   - BUFFERS. Disk buffers come from libos_heap_alloc() inside ExoFS. The
 *     buffers passed in here are only memcpy'd to and from, so plain stack
 *     arrays are fine.
 *
 * Nothing here calls a file syscall, because there is none: #9-#16 are
 * reserved and unbound (test_syscall_k.c checks #9 answers -EXO_ENOSYS).
 * Every byte goes descriptor -> ExoFS -> exo_disk_read/exo_disk_write,
 * which is exactly what the ticket's acceptance criterion asks to show.
 */

#include "kunit.h"
#include "exo_errno.h"
#include "ata.h"
#include "string.h"
#include "stdio.h"

#include "libos_fs/exofs.h"
#include "libos_fs/exofs_blockdev.h"
#include "libos_fs/exo_file.h"

#include <stdint.h>
#include <stddef.h>

#define EXOFS_TEST_BASE_LBA 8192u

/* The volume the tests in this file are working on (SCRUM-265): set by
 * whichever helper mounts, read by everything after. A slot is zeroed on
 * unmount, so a stale pointer reads as "not mounted". */
static exofs_vol_t *g_tv;
#define TV_MOUNTED() (g_tv != NULL && g_tv->mounted)

/* Mount the test volume and make it the descriptor API's default. */
static int tv_mount(void)
{
    int rc = exofs_mount(0, EXOFS_TEST_BASE_LBA, &g_tv);
    if (rc == 0) exo_file_set_volume(g_tv);
    return rc;
}

/* See test_exofs_k.c's LBA RANGE note before moving either. */
#define VOL_SECTORS         8192u

static int drive_present(void)
{
    return ata_init() == ATA_OK;
}

int exo_file_suite_init(void)
{
    exofs_bdev_acquire(0);
    return 0;
}

int exo_file_suite_cleanup(void)
{
    /* Descriptors before the volume: unmounting under an open descriptor
     * is the silent-size-loss case exo_file.h warns about. */
    exo_file_close_all();
    exofs_unmount(g_tv);
    exofs_bdev_release(0);
    return 0;
}

/* Format + mount a clean volume. Also closes anything a previous, failed
 * test left open -- the descriptor table outlives a volume, so a stale open
 * slot would otherwise skew this test's descriptor numbers. */
static int fresh_volume(void)
{
    exo_file_close_all();
    exofs_unmount(g_tv);
    if (exofs_format(0, EXOFS_TEST_BASE_LBA, VOL_SECTORS) != 0) return 0;
    if (tv_mount() != 0) return 0;
    return 1;
}

static int remount(void)
{
    exofs_unmount(g_tv);
    return tv_mount() == 0;
}

/* ---- Constants ---------------------------------------------------------- */

static void test_constants(void)
{
    /* Same ordering as stdio's, so the FILE* shim (SCRUM-42) passes fseek's
     * whence straight through. */
    CU_ASSERT_EQUAL(EXO_FILE_SEEK_SET, SEEK_SET);
    CU_ASSERT_EQUAL(EXO_FILE_SEEK_CUR, SEEK_CUR);
    CU_ASSERT_EQUAL(EXO_FILE_SEEK_END, SEEK_END);

    /* Flags are a mask: five distinct bits. */
    CU_ASSERT_EQUAL(EXO_FILE_O_READ | EXO_FILE_O_WRITE | EXO_FILE_O_CREATE
                    | EXO_FILE_O_TRUNC | EXO_FILE_O_APPEND, 0x1Fu);

    /* A bigger table than ExoFS's open-entry table would only trade
     * -EXO_EMFILE for -EXO_ENFILE. */
    CU_ASSERT_EQUAL(EXO_FILE_MAX_FDS, 8);
}

/* ---- Descriptor table --------------------------------------------------- */

static void test_bad_descriptors(void)
{
    char c;

    /* No volume needed: these never reach ExoFS. */
    CU_ASSERT_EQUAL(exo_file_close(-1), -EXO_EBADF);
    CU_ASSERT_EQUAL(exo_file_close(EXO_FILE_MAX_FDS), -EXO_EBADF);
    CU_ASSERT_EQUAL(exo_file_read(-1, &c, 1u), -EXO_EBADF);
    CU_ASSERT_EQUAL(exo_file_write(EXO_FILE_MAX_FDS, &c, 1u), -EXO_EBADF);
    CU_ASSERT_EQUAL(exo_file_seek(1000, 0, EXO_FILE_SEEK_SET), -EXO_EBADF);

    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    int fd = exo_file_open("/a", EXO_FILE_O_WRITE | EXO_FILE_O_CREATE);
    CU_ASSERT_EQUAL(fd, 0);
    CU_ASSERT_EQUAL(exo_file_close(fd), 0);

    /* Closed is as bad as never opened -- including a second close. */
    CU_ASSERT_EQUAL(exo_file_close(fd), -EXO_EBADF);
    CU_ASSERT_EQUAL(exo_file_read(fd, &c, 1u), -EXO_EBADF);
    CU_ASSERT_EQUAL(exo_file_write(fd, &c, 1u), -EXO_EBADF);
    CU_ASSERT_EQUAL(exo_file_seek(fd, 0, EXO_FILE_SEEK_SET), -EXO_EBADF);

    exofs_unmount(g_tv);
}

static void test_table_full_and_slot_reuse(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    char path[4] = "/f0";
    for (int i = 0; i < EXO_FILE_MAX_FDS; i++) {
        path[2] = (char)('0' + i);
        CU_ASSERT_EQUAL(exo_file_open(path,
                                      EXO_FILE_O_WRITE | EXO_FILE_O_CREATE), i);
    }

    CU_ASSERT_EQUAL(exo_file_open("/f9", EXO_FILE_O_WRITE | EXO_FILE_O_CREATE),
                    -EXO_EMFILE);
    /* The refused open created nothing: the table was checked first. */
    uint32_t size;
    CU_ASSERT_EQUAL(exo_file_stat("/f9", &size), -EXO_ENOENT);

    /* Lowest free descriptor is reused. */
    CU_ASSERT_EQUAL(exo_file_close(3), 0);
    CU_ASSERT_EQUAL(exo_file_open("/f3", EXO_FILE_O_READ), 3);

    CU_ASSERT_EQUAL(exo_file_close_all(), 0);
    exofs_unmount(g_tv);
}

static void test_failed_open_consumes_no_descriptor(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    CU_ASSERT_EQUAL(exo_file_open("/missing", EXO_FILE_O_READ), -EXO_ENOENT);
    CU_ASSERT_EQUAL(exo_file_open("/", EXO_FILE_O_READ), -EXO_EISDIR);
    CU_ASSERT_EQUAL(exo_file_open("/x", 0u), -EXO_EINVAL);

    CU_ASSERT_EQUAL(exo_file_open("/x", EXO_FILE_O_WRITE | EXO_FILE_O_CREATE),
                    0);
    CU_ASSERT_EQUAL(exo_file_close(0), 0);
    exofs_unmount(g_tv);
}

static void test_nothing_mounted(void)
{
    exo_file_close_all();
    exofs_unmount(g_tv);

    uint32_t size;
    CU_ASSERT_EQUAL(exo_file_open("/a", EXO_FILE_O_READ), -EXO_EINVAL);
    CU_ASSERT_EQUAL(exo_file_stat("/a", &size), -EXO_EINVAL);
}

/* ---- I/O ---------------------------------------------------------------- */

static void test_access_mode_is_enforced(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    char c = 'x';
    int fd = exo_file_open("/w", EXO_FILE_O_WRITE | EXO_FILE_O_CREATE);
    CU_ASSERT_EQUAL(exo_file_read(fd, &c, 1u), -EXO_EACCES);
    CU_ASSERT_EQUAL(exo_file_write(fd, &c, 1u), 1);
    CU_ASSERT_EQUAL(exo_file_close(fd), 0);

    fd = exo_file_open("/w", EXO_FILE_O_READ);
    CU_ASSERT_EQUAL(exo_file_write(fd, &c, 1u), -EXO_EACCES);
    CU_ASSERT_EQUAL(exo_file_close(fd), 0);

    exofs_unmount(g_tv);
}

static void test_seek(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    int fd = exo_file_open("/s", EXO_FILE_O_READ | EXO_FILE_O_WRITE
                                 | EXO_FILE_O_CREATE);
    CU_ASSERT_EQUAL(exo_file_write(fd, "0123456789", 10u), 10);

    /* SEEK_CUR with 0 is ftell. */
    CU_ASSERT_EQUAL(exo_file_seek(fd, 0, EXO_FILE_SEEK_CUR), 10);
    CU_ASSERT_EQUAL(exo_file_seek(fd, -4, EXO_FILE_SEEK_END), 6);

    char back[8];
    memset(back, 0, sizeof(back));
    CU_ASSERT_EQUAL(exo_file_read(fd, back, 8u), 4);
    CU_ASSERT_STRING_EQUAL(back, "6789");

    CU_ASSERT_EQUAL(exo_file_seek(fd, -1, EXO_FILE_SEEK_SET), -EXO_EINVAL);
    CU_ASSERT_EQUAL(exo_file_seek(fd, 0, 7u), -EXO_EINVAL);
    /* A refused seek leaves the position alone. */
    CU_ASSERT_EQUAL(exo_file_seek(fd, 0, EXO_FILE_SEEK_CUR), 10);

    CU_ASSERT_EQUAL(exo_file_close(fd), 0);
    exofs_unmount(g_tv);
}

static void test_truncate_and_append(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    uint32_t size = 0;
    int fd = exo_file_open("/t", EXO_FILE_O_WRITE | EXO_FILE_O_CREATE);
    CU_ASSERT_EQUAL(exo_file_write(fd, "abcdefghij", 10u), 10);
    CU_ASSERT_EQUAL(exo_file_close(fd), 0);

    /* TRUNC is recorded at open, not at close (docs/filesystem.md §6c). */
    fd = exo_file_open("/t", EXO_FILE_O_WRITE | EXO_FILE_O_TRUNC);
    CU_ASSERT_TRUE(fd >= 0);
    CU_ASSERT_EQUAL(exo_file_stat("/t", &size), 0);
    CU_ASSERT_EQUAL(size, 0u);
    CU_ASSERT_EQUAL(exo_file_write(fd, "abc", 3u), 3);
    CU_ASSERT_EQUAL(exo_file_close(fd), 0);

    /* APPEND writes at the end even after a seek to the start. */
    fd = exo_file_open("/t", EXO_FILE_O_WRITE | EXO_FILE_O_APPEND);
    CU_ASSERT_EQUAL(exo_file_seek(fd, 0, EXO_FILE_SEEK_SET), 0);
    CU_ASSERT_EQUAL(exo_file_write(fd, "de", 2u), 2);
    CU_ASSERT_EQUAL(exo_file_close(fd), 0);

    char back[8];
    memset(back, 0, sizeof(back));
    fd = exo_file_open("/t", EXO_FILE_O_READ);
    CU_ASSERT_EQUAL(exo_file_read(fd, back, 8u), 5);
    CU_ASSERT_EQUAL(exo_file_close(fd), 0);
    CU_ASSERT_STRING_EQUAL(back, "abcde");

    exofs_unmount(g_tv);
}

/* ---- stat --------------------------------------------------------------- */

static void test_stat(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    uint32_t size = 123u;
    CU_ASSERT_EQUAL(exo_file_stat("/nope", &size), -EXO_ENOENT);
    CU_ASSERT_EQUAL(size, 123u);   /* untouched on failure */
    CU_ASSERT_EQUAL(exo_file_stat("/nope", NULL), -EXO_EINVAL);

    CU_ASSERT_EQUAL(exofs_mkdir(g_tv, "/dir"), 0);
    CU_ASSERT_EQUAL(exo_file_stat("/dir", &size), 0);
    CU_ASSERT_EQUAL(size, 0u);

    int fd = exo_file_open("/dir/f", EXO_FILE_O_WRITE | EXO_FILE_O_CREATE);
    CU_ASSERT_EQUAL(exo_file_write(fd, "hello", 5u), 5);
    CU_ASSERT_EQUAL(exo_file_close(fd), 0);
    CU_ASSERT_EQUAL(exo_file_stat("/dir/f", &size), 0);
    CU_ASSERT_EQUAL(size, 5u);

    exofs_unmount(g_tv);
}

/* ---- Durability --------------------------------------------------------- */

static void test_close_all_makes_sizes_durable(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    int a = exo_file_open("/a", EXO_FILE_O_WRITE | EXO_FILE_O_CREATE);
    int b = exo_file_open("/b", EXO_FILE_O_WRITE | EXO_FILE_O_CREATE);
    int c = exo_file_open("/c", EXO_FILE_O_WRITE | EXO_FILE_O_CREATE);
    CU_ASSERT_EQUAL(exo_file_write(a, "1", 1u), 1);
    CU_ASSERT_EQUAL(exo_file_write(b, "22", 2u), 2);
    CU_ASSERT_EQUAL(exo_file_write(c, "333", 3u), 3);

    CU_ASSERT_EQUAL(exo_file_close_all(), 0);
    CU_ASSERT_EQUAL(exo_file_close(a), -EXO_EBADF);
    CU_ASSERT_EQUAL(exo_file_close(b), -EXO_EBADF);
    CU_ASSERT_EQUAL(exo_file_close(c), -EXO_EBADF);
    /* Nothing open is not an error. */
    CU_ASSERT_EQUAL(exo_file_close_all(), 0);

    if (!remount()) { CU_ASSERT_TRUE(0); return; }
    uint32_t size;
    CU_ASSERT_EQUAL(exo_file_stat("/a", &size), 0);
    CU_ASSERT_EQUAL(size, 1u);
    CU_ASSERT_EQUAL(exo_file_stat("/b", &size), 0);
    CU_ASSERT_EQUAL(size, 2u);
    CU_ASSERT_EQUAL(exo_file_stat("/c", &size), 0);
    CU_ASSERT_EQUAL(size, 3u);

    exofs_unmount(g_tv);
}

/*
 * The ticket's acceptance criterion, in its own order: create a file, write
 * bytes, seek to the start, read them back, stat returns the right size --
 * then the same file again after a remount, so "right size" means the size
 * on disk and not a cached one.
 */
static void test_acceptance(void)
{
    if (!drive_present()) return;
    if (!fresh_volume()) { CU_ASSERT_TRUE(0); return; }

    /* More than one 512-byte block, so the read back crosses a chain link. */
    static char out[1300];
    static char back[1300];
    for (unsigned i = 0; i < sizeof(out); i++) out[i] = (char)(i * 7u + 3u);

    int fd = exo_file_open("/save.dsg", EXO_FILE_O_READ | EXO_FILE_O_WRITE
                                        | EXO_FILE_O_CREATE);
    CU_ASSERT_TRUE(fd >= 0);
    CU_ASSERT_EQUAL(exo_file_write(fd, out, sizeof(out)), (int64_t)sizeof(out));
    CU_ASSERT_EQUAL(exo_file_seek(fd, 0, EXO_FILE_SEEK_SET), 0);

    memset(back, 0, sizeof(back));
    CU_ASSERT_EQUAL(exo_file_read(fd, back, sizeof(back)),
                    (int64_t)sizeof(back));
    CU_ASSERT_EQUAL(memcmp(back, out, sizeof(out)), 0);
    CU_ASSERT_EQUAL(exo_file_close(fd), 0);

    uint32_t size = 0;
    CU_ASSERT_EQUAL(exo_file_stat("/save.dsg", &size), 0);
    CU_ASSERT_EQUAL(size, (uint32_t)sizeof(out));

    if (!remount()) { CU_ASSERT_TRUE(0); return; }
    CU_ASSERT_EQUAL(exo_file_stat("/save.dsg", &size), 0);
    CU_ASSERT_EQUAL(size, (uint32_t)sizeof(out));

    memset(back, 0, sizeof(back));
    fd = exo_file_open("/save.dsg", EXO_FILE_O_READ);
    CU_ASSERT_EQUAL(exo_file_read(fd, back, sizeof(back)),
                    (int64_t)sizeof(back));
    CU_ASSERT_EQUAL(exo_file_read(fd, back, 1u), 0);   /* EOF */
    CU_ASSERT_EQUAL(exo_file_close(fd), 0);
    CU_ASSERT_EQUAL(memcmp(back, out, sizeof(out)), 0);

    exofs_unmount(g_tv);
}

void suite_exo_file_tests(CU_pSuite s)
{
    CU_add_test(s, "constants", test_constants);
    CU_add_test(s, "bad descriptors are EBADF", test_bad_descriptors);
    CU_add_test(s, "a full table is EMFILE; slots are reused",
                test_table_full_and_slot_reuse);
    CU_add_test(s, "a failed open consumes no descriptor",
                test_failed_open_consumes_no_descriptor);
    CU_add_test(s, "nothing mounted", test_nothing_mounted);
    CU_add_test(s, "access mode is enforced", test_access_mode_is_enforced);
    CU_add_test(s, "seek", test_seek);
    CU_add_test(s, "truncate and append", test_truncate_and_append);
    CU_add_test(s, "stat", test_stat);
    CU_add_test(s, "close_all makes sizes durable",
                test_close_all_makes_sizes_durable);
    CU_add_test(s, "ACCEPTANCE: create, write, seek, read back, stat",
                test_acceptance);
}
