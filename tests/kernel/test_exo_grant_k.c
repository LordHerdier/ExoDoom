/*
 * test_exo_grant_k.c -- SCRUM-235's launch read-out (src/exo_grant.h).
 *
 * Pure text over hand-built manifest/grant/resstat structs: every line is
 * checked against the figures that went in, and a buffer too small for the
 * text is checked for never being written past. No syscall, no device;
 * registered with (NULL, NULL) like "exo_admit".
 */

#include "kunit.h"

#include "exo_grant.h"
#include "string.h"

#include <stdint.h>

static exo_pkg_header_t mk_hdr(uint32_t want, uint32_t voices)
{
    exo_pkg_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = EXO_PKG_MAGIC;
    h.version = EXO_PKG_VERSION;
    h.min_pages = 271;
    h.recommended_pages = 287;
    h.sound_voices = voices;
    h.want_flags = want;
    return h;
}

static exo_grant_t mk_grant(uint32_t ctx, uint32_t pages)
{
    exo_grant_t g;
    memset(&g, 0, sizeof(g));
    g.ctx_id = ctx;
    g.pages = pages;
    return g;
}

static exo_resstat_t mk_res(uint32_t voices_free)
{
    exo_resstat_t r;
    memset(&r, 0, sizeof(r));
    r.sound_voices_free = voices_free;
    return r;
}

static void test_plain_program_names_every_resource(void)
{
    exo_pkg_header_t h = mk_hdr(0, 0);
    exo_grant_t g = mk_grant(4, 271);
    exo_resstat_t r = mk_res(8);
    char buf[EXO_GRANT_TEXT_MAX];

    size_t n = exo_grant_format(buf, sizeof(buf), &h, &g, &r, 0);
    CU_ASSERT_EQUAL(n, strlen(buf));
    CU_ASSERT_STRING_EQUAL(buf,
        "granted: context 4\n"
        "  pages  271 (asked min 271, rec 287)\n"
        "  fb     not asked for\n"
        "  disk   none\n"
        "  sound  none asked for\n");
}

static void test_framebuffer_line_follows_the_previous_holder(void)
{
    exo_pkg_header_t h = mk_hdr(EXO_PKG_WANT_FRAMEBUFFER, 0);
    exo_grant_t g = mk_grant(5, 100);
    exo_resstat_t r = mk_res(8);
    char buf[EXO_GRANT_TEXT_MAX];

    exo_grant_format(buf, sizeof(buf), &h, &g, &r, 0);
    CU_ASSERT_PTR_NOT_NULL(strstr(buf, "fb     binding free, program may take it\n"));

    exo_grant_format(buf, sizeof(buf), &h, &g, &r, 2);
    CU_ASSERT_PTR_NOT_NULL(strstr(buf, "fb     takes the binding from context 2\n"));
}

static void test_disk_line_reports_the_extent_or_its_absence(void)
{
    exo_pkg_header_t h = mk_hdr(EXO_PKG_WANT_DISK, 0);
    h.volume_slot = 2;
    h.volume_sectors = 64;
    exo_grant_t g = mk_grant(6, 100);
    exo_resstat_t r = mk_res(8);
    char buf[EXO_GRANT_TEXT_MAX];

    g.disk_base = 4096;
    g.disk_sectors = 64;
    exo_grant_format(buf, sizeof(buf), &h, &g, &r, 0);
    CU_ASSERT_PTR_NOT_NULL(strstr(buf, "disk   volume slot 2, 64 sectors from 4096\n"));

    /* Asked for a disk, nothing assigned: say so rather than "none". */
    g.disk_sectors = 0;
    exo_grant_format(buf, sizeof(buf), &h, &g, &r, 0);
    CU_ASSERT_PTR_NOT_NULL(strstr(buf, "disk   asked for, none assigned\n"));
}

/* SCRUM-267: a context can hold a private volume and the read-only program
 * volume at once (Doom). Neither may hide the other, and a context with only
 * the program volume must not be described as having a "volume slot". */
static void test_disk_line_names_both_volumes(void)
{
    exo_pkg_header_t h = mk_hdr(EXO_PKG_WANT_DISK | EXO_PKG_WANT_PROGRAM_RO, 0);
    h.volume_slot = 2;
    h.volume_sectors = 64;
    exo_grant_t g = mk_grant(8, 100);
    exo_resstat_t r = mk_res(8);
    char buf[EXO_GRANT_TEXT_MAX];

    g.disk_base = 4096;
    g.disk_sectors = 64;
    g.prog_base = 16384;
    g.prog_sectors = 245760;
    exo_grant_format(buf, sizeof(buf), &h, &g, &r, 0);
    CU_ASSERT_PTR_NOT_NULL(strstr(buf,
        "disk   volume slot 2, 64 sectors from 4096\n"
        "         program volume (read-only), 245760 sectors from 16384\n"));

    /* Program volume alone: no slot, and not "none assigned". */
    g.disk_base = 0;
    g.disk_sectors = 0;
    exo_grant_format(buf, sizeof(buf), &h, &g, &r, 0);
    CU_ASSERT_PTR_NOT_NULL(strstr(buf,
        "disk   program volume (read-only), 245760 sectors from 16384\n"));
    CU_ASSERT_PTR_NULL(strstr(buf, "volume slot"));
    CU_ASSERT_PTR_NULL(strstr(buf, "none assigned"));
}

