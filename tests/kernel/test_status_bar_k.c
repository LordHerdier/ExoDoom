/*
 * test_status_bar_k.c -- the compositor's status bar (SCRUM-278).
 *
 * Two layers. status_bar_format() is pure text layout, tested on hand-made
 * context lists. The rest runs the real compositor and bar against a RAM
 * "screen" tall enough to have a reserved strip (FB_BAR_MIN_HEIGHT), so what
 * is checked is bytes in a buffer: the strip painted, left black when off,
 * untouched by the program copy, and repainted only on a change.
 */

#include "kunit.h"
#include "status_bar.h"
#include "fb_binding.h"
#include "fb_compositor.h"
#include "fb_layout.h"
#include "fb_shadow.h"
#include "context.h"
#include "page_alloc.h"
#include "exo_syscall.h"
#include "syscall.h"
#include "syscall_statusbar.h"
#include "vmm.h"
#include "string.h"

#include <stdint.h>

#define BAR_OWN   ((page_owner_t)(PAGE_OWNER_LIBOS + 40))
#define BAR_PROG  ((page_owner_t)(PAGE_OWNER_LIBOS + 41))

#define SCR_W      256u
#define SCR_H      FB_BAR_MIN_HEIGHT
#define SCR_PITCH  (SCR_W * 4u)
#define SCR_BYTES  (SCR_PITCH * SCR_H)
#define SCR_PAGES  ((SCR_BYTES + VMM_PAGE_SIZE - 1u) / VMM_PAGE_SIZE)
#define STRIP_OFF  (SCR_PITCH * (SCR_H - FB_BAR_ROWS))

static fb_geometry_t boot_geometry;
static int           boot_had_fb;
static page_owner_t  boot_current;
static uint8_t      *screen;

int status_bar_suite_init(void)
{
    const fb_geometry_t *g = fb_binding_geometry();

    boot_had_fb = (g != NULL);
    if (boot_had_fb)
        boot_geometry = *g;
    boot_current = context_current();

    screen = alloc_pages_contig_owned(BAR_OWN, SCR_PAGES);
    if (screen == NULL)
        return 1;

    fb_layout_reset();
    return 0;
}

int status_bar_suite_cleanup(void)
{
    fb_shadow_release(BAR_PROG);
    fb_layout_reset();
    status_bar_reset();
    context_set_current(boot_current);

    if (screen != NULL) {
        for (uint32_t i = 0; i < SCR_PAGES; i++)
            free_page_owned(screen + i * VMM_PAGE_SIZE, BAR_OWN);
    }

    fb_binding_init(boot_had_fb ? &boot_geometry : (const fb_geometry_t *)0);
    return 0;
}

static void install_screen(uint32_t height, uint8_t fill)
{
    fb_geometry_t g = {
        .phys_addr = (uint64_t)(uintptr_t)screen,
        .width     = SCR_W,
        .height    = height,
        .pitch     = SCR_PITCH,
        .bpp       = 32,
    };
    CU_ASSERT_EQUAL(fb_binding_init(&g), FB_BIND_OK);
    status_bar_reset();
    memset(screen, fill, SCR_BYTES);
}

static int strip_all(uint8_t v)
{
    for (uint32_t i = STRIP_OFF; i < SCR_BYTES; i++)
        if (screen[i] != v)
            return 0;
    return 1;
}

static int strip_has_ink(void)
{
    for (uint32_t i = STRIP_OFF; i < SCR_BYTES; i++)
        if (screen[i] != 0)
            return 1;
    return 0;
}

static context_info_t mk(page_owner_t id, context_state_t st, const char *name)
{
    context_info_t c;
    memset(&c, 0, sizeof(c));
    c.id = id;
    c.state = st;
    for (int i = 0; name[i] != '\0' && i < CONTEXT_NAME_MAX - 1; i++)
        c.name[i] = name[i];
    return c;
}

/* ── pure layout ───────────────────────────────────────────────────────── */

