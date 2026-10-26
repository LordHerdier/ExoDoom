/*
 * test_stdio_exofs_k.c — fopen/fread/fwrite/fseek/ftell/fflush/fclose/mkdir
 * over ExoFS (SCRUM-42, SCRUM-75).
 *
 * Drives src/stdio.c with the backend from src/libos_fs/exo_file_stdio.c
 * installed, the way Doom's LibOS installs it after mounting its volume. The
 * volume fixture, LBA range and disk-binding rules are test_exo_file_k.c's:
 * this suite reuses its suite_init/cleanup and the same scratch region.
 */

#include "kunit.h"
#include "exo_errno.h"
#include "ata.h"
#include "errno.h"
#include "string.h"
#include "stdio.h"
#include "sys/stat.h"

#include "libos_fs/exofs.h"
#include "libos_fs/exofs_attach.h"
#include "libos_fs/exofs_blockdev.h"
#include "libos_fs/exo_file.h"
#include "libos_fs/exo_file_stdio.h"
#include "doom_wad.h"
#include "doomgeneric_exo.h"
#include "libos_heap.h"

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

#define VOL_SECTORS         8192u

#define FILE_LEN 3000u   /* spans several ExoFS blocks */

static int drive_present(void)
{
    return ata_init() == ATA_OK;
}

static uint8_t pattern(uint32_t i)
{
    return (uint8_t)((i * 7u + 3u) & 0xFFu);
}

/* Fresh volume holding /data.bin (FILE_LEN bytes of pattern()) and
 * /cfg.txt, with the stdio backend installed. */
static int setup(void)
{
    uint8_t buf[FILE_LEN];
    uint32_t i;
    static const char cfg[] = "alpha 12\nbeta 34\n";
    int fd;

    exo_file_stdio_remove();
    exo_file_close_all();
    exofs_unmount(g_tv);
    if (exofs_format(0, EXOFS_TEST_BASE_LBA, VOL_SECTORS) != 0) return 0;
    if (tv_mount() != 0) return 0;

    for (i = 0; i < FILE_LEN; i++) buf[i] = pattern(i);
    fd = exo_file_open("/data.bin", EXO_FILE_O_WRITE | EXO_FILE_O_CREATE);
    if (fd < 0) return 0;
    if (exo_file_write(fd, buf, FILE_LEN) != (int64_t)FILE_LEN) return 0;
    if (exo_file_close(fd) != 0) return 0;

    fd = exo_file_open("/cfg.txt", EXO_FILE_O_WRITE | EXO_FILE_O_CREATE);
    if (fd < 0) return 0;
    if (exo_file_write(fd, cfg, sizeof cfg - 1) != (int64_t)(sizeof cfg - 1))
        return 0;
    if (exo_file_close(fd) != 0) return 0;

    exo_file_stdio_install();
    return 1;
}

static void teardown(void)
{
    exo_file_stdio_remove();
    exo_file_close_all();
    exofs_unmount(g_tv);
}

static void test_read_whole_file(void)
{
    if (!drive_present()) return;
    CU_ASSERT_TRUE(setup());

    uint8_t back[FILE_LEN];
    uint32_t i, bad = 0;
    FILE *f = fopen("data.bin", "rb");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        CU_ASSERT_EQUAL(fread(back, 1, FILE_LEN, f), (size_t)FILE_LEN);
        for (i = 0; i < FILE_LEN; i++) if (back[i] != pattern(i)) bad++;
        CU_ASSERT_EQUAL(bad, 0u);
        CU_ASSERT_EQUAL(ftell(f), (long)FILE_LEN);
        CU_ASSERT_FALSE(feof(f));
        CU_ASSERT_EQUAL(fread(back, 1, 1, f), (size_t)0);
        CU_ASSERT_TRUE(feof(f));
        CU_ASSERT_EQUAL(fclose(f), 0);
    }
    teardown();
}

static void test_seek_and_random_read(void)
{
    if (!drive_present()) return;
    CU_ASSERT_TRUE(setup());

    uint8_t b[16];
    uint32_t i, bad = 0;
    FILE *f = fopen("/data.bin", "r");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        /* The W_StdC_Read pattern: seek, read, seek elsewhere, read. */
        CU_ASSERT_EQUAL(fseek(f, 2000, SEEK_SET), 0);
        CU_ASSERT_EQUAL(fread(b, 1, sizeof b, f), sizeof b);
        for (i = 0; i < sizeof b; i++) if (b[i] != pattern(2000u + i)) bad++;
        CU_ASSERT_EQUAL(fseek(f, 5, SEEK_SET), 0);
        CU_ASSERT_EQUAL(fread(b, 1, sizeof b, f), sizeof b);
        for (i = 0; i < sizeof b; i++) if (b[i] != pattern(5u + i)) bad++;
        CU_ASSERT_EQUAL(bad, 0u);

        CU_ASSERT_EQUAL(fseek(f, 0, SEEK_END), 0);
        CU_ASSERT_EQUAL(ftell(f), (long)FILE_LEN);
        CU_ASSERT_EQUAL(fseek(f, -4, SEEK_CUR), 0);
        CU_ASSERT_EQUAL(fread(b, 1, 8, f), (size_t)4);   /* short at EOF */
        CU_ASSERT_TRUE(feof(f));
        CU_ASSERT_EQUAL(fseek(f, 1, SEEK_END), -1);       /* past the end */
        fclose(f);
    }
    teardown();
}

