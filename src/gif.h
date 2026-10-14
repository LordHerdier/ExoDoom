#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * gif — a minimal, allocation-free GIF87a/GIF89a decoder.
 *
 * It exists for the boot animation (src/boot_anim.c): the animation arrives
 * the same way the IWAD does -- as a GRUB multiboot 2 module, identity-mapped
 * by vmm_init() and reserved by page_alloc_init() -- so the bytes are already
 * in memory and the only thing missing is something that understands them.
 *
 * ── Why decode in the kernel at all ───────────────────────────────────────
 *
 * The alternative was converting the GIF to raw frames at build time and
 * objcopy'ing the result into the kernel image, the way every ring-3 LibOS
 * blob gets there.  That trades ~250 lines of C for a multi-megabyte .rodata
 * blob, a new image-tooling dependency in docker/Dockerfile.build, and a
 * conversion step whose output nothing in-tree can check.  Decoding keeps the
 * asset a plain .gif on the ISO, keeps the kernel image the size it was, and
 * -- because this file touches no hardware and allocates nothing -- is
 * drivable end to end from a ring-0 KUnit suite
 * (tests/kernel/test_gif_k.c).
 *
 * ── Allocation-free, and what that costs the caller ───────────────────────
 *
 * Nothing here calls kmalloc()/alloc_page(): the decoder writes palette
 * indices into a canvas the *caller* owns and reads straight out of the
 * mapped file.  A GIF frame is not a picture on its own -- it is a patch
 * composited onto whatever the previous frames left behind, which is why the
 * canvas is the caller's to keep across gif_next_frame() calls rather than
 * something returned per frame.  gif_canvas_clear() puts it in the state the
 * first frame expects, and must be called again before replaying from
 * gif_rewind().
 *
 * ── Deliberate omissions ──────────────────────────────────────────────────
 *
 * Disposal method 3 ("restore to previous") is treated as method 0 ("do not
 * dispose"), because honouring it means keeping a second full canvas for a
 * case no GIF this kernel ships uses.  Plain-text extensions (0x01) are
 * skipped like any other extension; no renderer has drawn them since the
 * 1980s.  Everything else in the format -- interlacing, local colour tables,
 * transparency, the clear/EOI codes and mid-stream dictionary resets -- is
 * handled, because a GIF in the wild will use it without warning.
 */

#define GIF_OK        0
#define GIF_END       1  /* trailer reached: no more frames this pass */
#define GIF_EINVAL   -1  /* not a GIF, or a header this decoder refuses */
#define GIF_ETOOBIG  -2  /* larger than GIF_MAX_WIDTH x GIF_MAX_HEIGHT */
#define GIF_EDATA    -3  /* truncated or malformed past the header */

/* Canvas ceiling.  The canvas is one byte per pixel and the only sizeable
 * thing in this subsystem, so the cap is what bounds its .bss cost (see
 * src/boot_anim.c's g_canvas).  Raising it costs width*height bytes of
 * permanently resident memory, so raise it only for an asset that needs it. */
#define GIF_MAX_WIDTH    768u
#define GIF_MAX_HEIGHT   768u
#define GIF_CANVAS_BYTES (GIF_MAX_WIDTH * GIF_MAX_HEIGHT)

typedef struct {
    /* ── the file ── */
    const uint8_t *data;
    size_t         size;
    size_t         pos;          /* cursor: start of the next block */
    size_t         first_block;  /* where gif_rewind() returns the cursor to */

    /* ── logical screen, from the header ── */
    uint16_t width;
    uint16_t height;
    uint8_t  bg_index;
    uint8_t  gct[768];           /* global colour table, RGB triples */
    uint16_t gct_colors;         /* 0 when the file has no global table */

    /* ── state of the frame gif_next_frame() last returned ── */
    uint8_t  palette[768];       /* the table that frame was drawn with */
    uint16_t palette_colors;
    uint16_t delay_ms;           /* how long to show it; 0 if unspecified */
    int16_t  transparent;        /* transparent index, or -1 */

    /* ── carried between frames ── */
    uint8_t  disposal;           /* pending disposal of the last frame */
    uint16_t fx, fy, fw, fh;     /* that frame's rect, for the disposal */
} gif_t;

/*
 * Parse the header of [data, size) and leave the cursor at the first block.
 * Rejects anything whose logical screen exceeds GIF_MAX_WIDTH/_HEIGHT with
 * GIF_ETOOBIG, since the caller's canvas is sized by those constants.
 */
int gif_open(gif_t *g, const uint8_t *data, size_t size);

/*
 * Fill `canvas` (g->width * g->height bytes) with the background, and reset
 * the disposal state.  Call before the first gif_next_frame() of a pass.
 */
void gif_canvas_clear(gif_t *g, uint8_t *canvas);

/*
 * Composite the next frame onto `canvas` and describe it in g->palette /
 * g->delay_ms.  Returns GIF_OK with a frame drawn, GIF_END at the trailer
 * (canvas untouched), or a negative code if the stream is malformed -- in
 * which case the canvas may hold a partly drawn frame, which is strictly
 * better than a black screen for an animation nobody can debug from.
 */
int gif_next_frame(gif_t *g, uint8_t *canvas);

/* Return the cursor to the first block, so the next gif_next_frame() decodes
 * frame 0 again.  The caller is responsible for gif_canvas_clear(). */
void gif_rewind(gif_t *g);
