#include "gif.h"
#include "string.h"

/* ── LZW dictionary ────────────────────────────────────────────────────────
 *
 * File scope, not locals: 4096 prefixes + 4096 suffixes + a 4096-byte unwind
 * stack is ~12 KiB, and the kernel's boot stack is 16 KiB total (src/boot.s).
 * The decoder is single-threaded by construction -- it runs from kernel_main
 * before the shell LibOS exists -- so there is nothing to make reentrant.
 */
static uint16_t lzw_prefix[4096];
static uint8_t  lzw_suffix[4096];
static uint8_t  lzw_stack[4096];

/* ── Sub-block reader ──────────────────────────────────────────────────────
 *
 * GIF pixel data is not one run of bytes: it is a chain of sub-blocks, each
 * introduced by a length byte and terminated by a zero-length one.  The bit
 * reader below sits on top of this so the LZW codes, which straddle sub-block
 * boundaries freely, never have to know the chain exists.
 */
typedef struct {
    const uint8_t *data;
    size_t         size;
    size_t         pos;       /* next unread byte in the file */
    uint8_t        left;      /* bytes still unread in the current sub-block */
    uint8_t        done;      /* terminator seen (or the stream ran out) */

    uint32_t       bits;      /* bit accumulator, LSB first */
    uint8_t        nbits;
} gif_bits_t;

static void bits_init(gif_bits_t *b, const uint8_t *data, size_t size, size_t pos) {
    b->data  = data;
    b->size  = size;
    b->pos   = pos;
    b->left  = 0;
    b->done  = 0;
    b->bits  = 0;
    b->nbits = 0;
}

/* Next data byte of the sub-block chain, or -1 once it ends. */
static int bits_next_byte(gif_bits_t *b) {
    while (b->left == 0) {
        if (b->done || b->pos >= b->size) {
            b->done = 1;
            return -1;
        }
        b->left = b->data[b->pos++];
        if (b->left == 0) {       /* block terminator: the chain is over */
            b->done = 1;
            return -1;
        }
    }
    if (b->pos >= b->size) {
        b->done = 1;
        return -1;
    }
    b->left--;
    return (int)b->data[b->pos++];
}

/* Read `n` bits (n <= 12), or -1 if the stream ended mid-code. */
static int bits_get(gif_bits_t *b, uint8_t n) {
    while (b->nbits < n) {
        int byte = bits_next_byte(b);
        if (byte < 0) {
            return -1;
        }
        b->bits |= (uint32_t)byte << b->nbits;
        b->nbits = (uint8_t)(b->nbits + 8);
    }
    int val = (int)(b->bits & ((1u << n) - 1u));
    b->bits >>= n;
    b->nbits = (uint8_t)(b->nbits - n);
    return val;
}

/* Walk the reader past whatever is left of the chain, so the caller's cursor
 * lands on the block that follows the image.  Called even on a decode error:
 * one corrupt frame should not cost the frames after it. */
static size_t bits_finish(gif_bits_t *b) {
    while (!b->done) {
        if (bits_next_byte(b) < 0) {
            break;
        }
        /* Skip the rest of this sub-block in one step. */
        if (b->left > 0) {
            size_t skip = b->left;
            if (skip > b->size - b->pos) {
                skip = b->size - b->pos;
            }
            b->pos += skip;
            b->left = 0;
        }
    }
    return b->pos;
}

/* ── Header ───────────────────────────────────────────────────────────────*/

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

int gif_open(gif_t *g, const uint8_t *data, size_t size) {
    if (g == NULL || data == NULL || size < 13) {
        return GIF_EINVAL;
    }
    if (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0) {
        return GIF_EINVAL;
    }

    memset(g, 0, sizeof(*g));
    g->data        = data;
    g->size        = size;
    g->width       = rd16(data + 6);
    g->height      = rd16(data + 8);
    g->bg_index    = data[11];
    g->transparent = -1;

    if (g->width == 0 || g->height == 0) {
        return GIF_EINVAL;
    }
    if (g->width > GIF_MAX_WIDTH || g->height > GIF_MAX_HEIGHT) {
        return GIF_ETOOBIG;
    }

    size_t pos = 13;
    uint8_t flags = data[10];
    if (flags & 0x80) {
        uint16_t colors = (uint16_t)(1u << ((flags & 0x07) + 1));
        if (pos + (size_t)colors * 3u > size) {
            return GIF_EINVAL;
        }
        memcpy(g->gct, data + pos, (size_t)colors * 3u);
        g->gct_colors = colors;
        pos += (size_t)colors * 3u;
    }

    /* A file with no global table and no local table has no colours at all;
     * that is caught per frame, since a local table is still legal here. */
    memcpy(g->palette, g->gct, sizeof(g->palette));
    g->palette_colors = g->gct_colors;

    g->pos         = pos;
    g->first_block = pos;
    return GIF_OK;
}