static void test_path_forms(void)
{
    if (!drive_present()) return;
    CU_ASSERT_TRUE(setup());

    const char *names[] = { "cfg.txt", "./cfg.txt", "/cfg.txt", "//cfg.txt" };
    unsigned i;
    for (i = 0; i < 4; i++) {
        FILE *f = fopen(names[i], "r");
        CU_ASSERT_PTR_NOT_NULL(f);
        if (f != NULL) fclose(f);
    }
    teardown();
}

static void test_missing_and_write_modes(void)
{
    if (!drive_present()) return;
    CU_ASSERT_TRUE(setup());

    errno = 0;
    CU_ASSERT_PTR_NULL(fopen("nope.wad", "rb"));
    CU_ASSERT_EQUAL(errno, ENOENT);

    errno = 0;
    CU_ASSERT_PTR_NULL(fopen("", "r"));      /* not the root directory */
    CU_ASSERT_EQUAL(errno, ENOENT);
    CU_ASSERT_PTR_NULL(fopen("/", "r"));

    /* "r+" never creates; a mode C99 does not define is EINVAL. */
    errno = 0;
    CU_ASSERT_PTR_NULL(fopen("nope.txt", "r+"));
    CU_ASSERT_EQUAL(errno, ENOENT);
    errno = 0;
    CU_ASSERT_PTR_NULL(fopen("cfg.txt", "x"));
    CU_ASSERT_EQUAL(errno, EINVAL);
    teardown();
}

static void test_fscanf_from_file(void)
{
    if (!drive_present()) return;
    CU_ASSERT_TRUE(setup());

    char name[16];
    int  val = 0;
    FILE *f = fopen("cfg.txt", "r");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        CU_ASSERT_EQUAL(fscanf(f, "%15s %d", name, &val), 2);
        CU_ASSERT_EQUAL(strcmp(name, "alpha"), 0);
        CU_ASSERT_EQUAL(val, 12);
        CU_ASSERT_EQUAL(fscanf(f, "%15s %d", name, &val), 2);
        CU_ASSERT_EQUAL(strcmp(name, "beta"), 0);
        CU_ASSERT_EQUAL(val, 34);
        fclose(f);
    }
    teardown();
}

static void test_table_full_and_close_frees_descriptor(void)
{
    if (!drive_present()) return;
    CU_ASSERT_TRUE(setup());

    /* ExoFS can hold EXO_FILE_MAX_FDS (8) open; the 9th open fails, and
     * closing one makes room again. */
    FILE *fs[EXO_FILE_MAX_FDS];
    unsigned i, opened = 0;
    for (i = 0; i < EXO_FILE_MAX_FDS; i++) {
        fs[i] = fopen("cfg.txt", "r");
        if (fs[i] != NULL) opened++;
    }
    CU_ASSERT_TRUE(opened >= 1u);
    CU_ASSERT_PTR_NULL(fopen("cfg.txt", "r"));
    for (i = 0; i < EXO_FILE_MAX_FDS; i++)
        if (fs[i] != NULL) { fclose(fs[i]); break; }
    FILE *again = fopen("cfg.txt", "r");
    CU_ASSERT_PTR_NOT_NULL(again);
    if (again != NULL) fclose(again);
    for (i = 1; i < EXO_FILE_MAX_FDS; i++)
        if (fs[i] != NULL) fclose(fs[i]);
    teardown();
}

static void test_remove_restores_blob_behaviour(void)
{
    if (!drive_present()) return;
    CU_ASSERT_TRUE(setup());

    exo_file_stdio_remove();
    errno = 0;
    CU_ASSERT_PTR_NULL(fopen("cfg.txt", "r"));
    CU_ASSERT_EQUAL(errno, ENOENT);
    teardown();
}

/* ---- dg_check_iwad (SCRUM-75) ------------------------------------------- */

static void write_file(const char *path, const void *data, uint32_t n)
{
    int fd = exo_file_open(path, EXO_FILE_O_WRITE | EXO_FILE_O_CREATE);
    CU_ASSERT_TRUE(fd >= 0);
    if (fd >= 0) {
        CU_ASSERT_EQUAL(exo_file_write(fd, data, n), (int64_t)n);
        CU_ASSERT_EQUAL(exo_file_close(fd), 0);
    }
}

static void test_check_iwad(void)
{
    if (!drive_present()) return;
    CU_ASSERT_TRUE(setup());

    /* magic, numlumps, dirofs -- the directory is W_AddFile's to read. */
    static const uint8_t iwad[12] = { 'I','W','A','D', 2,0,0,0, 12,0,0,0 };
    static const uint8_t pwad[12] = { 'P','W','A','D', 2,0,0,0, 12,0,0,0 };
    static const uint8_t junk[12] = { 'J','U','N','K', 2,0,0,0, 12,0,0,0 };
    static const uint8_t shorty[5] = { 'I','W','A','D', 0 };

    write_file("/good.wad", iwad, sizeof iwad);
    write_file("/patch.wad", pwad, sizeof pwad);
    write_file("/junk.wad", junk, sizeof junk);
    write_file("/short.wad", shorty, sizeof shorty);

    CU_ASSERT_EQUAL(dg_check_iwad("good.wad"), DOOM_WAD_OK);
    CU_ASSERT_EQUAL(dg_check_iwad("patch.wad"), DOOM_WAD_OK);   /* warned */
    CU_ASSERT_EQUAL(dg_check_iwad("junk.wad"), DOOM_WAD_EINVAL);
    CU_ASSERT_EQUAL(dg_check_iwad("short.wad"), DOOM_WAD_EINVAL);
    CU_ASSERT_EQUAL(dg_check_iwad("absent.wad"), DOOM_WAD_ENOENT);
    teardown();
}