static void test_format_lists_in_order_with_focus_brackets(void)
{
    context_info_t c[3] = {
        mk(2, CONTEXT_STATE_READY, "shell"),
        mk(3, CONTEXT_STATE_RUNNING, "snake"),
        mk(4, CONTEXT_STATE_READY, "top"),
    };
    char line[41];
    uint32_t hs, he;

    status_bar_format(c, 3, 3, "HINT", 40, line, &hs, &he);
    CU_ASSERT_EQUAL(strlen(line), 40u);
    CU_ASSERT_EQUAL(memcmp(line, "0:shell  [1:snake]  2:top", 25), 0);
    CU_ASSERT_EQUAL(hs, 9u);
    CU_ASSERT_EQUAL(he, 18u);
    CU_ASSERT_EQUAL(memcmp(line + 36, "HINT", 4), 0);
}

static void test_format_skips_contexts_that_cannot_run(void)
{
    context_info_t c[3] = {
        mk(2, CONTEXT_STATE_READY, "shell"),
        mk(3, CONTEXT_STATE_BLOCKED, "dead"),
        mk(4, CONTEXT_STATE_RUNNING, "top"),
    };
    char line[41];
    uint32_t hs, he;

    status_bar_format(c, 3, 4, "", 40, line, &hs, &he);
    CU_ASSERT_EQUAL(memcmp(line, "0:shell  [1:top]", 16), 0);
    CU_ASSERT_TRUE(strstr(line, "dead") == NULL);
}

static void test_format_no_focus_matches_nothing(void)
{
    context_info_t c[1] = { mk(2, CONTEXT_STATE_READY, "shell") };
    char line[21];
    uint32_t hs = 7, he = 7;

    status_bar_format(c, 1, 99, "", 20, line, &hs, &he);
    CU_ASSERT_EQUAL(hs, 0u);
    CU_ASSERT_EQUAL(he, 0u);
    CU_ASSERT_EQUAL(line[0], '0');
}

static void test_format_unnamed_context_gets_placeholder(void)
{
    context_info_t c[1] = { mk(2, CONTEXT_STATE_READY, "") };
    char line[21];
    uint32_t hs, he;

    status_bar_format(c, 1, 2, "", 20, line, &hs, &he);
    CU_ASSERT_EQUAL(memcmp(line, "[0:?]", 5), 0);
}

static void test_format_cuts_entries_but_keeps_right_text(void)
{
    context_info_t c[3] = {
        mk(2, CONTEXT_STATE_READY, "shell"),
        mk(3, CONTEXT_STATE_READY, "snake"),
        mk(4, CONTEXT_STATE_READY, "doomdoomdoom"),
    };
    char line[31];
    uint32_t hs, he;

    /* 30 columns, 14-column hint + 1 gap leaves 15 for the entries. */
    status_bar_format(c, 3, 2, STATUS_BAR_HINT, 30, line, &hs, &he);
    CU_ASSERT_EQUAL(strlen(line), 30u);
    CU_ASSERT_EQUAL(memcmp(line + 30 - 14, STATUS_BAR_HINT, 14), 0);
    CU_ASSERT_TRUE(strstr(line, "..") != NULL);
    CU_ASSERT_TRUE(strstr(line, "doomdoom") == NULL);
}

static void test_format_clamps_to_max_cols(void)
{
    context_info_t c[1] = { mk(2, CONTEXT_STATE_READY, "shell") };
    char line[STATUS_BAR_MAX_COLS + 1];
    uint32_t hs, he;

    status_bar_format(c, 1, 2, "x", STATUS_BAR_MAX_COLS + 500u, line, &hs, &he);
    CU_ASSERT_EQUAL(strlen(line), STATUS_BAR_MAX_COLS);
    CU_ASSERT_EQUAL(line[STATUS_BAR_MAX_COLS - 1], 'x');
}

/* ── reserved row ──────────────────────────────────────────────────────── */