void gif_rewind(gif_t *g) {
    if (g == NULL) {
        return;
    }
    g->pos         = g->first_block;
    g->disposal    = 0;
    g->transparent = -1;
    g->fw          = 0;
    g->fh          = 0;
}

void gif_canvas_clear(gif_t *g, uint8_t *canvas) {
    if (g == NULL || canvas == NULL) {
        return;
    }
    memset(canvas, g->bg_index, (size_t)g->width * (size_t)g->height);
    g->disposal = 0;
    g->fw       = 0;
    g->fh       = 0;
}

/* ── Frame decode ─────────────────────────────────────────────────────────*/

/* Apply the previous frame's disposal before the next one is drawn.  Method 2
 * ("restore to background") is the only one with work to do: method 0/1 leave
 * the canvas alone, and method 3 is deliberately treated as 0 (see gif.h). */
static void apply_disposal(gif_t *g, uint8_t *canvas) {
    if (g->disposal != 2 || g->fw == 0 || g->fh == 0) {
        return;
    }
    /* The background index, not the transparent one: a browser restores to
     * "whatever is behind the image", but here there is nothing behind it. */
    uint8_t fill = g->bg_index;
    for (uint16_t y = 0; y < g->fh; y++) {
        uint32_t row = (uint32_t)(g->fy + y) * g->width + g->fx;
        memset(canvas + row, fill, g->fw);
    }
}

/* Skip an extension's sub-block chain, leaving the cursor after it. */
static size_t skip_blocks(const uint8_t *data, size_t size, size_t pos) {
    while (pos < size) {
        uint8_t len = data[pos++];
        if (len == 0) {
            break;
        }
        pos += len;
    }
    return (pos > size) ? size : pos;
}

