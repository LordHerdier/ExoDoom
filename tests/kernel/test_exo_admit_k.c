/*
 * test_exo_admit_k.c -- SCRUM-232's admission check (src/exo_admit.h).
 *
 * Pure logic over hand-built manifest/report structs, so every boundary is
 * driven exactly: min-1, min, recommended-1, recommended. No syscall, no
 * device; registered with (NULL, NULL) like "exo_pkg".
 */

#include "kunit.h"

#include "exo_admit.h"

#include <stdint.h>

static exo_pkg_header_t mk_hdr(uint32_t min, uint32_t rec, uint32_t voices,
                               uint32_t want)
{
    exo_pkg_header_t h = {0};
    h.magic = EXO_PKG_MAGIC;
    h.version = EXO_PKG_VERSION;
    h.stack_pages = 1;
    h.min_pages = min;
    h.recommended_pages = rec;
    h.sound_voices = voices;
    h.want_flags = want;
    return h;
}

static exo_resstat_t mk_res(uint32_t pages, uint32_t ctx, uint32_t voices,
                            uint32_t fb_held)
{
    exo_resstat_t r = {0};
    r.free_pages = pages;
    r.free_contexts = ctx;
    r.sound_voices_free = voices;
    r.fb_held = fb_held;
    return r;
}

static void test_plenty_is_granted(void)
{
    exo_pkg_header_t h = mk_hdr(50, 100, 8, EXO_PKG_WANT_FRAMEBUFFER);
    exo_resstat_t r = mk_res(500, 2, 8, 0);
    exo_admit_t a;
    exo_admit(&h, &r, &a);
    CU_ASSERT_EQUAL(a.verdict, EXO_ADMIT_GRANT);
    CU_ASSERT_EQUAL(a.reason, EXO_ADMIT_NONE);
    CU_ASSERT_EQUAL(a.fb_displaces, 0);
}

static void test_page_boundaries(void)
{
    exo_pkg_header_t h = mk_hdr(97, 150, 0, 0);
    exo_admit_t a;

    exo_resstat_t r = mk_res(96, 1, 0, 0);          /* min - 1 */
    exo_admit(&h, &r, &a);
    CU_ASSERT_EQUAL(a.verdict, EXO_ADMIT_REFUSED);
    CU_ASSERT_EQUAL(a.reason, EXO_ADMIT_PAGES);
    CU_ASSERT_EQUAL(a.needed, 97);
    CU_ASSERT_EQUAL(a.have, 96);

    r = mk_res(97, 1, 0, 0);                         /* exactly min */
    exo_admit(&h, &r, &a);
    CU_ASSERT_EQUAL(a.verdict, EXO_ADMIT_DEGRADED);
    CU_ASSERT_EQUAL(a.reason, EXO_ADMIT_PAGES);
    CU_ASSERT_EQUAL(a.needed, 150);
    CU_ASSERT_EQUAL(a.have, 97);

    r = mk_res(149, 1, 0, 0);                        /* recommended - 1 */
    exo_admit(&h, &r, &a);
    CU_ASSERT_EQUAL(a.verdict, EXO_ADMIT_DEGRADED);

    r = mk_res(150, 1, 0, 0);                        /* exactly recommended */
    exo_admit(&h, &r, &a);
    CU_ASSERT_EQUAL(a.verdict, EXO_ADMIT_GRANT);
}

static void test_no_context_slot_is_refused(void)
{
    exo_pkg_header_t h = mk_hdr(10, 20, 0, 0);
    exo_resstat_t r = mk_res(1000, 0, 8, 0);
    exo_admit_t a;
    exo_admit(&h, &r, &a);
    CU_ASSERT_EQUAL(a.verdict, EXO_ADMIT_REFUSED);
    CU_ASSERT_EQUAL(a.reason, EXO_ADMIT_CONTEXTS);
    CU_ASSERT_EQUAL(a.needed, 1);
    CU_ASSERT_EQUAL(a.have, 0);
}

static void test_contexts_reported_before_pages(void)
{
    exo_pkg_header_t h = mk_hdr(10, 20, 0, 0);
    exo_resstat_t r = mk_res(0, 0, 0, 0);
    exo_admit_t a;
    exo_admit(&h, &r, &a);
    CU_ASSERT_EQUAL(a.verdict, EXO_ADMIT_REFUSED);
    CU_ASSERT_EQUAL(a.reason, EXO_ADMIT_CONTEXTS);
}

static void test_missing_voices_degrade_not_refuse(void)
{
    exo_pkg_header_t h = mk_hdr(10, 20, 8, 0);
    exo_resstat_t r = mk_res(1000, 1, 3, 0);
    exo_admit_t a;
    exo_admit(&h, &r, &a);
    CU_ASSERT_EQUAL(a.verdict, EXO_ADMIT_DEGRADED);
    CU_ASSERT_EQUAL(a.reason, EXO_ADMIT_SOUND);
    CU_ASSERT_EQUAL(a.needed, 8);
    CU_ASSERT_EQUAL(a.have, 3);
}

static void test_pages_shortfall_outranks_sound(void)
{
    exo_pkg_header_t h = mk_hdr(10, 20, 8, 0);
    exo_resstat_t r = mk_res(15, 1, 0, 0);
    exo_admit_t a;
    exo_admit(&h, &r, &a);
    CU_ASSERT_EQUAL(a.verdict, EXO_ADMIT_DEGRADED);
    CU_ASSERT_EQUAL(a.reason, EXO_ADMIT_PAGES);
}

static void test_held_framebuffer_is_not_a_refusal(void)
{
    exo_pkg_header_t h = mk_hdr(10, 20, 0, EXO_PKG_WANT_FRAMEBUFFER);
    exo_resstat_t r = mk_res(1000, 1, 0, 1);
    exo_admit_t a;
    exo_admit(&h, &r, &a);
    CU_ASSERT_EQUAL(a.verdict, EXO_ADMIT_GRANT);
    CU_ASSERT_EQUAL(a.fb_displaces, 1);

    /* A program that does not want it displaces nobody. */
    h = mk_hdr(10, 20, 0, EXO_PKG_WANT_DISK | EXO_PKG_WANT_KEYBOARD);
    exo_admit(&h, &r, &a);
    CU_ASSERT_EQUAL(a.verdict, EXO_ADMIT_GRANT);
    CU_ASSERT_EQUAL(a.fb_displaces, 0);
}

void suite_exo_admit_tests(CU_pSuite s)
{
    CU_add_test(s, "ample resources are granted", test_plenty_is_granted);
    CU_add_test(s, "page boundaries: min-1 refuses, below recommended degrades",
                test_page_boundaries);
    CU_add_test(s, "no free context slot is refused", test_no_context_slot_is_refused);
    CU_add_test(s, "context shortfall is reported before pages",
                test_contexts_reported_before_pages);
    CU_add_test(s, "missing sound voices degrade, never refuse",
                test_missing_voices_degrade_not_refuse);
    CU_add_test(s, "page shortfall outranks a sound shortfall",
                test_pages_shortfall_outranks_sound);
    CU_add_test(s, "a held framebuffer is reported, not refused",
                test_held_framebuffer_is_not_a_refusal);
}