static void test_usable_height_reserves_only_on_tall_screens(void)
{
    install_screen(SCR_H, 0);
    CU_ASSERT_EQUAL(fb_binding_has_bar(), 1);
    CU_ASSERT_EQUAL(fb_binding_usable_height(), SCR_H - FB_BAR_ROWS);

    install_screen(FB_BAR_MIN_HEIGHT - 1u, 0);
    CU_ASSERT_EQUAL(fb_binding_has_bar(), 0);
    CU_ASSERT_EQUAL(fb_binding_usable_height(), FB_BAR_MIN_HEIGHT - 1u);

    CU_ASSERT_EQUAL(fb_binding_init((const fb_geometry_t *)0), FB_BIND_ENODEV);
    CU_ASSERT_EQUAL(fb_binding_usable_height(), 0u);
}

static void test_shadow_and_pane_exclude_the_strip(void)
{
    install_screen(SCR_H, 0);

    exo_fb_info_t info;
    CU_ASSERT_EQUAL(fb_shadow_acquire(BAR_PROG, &info), FB_SHADOW_OK);
    CU_ASSERT_EQUAL(info.height, SCR_H - FB_BAR_ROWS);
    fb_shadow_release(BAR_PROG);

    fb_rect_t fits = { 0, 0, SCR_W, SCR_H - FB_BAR_ROWS };
    fb_rect_t over = { 0, 0, SCR_W, SCR_H - FB_BAR_ROWS + 1u };
    CU_ASSERT_EQUAL(fb_layout_set(BAR_PROG, &over), FB_LAYOUT_EINVAL);
    CU_ASSERT_EQUAL(fb_layout_set(BAR_PROG, &fits), FB_LAYOUT_OK);
    fb_layout_clear(BAR_PROG);
}

static void test_program_copy_never_touches_the_strip(void)
{
    install_screen(SCR_H, 0xAA);

    exo_fb_info_t info;
    CU_ASSERT_EQUAL(fb_shadow_acquire(BAR_PROG, &info), FB_SHADOW_OK);
    memset((void *)(uintptr_t)info.phys_addr, 0x55, info.pitch * info.height);

    context_set_current(BAR_PROG);
    fb_compositor_tick();

    CU_ASSERT_EQUAL(screen[0], 0x55);
    CU_ASSERT_EQUAL(screen[STRIP_OFF - 1], 0x55);
    /* Bar is off: the strip is blanked, not left as the program's or 0xAA. */
    CU_ASSERT_TRUE(strip_all(0));
    fb_shadow_release(BAR_PROG);
}

/* ── painting ──────────────────────────────────────────────────────────── */

static void test_off_leaves_strip_black_on_paints_it(void)
{
    install_screen(SCR_H, 0xAA);
    CU_ASSERT_EQUAL(status_bar_enabled(), 0);

    status_bar_update(fb_binding_geometry(), 1000);
    CU_ASSERT_TRUE(strip_all(0));
    CU_ASSERT_EQUAL(screen[STRIP_OFF - 1], 0xAA);   /* program rows untouched */

    status_bar_set_enabled(1);
    status_bar_update(fb_binding_geometry(), 1000);
    CU_ASSERT_TRUE(strip_has_ink());
    CU_ASSERT_EQUAL(screen[STRIP_OFF - 1], 0xAA);

    status_bar_set_enabled(0);
    status_bar_update(fb_binding_geometry(), 1000);
    CU_ASSERT_TRUE(strip_all(0));
}

static void test_repaint_only_on_change(void)
{
    install_screen(SCR_H, 0);
    status_bar_set_enabled(1);
    status_bar_update(fb_binding_geometry(), 1000);

    /* A sentinel survives an identical update... */
    screen[STRIP_OFF] ^= 0xFF;
    uint8_t marked = screen[STRIP_OFF];
    status_bar_update(fb_binding_geometry(), 1001);
    CU_ASSERT_EQUAL(screen[STRIP_OFF], marked);

    /* ...and is repainted once the tooltip expires. */
    status_bar_update(fb_binding_geometry(), 1000 + STATUS_BAR_TOOLTIP_MS + 1u);
    CU_ASSERT_NOT_EQUAL(screen[STRIP_OFF], marked);
}