/* ---- writing (SCRUM-42) -------------------------------------------------- */

static uint32_t size_of(const char *path)
{
    uint32_t sz = 0xFFFFFFFFu;

    CU_ASSERT_EQUAL(exo_file_stat(path, &sz), 0);
    return sz;
}

/* Write len bytes of pattern() to `path` in `chunk`-byte fwrite calls. */
static void write_pattern(const char *path, uint32_t len, uint32_t chunk)
{
    uint8_t  buf[FILE_LEN];
    uint32_t i, off;
    FILE    *f = fopen(path, "wb");

    CU_ASSERT_PTR_NOT_NULL(f);
    if (f == NULL) return;
    for (i = 0; i < len; i++) buf[i] = pattern(i);
    for (off = 0; off < len; off += chunk) {
        uint32_t n = len - off < chunk ? len - off : chunk;
        CU_ASSERT_EQUAL(fwrite(buf + off, 1, n, f), (size_t)n);
    }
    CU_ASSERT_EQUAL(ftell(f), (long)len);
    CU_ASSERT_EQUAL(fclose(f), 0);
}

static void check_pattern(const char *path, uint32_t len)
{
    uint8_t  back[FILE_LEN];
    uint32_t i, bad = 0;
    FILE    *f;

    CU_ASSERT_EQUAL(size_of(path), len);
    f = fopen(path, "rb");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f == NULL) return;
    CU_ASSERT_EQUAL(fread(back, 1, FILE_LEN, f), (size_t)len);
    for (i = 0; i < len; i++) if (back[i] != pattern(i)) bad++;
    CU_ASSERT_EQUAL(bad, 0u);
    fclose(f);
}

static void test_write_round_trip(void)
{
    static const uint32_t lens[] = { 1, 511, 512, 513, 1024, FILE_LEN };
    unsigned i;

    if (!drive_present()) return;
    CU_ASSERT_TRUE(setup());

    for (i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        write_pattern("/out.bin", lens[i], lens[i]);   /* one fwrite  */
        check_pattern("/out.bin", lens[i]);
        write_pattern("/out.bin", lens[i], 1);         /* byte at a time */
        check_pattern("/out.bin", lens[i]);
        write_pattern("/out.bin", lens[i], 100);       /* straddles blocks */
        check_pattern("/out.bin", lens[i]);
    }
    teardown();
}

static void test_write_modes(void)
{
    char  line[64];
    FILE *f;

    if (!drive_present()) return;
    CU_ASSERT_TRUE(setup());

    /* "w" truncates what was there. */
    f = fopen("data.bin", "w");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) fclose(f);
    CU_ASSERT_EQUAL(size_of("/data.bin"), 0u);

    /* "a" appends to the end, and ftell starts there. */
    f = fopen("cfg.txt", "a");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        CU_ASSERT_EQUAL(ftell(f), 17L);
        CU_ASSERT_EQUAL(fwrite("gamma 56\n", 1, 9, f), (size_t)9);
        CU_ASSERT_EQUAL(fclose(f), 0);
    }
    CU_ASSERT_EQUAL(size_of("/cfg.txt"), 26u);
    f = fopen("cfg.txt", "r");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        memset(line, 0, sizeof line);
        CU_ASSERT_EQUAL(fread(line, 1, sizeof line, f), (size_t)26);
        CU_ASSERT_EQUAL(memcmp(line, "alpha 12\nbeta 34\ngamma 56\n", 26), 0);
        fclose(f);
    }

    /* "r+" overwrites in place without truncating. */
    f = fopen("cfg.txt", "r+");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        CU_ASSERT_EQUAL(fwrite("ALPHA", 1, 5, f), (size_t)5);
        CU_ASSERT_EQUAL(fseek(f, 0, SEEK_SET), 0);
        memset(line, 0, sizeof line);
        CU_ASSERT_EQUAL(fread(line, 1, 8, f), (size_t)8);
        CU_ASSERT_EQUAL(memcmp(line, "ALPHA 12", 8), 0);
        CU_ASSERT_EQUAL(fclose(f), 0);
    }
    CU_ASSERT_EQUAL(size_of("/cfg.txt"), 26u);

    /* "w+" reads back what it wrote, with no seek-flush by the caller:
     * the fread itself has to push the buffered bytes out. */
    f = fopen("scratch.txt", "w+");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        CU_ASSERT_EQUAL(fwrite("hello", 1, 5, f), (size_t)5);
        CU_ASSERT_EQUAL(fseek(f, 0, SEEK_SET), 0);
        memset(line, 0, sizeof line);
        CU_ASSERT_EQUAL(fread(line, 1, 5, f), (size_t)5);
        CU_ASSERT_EQUAL(memcmp(line, "hello", 5), 0);
        CU_ASSERT_EQUAL(fclose(f), 0);
    }

    /* A read-only stream refuses to write rather than faulting. */
    f = fopen("cfg.txt", "r");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        errno = 0;
        CU_ASSERT_EQUAL(fwrite("x", 1, 1, f), (size_t)0);
        CU_ASSERT_EQUAL(errno, EBADF);
        fclose(f);
    }
    teardown();
}

