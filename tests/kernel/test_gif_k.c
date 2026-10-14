/*
 * test_gif_k.c — acceptance tests for the boot animation's GIF decoder.
 *
 * Two halves, the same split tests/kernel/test_doom_dmx_k.c makes and for the
 * same reasons.
 *
 * The synthetic half embeds two tiny GIFs byte for byte.  They are not
 * hand-written: both were produced by ImageMagick and dumped, so they are
 * what a real encoder emits rather than what this decoder happens to accept
 * -- which matters most for the LZW stream, where a hand-rolled fixture would
 * only ever prove the decoder agrees with the person who wrote it.  Being
 * 4x4, every pixel of every frame is asserted, so a frame drawn upside down,
 * mirrored, or one row short cannot pass.
 *
 * The real half decodes the actual boot animation module out of the ISO --
 * `make docker-test` boots through GRUB, modules and all -- which is the only
 * way to find out that a 2 MiB, 20-frame, 498x477 file with interleaved
 * graphic-control extensions decodes from first byte to trailer.  Checking
 * the 98-byte fixture proves the algorithm; checking tenor.gif proves the
 * thing that actually plays at boot.
 */

#include "kunit.h"

#include "boot_anim.h"
#include "gif.h"
#include "mmap.h"
#include "string.h"

#include <stddef.h>
#include <stdint.h>

/* A 4x4, two-frame GIF89a.  Global colour table of four entries --
 * 0 red, 1 blue, 2 green, 3 white -- and a 70 ms delay on both frames.
 * Frame 0 is red | blue split down the middle; frame 1 is green over white. */
static const uint8_t two_frame_gif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x04, 0x00, 0x04, 0x00, 0xF1, 0x00,
    0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xFF,
    0xFF, 0x21, 0xFF, 0x0B, 0x4E, 0x45, 0x54, 0x53, 0x43, 0x41, 0x50, 0x45,
    0x32, 0x2E, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xF9, 0x04, 0x04,
    0x07, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04,
    0x00, 0x00, 0x02, 0x06, 0x04, 0x12, 0x86, 0x9A, 0x17, 0x05, 0x00, 0x21,
    0xF9, 0x04, 0x04, 0x07, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x04, 0x00, 0x00, 0x02, 0x05, 0x94, 0x6F, 0xA3, 0xAB, 0x05,
    0x00, 0x3B,
};

/* The same size, one frame, written *interlaced*: its four rows are stored in
 * GIF's four-pass order (0, 2, 1, 3 for a 4-row image) rather than top to
 * bottom.  As stored they are red, white, blue, green; displayed, they are
 * red, blue, white, green -- which is what ImageMagick's own decoder makes of
 * this file, and what the expectations below are taken from.  Reading it as a
 * sequential image yields the storage order instead, with nothing else wrong,
 * which is exactly the kind of bug a "did it decode?" test misses. */
static const uint8_t interlaced_gif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x04, 0x00, 0x04, 0x00, 0xF1, 0x03,
    0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
    0xFF, 0x21, 0xF9, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00,
    0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x40, 0x02, 0x07, 0x84, 0x31, 0x39,
    0xC2, 0x12, 0x1F, 0x0A, 0x00, 0x3B,
};

static gif_t g;
static uint8_t small_canvas[64];

/* The real module needs the full canvas; one buffer, shared by the tests that
 * touch it, rather than 576 KiB of .bss per test. */
static uint8_t big_canvas[GIF_CANVAS_BYTES];

/* Palette lookup as a packed RGB, so an expected colour reads as 0xRRGGBB. */
static uint32_t rgb_at(const gif_t *gp, const uint8_t *canvas, int x, int y) {
    uint8_t idx = canvas[(size_t)y * gp->width + (size_t)x];
    const uint8_t *c = &gp->palette[(size_t)idx * 3];
    return ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
}

/* ── Header ───────────────────────────────────────────────────────────────*/

static void test_open_reads_the_logical_screen(void) {
    CU_ASSERT_EQUAL(gif_open(&g, two_frame_gif, sizeof(two_frame_gif)), GIF_OK);
    CU_ASSERT_EQUAL(g.width, 4);
    CU_ASSERT_EQUAL(g.height, 4);
    CU_ASSERT_EQUAL(g.gct_colors, 4);
    CU_ASSERT_EQUAL(g.bg_index, 0);
}

static void test_open_rejects_what_is_not_a_gif(void) {
    static const uint8_t not_a_gif[16] = { 'P', 'N', 'G', 0 };
    CU_ASSERT_EQUAL(gif_open(&g, not_a_gif, sizeof(not_a_gif)), GIF_EINVAL);
    CU_ASSERT_EQUAL(gif_open(&g, two_frame_gif, 8), GIF_EINVAL);
    CU_ASSERT_EQUAL(gif_open(&g, NULL, 100), GIF_EINVAL);
}

