/*
 * test_stdio_exofs_k.c — fopen/fread/fseek/ftell/fclose over ExoFS
 * (SCRUM-42, SCRUM-75).
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

#include "libos_fs/exofs.h"
#include "libos_fs/exo_file.h"
#include "libos_fs/exo_file_stdio.h"
#include "doom_wad.h"
#include "doomgeneric_exo.h"

#include <stdint.h>
#include <stddef.h>

#define EXOFS_TEST_BASE_LBA 8192u
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
    exofs_unmount();
    if (exofs_format(0, EXOFS_TEST_BASE_LBA, VOL_SECTORS) != 0) return 0;
    if (exofs_mount(0, EXOFS_TEST_BASE_LBA) != 0) return 0;

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
    exofs_unmount();
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
    CU_ASSERT_PTR_NULL(fopen("cfg.txt", "w"));
    CU_ASSERT_EQUAL(errno, EROFS);
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

void suite_stdio_exofs_tests(CU_pSuite s)
{
    CU_add_test(s, "dg_check_iwad reads the header off ExoFS", test_check_iwad);
    CU_add_test(s, "fread a whole file off ExoFS", test_read_whole_file);
    CU_add_test(s, "fseek then fread at random offsets",
                test_seek_and_random_read);
    CU_add_test(s, "relative, ./ and absolute paths", test_path_forms);
    CU_add_test(s, "missing file ENOENT, write mode EROFS",
                test_missing_and_write_modes);
    CU_add_test(s, "fscanf reads a config line off ExoFS",
                test_fscanf_from_file);
    CU_add_test(s, "descriptor exhaustion and reuse",
                test_table_full_and_close_frees_descriptor);
    CU_add_test(s, "removing the backend restores blob lookup",
                test_remove_restores_blob_behaviour);
}
