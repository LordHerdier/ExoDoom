#ifndef EXO_SLIDES_H
#define EXO_SLIDES_H

/*
 * exo_slides — the ExoDoom slide-deck format (".exs").
 *
 * A presentation is rendered to pixels on the build host
 * (tools/mkslides/), stored on the program volume like any other file, and
 * shown by the slides LibOS (src/libos_slides/). Nothing in ExoDoom parses
 * PowerPoint, PDF or PNG: by the time a deck is on the disk it is already
 * framebuffer-ready RGB, so the ring-3 side is a bounds-checked RLE decode
 * and a blit.
 *
 * Why RLE and not the GIF decoder from the fun/gif-boot branch: that decoder
 * is palette-indexed (256 colours a frame), and a slide is anti-aliased text
 * on flat fills -- quantising it bands the edges for no gain, while plain
 * run-length coding of 24-bit pixels already shrinks a mostly flat slide by
 * an order of magnitude and decodes in one pass with no tables.
 *
 * Header-only and restricted to <stddef.h>/<stdint.h>, for the same reason
 * as src/exo_pkg.h: the packer is hosted C built without "-I src", the
 * viewer is a ring-3 link target, and tests/kernel/test_exo_slides_k.c runs
 * the codec in ring 0. All three include this one file.
 *
 * Layout (all integers little-endian):
 *
 *   exo_slides_header_t                       (32 bytes)
 *   exo_slides_index_t[slide_count]           (8 bytes each, at index_off)
 *   slide streams                              (anywhere after the index)
 *
 * Each slide is one RLE stream covering width*height pixels in row-major
 * order; runs may cross row boundaries. A stream is a sequence of packets
 * that starts with a control byte c:
 *
 *   c & 0x80   repeat: one R,G,B triple follows, written (c & 0x7F) + 1 times
 *   otherwise  literal: c + 1 R,G,B triples follow, written once each
 *
 * so a packet covers 1..128 pixels. A stream must decode to exactly
 * width*height pixels and end exactly at its index entry's length; anything
 * else is EXO_SLIDES_EDATA.
 */

#include <stddef.h>
#include <stdint.h>

#define EXO_SLIDES_MAGIC   0x4C535845u /* "EXSL" read as a little-endian u32 */
#define EXO_SLIDES_VERSION 1u

/* Caps that keep every size computation inside 32 bits (4096*4096*4 is
 * 64 MiB) and refuse a header that is plainly garbage. */
#define EXO_SLIDES_MAX_DIM    4096u
#define EXO_SLIDES_MAX_SLIDES 1024u

/* Negative sentinels, not -EXO_E*: this header also builds on the host. */
#define EXO_SLIDES_EMAGIC   (-1) /* not an .exs file                         */
#define EXO_SLIDES_EVERSION (-2) /* a version this code does not read        */
#define EXO_SLIDES_ESHORT   (-3) /* buffer shorter than header/index/a slide */
#define EXO_SLIDES_EINVAL   (-4) /* bad dimension, count, index, or argument */
#define EXO_SLIDES_EDATA    (-5) /* a stream over- or under-runs its slide   */

typedef struct {
    uint32_t magic;        /* EXO_SLIDES_MAGIC                       */
    uint16_t version;      /* EXO_SLIDES_VERSION                     */
    uint16_t reserved0;    /* 0                                      */
    uint16_t width;        /* pixels, 1..EXO_SLIDES_MAX_DIM          */
    uint16_t height;       /* pixels, 1..EXO_SLIDES_MAX_DIM          */
    uint32_t slide_count;  /* 1..EXO_SLIDES_MAX_SLIDES               */
    uint32_t index_off;    /* byte offset of the index table         */
    uint32_t reserved[3];  /* 0                                      */
} exo_slides_header_t;

typedef struct {
    uint32_t offset;       /* byte offset of the slide's RLE stream  */
    uint32_t length;       /* stream length in bytes                 */
} exo_slides_index_t;

_Static_assert(sizeof(exo_slides_header_t) == 32, "exo_slides_header_t is 32 bytes");
_Static_assert(sizeof(exo_slides_index_t) == 8, "exo_slides_index_t is 8 bytes");

/* A validated deck: points into the caller's buffer, copies nothing. */
typedef struct {
    const uint8_t            *base;
    size_t                    len;
    uint32_t                  width;
    uint32_t                  height;
    uint32_t                  slide_count;
    const exo_slides_index_t *index;
} exo_slides_t;

/* Pack one pixel the way the framebuffer stores it: BGRX8888, i.e. the u32
 * 0x00RRGGBB (pack_bgrx8888() in src/fb.c). */
static inline uint32_t exo_slides_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Validate the header and every index entry of [buf, buf + len) and fill
 * `out`. Each slide's stream is only checked for bounds here; its contents
 * are checked by exo_slides_decode(). */
