/*
 * mkslides -- host-side packer for the ExoDoom slide-deck format
 * (src/exo_slides.h).
 *
 * Input is one binary PPM (P6, maxval 255) per slide, in presentation
 * order, all the same size; tools/mkslides/pptx2exs.sh produces them from a
 * .pptx or .pdf with LibreOffice and pdftoppm. Output is one .exs file the
 * slides LibOS (src/libos_slides/) reads off the program volume. The RLE
 * coder is exo_slides_encode() from the shared header, not a second copy,
 * so a format change there changes this tool with it.
 *
 * Hosted C, built without "-I src" for the same reason as tools/exopkg (the
 * project's libc shims would shadow <stdio.h>); exo_slides.h depends on
 * nothing but <stddef.h>/<stdint.h>.
 *
 * usage: mkslides <out.exs> <slide.ppm>...
 *        mkslides --check <deck.exs>
 *   --check opens the deck with exo_slides_open() and decodes every slide
 *   with exo_slides_decode(), exactly as the viewer will; build.sh runs it
 *   over every deck it puts on the disk image, so a bad deck fails the build
 *   rather than the talk.
 */

#include "../../src/exo_slides.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        perror(path);
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        perror(path);
        fclose(f);
        return NULL;
    }
    unsigned char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "mkslides: could not read %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* Skip whitespace and '#' comments between PPM header fields. */
static size_t ppm_skip(const unsigned char *b, size_t len, size_t i)
{
    while (i < len) {
        if (b[i] == '#') {
            while (i < len && b[i] != '\n')
                i++;
        } else if (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n') {
            i++;
        } else {
            break;
        }
    }
    return i;
}

static int ppm_uint(const unsigned char *b, size_t len, size_t *i, unsigned *out)
{
    *i = ppm_skip(b, len, *i);
    unsigned v = 0;
    size_t start = *i;
    while (*i < len && b[*i] >= '0' && b[*i] <= '9' && v < 100000u) {
        v = v * 10u + (unsigned)(b[*i] - '0');
        (*i)++;
    }
    if (*i == start)
        return -1;
    *out = v;
    return 0;
}

/* Parse a P6 file; *rgb points into buf at the pixel data. */
static int ppm_parse(const char *path, const unsigned char *buf, size_t len,
                     unsigned *w, unsigned *h, const unsigned char **rgb)
{
    size_t i = 2;
    unsigned maxval;
    if (len < 2 || buf[0] != 'P' || buf[1] != '6' ||
        ppm_uint(buf, len, &i, w) != 0 || ppm_uint(buf, len, &i, h) != 0 ||
        ppm_uint(buf, len, &i, &maxval) != 0 || i >= len) {
        fprintf(stderr, "mkslides: %s: not a binary PPM (P6)\n", path);
        return -1;
    }
    if (maxval != 255) {
        fprintf(stderr, "mkslides: %s: maxval %u, need 255\n", path, maxval);
        return -1;
    }
    i++; /* the single whitespace byte before the raster */
    if (*w == 0 || *h == 0 || *w > EXO_SLIDES_MAX_DIM || *h > EXO_SLIDES_MAX_DIM) {
        fprintf(stderr, "mkslides: %s: %ux%u is outside 1..%u\n", path, *w, *h,
                EXO_SLIDES_MAX_DIM);
        return -1;
    }
    if (len - i < (size_t)*w * *h * 3u) {
        fprintf(stderr, "mkslides: %s: truncated raster\n", path);
        return -1;
    }
    *rgb = buf + i;
    return 0;
}