static void test_tooltip_restarts_when_focus_changes(void)
{
    install_screen(SCR_H, 0);
    status_bar_set_enabled(1);
    context_set_current(boot_current);
    status_bar_update(fb_binding_geometry(), 1000);

    /* Past the tooltip: the short hint is on screen; snapshot it. */
    status_bar_update(fb_binding_geometry(), 1000 + STATUS_BAR_TOOLTIP_MS + 1u);
    static uint8_t hint_strip[SCR_BYTES - STRIP_OFF];
    memcpy(hint_strip, screen + STRIP_OFF, sizeof(hint_strip));

    /* A different focus restarts the tooltip, which repaints differently
     * (only if the focus actually names a different live entry, so give the
     * table one: BAR_PROG is a real context). */
    page_owner_t id;
    if (context_create(0, &id) != CONTEXT_OK)
        return;
    context_set_name(id, "probe");
    status_bar_update(fb_binding_geometry(), 2 * STATUS_BAR_TOOLTIP_MS);
    CU_ASSERT_NOT_EQUAL(memcmp(hint_strip, screen + STRIP_OFF, sizeof(hint_strip)), 0);

    context_destroy(id);
}

static void test_no_strip_on_short_screen(void)
{
    install_screen(FB_BAR_MIN_HEIGHT - 1u, 0xAA);
    status_bar_set_enabled(1);
    status_bar_update(fb_binding_geometry(), 1000);
    CU_ASSERT_EQUAL(screen[SCR_PITCH * (FB_BAR_MIN_HEIGHT - 1u) - 1u], 0xAA);
}

/* ── syscall #45 ───────────────────────────────────────────────────────── */

static void test_statusbar_syscall(void)
{
    status_bar_reset();
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_STATUSBAR, EXO_STATUSBAR_QUERY,
                                         0, 0, 0, 0, 0), 0);
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_STATUSBAR, EXO_STATUSBAR_ON,
                                         0, 0, 0, 0, 0), 1);
    CU_ASSERT_EQUAL(status_bar_enabled(), 1);
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_STATUSBAR, EXO_STATUSBAR_QUERY,
                                         0, 0, 0, 0, 0), 1);
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_STATUSBAR, 9, 0, 0, 0, 0, 0),
                    -EXO_EINVAL);
    CU_ASSERT_EQUAL(status_bar_enabled(), 1);   /* a bad mode changes nothing */
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_STATUSBAR, EXO_STATUSBAR_OFF,
                                         0, 0, 0, 0, 0), 0);
    status_bar_reset();
}

void suite_status_bar_tests(CU_pSuite s)
{
    CU_add_test(s, "format lists in order, brackets focus",
                test_format_lists_in_order_with_focus_brackets);
    CU_add_test(s, "format skips contexts that cannot run",
                test_format_skips_contexts_that_cannot_run);
    CU_add_test(s, "format with no matching focus", test_format_no_focus_matches_nothing);
    CU_add_test(s, "format names an unnamed context",
                test_format_unnamed_context_gets_placeholder);
    CU_add_test(s, "format cuts entries, keeps the right text",
                test_format_cuts_entries_but_keeps_right_text);
    CU_add_test(s, "format clamps to the max columns", test_format_clamps_to_max_cols);
    CU_add_test(s, "usable height reserves only on tall screens",
                test_usable_height_reserves_only_on_tall_screens);
    CU_add_test(s, "shadow and pane exclude the strip",
                test_shadow_and_pane_exclude_the_strip);
    CU_add_test(s, "program copy never touches the strip",
                test_program_copy_never_touches_the_strip);
    CU_add_test(s, "off leaves the strip black, on paints it",
                test_off_leaves_strip_black_on_paints_it);
    CU_add_test(s, "repaint only on change", test_repaint_only_on_change);
    CU_add_test(s, "tooltip restarts when the entries change",
                test_tooltip_restarts_when_focus_changes);
    CU_add_test(s, "no strip on a short screen", test_no_strip_on_short_screen);
    CU_add_test(s, "exo_statusbar modes", test_statusbar_syscall);
}
