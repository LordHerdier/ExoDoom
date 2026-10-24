/*
 * test_exo_slides_k.c — the slide-deck format (src/exo_slides.h): RLE
 * round-trips, the exact packet bytes the encoder emits, and every rejection
 * exo_slides_open()/exo_slides_decode() document.
 *
 * Pure buffer logic, so (NULL, NULL) like "exo_pkg". Buffers are static:
 * the boot stack is 16 KiB.
 */

#include "kunit.h"

#include "exo_slides.h"
#include "string.h"

#include <stddef.h>
#include <stdint.h>

#define T_W 7u
#define T_H 3u
#define T_PIXELS (T_W * T_H)
#define T_STRIDE (T_W + 2u)

static uint8_t  t_rgb[2][T_PIXELS * 3];
static uint8_t  t_deck[1024];
static uint32_t t_px[T_STRIDE * T_H];

/* Slide 0: flat runs that cross row boundaries plus a few lone pixels.
 * Slide 1: every pixel different (all literals). */
static void fill_fixture(void)
{
    for (uint32_t i = 0; i < T_PIXELS; i++) {
        uint8_t v = (i < 10) ? 0x10 : (i < 13 ? (uint8_t)(0x40 + i) : 0x20);
        t_rgb[0][i * 3 + 0] = v;
        t_rgb[0][i * 3 + 1] = (uint8_t)(v + 1);
        t_rgb[0][i * 3 + 2] = (uint8_t)(v + 2);
        t_rgb[1][i * 3 + 0] = (uint8_t)i;
        t_rgb[1][i * 3 + 1] = (uint8_t)(i * 7);
        t_rgb[1][i * 3 + 2] = (uint8_t)(255 - i);
    }
}

/* Build a deck of `count` fixture slides into t_deck; returns its length. */
static size_t build_deck(uint32_t count)
{
    memset(t_deck, 0, sizeof(t_deck));
    exo_slides_header_t *h = (exo_slides_header_t *)t_deck;
    h->magic       = EXO_SLIDES_MAGIC;
    h->version     = EXO_SLIDES_VERSION;
    h->width       = T_W;
    h->height      = T_H;
    h->slide_count = count;
    h->index_off   = sizeof(exo_slides_header_t);

    exo_slides_index_t *idx = (exo_slides_index_t *)(t_deck + h->index_off);
    size_t off = h->index_off + count * sizeof(exo_slides_index_t);
    for (uint32_t n = 0; n < count; n++) {
        size_t got = exo_slides_encode(t_rgb[n], T_W, T_H, t_deck + off,
                                       sizeof(t_deck) - off);
        idx[n].offset = (uint32_t)off;
        idx[n].length = (uint32_t)got;
        off += got;
    }
    return off;
}

static int slide_matches(uint32_t n)
{
    for (uint32_t y = 0; y < T_H; y++) {
        for (uint32_t x = 0; x < T_STRIDE; x++) {
            uint32_t v = t_px[y * T_STRIDE + x];
            if (x >= T_W) {
                if (v != 0xA5A5A5A5u) return 0;   /* padding untouched */
                continue;
            }
            const uint8_t *p = &t_rgb[n][(y * T_W + x) * 3];
            if (v != exo_slides_pixel(p[0], p[1], p[2])) return 0;
        }
    }
    return 1;
}

static void test_round_trip_two_slides(void)
{
    fill_fixture();
    size_t len = build_deck(2);

    exo_slides_t d = { 0 };
    CU_ASSERT_EQUAL(exo_slides_open(&d, t_deck, len), 0);
    CU_ASSERT_EQUAL(d.width, T_W);
    CU_ASSERT_EQUAL(d.height, T_H);
    CU_ASSERT_EQUAL(d.slide_count, 2);

    for (uint32_t n = 0; n < 2; n++) {
        memset(t_px, 0xA5, sizeof(t_px));
        CU_ASSERT_EQUAL(exo_slides_decode(&d, n, t_px, T_STRIDE), 0);
        CU_ASSERT_TRUE(slide_matches(n));
    }
}

