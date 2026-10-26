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
 * The framebuffer does not exist during a TESTING boot (docs/architecture.md §4), so this
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

/* Every printable ASCII character draws something. The font used to leave
 * :;<=>?@ [\]^_` {|}~ blank, so the shell's prompt '>' and SCRUM-263's
 * `<cmd> ?` rendered as empty cells. */
static void test_every_printable_glyph_draws(void)
{
    framebuffer_t fb;
    CU_ASSERT_TRUE(fb_init_bgrx8888(&fb, (uintptr_t)fb_page, TEST_FB_PITCH,
                                     TEST_FB_WIDTH, TEST_FB_HEIGHT, TEST_FB_BPP));
    fb_console_t con;
    CU_ASSERT_TRUE(fbcon_init(&con, &fb));
    fbcon_enable_cursor(&con, false);
    fbcon_set_color(&con, 255, 255, 255, 0, 0, 0);

    int blank = 0;
    for (int ch = 33; ch <= 126; ch++) {
        con.cursor_x = 0;
        con.cursor_y = 0;
        fbcon_putc(&con, (char)ch);
        /* putc clears the cell to the black background before drawing, so
         * an all-zero cell means the glyph is empty. */
        if (char_row_is(&fb, 0, 0x00000000u)) {
            blank++;
        }
    }
    CU_ASSERT_EQUAL(blank, 0);
}

/*
 * fb_clear() and fb_fill_rect() fill a row at a time with `rep stosl`
 * (SCRUM-192). With a pitch wider than the visible row, each fill has to
 * stop at the row's last pixel: the padding after it, and every pixel
 * outside a rectangle, must come through untouched.
 */
#define PAD_FB_WIDTH  16u
#define PAD_FB_HEIGHT 16u
#define PAD_FB_PITCH  (PAD_FB_WIDTH * 4u + 16u)
#define PAD_SENTINEL  0xA5A5A5A5u

static void test_clear_and_fill_respect_pitch_and_bounds(void)
{
    uint32_t *words = (uint32_t *)fb_page;
    for (uint32_t i = 0; i < PAD_FB_PITCH * PAD_FB_HEIGHT / 4u; i++) {
        words[i] = PAD_SENTINEL;
    }

    framebuffer_t fb;
    CU_ASSERT_TRUE(fb_init_bgrx8888(&fb, (uintptr_t)fb_page, PAD_FB_PITCH,
                                     PAD_FB_WIDTH, PAD_FB_HEIGHT, 32));

    fb_clear(&fb, 0x12, 0x34, 0x56);
    fb_fill_rect(&fb, 3, 2, 5, 4, 0xAB, 0xCD, 0xEF);

    int ok = 1;
    for (uint32_t y = 0; y < PAD_FB_HEIGHT; y++) {
        const uint32_t *row = (const uint32_t *)(fb.addr + y * fb.pitch);
        for (uint32_t x = 0; x < PAD_FB_PITCH / 4u; x++) {
            uint32_t want;
            if (x >= PAD_FB_WIDTH) {
                want = PAD_SENTINEL;
            } else if (x >= 3 && x < 8 && y >= 2 && y < 6) {
                want = 0x00ABCDEFu;
            } else {
                want = 0x00123456u;
            }
            if (row[x] != want) ok = 0;
        }
    }
    CU_ASSERT_TRUE(ok);
}

void suite_fb_console_tests(CU_pSuite s)
{
    CU_add_test(s, "scroll shifts rows and clears bottom",
                test_scroll_shifts_rows_and_clears_bottom);
    CU_add_test(s, "every printable glyph draws",
                test_every_printable_glyph_draws);
    CU_add_test(s, "clear and fill respect pitch and bounds",
                test_clear_and_fill_respect_pitch_and_bounds);
}
