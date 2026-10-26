/*
 * test_fb_compositor_k.c — fb_compositor_tick()'s split mode (SCRUM-197).
 *
 * The "screen" is ordinary RAM from the PMM so its bytes can be checked, the
 * same trick test_fb_shadow_k.c's seed test uses. The panes' contexts are
 * made-up owner ids: the compositor only asks context_current() which mode
 * applies, and that is set directly here.
 */

#include "kunit.h"
#include "fb_compositor.h"
#include "fb_layout.h"
#include "fb_shadow.h"
#include "fb_binding.h"
#include "context.h"
#include "page_alloc.h"

#include <stdint.h>

#define PANE_LEFT  ((page_owner_t)(PAGE_OWNER_LIBOS + 30))
#define PANE_RIGHT ((page_owner_t)(PAGE_OWNER_LIBOS + 31))
#define NO_PANE    ((page_owner_t)(PAGE_OWNER_LIBOS + 32))
#define SCREEN_OWN ((page_owner_t)(PAGE_OWNER_LIBOS + 33))

/* 16x8 at 4 bytes per pixel, one page, pitch == width * 4. */
#define SCR_W     16u
#define SCR_H     8u
#define SCR_PITCH 64u
#define SCR_BPP   32u
#define BPP_BYTES 4u

static fb_geometry_t boot_geometry;
static int           boot_had_fb;
static page_owner_t  boot_current;
static uint8_t      *screen;

int fb_compositor_suite_init(void)
{
    const fb_geometry_t *g = fb_binding_geometry();

    boot_had_fb = (g != NULL);
    if (boot_had_fb)
        boot_geometry = *g;
    boot_current = context_current();

    screen = alloc_pages_contig_owned(SCREEN_OWN, 1);
    if (screen == NULL)
        return 1;

    fb_layout_reset();
    return 0;
}

int fb_compositor_suite_cleanup(void)
{
    fb_shadow_release(PANE_LEFT);
    fb_shadow_release(PANE_RIGHT);
    fb_shadow_release(NO_PANE);
    fb_layout_reset();
    context_set_current(boot_current);

    if (screen != NULL)
        free_page_owned(screen, SCREEN_OWN);

    fb_binding_init(boot_had_fb ? &boot_geometry : (const fb_geometry_t *)0);
    return 0;
}

static void install_screen(uint8_t fill)
{
    fb_geometry_t g = {
        .phys_addr = (uint64_t)(uintptr_t)screen,
        .width     = SCR_W,
        .height    = SCR_H,
        .pitch     = SCR_PITCH,
        .bpp       = SCR_BPP,
    };
    CU_ASSERT_EQUAL(fb_binding_init(&g), FB_BIND_OK);

    for (uint32_t i = 0; i < SCR_PITCH * SCR_H; i++)
        screen[i] = fill;
}

/* Give `who` a pane, acquire its shadow, fill it with `fill`. */
static void make_pane(page_owner_t who, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint8_t fill)
{
    fb_rect_t r = { x, y, w, h };
    CU_ASSERT_EQUAL(fb_layout_set(who, &r), FB_LAYOUT_OK);

    exo_fb_info_t info;
    CU_ASSERT_EQUAL(fb_shadow_acquire(who, &info), FB_SHADOW_OK);
    CU_ASSERT_EQUAL(info.width, w);
    CU_ASSERT_EQUAL(info.height, h);

    uint8_t *buf = (uint8_t *)(uintptr_t)info.phys_addr;
    for (uint32_t i = 0; i < info.pitch * info.height; i++)
        buf[i] = fill;
}

static uint8_t px(uint32_t x, uint32_t y)
{
    return screen[y * SCR_PITCH + x * BPP_BYTES];
}

/* Each context's pixels land inside its own rect, and the screen around the
 * panes is cleared rather than left holding whatever was there. */
static void test_two_panes_land_in_their_own_rects(void)
{
    install_screen(0xCC);
    fb_layout_reset();

    make_pane(PANE_LEFT,  0, 0, 4, 4, 0xAA);   /* top-left     */
    make_pane(PANE_RIGHT, 8, 4, 4, 4, 0xBB);   /* bottom-right */

    context_set_current(PANE_LEFT);
    fb_compositor_tick();

    for (uint32_t y = 0; y < SCR_H; y++) {
        for (uint32_t x = 0; x < SCR_W; x++) {
            uint8_t want = 0;
            if (x < 4 && y < 4)                          want = 0xAA;
            if (x >= 8 && x < 12 && y >= 4 && y < 8)     want = 0xBB;
            CU_ASSERT_EQUAL(px(x, y), want);
        }
    }

    /* Both panes are still shown when the other context is the running one. */
    context_set_current(PANE_RIGHT);
    fb_compositor_tick();
    CU_ASSERT_EQUAL(px(0, 0), 0xAA);
    CU_ASSERT_EQUAL(px(8, 4), 0xBB);

    fb_shadow_release(PANE_LEFT);
    fb_shadow_release(PANE_RIGHT);
}

/* A row of the pane is copied whole, and not a byte further. */
static void test_pane_edges_are_exact(void)
{
    install_screen(0x00);
    fb_layout_reset();

    make_pane(PANE_LEFT, 3, 2, 5, 3, 0x77);
    context_set_current(PANE_LEFT);
    fb_compositor_tick();

    for (uint32_t y = 0; y < SCR_H; y++) {
        for (uint32_t x = 0; x < SCR_W; x++) {
            int inside = x >= 3 && x < 8 && y >= 2 && y < 5;
            CU_ASSERT_EQUAL(px(x, y), inside ? 0x77 : 0x00);
        }
    }

    fb_shadow_release(PANE_LEFT);
}