static int check_deck(const char *path)
{
    size_t len;
    unsigned char *buf = read_file(path, &len);
    if (!buf)
        return 1;

    exo_slides_t d;
    int rc = exo_slides_open(&d, buf, len);
    if (rc != 0) {
        fprintf(stderr, "mkslides: %s: exo_slides_open failed: %d\n", path, rc);
        return 1;
    }

    uint32_t *px = malloc((size_t)d.width * d.height * sizeof(uint32_t));
    if (!px) {
        fprintf(stderr, "mkslides: out of memory\n");
        return 1;
    }
    for (uint32_t n = 0; n < d.slide_count; n++) {
        rc = exo_slides_decode(&d, n, px, d.width);
        if (rc != 0) {
            fprintf(stderr, "mkslides: %s: slide %u: decode failed: %d\n",
                    path, n + 1, rc);
            return 1;
        }
    }

    printf("    mkslides: %s: %u slides, %ux%u, %zu bytes -- OK\n", path,
           d.slide_count, d.width, d.height, len);
    free(px);
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--check") == 0)
        return check_deck(argv[2]);

    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <out.exs> <slide.ppm>...\n"
                "       %s --check <deck.exs>\n",
                argv[0], argv[0]);
        return 1;
    }

    uint32_t count = (uint32_t)(argc - 2);
    if (count > EXO_SLIDES_MAX_SLIDES) {
        fprintf(stderr, "mkslides: %u slides, at most %u\n", count,
                EXO_SLIDES_MAX_SLIDES);
        return 1;
    }

    unsigned w = 0, h = 0;
    size_t bound = 0;
    uint8_t *scratch = NULL;

    /* Header + index first, streams appended in order. */
    size_t index_off = sizeof(exo_slides_header_t);
    size_t out_len = index_off + count * sizeof(exo_slides_index_t);
    uint8_t *out = calloc(1, out_len);
    if (!out) {
        fprintf(stderr, "mkslides: out of memory\n");
        return 1;
    }

    for (uint32_t n = 0; n < count; n++) {
        const char *path = argv[2 + n];
        size_t len;
        unsigned char *buf = read_file(path, &len);
        if (!buf)
            return 1;

        unsigned sw, sh;
        const unsigned char *rgb;
        if (ppm_parse(path, buf, len, &sw, &sh, &rgb) != 0)
            return 1;
        if (n == 0) {
            w = sw;
            h = sh;
            bound = exo_slides_encode_bound(w, h);
            scratch = malloc(bound);
            if (!scratch) {
                fprintf(stderr, "mkslides: out of memory\n");
                return 1;
            }
        } else if (sw != w || sh != h) {
            fprintf(stderr, "mkslides: %s is %ux%u, slide 1 is %ux%u\n", path,
                    sw, sh, w, h);
            return 1;
        }

        size_t got = exo_slides_encode(rgb, w, h, scratch, bound);
        if (got == 0 || out_len + got > UINT32_MAX) {
            fprintf(stderr, "mkslides: %s: encode failed\n", path);
            return 1;
        }

        uint8_t *grown = realloc(out, out_len + got);
        if (!grown) {
            fprintf(stderr, "mkslides: out of memory\n");
            return 1;
        }
        out = grown;
        exo_slides_index_t *idx = (exo_slides_index_t *)(out + index_off);
        idx[n].offset = (uint32_t)out_len;
        idx[n].length = (uint32_t)got;
        memcpy(out + out_len, scratch, got);
        out_len += got;
        free(buf);
    }

    exo_slides_header_t *hdr = (exo_slides_header_t *)out;
    hdr->magic       = EXO_SLIDES_MAGIC;
    hdr->version     = EXO_SLIDES_VERSION;
    hdr->width       = (uint16_t)w;
    hdr->height      = (uint16_t)h;
    hdr->slide_count = count;
    hdr->index_off   = (uint32_t)index_off;

    FILE *f = fopen(argv[1], "wb");
    if (!f || fwrite(out, 1, out_len, f) != out_len || fclose(f) != 0) {
        fprintf(stderr, "mkslides: could not write %s\n", argv[1]);
        return 1;
    }

    printf("    mkslides: %s: %u slides, %ux%u, %zu bytes (raw %zu)\n", argv[1],
           count, w, h, out_len, (size_t)count * w * h * 3u);
    free(scratch);
    free(out);
    return 0;
}
