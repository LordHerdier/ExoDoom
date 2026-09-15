/*
 * test_fb_console_k.c — fb_console scroll correctness (SCRUM-162).
 *
 * scroll_up_one_row() in src/fb_console.c used to copy the framebuffer one
 * byte at a time; SCRUM-162 switched it to whole 32-bit pixels (the
 * framebuffer is always 32bpp per fb_init_bgrx8888()). This suite checks the
 * word-wise copy still produces exactly the same result the byte-wise one
 * did: an exact upward shift of pixel rows, with the newly exposed bottom
 * row cleared to the console background colour.
 *
 * The framebuffer does not exist during a TESTING boot (CLAUDE.md), so this
 * suite builds a synthetic one over a real, kernel-identity-mapped page from
 * alloc_page() rather than the machine's real MMIO aperture — scroll_up_one_row
 * only ever deals in fb->addr/pitch/width/height, so a RAM-backed
 * framebuffer_t exercises the exact same code path.
 *
 * Geometry: 16x64 px at 32bpp (pitch = 64 B) puts the whole framebuffer in
 * one 4 KiB page and gives 4 character rows (8x16 cells) — enough to stamp a
 * distinct row above, at, and below the one that gets scrolled off, and check
 * every row shifts up by exactly one without disturbing its neighbours.
 */

#include "kunit.h"
#include "fb.h"
#include "fb_console.h"
#include "page_alloc.h"

#include <stdint.h>

#define TEST_FB_WIDTH  16u
#define TEST_FB_HEIGHT 64u
#define TEST_FB_PITCH  (TEST_FB_WIDTH * 4u)
#define TEST_FB_BPP    32u

static void* fb_page;

int fb_console_suite_init(void)
{
    fb_page = alloc_page();
    return fb_page ? 0 : -1;
}

int fb_console_suite_cleanup(void)
{
    if (fb_page) free_page(fb_page);
    fb_page = 0;
    return 0;
}

/* Fill one 8x16 character row (0-based) with a raw pixel value, bypassing
 * fb_fill_rect's r/g/b packing so the test doesn't depend on it. */
static void stamp_char_row(framebuffer_t* fb, uint32_t char_row, uint32_t value)
{
    for (uint32_t y = 0; y < 16; y++) {
        uint32_t* row = (uint32_t*)(fb->addr + (char_row * 16 + y) * fb->pitch);
        for (uint32_t x = 0; x < fb->width; x++) row[x] = value;
    }
}

/* True if every pixel in the given character row equals value. */
static bool char_row_is(framebuffer_t* fb, uint32_t char_row, uint32_t value)
{
    for (uint32_t y = 0; y < 16; y++) {
        uint32_t* row = (uint32_t*)(fb->addr + (char_row * 16 + y) * fb->pitch);
        for (uint32_t x = 0; x < fb->width; x++) {
            if (row[x] != value) return false;
        }
    }
    return true;
}

static void test_scroll_shifts_rows_and_clears_bottom(void)
{
    framebuffer_t fb;
    CU_ASSERT_TRUE(fb_init_bgrx8888(&fb, (uintptr_t)fb_page, TEST_FB_PITCH,
                                     TEST_FB_WIDTH, TEST_FB_HEIGHT, TEST_FB_BPP));

    fb_console_t con;
    CU_ASSERT_TRUE(fbcon_init(&con, &fb));
    CU_ASSERT_EQUAL(con.cols, TEST_FB_WIDTH / 8);
    CU_ASSERT_EQUAL(con.rows, TEST_FB_HEIGHT / 16);

    /* The cursor underline is drawn/erased on every fbcon_putc(), including
     * '\n' -- left on, it would stamp a pixel or two into the last scanline
     * of whatever row it lands on and make the row-uniformity checks below
     * fail for a reason that has nothing to do with scroll_up_one_row().
     * Disabling it is the documented way to get clean scroll output
     * (docs/drivers/framebuffer.md "Cursor"). */
    fbcon_enable_cursor(&con, false);

    /* fbcon_init() clears to black (0,0,0) -> raw word 0, which doubles as
     * the "background" sentinel below. */
    const uint32_t bg = 0x00000000u;
    const uint32_t row1_val = 0x00112233u;
    const uint32_t row2_val = 0x00445566u;
    const uint32_t row3_val = 0x00778899u;

    stamp_char_row(&fb, 0, 0x00DEAD00u); /* about to be scrolled off */
    stamp_char_row(&fb, 1, row1_val);
    stamp_char_row(&fb, 2, row2_val);
    stamp_char_row(&fb, 3, row3_val);

    /* Four newlines walk cursor_y from 0 to 4, tripping the
     * "cursor_y >= rows" branch in newline() exactly once -- one scroll. '\n'
     * draws no glyph, so it disturbs nothing but the scroll itself does. */
    fbcon_write(&con, "\n\n\n\n");

    /* Each row should now hold what used to be one row below it, and the
     * newly exposed bottom row should be background. */
    CU_ASSERT_TRUE(char_row_is(&fb, 0, row1_val));
    CU_ASSERT_TRUE(char_row_is(&fb, 1, row2_val));
    CU_ASSERT_TRUE(char_row_is(&fb, 2, row3_val));
    CU_ASSERT_TRUE(char_row_is(&fb, 3, bg));
}

void suite_fb_console_tests(CU_pSuite s)
{
    CU_add_test(s, "scroll shifts rows and clears bottom",
                test_scroll_shifts_rows_and_clears_bottom);
}