static void test_write_buffering_and_position(void)
{
    uint8_t  blk[600];
    FILE    *f;

    if (!drive_present()) return;
    CU_ASSERT_TRUE(setup());

    memset(blk, 'z', sizeof blk);
    f = fopen("buf.bin", "wb");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        /* Sub-block writes stay in the stream: ftell counts them. */
        CU_ASSERT_EQUAL(fwrite(blk, 1, 100, f), (size_t)100);
        CU_ASSERT_EQUAL(ftell(f), 100L);
        CU_ASSERT_EQUAL(fseek(f, 10, SEEK_SET), 0);
        CU_ASSERT_EQUAL(ftell(f), 10L);
        CU_ASSERT_EQUAL(fseek(f, 0, SEEK_END), 0);
        CU_ASSERT_EQUAL(ftell(f), 100L);
        /* One call larger than a block goes through whole. */
        CU_ASSERT_EQUAL(fwrite(blk, 1, sizeof blk, f), sizeof blk);
        CU_ASSERT_EQUAL(ftell(f), 700L);
        CU_ASSERT_EQUAL(fflush(f), 0);
        CU_ASSERT_EQUAL(fflush(NULL), 0);
        CU_ASSERT_EQUAL(fclose(f), 0);
    }
    CU_ASSERT_EQUAL(size_of("/buf.bin"), 700u);

    /* fwrite counts whole elements. */
    f = fopen("buf.bin", "wb");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        CU_ASSERT_EQUAL(fwrite(blk, 4, 25, f), (size_t)25);
        CU_ASSERT_EQUAL(fclose(f), 0);
    }
    CU_ASSERT_EQUAL(size_of("/buf.bin"), 100u);
    teardown();
}

static void test_fprintf_config_round_trip(void)
{
    char  name[32];
    int   val = 0;
    char  longstr[700];
    FILE *f;

    if (!drive_present()) return;
    CU_ASSERT_TRUE(setup());

    /* The M_SaveDefaults shape: "name   value\n" lines. */
    f = fopen("default.cfg", "w");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        CU_ASSERT_EQUAL(fprintf(f, "%s ", "screenblocks"), 13);
        CU_ASSERT_EQUAL(fprintf(f, "%i\n", 10), 3);
        CU_ASSERT_EQUAL(fprintf(f, "%s 0x%x\n", "mouse", 0x1f), 11);
        CU_ASSERT_EQUAL(fclose(f), 0);
    }
    f = fopen("default.cfg", "r");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        CU_ASSERT_EQUAL(fscanf(f, "%31s %d", name, &val), 2);
        CU_ASSERT_EQUAL(strcmp(name, "screenblocks"), 0);
        CU_ASSERT_EQUAL(val, 10);
        fclose(f);
    }

    /* A line that does not fit the stream's block is refused, not cut. */
    memset(longstr, 'q', sizeof longstr - 1);
    longstr[sizeof longstr - 1] = '\0';
    f = fopen("default.cfg", "w");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        errno = 0;
        CU_ASSERT_EQUAL(fprintf(f, "%s", longstr), -1);
        CU_ASSERT_EQUAL(errno, EFBIG);
        fclose(f);
    }
    CU_ASSERT_EQUAL(size_of("/default.cfg"), 0u);
    teardown();
}

static void test_mkdir_over_exofs(void)
{
    FILE *f;

    if (!drive_present()) return;
    CU_ASSERT_TRUE(setup());

    CU_ASSERT_EQUAL(mkdir("saves", 0755), 0);
    errno = 0;
    CU_ASSERT_EQUAL(mkdir("saves", 0755), -1);
    CU_ASSERT_EQUAL(errno, EEXIST);
    errno = 0;
    CU_ASSERT_EQUAL(mkdir("./saves/", 0755), -1);     /* same directory */
    CU_ASSERT_EQUAL(errno, EEXIST);
    errno = 0;
    CU_ASSERT_EQUAL(mkdir("./", 0755), -1);           /* the root */
    CU_ASSERT_EQUAL(errno, EEXIST);

    f = fopen("saves/doomsav0.dsg", "wb");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        CU_ASSERT_EQUAL(fwrite("x", 1, 1, f), (size_t)1);
        CU_ASSERT_EQUAL(fclose(f), 0);
    }
    CU_ASSERT_EQUAL(size_of("/saves/doomsav0.dsg"), 1u);
    teardown();
}

/* Doom's savegame rotation (g_game.c G_DoSaveGame): write a temp file,
 * remove the old save, rename the temp over it. Rename never replaces
 * (docs/filesystem.md section 1.4), so the remove is what lets a second save
 * of the same slot through, and the first save's remove finds nothing and
 * must fail rather than claim a delete (SCRUM-267). */