static void test_open_rejects_a_screen_bigger_than_the_canvas(void) {
    /* A header claiming more pixels than GIF_CANVAS_BYTES holds must be
     * refused *here*, since every write below is bounds-checked against
     * g.width/g.height and nothing else. */
    static uint8_t huge[sizeof(two_frame_gif)];
    memcpy(huge, two_frame_gif, sizeof(huge));
    huge[6] = (uint8_t)((GIF_MAX_WIDTH + 1) & 0xFF);
    huge[7] = (uint8_t)((GIF_MAX_WIDTH + 1) >> 8);
    CU_ASSERT_EQUAL(gif_open(&g, huge, sizeof(huge)), GIF_ETOOBIG);
}

/* ── Frames ───────────────────────────────────────────────────────────────*/

static void test_first_frame_decodes_pixel_for_pixel(void) {
    CU_ASSERT_EQUAL(gif_open(&g, two_frame_gif, sizeof(two_frame_gif)), GIF_OK);
    gif_canvas_clear(&g, small_canvas);

    CU_ASSERT_EQUAL(gif_next_frame(&g, small_canvas), GIF_OK);
    CU_ASSERT_EQUAL(g.delay_ms, 70);

    for (int y = 0; y < 4; y++) {
        CU_ASSERT_EQUAL(rgb_at(&g, small_canvas, 0, y), 0xFF0000u);
        CU_ASSERT_EQUAL(rgb_at(&g, small_canvas, 1, y), 0xFF0000u);
        CU_ASSERT_EQUAL(rgb_at(&g, small_canvas, 2, y), 0x0000FFu);
        CU_ASSERT_EQUAL(rgb_at(&g, small_canvas, 3, y), 0x0000FFu);
    }
}

static void test_second_frame_replaces_the_first(void) {
    CU_ASSERT_EQUAL(gif_open(&g, two_frame_gif, sizeof(two_frame_gif)), GIF_OK);
    gif_canvas_clear(&g, small_canvas);

    CU_ASSERT_EQUAL(gif_next_frame(&g, small_canvas), GIF_OK);
    CU_ASSERT_EQUAL(gif_next_frame(&g, small_canvas), GIF_OK);

    for (int x = 0; x < 4; x++) {
        CU_ASSERT_EQUAL(rgb_at(&g, small_canvas, x, 0), 0x00FF00u);
        CU_ASSERT_EQUAL(rgb_at(&g, small_canvas, x, 1), 0x00FF00u);
        CU_ASSERT_EQUAL(rgb_at(&g, small_canvas, x, 2), 0xFFFFFFu);
        CU_ASSERT_EQUAL(rgb_at(&g, small_canvas, x, 3), 0xFFFFFFu);
    }
}

static void test_trailer_ends_the_pass(void) {
    CU_ASSERT_EQUAL(gif_open(&g, two_frame_gif, sizeof(two_frame_gif)), GIF_OK);
    gif_canvas_clear(&g, small_canvas);

    CU_ASSERT_EQUAL(gif_next_frame(&g, small_canvas), GIF_OK);
    CU_ASSERT_EQUAL(gif_next_frame(&g, small_canvas), GIF_OK);
    CU_ASSERT_EQUAL(gif_next_frame(&g, small_canvas), GIF_END);
    /* Still GIF_END on the call after: a finished stream stays finished. */
    CU_ASSERT_EQUAL(gif_next_frame(&g, small_canvas), GIF_END);
}

static void test_rewind_replays_the_same_frames(void) {
    CU_ASSERT_EQUAL(gif_open(&g, two_frame_gif, sizeof(two_frame_gif)), GIF_OK);
    gif_canvas_clear(&g, small_canvas);
    CU_ASSERT_EQUAL(gif_next_frame(&g, small_canvas), GIF_OK);
    CU_ASSERT_EQUAL(gif_next_frame(&g, small_canvas), GIF_OK);
    CU_ASSERT_EQUAL(gif_next_frame(&g, small_canvas), GIF_END);

    /* This is what the player does between loops, and the only thing that
     * makes an animation loop at all. */
    gif_rewind(&g);
    gif_canvas_clear(&g, small_canvas);
    CU_ASSERT_EQUAL(gif_next_frame(&g, small_canvas), GIF_OK);
    CU_ASSERT_EQUAL(rgb_at(&g, small_canvas, 0, 0), 0xFF0000u);
    CU_ASSERT_EQUAL(rgb_at(&g, small_canvas, 3, 0), 0x0000FFu);
}

static void test_interlaced_rows_land_in_order(void) {
    CU_ASSERT_EQUAL(gif_open(&g, interlaced_gif, sizeof(interlaced_gif)), GIF_OK);
    gif_canvas_clear(&g, small_canvas);
    CU_ASSERT_EQUAL(gif_next_frame(&g, small_canvas), GIF_OK);

    for (int x = 0; x < 4; x++) {
        CU_ASSERT_EQUAL(rgb_at(&g, small_canvas, x, 0), 0xFF0000u);
        CU_ASSERT_EQUAL(rgb_at(&g, small_canvas, x, 1), 0x0000FFu);
        CU_ASSERT_EQUAL(rgb_at(&g, small_canvas, x, 2), 0xFFFFFFu);
        CU_ASSERT_EQUAL(rgb_at(&g, small_canvas, x, 3), 0x00FF00u);
    }
}

