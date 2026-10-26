/*
 * test_fb_layout_k.c — the split-screen pane table (SCRUM-197) and its effect
 * on fb_shadow.c's buffer sizing.
 *
 * fb_layout.c never touches the framebuffer's memory, only its published
 * geometry, so a synthetic geometry with a made-up base address is enough
 * here; the compositor suite (test_fb_compositor_k.c) is the one that needs a
 * real RAM-backed "screen".
 */

#include "kunit.h"
#include "fb_layout.h"
#include "fb_shadow.h"
#include "fb_binding.h"
#include "page_alloc.h"

#include <stdint.h>

#define PANE_A ((page_owner_t)(PAGE_OWNER_LIBOS + 10))
#define PANE_B ((page_owner_t)(PAGE_OWNER_LIBOS + 11))
#define PANE_C ((page_owner_t)(PAGE_OWNER_LIBOS + 12))

#define TEST_FB_BASE   0x00000000e4000000ull
#define TEST_FB_WIDTH  64u
#define TEST_FB_HEIGHT 16u
#define TEST_FB_PITCH  256u   /* width * 4 */
#define TEST_FB_BPP    32u

static fb_geometry_t boot_geometry;
static int           boot_had_fb;

int fb_layout_suite_init(void)
{
    const fb_geometry_t *g = fb_binding_geometry();

    boot_had_fb = (g != NULL);
    if (boot_had_fb)
        boot_geometry = *g;

    fb_layout_reset();
    return 0;
}

int fb_layout_suite_cleanup(void)
{
    fb_shadow_release(PANE_A);
    fb_shadow_release(PANE_B);
    fb_shadow_release(PANE_C);
    fb_layout_reset();

    fb_binding_init(boot_had_fb ? &boot_geometry : (const fb_geometry_t *)0);
    return 0;
}

static void install_test_fb(void)
{
    fb_geometry_t g = {
        .phys_addr = TEST_FB_BASE,
        .width     = TEST_FB_WIDTH,
        .height    = TEST_FB_HEIGHT,
        .pitch     = TEST_FB_PITCH,
        .bpp       = TEST_FB_BPP,
    };

    CU_ASSERT_EQUAL(fb_binding_init(&g), FB_BIND_OK);
}

static void test_set_get_clear(void)
{
    install_test_fb();
    fb_layout_reset();

    fb_rect_t r = { .x = 4, .y = 2, .w = 20, .h = 8 };
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &r), FB_LAYOUT_OK);
    CU_ASSERT_EQUAL(fb_layout_count(), 1);

    fb_rect_t got;
    CU_ASSERT_EQUAL(fb_layout_get(PANE_A, &got), 0);
    CU_ASSERT_EQUAL(got.x, 4u);
    CU_ASSERT_EQUAL(got.y, 2u);
    CU_ASSERT_EQUAL(got.w, 20u);
    CU_ASSERT_EQUAL(got.h, 8u);

    CU_ASSERT_NOT_EQUAL(fb_layout_get(PANE_B, &got), 0);

    fb_layout_clear(PANE_A);
    CU_ASSERT_EQUAL(fb_layout_count(), 0);
    CU_ASSERT_NOT_EQUAL(fb_layout_get(PANE_A, &got), 0);

    /* Clearing a context with no pane is a no-op. */
    fb_layout_clear(PANE_A);
    CU_ASSERT_EQUAL(fb_layout_count(), 0);
}

/* The rect must be non-empty and entirely on the screen; a rejected call
 * changes nothing. */