static void test_pixel_is_bgrx(void)
{
    CU_ASSERT_EQUAL(exo_slides_pixel(0x12, 0x34, 0x56), 0x00123456u);
}

static void test_encode_exact_packets(void)
{
    /* AAAA B C -> repeat(4) A, literal(2) B C */
    static const uint8_t rgb[6 * 3] = {
        1, 2, 3,  1, 2, 3,  1, 2, 3,  1, 2, 3,  9, 9, 9,  7, 7, 7,
    };
    static const uint8_t want[] = { 0x83, 1, 2, 3, 0x01, 9, 9, 9, 7, 7, 7 };
    uint8_t out[32];
    size_t got = exo_slides_encode(rgb, 6, 1, out, sizeof(out));
    CU_ASSERT_EQUAL(got, sizeof(want));
    CU_ASSERT_EQUAL(memcmp(out, want, sizeof(want)), 0);
}

static void test_encode_splits_long_runs(void)
{
    /* 300 identical pixels: 128 + 128 + 44, four bytes each. */
    static uint8_t rgb[300 * 3];
    memset(rgb, 0x42, sizeof(rgb));
    uint8_t out[32];
    size_t got = exo_slides_encode(rgb, 300, 1, out, sizeof(out));
    CU_ASSERT_EQUAL(got, 12);
    CU_ASSERT_EQUAL(out[0], 0xFF);
    CU_ASSERT_EQUAL(out[4], 0xFF);
    CU_ASSERT_EQUAL(out[8], 0x80 | 43);
}

static void test_encode_reports_small_cap(void)
{
    fill_fixture();
    uint8_t out[8];
    CU_ASSERT_EQUAL(exo_slides_encode(t_rgb[1], T_W, T_H, out, sizeof(out)), 0);
    CU_ASSERT_TRUE(exo_slides_encode_bound(T_W, T_H) >= T_PIXELS * 3);
}

static void test_open_rejects_bad_headers(void)
{
    fill_fixture();
    size_t len = build_deck(1);
    exo_slides_header_t *h = (exo_slides_header_t *)t_deck;
    exo_slides_t d = { 0 };

    CU_ASSERT_EQUAL(exo_slides_open(&d, t_deck, 16), EXO_SLIDES_ESHORT);
    CU_ASSERT_EQUAL(exo_slides_open(NULL, t_deck, len), EXO_SLIDES_EINVAL);

    h->magic ^= 1;
    CU_ASSERT_EQUAL(exo_slides_open(&d, t_deck, len), EXO_SLIDES_EMAGIC);
    h->magic ^= 1;

    h->version = 2;
    CU_ASSERT_EQUAL(exo_slides_open(&d, t_deck, len), EXO_SLIDES_EVERSION);
    h->version = EXO_SLIDES_VERSION;

    h->width = 0;
    CU_ASSERT_EQUAL(exo_slides_open(&d, t_deck, len), EXO_SLIDES_EINVAL);
    h->width = EXO_SLIDES_MAX_DIM + 1;
    CU_ASSERT_EQUAL(exo_slides_open(&d, t_deck, len), EXO_SLIDES_EINVAL);
    h->width = T_W;

    h->slide_count = 0;
    CU_ASSERT_EQUAL(exo_slides_open(&d, t_deck, len), EXO_SLIDES_EINVAL);
    h->slide_count = 1;

    h->reserved[1] = 1;
    CU_ASSERT_EQUAL(exo_slides_open(&d, t_deck, len), EXO_SLIDES_EINVAL);
    h->reserved[1] = 0;

    CU_ASSERT_EQUAL(exo_slides_open(&d, t_deck, len), 0);
}