static void test_save_rotation_remove_rename(void)
{
    char  got[8];
    FILE *f;

    if (!drive_present()) return;
    CU_ASSERT_TRUE(setup());

    for (int round = 0; round < 2; round++) {
        const char *body = round ? "second" : "first";
        size_t      len  = round ? 6 : 5;

        f = fopen("temp.dsg", "wb");
        CU_ASSERT_PTR_NOT_NULL(f);
        if (f != NULL) {
            CU_ASSERT_EQUAL(fwrite(body, 1, len, f), len);
            CU_ASSERT_EQUAL(fclose(f), 0);
        }

        errno = 0;
        if (round == 0) {
            CU_ASSERT_EQUAL(remove("doomsav0.dsg"), -1);
            CU_ASSERT_EQUAL(errno, ENOENT);
        } else {
            errno = 0;
            CU_ASSERT_EQUAL(rename("temp.dsg", "doomsav0.dsg"), -1);
            CU_ASSERT_EQUAL(errno, EEXIST);       /* never replaces */
            CU_ASSERT_EQUAL(remove("doomsav0.dsg"), 0);
        }
        CU_ASSERT_EQUAL(rename("temp.dsg", "./doomsav0.dsg"), 0);

        errno = 0;
        CU_ASSERT_PTR_NULL(fopen("temp.dsg", "rb"));   /* moved, not copied */
        CU_ASSERT_EQUAL(errno, ENOENT);

        memset(got, 0, sizeof got);
        f = fopen("doomsav0.dsg", "rb");
        CU_ASSERT_PTR_NOT_NULL(f);
        if (f != NULL) {
            CU_ASSERT_EQUAL(fread(got, 1, sizeof got, f), len);
            CU_ASSERT_EQUAL(memcmp(got, body, len), 0);
            fclose(f);
        }
    }

    /* The root is not a file to remove. */
    errno = 0;
    CU_ASSERT_EQUAL(remove("./"), -1);
    CU_ASSERT_EQUAL(errno, EISDIR);
    teardown();
}

/* A backend with no write op: every write mode stays EROFS, which is how
 * the table behaved before SCRUM-42. */
static int ro_open(const char *path, unsigned flags)
{
    (void)path; (void)flags;
    return -ENOENT;
}
static int64_t ro_read(int h, void *b, size_t n) { (void)h; (void)b; (void)n; return -EIO; }
static int64_t ro_seek(int h, int64_t o, int w)  { (void)h; (void)o; (void)w; return -EIO; }
static int ro_close(int h)                       { (void)h; return 0; }

static void test_backend_without_write_is_read_only(void)
{
    static const exo_stdio_backend_t ro = {
        ro_open, ro_read, ro_seek, ro_close, NULL, NULL, NULL, NULL,
    };

    exo_stdio_set_backend(&ro);
    errno = 0;
    CU_ASSERT_PTR_NULL(fopen("a", "w"));
    CU_ASSERT_EQUAL(errno, EROFS);
    errno = 0;
    CU_ASSERT_PTR_NULL(fopen("a", "r+"));
    CU_ASSERT_EQUAL(errno, EROFS);
    errno = 0;
    CU_ASSERT_EQUAL(mkdir("d", 0755), -1);
    CU_ASSERT_EQUAL(errno, EROFS);
    errno = 0;
    CU_ASSERT_EQUAL(remove("a"), -1);                 /* no remove/rename op */
    CU_ASSERT_EQUAL(errno, EROFS);
    errno = 0;
    CU_ASSERT_EQUAL(rename("a", "b"), -1);
    CU_ASSERT_EQUAL(errno, EROFS);
    errno = 0;
    CU_ASSERT_PTR_NULL(fopen("a", "r"));              /* reading still goes through */
    CU_ASSERT_EQUAL(errno, ENOENT);
    exo_stdio_set_backend(NULL);
}


/* ---- fwrite error and EOF bookkeeping (code review of SCRUM-42) ----------
 *
 * An in-memory backend with a write budget, so a full volume can be
 * simulated without filling one: it accepts `mem_limit` more bytes (a prefix
 * of a larger write included) and then answers -ENOSPC. -1 is unlimited. */

static uint8_t mem[4096];
static size_t  mem_pos, mem_len;
static long    mem_limit;

static int mem_open(const char *path, unsigned flags)
{
    (void)path;
    mem_pos = 0;
    if (flags & EXO_STDIO_TRUNC) mem_len = 0;
    return 1;
}
static int64_t mem_read(int h, void *b, size_t n)
{
    (void)h;
    if (n > mem_len - mem_pos) n = mem_len - mem_pos;
    memcpy(b, mem + mem_pos, n);
    mem_pos += n;
    return (int64_t)n;
}
static int64_t mem_seek(int h, int64_t off, int whence)
{
    (void)h;
    if (whence == SEEK_END) off += (int64_t)mem_len;
    else if (whence == SEEK_CUR) off += (int64_t)mem_pos;
    mem_pos = (size_t)off;
    return off;
}
static int64_t mem_write(int h, const void *b, size_t n)
{
    (void)h;
    if (mem_limit == 0) return -ENOSPC;
    if (mem_limit > 0 && n > (size_t)mem_limit) n = (size_t)mem_limit;
    memcpy(mem + mem_pos, b, n);
    mem_pos += n;
    if (mem_pos > mem_len) mem_len = mem_pos;
    if (mem_limit > 0) mem_limit -= (long)n;
    return (int64_t)n;
}
static int mem_close(int h) { (void)h; return 0; }

static const exo_stdio_backend_t mem_backend = {
    mem_open, mem_read, mem_seek, mem_close, mem_write, NULL, NULL, NULL,
};

static void mem_reset(long limit)
{
    mem_pos = mem_len = 0;
    mem_limit = limit;
    exo_stdio_set_backend(&mem_backend);
}

static void test_failed_flush_keeps_pos_within_size(void)
{
    uint8_t buf[16];
    FILE   *f;
    unsigned i;
    size_t  put = 0;

    mem_reset(0);                                    /* every write fails */
    f = fopen("a", "w+");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        for (i = 0; i < 512; i++)                    /* the 512th fills the block */
            put += fwrite("q", 1, 1, f);
        CU_ASSERT_EQUAL(put, (size_t)511);           /* the lost byte is not counted */
        CU_ASSERT_EQUAL(errno, ENOSPC);

        /* pos must not be past size: fread would see a huge `avail`. */
        CU_ASSERT_EQUAL(fread(buf, 1, sizeof buf, f), (size_t)0);
        CU_ASSERT_EQUAL(fseek(f, 0, SEEK_END), 0);
        CU_ASSERT_EQUAL(ftell(f), 512L);
        fclose(f);
    }
    exo_stdio_set_backend(NULL);
}