static void test_truncated_pixel_data_is_edata_not_a_hang(void) {
    /* Cut the file off two bytes into the first frame's LZW data (the stream
     * runs to offset 70). The decoder has to come back with an error: a boot
     * animation that spins here hangs the boot before the first log line,
     * with nothing on screen to say why. */
    static uint8_t cut[66];
    memcpy(cut, two_frame_gif, sizeof(cut));
    CU_ASSERT_EQUAL(gif_open(&g, cut, sizeof(cut)), GIF_OK);
    gif_canvas_clear(&g, small_canvas);
    CU_ASSERT_EQUAL(gif_next_frame(&g, small_canvas), GIF_EDATA);
}

/* ── The real module ──────────────────────────────────────────────────────*/

static int find_module(const uint8_t **data, size_t *len) {
    uint64_t start = 0, end = 0;
    if (mmap_find_module_named(BOOT_ANIM_MODULE, &start, &end) != 0) {
        return -1;
    }
    *data = (const uint8_t *)(uintptr_t)start;
    *len  = (size_t)(end - start);
    return 0;
}

static void test_real_module_is_loaded(void) {
    const uint8_t *data = NULL;
    size_t len = 0;
    CU_ASSERT_EQUAL(find_module(&data, &len), 0);
    CU_ASSERT_TRUE(len > 1024);
    /* GRUB handed us the file itself, not a fragment of something else. */
    CU_ASSERT_EQUAL(memcmp(data, "GIF89a", 6), 0);
}

static void test_real_module_decodes_every_frame(void) {
    const uint8_t *data = NULL;
    size_t len = 0;
    CU_ASSERT_EQUAL(find_module(&data, &len), 0);
    CU_ASSERT_EQUAL(gif_open(&g, data, len), GIF_OK);
    CU_ASSERT_TRUE(g.width <= GIF_MAX_WIDTH);
    CU_ASSERT_TRUE(g.height <= GIF_MAX_HEIGHT);

    gif_canvas_clear(&g, big_canvas);

    int frames = 0;
    uint32_t total_ms = 0;
    int rc;
    while ((rc = gif_next_frame(&g, big_canvas)) == GIF_OK) {
        frames++;
        total_ms += g.delay_ms;
        if (frames > 4096) {
            break;      /* a stream that never ends is a failure, not a loop */
        }
    }

    /* GIF_END, not GIF_EDATA: every byte from the header to the trailer was
     * understood. */
    CU_ASSERT_EQUAL(rc, GIF_END);
    CU_ASSERT_TRUE(frames > 1);
    /* A real animation, not a still with a stuck delay field. */
    CU_ASSERT_TRUE(total_ms > 0);
    CU_ASSERT_TRUE(total_ms < 60000);
}

static void test_real_module_is_not_a_blank_screen(void) {
    /* The failure this catches is a decoder that returns GIF_OK for every
     * frame and draws nothing: all the pass/fail above would still pass. */
    const uint8_t *data = NULL;
    size_t len = 0;
    CU_ASSERT_EQUAL(find_module(&data, &len), 0);
    CU_ASSERT_EQUAL(gif_open(&g, data, len), GIF_OK);

    gif_canvas_clear(&g, big_canvas);
    CU_ASSERT_EQUAL(gif_next_frame(&g, big_canvas), GIF_OK);

    size_t pixels = (size_t)g.width * (size_t)g.height;
    uint8_t first = big_canvas[0];
    size_t differing = 0;
    for (size_t i = 0; i < pixels; i++) {
        if (big_canvas[i] != first) {
            differing++;
        }
    }
    /* Any real photographic frame is overwhelmingly not one flat colour. */
    CU_ASSERT_TRUE(differing > pixels / 10);
}

void suite_gif_tests(CU_pSuite s) {
    CU_add_test(s, "open reads the logical screen",
                test_open_reads_the_logical_screen);
    CU_add_test(s, "open rejects what is not a GIF",
                test_open_rejects_what_is_not_a_gif);
    CU_add_test(s, "open rejects a screen bigger than the canvas",
                test_open_rejects_a_screen_bigger_than_the_canvas);
    CU_add_test(s, "the first frame decodes pixel for pixel",
                test_first_frame_decodes_pixel_for_pixel);
    CU_add_test(s, "the second frame replaces the first",
                test_second_frame_replaces_the_first);
    CU_add_test(s, "the trailer ends the pass",
                test_trailer_ends_the_pass);
    CU_add_test(s, "rewind replays the same frames",
                test_rewind_replays_the_same_frames);
    CU_add_test(s, "interlaced rows land in order",
                test_interlaced_rows_land_in_order);
    CU_add_test(s, "truncated pixel data is EDATA, not a hang",
                test_truncated_pixel_data_is_edata_not_a_hang);

    CU_add_test(s, "the real animation module is loaded",
                test_real_module_is_loaded);
    CU_add_test(s, "the real animation decodes every frame",
                test_real_module_decodes_every_frame);
    CU_add_test(s, "the real animation is not a blank screen",
                test_real_module_is_not_a_blank_screen);
}