static void test_sound_line_caps_at_what_is_free(void)
{
    exo_pkg_header_t h = mk_hdr(0, 4);
    exo_grant_t g = mk_grant(7, 100);
    char buf[EXO_GRANT_TEXT_MAX];

    exo_resstat_t r = mk_res(8);
    exo_grant_format(buf, sizeof(buf), &h, &g, &r, 0);
    CU_ASSERT_PTR_NOT_NULL(strstr(buf, "sound  4 of 4 voices available\n"));

    r = mk_res(1);
    exo_grant_format(buf, sizeof(buf), &h, &g, &r, 0);
    CU_ASSERT_PTR_NOT_NULL(strstr(buf, "sound  1 of 4 voices available\n"));
}

static void test_largest_numbers_fit_the_documented_maximum(void)
{
    exo_pkg_header_t h = mk_hdr(EXO_PKG_WANT_FRAMEBUFFER | EXO_PKG_WANT_DISK,
                                0xFFFFFFFFu);
    h.min_pages = 0xFFFFFFFFu;
    h.recommended_pages = 0xFFFFFFFFu;
    h.volume_slot = 0xFFFFFFFFu;
    exo_grant_t g = mk_grant(0xFFFFFFFFu, 0xFFFFFFFFu);
    g.disk_base = 0xFFFFFFFFu;
    g.disk_sectors = 0xFFFFFFFFu;
    g.prog_base = 0xFFFFFFFFu;       /* both volumes: the longest disk line */
    g.prog_sectors = 0xFFFFFFFFu;
    exo_resstat_t r = mk_res(0xFFFFFFFFu);
    char buf[EXO_GRANT_TEXT_MAX];

    size_t n = exo_grant_format(buf, sizeof(buf), &h, &g, &r, 0xFFFFFFFFu);
    CU_ASSERT_EQUAL(n, strlen(buf));
    CU_ASSERT_TRUE(n < EXO_GRANT_TEXT_MAX);
    /* Nothing was cut: the last line is intact. */
    CU_ASSERT_PTR_NOT_NULL(strstr(buf, "voices available\n"));
    CU_ASSERT_PTR_NOT_NULL(strstr(buf, "4294967295"));
}

static void test_a_short_buffer_is_never_overrun(void)
{
    exo_pkg_header_t h = mk_hdr(0, 0);
    exo_grant_t g = mk_grant(4, 271);
    exo_resstat_t r = mk_res(8);

    /* Canary bytes either side of a buffer of every size up to the full text. */
    for (size_t cap = 1; cap < 80; cap++) {
        char arena[EXO_GRANT_TEXT_MAX + 2];
        memset(arena, 0x5A, sizeof(arena));
        char *buf = arena + 1;

        size_t n = exo_grant_format(buf, cap, &h, &g, &r, 0);
        CU_ASSERT_TRUE(n < cap);
        CU_ASSERT_EQUAL(buf[n], '\0');
        CU_ASSERT_EQUAL((unsigned char)arena[0], 0x5Au);
        CU_ASSERT_EQUAL((unsigned char)buf[cap], 0x5Au);
    }

    CU_ASSERT_EQUAL(exo_grant_format((char *)0, 0, &h, &g, &r, 0), 0);
}

void suite_exo_grant_tests(CU_pSuite s)
{
    CU_add_test(s, "a plain program: every resource named",
                test_plain_program_names_every_resource);
    CU_add_test(s, "the fb line follows the previous holder",
                test_framebuffer_line_follows_the_previous_holder);
    CU_add_test(s, "the disk line reports the extent or its absence",
                test_disk_line_reports_the_extent_or_its_absence);
    CU_add_test(s, "the disk line names a private and a program volume",
                test_disk_line_names_both_volumes);
    CU_add_test(s, "the sound line caps at what is free",
                test_sound_line_caps_at_what_is_free);
    CU_add_test(s, "the largest numbers fit EXO_GRANT_TEXT_MAX",
                test_largest_numbers_fit_the_documented_maximum);
    CU_add_test(s, "a short buffer is never overrun",
                test_a_short_buffer_is_never_overrun);
}