static void test_partial_direct_write_counts_the_prefix(void)
{
    static uint8_t big[2000];
    FILE *f;

    memset(big, 'b', sizeof big);
    mem_reset(700);                                  /* room for 700 bytes */
    f = fopen("a", "wb");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        errno = 0;
        CU_ASSERT_EQUAL(fwrite(big, 1, sizeof big, f), (size_t)700);
        CU_ASSERT_EQUAL(errno, ENOSPC);
        CU_ASSERT_EQUAL(ftell(f), 700L);             /* follows the file */
        CU_ASSERT_EQUAL(mem_len, (size_t)700);
        CU_ASSERT_EQUAL(fseek(f, 0, SEEK_END), 0);
        CU_ASSERT_EQUAL(ftell(f), 700L);

        /* Whole elements: 700 / 300 is two, not 2.33. */
        mem_reset(700);
        fclose(f);
        f = fopen("a", "wb");
        CU_ASSERT_PTR_NOT_NULL(f);
        if (f != NULL) {
            CU_ASSERT_EQUAL(fwrite(big, 300, 6, f), (size_t)2);
            fclose(f);
        }
    }
    exo_stdio_set_backend(NULL);
}

static void test_fwrite_clears_eof(void)
{
    uint8_t buf[8];
    FILE   *f;

    mem_reset(-1);
    f = fopen("a", "w+");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        CU_ASSERT_EQUAL(fread(buf, 1, sizeof buf, f), (size_t)0);
        CU_ASSERT_TRUE(feof(f) != 0);
        CU_ASSERT_EQUAL(fwrite("hello", 1, 5, f), (size_t)5);
        CU_ASSERT_FALSE(feof(f));
        CU_ASSERT_EQUAL(fseek(f, 0, SEEK_SET), 0);
        CU_ASSERT_EQUAL(fread(buf, 1, sizeof buf, f), (size_t)5);
        fclose(f);
    }
    exo_stdio_set_backend(NULL);
}

/* ---- Doom's two volumes (SCRUM-267) ------------------------------------
 *
 * Two small volumes inside the 8192..16383 range this suite owns: "A" is the
 * private one (the default volume, what fopen writes to), "B" the shared
 * read-only one the IWAD lives on (the fallback for reads). */

#define A_BASE 8192u
#define B_BASE 10240u
#define SMALL_SECTORS 2048u

static void put_file(exofs_vol_t *v, const char *path, const char *body)
{
    exofs_file_t f;
    uint32_t     n = (uint32_t)strlen(body);

    CU_ASSERT_EQUAL(exofs_open(v, path, EXOFS_O_WRITE | EXOFS_O_CREATE, &f), 0);
    CU_ASSERT_EQUAL(exofs_write(&f, body, n), (int64_t)n);
    CU_ASSERT_EQUAL(exofs_close(&f), 0);
}

static uint32_t vol_file_size(exofs_vol_t *v, const char *path)
{
    exofs_stat_t st;

    return exofs_stat(v, path, &st) == 0 ? st.size : 0xFFFFFFFFu;
}

static void test_blank_sector_detection(void)
{
    uint8_t sec[512];

    memset(sec, 0, sizeof sec);
    CU_ASSERT_TRUE(exofs_sector_is_blank(sec, sizeof sec));
    CU_ASSERT_TRUE(exofs_sector_is_blank(sec, 0));
    sec[511] = 1;                          /* the last byte counts too */
    CU_ASSERT_FALSE(exofs_sector_is_blank(sec, sizeof sec));
    sec[511] = 0;
    sec[0] = 0x45;
    CU_ASSERT_FALSE(exofs_sector_is_blank(sec, sizeof sec));
    CU_ASSERT_FALSE(exofs_sector_is_blank(NULL, 512));
}

/* First launch formats, every later one mounts and finds what was there. */
static void test_attach_formats_a_blank_slot_once(void)
{
    exofs_vol_t *v = NULL;
    uint8_t     *sec;

    if (!drive_present()) return;
    exo_file_stdio_remove();
    exo_file_close_all();
    exofs_unmount_all();

    /* A slot nobody has written: whatever an earlier test left is wiped. Disk
     * buffers must be in the LibOS window, hence the heap. */
    sec = libos_heap_alloc(512);
    CU_ASSERT_PTR_NOT_NULL(sec);
    if (sec == NULL) return;
    memset(sec, 0, 512);
    CU_ASSERT_EQUAL(exofs_bdev_write(0, B_BASE, sec, 1), 0);
    libos_heap_free(sec);

    CU_ASSERT_EQUAL(exofs_attach(0, B_BASE, SMALL_SECTORS, &v),
                    EXOFS_ATTACH_FORMATTED);
    CU_ASSERT_PTR_NOT_NULL(v);
    if (v == NULL) return;
    put_file(v, "/slot0.dsg", "saved");
    exofs_unmount(v);

    v = NULL;
    CU_ASSERT_EQUAL(exofs_attach(0, B_BASE, SMALL_SECTORS, &v),
                    EXOFS_ATTACH_MOUNTED);
    CU_ASSERT_PTR_NOT_NULL(v);
    if (v != NULL) {
        CU_ASSERT_EQUAL(vol_file_size(v, "/slot0.dsg"), 5u);   /* kept */
        exofs_unmount(v);
    }
}