static void test_set_validates_the_rect(void)
{
    install_test_fb();
    fb_layout_reset();

    fb_rect_t zero_w = { 0, 0, 0, 4 };
    fb_rect_t zero_h = { 0, 0, 4, 0 };
    fb_rect_t past_x = { 60, 0, 8, 4 };    /* 60 + 8 > 64  */
    fb_rect_t past_y = { 0, 14, 4, 4 };    /* 14 + 4 > 16  */
    fb_rect_t wraps  = { 0xFFFFFFF0u, 0, 0x20u, 4 };
    fb_rect_t whole  = { 0, 0, TEST_FB_WIDTH, TEST_FB_HEIGHT };

    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &zero_w), FB_LAYOUT_EINVAL);
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &zero_h), FB_LAYOUT_EINVAL);
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &past_x), FB_LAYOUT_EINVAL);
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &past_y), FB_LAYOUT_EINVAL);
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &wraps), FB_LAYOUT_EINVAL);
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, (const fb_rect_t *)0),
                    FB_LAYOUT_EINVAL);
    CU_ASSERT_EQUAL(fb_layout_count(), 0);

    /* Exactly the whole screen is fine. */
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &whole), FB_LAYOUT_OK);
}

static void test_headless_rejects_every_pane(void)
{
    fb_layout_reset();
    CU_ASSERT_EQUAL(fb_binding_init((const fb_geometry_t *)0), FB_BIND_ENODEV);

    fb_rect_t r = { 0, 0, 4, 4 };
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &r), FB_LAYOUT_EINVAL);
    CU_ASSERT_EQUAL(fb_layout_count(), 0);
}

/* One pane per context: setting again moves it and keeps its draw slot. */
static void test_set_again_moves_the_pane(void)
{
    install_test_fb();
    fb_layout_reset();

    fb_rect_t a1 = { 0, 0, 8, 8 }, b = { 8, 0, 8, 8 }, a2 = { 16, 4, 8, 8 };
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &a1), FB_LAYOUT_OK);
    CU_ASSERT_EQUAL(fb_layout_set(PANE_B, &b), FB_LAYOUT_OK);
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &a2), FB_LAYOUT_OK);
    CU_ASSERT_EQUAL(fb_layout_count(), 2);

    page_owner_t who;
    fb_rect_t    r;
    CU_ASSERT_EQUAL(fb_layout_pane(0, &who, &r), 0);
    CU_ASSERT_EQUAL(who, PANE_A);
    CU_ASSERT_EQUAL(r.x, 16u);
    CU_ASSERT_EQUAL(fb_layout_pane(1, &who, &r), 0);
    CU_ASSERT_EQUAL(who, PANE_B);
    CU_ASSERT_NOT_EQUAL(fb_layout_pane(2, &who, &r), 0);
    CU_ASSERT_NOT_EQUAL(fb_layout_pane(-1, &who, &r), 0);
}

static void test_table_full(void)
{
    install_test_fb();
    fb_layout_reset();

    fb_rect_t r = { 0, 0, 4, 4 };
    for (int i = 0; i < FB_LAYOUT_MAX_PANES; i++) {
        CU_ASSERT_EQUAL(
            fb_layout_set((page_owner_t)(PAGE_OWNER_LIBOS + 20 + i), &r),
            FB_LAYOUT_OK);
    }
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &r), FB_LAYOUT_ENOSPC);
    CU_ASSERT_EQUAL(fb_layout_count(), FB_LAYOUT_MAX_PANES);

    /* A full table can still move a pane it already has. */
    CU_ASSERT_EQUAL(fb_layout_set((page_owner_t)(PAGE_OWNER_LIBOS + 20), &r),
                    FB_LAYOUT_OK);

    fb_layout_reset();
    CU_ASSERT_EQUAL(fb_layout_count(), 0);
}

static void test_generation_bumps_on_every_change(void)
{
    install_test_fb();
    fb_layout_reset();

    fb_rect_t r = { 0, 0, 4, 4 };
    uint32_t g0 = fb_layout_generation();
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &r), FB_LAYOUT_OK);
    uint32_t g1 = fb_layout_generation();
    CU_ASSERT_NOT_EQUAL(g1, g0);

    fb_layout_clear(PANE_A);
    CU_ASSERT_NOT_EQUAL(fb_layout_generation(), g1);
}