static inline int exo_slides_open(exo_slides_t *out, const void *buf, size_t len)
{
    if (out == NULL || buf == NULL) {
        return EXO_SLIDES_EINVAL;
    }
    if (len < sizeof(exo_slides_header_t)) {
        return EXO_SLIDES_ESHORT;
    }

    const exo_slides_header_t *h = (const exo_slides_header_t *)buf;
    if (h->magic != EXO_SLIDES_MAGIC) {
        return EXO_SLIDES_EMAGIC;
    }
    if (h->version != EXO_SLIDES_VERSION) {
        return EXO_SLIDES_EVERSION;
    }
    if (h->reserved0 != 0 || h->reserved[0] != 0 || h->reserved[1] != 0 ||
        h->reserved[2] != 0 ||
        h->width == 0 || h->width > EXO_SLIDES_MAX_DIM ||
        h->height == 0 || h->height > EXO_SLIDES_MAX_DIM ||
        h->slide_count == 0 || h->slide_count > EXO_SLIDES_MAX_SLIDES ||
        h->index_off < sizeof(exo_slides_header_t) || (h->index_off & 3u) != 0) {
        return EXO_SLIDES_EINVAL;
    }

    uint64_t index_end = (uint64_t)h->index_off +
                         (uint64_t)h->slide_count * sizeof(exo_slides_index_t);
    if (index_end > len) {
        return EXO_SLIDES_ESHORT;
    }

    const exo_slides_index_t *idx =
        (const exo_slides_index_t *)((const uint8_t *)buf + h->index_off);
    for (uint32_t i = 0; i < h->slide_count; i++) {
        if (idx[i].length == 0 || idx[i].offset < index_end) {
            return EXO_SLIDES_EINVAL;
        }
        if ((uint64_t)idx[i].offset + idx[i].length > len) {
            return EXO_SLIDES_ESHORT;
        }
    }

    out->base        = (const uint8_t *)buf;
    out->len         = len;
    out->width       = h->width;
    out->height      = h->height;
    out->slide_count = h->slide_count;
    out->index       = idx;
    return 0;
}

/*
 * Decode slide `n` into `dst`, a width x height block of BGRX8888 pixels
 * whose rows are `stride` pixels apart (stride >= width). Returns 0, or
 * EXO_SLIDES_EDATA if the stream is malformed -- in which case `dst` may hold
 * part of the slide, but nothing outside the width x height block was
 * written.
 */
static inline int exo_slides_decode(const exo_slides_t *d, uint32_t n,
                                    uint32_t *dst, uint32_t stride)
{
    if (d == NULL || dst == NULL || n >= d->slide_count || stride < d->width) {
        return EXO_SLIDES_EINVAL;
    }

    const uint8_t *p   = d->base + d->index[n].offset;
    const uint8_t *end = p + d->index[n].length;
    uint32_t x = 0, y = 0;
    uint32_t *row = dst;

    while (p < end) {
        uint8_t  c     = *p++;
        uint32_t count = (uint32_t)(c & 0x7Fu) + 1u;
        int      run   = (c & 0x80u) != 0;

        if ((size_t)(end - p) < (run ? 3u : 3u * count)) {
            return EXO_SLIDES_EDATA;
        }
        uint32_t px = run ? exo_slides_pixel(p[0], p[1], p[2]) : 0;

        for (uint32_t i = 0; i < count; i++) {
            if (y >= d->height) {
                return EXO_SLIDES_EDATA;
            }
            if (!run) {
                px = exo_slides_pixel(p[0], p[1], p[2]);
                p += 3;
            }
            row[x] = px;
            if (++x == d->width) {
                x = 0;
                y++;
                row += stride;
            }
        }
        if (run) {
            p += 3;
        }
    }

    return (y == d->height && x == 0) ? 0 : EXO_SLIDES_EDATA;
}

/* Worst-case encoded size of one slide: every pixel a literal, one control
 * byte per 128 of them. */
static inline size_t exo_slides_encode_bound(uint32_t width, uint32_t height)
{
    size_t n = (size_t)width * height;
    return n * 3u + (n + 127u) / 128u;
}

/*
 * RLE-encode `width * height` pixels of packed R,G,B bytes (row-major, no
 * row padding -- a binary PPM's pixel data as-is) into `out`. Returns the
 * stream length, or 0 if `cap` is too small. Used by the host packer and by
 * the round-trip tests; the viewer never encodes.
 *
 * Greedy: any two or more equal pixels become a repeat packet, everything
 * else accumulates into literal packets.
 */
static inline size_t exo_slides_encode(const uint8_t *rgb, uint32_t width,
                                       uint32_t height, uint8_t *out, size_t cap)
{
    size_t n = (size_t)width * height;
    size_t i = 0, o = 0;

    while (i < n) {
        const uint8_t *px = rgb + i * 3u;
        size_t run = 1;
        while (i + run < n && run < 128u &&
               px[run * 3u] == px[0] && px[run * 3u + 1] == px[1] &&
               px[run * 3u + 2] == px[2]) {
            run++;
        }

        if (run >= 2) {
            if (o + 4u > cap) {
                return 0;
            }
            out[o++] = (uint8_t)(0x80u | (run - 1u));
            out[o++] = px[0];
            out[o++] = px[1];
            out[o++] = px[2];
            i += run;
            continue;
        }

        /* Literal: extend until the next pixel starts a run, or 128. */
        size_t lit = 1;
        while (i + lit < n && lit < 128u) {
            const uint8_t *a = rgb + (i + lit) * 3u;
            if (i + lit + 1 < n && a[0] == a[3] && a[1] == a[4] && a[2] == a[5]) {
                break;
            }
            lit++;
        }
        if (o + 1u + lit * 3u > cap) {
            return 0;
        }
        out[o++] = (uint8_t)(lit - 1u);
        for (size_t k = 0; k < lit * 3u; k++) {
            out[o++] = px[k];
        }
        i += lit;
    }

    return o;
}

#endif /* EXO_SLIDES_H */