/* A volume that will not mount but is not blank is somebody's data. */
static void test_attach_never_formats_over_data(void)
{
    uint8_t     *junk, *back;
    exofs_vol_t *v = NULL;

    if (!drive_present()) return;
    exo_file_stdio_remove();
    exo_file_close_all();
    exofs_unmount_all();

    junk = libos_heap_alloc(512);
    back = libos_heap_alloc(512);
    CU_ASSERT_PTR_NOT_NULL(junk);
    CU_ASSERT_PTR_NOT_NULL(back);
    if (junk == NULL || back == NULL) {
        libos_heap_free(junk);
        libos_heap_free(back);
        return;
    }
    memset(junk, 0x5a, 512);
    CU_ASSERT_EQUAL(exofs_bdev_write(0, B_BASE, junk, 1), 0);

    CU_ASSERT_EQUAL(exofs_attach(0, B_BASE, SMALL_SECTORS, &v), -EXO_EINVAL);
    CU_ASSERT_PTR_NULL(v);

    memset(back, 0, 512);
    CU_ASSERT_EQUAL(exofs_bdev_read(0, B_BASE, back, 1), 0);
    CU_ASSERT_EQUAL(memcmp(back, junk, 512), 0);   /* untouched */
    libos_heap_free(junk);
    libos_heap_free(back);

    /* Already mounted is its own answer, not a reason to write. */
    CU_ASSERT_EQUAL(exofs_format(0, B_BASE, SMALL_SECTORS), 0);
    CU_ASSERT_EQUAL(exofs_mount(0, B_BASE, &v), 0);
    exofs_vol_t *again = NULL;
    CU_ASSERT_EQUAL(exofs_attach(0, B_BASE, SMALL_SECTORS, &again), -EXO_EBUSY);
    exofs_unmount(v);
}

/* The IWAD is on the shared volume; writes and saves go to the private one. */
static void test_fallback_reads_shared_volume_writes_private(void)
{
    exofs_vol_t *a = NULL, *b = NULL;
    char         got[16];
    FILE        *f;

    if (!drive_present()) return;
    exo_file_stdio_remove();
    exo_file_close_all();
    exofs_unmount_all();

    CU_ASSERT_EQUAL(exofs_format(0, A_BASE, SMALL_SECTORS), 0);
    CU_ASSERT_EQUAL(exofs_format(0, B_BASE, SMALL_SECTORS), 0);
    CU_ASSERT_EQUAL(exofs_mount(0, A_BASE, &a), 0);
    CU_ASSERT_EQUAL(exofs_mount(0, B_BASE, &b), 0);
    if (a == NULL || b == NULL) { exofs_unmount_all(); return; }
    put_file(a, "/default.cfg", "mine");
    put_file(b, "/doom2.wad", "IWAD-from-b");

    exo_file_set_volume(a);
    exo_file_stdio_install();
    exo_file_stdio_set_fallback(b);

    /* In neither -> ENOENT; only on the private one -> read; only on the
     * shared one -> read through. */
    errno = 0;
    CU_ASSERT_PTR_NULL(fopen("nothing.dsg", "rb"));
    CU_ASSERT_EQUAL(errno, ENOENT);
    memset(got, 0, sizeof got);
    f = fopen("./default.cfg", "rb");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        CU_ASSERT_EQUAL(fread(got, 1, sizeof got, f), (size_t)4);
        fclose(f);
    }
    memset(got, 0, sizeof got);
    f = fopen("doom2.wad", "rb");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        CU_ASSERT_EQUAL(fread(got, 1, sizeof got, f), (size_t)11);
        CU_ASSERT_EQUAL(memcmp(got, "IWAD-from-b", 11), 0);
        CU_ASSERT_EQUAL(fseek(f, 0, SEEK_END), 0);        /* a live stream */
        CU_ASSERT_EQUAL(ftell(f), 11L);
        fclose(f);
    }
    /* The default is back where it was, so the next write goes to A. */
    CU_ASSERT_TRUE(exo_file_volume() == a);

    /* Writing a name only the shared volume has creates it on the private
     * one and leaves the shared one exactly as it was. */
    f = fopen("doom2.wad", "wb");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        CU_ASSERT_EQUAL(fwrite("mine!", 1, 5, f), (size_t)5);
        CU_ASSERT_EQUAL(fclose(f), 0);
    }
    CU_ASSERT_EQUAL(vol_file_size(a, "/doom2.wad"), 5u);
    CU_ASSERT_EQUAL(vol_file_size(b, "/doom2.wad"), 11u);
    /* Present on both: the private copy wins. */
    f = fopen("doom2.wad", "rb");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != NULL) {
        CU_ASSERT_EQUAL(fseek(f, 0, SEEK_END), 0);
        CU_ASSERT_EQUAL(ftell(f), 5L);
        fclose(f);
    }
    /* A name only the shared volume has: readable, but r+ is a write mode
     * and must not fall through to it. */
    put_file(b, "/only-b.txt", "x");
    errno = 0;
    CU_ASSERT_PTR_NULL(fopen("only-b.txt", "r+"));
    CU_ASSERT_EQUAL(errno, ENOENT);

    /* A fallback that has been unmounted reads as none. */
    CU_ASSERT_PTR_NOT_NULL((f = fopen("only-b.txt", "rb")));
    if (f != NULL) fclose(f);
    exofs_unmount(b);
    errno = 0;
    CU_ASSERT_PTR_NULL(fopen("only-b.txt", "rb"));
    CU_ASSERT_EQUAL(errno, ENOENT);

    exo_file_stdio_remove();
    exo_file_close_all();
    exofs_unmount_all();
}