/* fb_shadow_acquire() sizes the buffer to the pane, tightly pitched. */
static void test_shadow_is_pane_sized(void)
{
    install_test_fb();
    fb_layout_reset();

    fb_rect_t r = { 8, 4, 16, 8 };
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &r), FB_LAYOUT_OK);

    exo_fb_info_t info;
    CU_ASSERT_EQUAL(fb_shadow_acquire(PANE_A, &info), FB_SHADOW_OK);
    CU_ASSERT_EQUAL(info.width,  16u);
    CU_ASSERT_EQUAL(info.height, 8u);
    CU_ASSERT_EQUAL(info.pitch,  16u * 4u);
    CU_ASSERT_EQUAL(info.bpp,    TEST_FB_BPP);

    fb_shadow_info_t si;
    CU_ASSERT_EQUAL(fb_shadow_lookup_info(PANE_A, &si), 0);
    CU_ASSERT_EQUAL(si.phys_base, info.phys_addr);
    CU_ASSERT_EQUAL(si.width,  16u);
    CU_ASSERT_EQUAL(si.height, 8u);
    CU_ASSERT_EQUAL(si.pitch,  64u);

    /* A context with no pane still gets the whole screen. */
    exo_fb_info_t full;
    CU_ASSERT_EQUAL(fb_shadow_acquire(PANE_B, &full), FB_SHADOW_OK);
    CU_ASSERT_EQUAL(full.width,  TEST_FB_WIDTH);
    CU_ASSERT_EQUAL(full.height, TEST_FB_HEIGHT);
    CU_ASSERT_EQUAL(full.pitch,  TEST_FB_PITCH);

    fb_shadow_release(PANE_A);
    fb_shadow_release(PANE_B);
}

/* A re-acquire reports the buffer that exists, even if the pane has moved. */
static void test_reacquire_keeps_the_original_size(void)
{
    install_test_fb();
    fb_layout_reset();

    fb_rect_t r1 = { 0, 0, 16, 8 }, r2 = { 0, 0, 32, 16 };
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &r1), FB_LAYOUT_OK);

    exo_fb_info_t first, second;
    CU_ASSERT_EQUAL(fb_shadow_acquire(PANE_A, &first), FB_SHADOW_OK);
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &r2), FB_LAYOUT_OK);
    CU_ASSERT_EQUAL(fb_shadow_acquire(PANE_A, &second), FB_SHADOW_OK);

    CU_ASSERT_EQUAL(second.phys_addr, first.phys_addr);
    CU_ASSERT_EQUAL(second.width,  16u);
    CU_ASSERT_EQUAL(second.height, 8u);

    fb_shadow_release(PANE_A);
}

/* The pane goes with the context, acquired or not. */
static void test_release_drops_the_pane(void)
{
    install_test_fb();
    fb_layout_reset();

    fb_rect_t r = { 0, 0, 8, 8 };
    CU_ASSERT_EQUAL(fb_layout_set(PANE_A, &r), FB_LAYOUT_OK);
    CU_ASSERT_EQUAL(fb_layout_set(PANE_B, &r), FB_LAYOUT_OK);

    exo_fb_info_t info;
    CU_ASSERT_EQUAL(fb_shadow_acquire(PANE_A, &info), FB_SHADOW_OK);

    fb_shadow_release(PANE_A);   /* had a shadow */
    fb_shadow_release(PANE_B);   /* never acquired */
    CU_ASSERT_EQUAL(fb_layout_count(), 0);
    CU_ASSERT_EQUAL(page_owner((void *)(uintptr_t)info.phys_addr),
                    PAGE_OWNER_FREE);
}

void suite_fb_layout_tests(CU_pSuite s)
{
    CU_add_test(s, "set, get and clear", test_set_get_clear);
    CU_add_test(s, "set validates the rect", test_set_validates_the_rect);
    CU_add_test(s, "headless rejects every pane",
                test_headless_rejects_every_pane);
    CU_add_test(s, "setting again moves the pane",
                test_set_again_moves_the_pane);
    CU_add_test(s, "table full", test_table_full);
    CU_add_test(s, "generation bumps on every change",
                test_generation_bumps_on_every_change);
    CU_add_test(s, "shadow is pane sized", test_shadow_is_pane_sized);
    CU_add_test(s, "reacquire keeps the original size",
                test_reacquire_keeps_the_original_size);
    CU_add_test(s, "release drops the pane", test_release_drops_the_pane);
}