static int decode_image(gif_t *g, uint8_t *canvas) {
    const uint8_t *d = g->data;
    size_t pos = g->pos;

    if (pos + 10 > g->size) {
        return GIF_EDATA;
    }
    uint16_t x = rd16(d + pos + 1);
    uint16_t y = rd16(d + pos + 3);
    uint16_t w = rd16(d + pos + 5);
    uint16_t h = rd16(d + pos + 7);
    uint8_t  flags = d[pos + 9];
    pos += 10;

    /* Clip to the logical screen rather than refusing: a frame overhanging
     * the canvas is a thing real encoders emit, and the rest of it is still
     * worth drawing. */
    if (x >= g->width || y >= g->height) {
        w = 0;
        h = 0;
    } else {
        if ((uint32_t)x + w > g->width)  w = (uint16_t)(g->width  - x);
        if ((uint32_t)y + h > g->height) h = (uint16_t)(g->height - y);
    }

    if (flags & 0x80) {                       /* local colour table */
        uint16_t colors = (uint16_t)(1u << ((flags & 0x07) + 1));
        if (pos + (size_t)colors * 3u > g->size) {
            return GIF_EDATA;
        }
        memcpy(g->palette, d + pos, (size_t)colors * 3u);
        g->palette_colors = colors;
        pos += (size_t)colors * 3u;
    } else {
        memcpy(g->palette, g->gct, sizeof(g->palette));
        g->palette_colors = g->gct_colors;
    }
    if (g->palette_colors == 0) {
        return GIF_EDATA;                     /* no table anywhere: undrawable */
    }

    int interlaced = (flags & 0x40) != 0;

    if (pos >= g->size) {
        return GIF_EDATA;
    }
    uint8_t min_code_size = d[pos++];
    if (min_code_size < 1 || min_code_size > 11) {
        return GIF_EDATA;
    }

    const uint16_t clear_code = (uint16_t)(1u << min_code_size);
    const uint16_t end_code   = (uint16_t)(clear_code + 1);

    gif_bits_t bits;
    bits_init(&bits, d, g->size, pos);

    uint16_t code_size = (uint16_t)(min_code_size + 1);
    uint16_t next      = (uint16_t)(end_code + 1);
    int      prev      = -1;
    uint8_t  first     = 0;

    /* Where the next pixel lands.  `row` counts rows of the frame rect; for
     * an interlaced image it advances in the four-pass order instead of by
     * one, which is the only difference interlacing makes here. */
    uint16_t col = 0, row = 0;
    uint8_t  pass = 0;
    static const uint8_t pass_start[4] = { 0, 4, 2, 1 };
    static const uint8_t pass_step[4]  = { 8, 8, 4, 2 };

    int rc = GIF_OK;

    for (;;) {
        int code = bits_get(&bits, (uint8_t)code_size);
        if (code < 0) {
            rc = GIF_EDATA;
            break;
        }
        if (code == clear_code) {
            code_size = (uint16_t)(min_code_size + 1);
            next      = (uint16_t)(end_code + 1);
            prev      = -1;
            continue;
        }
        if (code == end_code) {
            break;
        }
        if (prev < 0 && code >= clear_code) {
            rc = GIF_EDATA;                   /* first code must be a root */
            break;
        }

        int incode = code;
        int sp = 0;

        if (code >= next) {
            if (prev < 0) {
                rc = GIF_EDATA;
                break;
            }
            lzw_stack[sp++] = first;
            code = prev;
        }
        /* Unwind the chain.  The bound is belt and braces: a well-formed
         * dictionary is acyclic, and a malformed one must not spin here. */
        int guard = 4096;
        while (code >= clear_code && guard-- > 0) {
            lzw_stack[sp++] = lzw_suffix[code];
            code = lzw_prefix[code];
        }
        if (guard <= 0 || code >= clear_code) {
            rc = GIF_EDATA;
            break;
        }
        first = (uint8_t)code;
        lzw_stack[sp++] = first;

        while (sp > 0) {
            uint8_t px = lzw_stack[--sp];
            if (row < h) {
                if (g->transparent < 0 || px != (uint8_t)g->transparent) {
                    canvas[(uint32_t)(y + row) * g->width + (x + col)] = px;
                }
            }
            if (++col >= w) {
                col = 0;
                if (interlaced) {
                    row = (uint16_t)(row + pass_step[pass]);
                    while (row >= h && pass < 3) {
                        pass++;
                        row = pass_start[pass];
                    }
                } else {
                    row++;
                }
            }
            if (w == 0) {                     /* fully clipped frame */
                break;
            }
        }

        if (prev >= 0 && next < 4096) {
            lzw_prefix[next] = (uint16_t)prev;
            lzw_suffix[next] = first;
            next++;
            if (next == (1u << code_size) && code_size < 12) {
                code_size++;
            }
        }
        prev = incode;
    }

    g->pos = bits_finish(&bits);
    g->fx = x;
    g->fy = y;
    g->fw = w;
    g->fh = h;
    return rc;
}

int gif_next_frame(gif_t *g, uint8_t *canvas) {
    if (g == NULL || canvas == NULL || g->data == NULL) {
        return GIF_EINVAL;
    }

    for (;;) {
        if (g->pos >= g->size) {
            return GIF_END;                   /* missing trailer: treat as one */
        }

        uint8_t block = g->data[g->pos];

        if (block == 0x3B) {                  /* trailer */
            return GIF_END;
        }

        if (block == 0x21) {                  /* extension */
            if (g->pos + 2 > g->size) {
                return GIF_EDATA;
            }
            uint8_t label = g->data[g->pos + 1];
            size_t  pos   = g->pos + 2;

            if (label == 0xF9 && pos < g->size && g->data[pos] == 4 &&
                pos + 5 <= g->size) {
                /* Graphic control: disposal + delay + transparency for the
                 * image that follows.  Delay is in hundredths of a second. */
                uint8_t gflags = g->data[pos + 1];
                g->delay_ms    = (uint16_t)(rd16(g->data + pos + 2) * 10u);
                g->disposal    = (uint8_t)((gflags >> 2) & 0x07);
                g->transparent = (gflags & 0x01) ? (int16_t)g->data[pos + 4]
                                                 : (int16_t)-1;
            }
            g->pos = skip_blocks(g->data, g->size, pos);
            continue;
        }

        if (block == 0x2C) {                  /* image descriptor */
            apply_disposal(g, canvas);
            return decode_image(g, canvas);
        }

        return GIF_EDATA;                     /* nothing else is legal here */
    }
}