/* exo_file_open_on() (SCRUM-267): open on a named volume, default untouched. */
static void test_open_on_names_the_volume_not_the_default(void)
{
    exofs_vol_t *a = NULL, *b = NULL;
    char         got[16];
    int          fd;

    if (!drive_present()) return;
    exo_file_stdio_remove();
    exo_file_close_all();
    exofs_unmount_all();

    CU_ASSERT_EQUAL(exofs_format(0, A_BASE, SMALL_SECTORS), 0);
    CU_ASSERT_EQUAL(exofs_format(0, B_BASE, SMALL_SECTORS), 0);
    CU_ASSERT_EQUAL(exofs_mount(0, A_BASE, &a), 0);
    CU_ASSERT_EQUAL(exofs_mount(0, B_BASE, &b), 0);
    if (a == NULL || b == NULL) { exofs_unmount_all(); return; }
    put_file(a, "/on-a.txt", "from-a");
    put_file(b, "/on-b.txt", "from-b");
    exo_file_set_volume(a);

    /* A file only B has: invisible to the default, readable through open_on. */
    CU_ASSERT_EQUAL(exo_file_open("/on-b.txt", EXO_FILE_O_READ), -EXO_ENOENT);
    fd = exo_file_open_on(b, "/on-b.txt", EXO_FILE_O_READ);
    CU_ASSERT_TRUE(fd >= 0);
    if (fd >= 0) {
        memset(got, 0, sizeof got);
        CU_ASSERT_EQUAL(exo_file_read(fd, got, sizeof got), (int64_t)6);
        CU_ASSERT_EQUAL(memcmp(got, "from-b", 6), 0);
        CU_ASSERT_EQUAL(exo_file_close(fd), 0);
    }
    CU_ASSERT_TRUE(exo_file_volume() == a);             /* never moved */

    /* The plain call is the same thing on the default volume. */
    fd = exo_file_open("/on-a.txt", EXO_FILE_O_READ);
    CU_ASSERT_TRUE(fd >= 0);
    if (fd >= 0) exo_file_close(fd);

    /* No volume, or one that is gone, is -EXO_EINVAL and uses no descriptor. */
    CU_ASSERT_EQUAL(exo_file_open_on(NULL, "/on-b.txt", EXO_FILE_O_READ),
                    -EXO_EINVAL);
    exofs_unmount(b);
    CU_ASSERT_EQUAL(exo_file_open_on(b, "/on-b.txt", EXO_FILE_O_READ),
                    -EXO_EINVAL);

    exo_file_close_all();
    exofs_unmount_all();
}

void suite_stdio_exofs_tests(CU_pSuite s)
{
    CU_add_test(s, "exo_file_open_on names the volume, not the default",
                test_open_on_names_the_volume_not_the_default);
    CU_add_test(s, "exofs_attach: blank-sector detection",
                test_blank_sector_detection);
    CU_add_test(s, "exofs_attach formats a blank slot once, then mounts it",
                test_attach_formats_a_blank_slot_once);
    CU_add_test(s, "exofs_attach never formats over data it cannot mount",
                test_attach_never_formats_over_data);
    CU_add_test(s, "reads fall through to the shared volume, writes stay private",
                test_fallback_reads_shared_volume_writes_private);
    CU_add_test(s, "dg_check_iwad reads the header off ExoFS", test_check_iwad);
    CU_add_test(s, "fread a whole file off ExoFS", test_read_whole_file);
    CU_add_test(s, "fseek then fread at random offsets",
                test_seek_and_random_read);
    CU_add_test(s, "relative, ./ and absolute paths", test_path_forms);
    CU_add_test(s, "missing file ENOENT, bad mode EINVAL",
                test_missing_and_write_modes);
    CU_add_test(s, "fscanf reads a config line off ExoFS",
                test_fscanf_from_file);
    CU_add_test(s, "descriptor exhaustion and reuse",
                test_table_full_and_close_frees_descriptor);
    CU_add_test(s, "removing the backend restores blob lookup",
                test_remove_restores_blob_behaviour);
    CU_add_test(s, "fwrite round trip across block boundaries",
                test_write_round_trip);
    CU_add_test(s, "fopen modes w, a, r+, w+ and read-only streams",
                test_write_modes);
    CU_add_test(s, "write buffering, ftell/fseek/fflush with pending bytes",
                test_write_buffering_and_position);
    CU_add_test(s, "fprintf a config file and fscanf it back",
                test_fprintf_config_round_trip);
    CU_add_test(s, "mkdir over ExoFS and a file inside it",
                test_mkdir_over_exofs);
    CU_add_test(s, "savegame rotation: remove then rename over ExoFS",
                test_save_rotation_remove_rename);
    CU_add_test(s, "a backend without write stays read-only",
                test_backend_without_write_is_read_only);
    CU_add_test(s, "failed flush leaves pos within size",
                test_failed_flush_keeps_pos_within_size);
    CU_add_test(s, "partial write on a full volume counts the prefix",
                test_partial_direct_write_counts_the_prefix);
    CU_add_test(s, "fwrite clears a pending EOF",
                test_fwrite_clears_eof);
}