/* A pane whose context has not acquired yet is just the cleared background. */
static void test_pane_without_a_shadow_stays_blank(void)
{
    install_screen(0xCC);
    fb_layout_reset();

    make_pane(PANE_LEFT, 0, 0, 4, 4, 0xAA);
    fb_rect_t r = { 8, 0, 4, 4 };
    CU_ASSERT_EQUAL(fb_layout_set(PANE_RIGHT, &r), FB_LAYOUT_OK);

    context_set_current(PANE_LEFT);
    fb_compositor_tick();

    CU_ASSERT_EQUAL(px(0, 0), 0xAA);
    CU_ASSERT_EQUAL(px(8, 0), 0x00);

    fb_shadow_release(PANE_LEFT);
    fb_shadow_release(PANE_RIGHT);
}

/* The running context is not in the split (the shell): full-screen, exactly
 * as before SCRUM-197. */
static void test_context_without_a_pane_is_fullscreen(void)
{
    install_screen(0xCC);
    fb_layout_reset();

    make_pane(PANE_LEFT, 0, 0, 4, 4, 0xAA);

    exo_fb_info_t info;
    CU_ASSERT_EQUAL(fb_shadow_acquire(NO_PANE, &info), FB_SHADOW_OK);
    CU_ASSERT_EQUAL(info.width, SCR_W);
    CU_ASSERT_EQUAL(info.height, SCR_H);
    uint8_t *buf = (uint8_t *)(uintptr_t)info.phys_addr;
    for (uint32_t i = 0; i < info.pitch * info.height; i++)
        buf[i] = 0xDD;

    context_set_current(NO_PANE);
    fb_compositor_tick();

    CU_ASSERT_EQUAL(px(0, 0), 0xDD);
    CU_ASSERT_EQUAL(px(SCR_W - 1, SCR_H - 1), 0xDD);

    fb_shadow_release(PANE_LEFT);
    fb_shadow_release(NO_PANE);
}

/* No panes at all: the pre-SCRUM-197 behaviour, untouched. */
static void test_empty_layout_is_fullscreen(void)
{
    install_screen(0xCC);
    fb_layout_reset();

    exo_fb_info_t info;
    CU_ASSERT_EQUAL(fb_shadow_acquire(NO_PANE, &info), FB_SHADOW_OK);
    uint8_t *buf = (uint8_t *)(uintptr_t)info.phys_addr;
    for (uint32_t i = 0; i < info.pitch * info.height; i++)
        buf[i] = 0xEE;

    context_set_current(NO_PANE);
    fb_compositor_tick();
    CU_ASSERT_EQUAL(px(0, 0), 0xEE);
    CU_ASSERT_EQUAL(px(SCR_W - 1, SCR_H - 1), 0xEE);

    fb_shadow_release(NO_PANE);
}

/* A shadow bigger than its pane (acquired first, pane assigned later) is
 * clipped to the pane rather than spilling past it. */
static void test_oversized_shadow_is_clipped_to_the_pane(void)
{
    install_screen(0x00);
    fb_layout_reset();

    exo_fb_info_t info;
    CU_ASSERT_EQUAL(fb_shadow_acquire(PANE_LEFT, &info), FB_SHADOW_OK);
    uint8_t *buf = (uint8_t *)(uintptr_t)info.phys_addr;
    for (uint32_t i = 0; i < info.pitch * info.height; i++)
        buf[i] = 0x55;

    fb_rect_t r = { 0, 0, 4, 4 };
    CU_ASSERT_EQUAL(fb_layout_set(PANE_LEFT, &r), FB_LAYOUT_OK);

    context_set_current(PANE_LEFT);
    fb_compositor_tick();

    CU_ASSERT_EQUAL(px(3, 3), 0x55);
    CU_ASSERT_EQUAL(px(4, 0), 0x00);
    CU_ASSERT_EQUAL(px(0, 4), 0x00);

    fb_shadow_release(PANE_LEFT);
}

/* Dropping a pane clears its rect on the next tick. */
static void test_dropped_pane_is_cleared(void)
{
    install_screen(0xCC);
    fb_layout_reset();

    make_pane(PANE_LEFT,  0, 0, 4, 4, 0xAA);
    make_pane(PANE_RIGHT, 8, 4, 4, 4, 0xBB);

    context_set_current(PANE_LEFT);
    fb_compositor_tick();
    CU_ASSERT_EQUAL(px(8, 4), 0xBB);

    fb_shadow_release(PANE_RIGHT);   /* as exo_exit would */
    fb_compositor_tick();
    CU_ASSERT_EQUAL(px(0, 0), 0xAA);
    CU_ASSERT_EQUAL(px(8, 4), 0x00);

    fb_shadow_release(PANE_LEFT);
}

void suite_fb_compositor_tests(CU_pSuite s)
{
    CU_add_test(s, "two panes land in their own rects",
                test_two_panes_land_in_their_own_rects);
    CU_add_test(s, "pane edges are exact", test_pane_edges_are_exact);
    CU_add_test(s, "pane without a shadow stays blank",
                test_pane_without_a_shadow_stays_blank);
    CU_add_test(s, "context without a pane is fullscreen",
                test_context_without_a_pane_is_fullscreen);
    CU_add_test(s, "empty layout is fullscreen",
                test_empty_layout_is_fullscreen);
    CU_add_test(s, "oversized shadow is clipped to the pane",
                test_oversized_shadow_is_clipped_to_the_pane);
    CU_add_test(s, "dropped pane is cleared", test_dropped_pane_is_cleared);
}