static void test_open_rejects_bad_index(void)
{
    fill_fixture();
    size_t len = build_deck(1);
    exo_slides_header_t *h = (exo_slides_header_t *)t_deck;
    exo_slides_index_t *idx = (exo_slides_index_t *)(t_deck + h->index_off);
    exo_slides_t d = { 0 };

    /* Index table runs past the buffer. */
    h->slide_count = 200;
    CU_ASSERT_EQUAL(exo_slides_open(&d, t_deck, len), EXO_SLIDES_ESHORT);
    h->slide_count = 1;

    /* Stream runs past the buffer. */
    CU_ASSERT_EQUAL(exo_slides_open(&d, t_deck, len - 1), EXO_SLIDES_ESHORT);

    /* Stream overlaps the header/index. */
    uint32_t off = idx[0].offset;
    idx[0].offset = 8;
    CU_ASSERT_EQUAL(exo_slides_open(&d, t_deck, len), EXO_SLIDES_EINVAL);
    idx[0].offset = off;

    uint32_t l = idx[0].length;
    idx[0].length = 0;
    CU_ASSERT_EQUAL(exo_slides_open(&d, t_deck, len), EXO_SLIDES_EINVAL);
    idx[0].length = l;
}

static void test_decode_rejects_bad_streams(void)
{
    fill_fixture();
    size_t len = build_deck(1);
    exo_slides_header_t *h = (exo_slides_header_t *)t_deck;
    exo_slides_index_t *idx = (exo_slides_index_t *)(t_deck + h->index_off);
    exo_slides_t d = { 0 };
    CU_ASSERT_EQUAL(exo_slides_open(&d, t_deck, len), 0);

    /* Truncated: under-runs the slide, or cuts a packet in half. */
    uint32_t l = idx[0].length;
    for (uint32_t cut = 1; cut < l; cut++) {
        idx[0].length = l - cut;
        memset(t_px, 0xA5, sizeof(t_px));
        CU_ASSERT_EQUAL(exo_slides_decode(&d, 0, t_px, T_STRIDE),
                        EXO_SLIDES_EDATA);
    }
    idx[0].length = l;

    /* One packet too many: over-runs the slide. Nothing past the slide's
     * own rows may be written. */
    static const uint8_t extra[4] = { 0x80, 1, 2, 3 };
    memcpy(t_deck + idx[0].offset + l, extra, sizeof(extra));
    idx[0].length = l + 4;
    CU_ASSERT_EQUAL(exo_slides_open(&d, t_deck, len + 4), 0);
    memset(t_px, 0xA5, sizeof(t_px));
    CU_ASSERT_EQUAL(exo_slides_decode(&d, 0, t_px, T_STRIDE), EXO_SLIDES_EDATA);
    CU_ASSERT_TRUE(slide_matches(0));
    idx[0].length = l;

    CU_ASSERT_EQUAL(exo_slides_decode(&d, 1, t_px, T_STRIDE), EXO_SLIDES_EINVAL);
    CU_ASSERT_EQUAL(exo_slides_decode(&d, 0, t_px, T_W - 1), EXO_SLIDES_EINVAL);
    CU_ASSERT_EQUAL(exo_slides_decode(&d, 0, NULL, T_STRIDE), EXO_SLIDES_EINVAL);
}

void suite_exo_slides_tests(CU_pSuite s)
{
    CU_add_test(s, "round-trips a two-slide deck with padded rows",
                test_round_trip_two_slides);
    CU_add_test(s, "decoded pixels are BGRX8888", test_pixel_is_bgrx);
    CU_add_test(s, "encoder emits the documented packets",
                test_encode_exact_packets);
    CU_add_test(s, "encoder splits runs longer than 128",
                test_encode_splits_long_runs);
    CU_add_test(s, "encoder reports a too-small buffer",
                test_encode_reports_small_cap);
    CU_add_test(s, "open rejects bad headers", test_open_rejects_bad_headers);
    CU_add_test(s, "open rejects bad index entries", test_open_rejects_bad_index);
    CU_add_test(s, "decode rejects truncated and over-long streams",
                test_decode_rejects_bad_streams);
}
