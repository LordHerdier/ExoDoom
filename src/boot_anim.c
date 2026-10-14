#include "boot_anim.h"

#include "gif.h"
#include "io.h"
#include "pit.h"
#include "serial.h"
#include "sleep.h"

/* The decode canvas: one byte of palette index per logical-screen pixel.
 * Static rather than kmalloc'd because this runs after page_alloc_init(),
 * where a bump allocation can alias a page the PMM has already handed out
 * (see CLAUDE.md / src/memory.c), and because 576 KiB is far past what the
 * 16 KiB boot stack could hold. */
static uint8_t g_canvas[GIF_CANVAS_BYTES];

/* One frame's palette, pre-packed into the framebuffer's pixel format so the
 * blit below is a table lookup rather than a shift/or per pixel. */
static uint32_t g_lut[256];

static gif_t g_gif;

static void build_lut(const gif_t *g) {
    for (uint32_t i = 0; i < 256; i++) {
        const uint8_t *c = &g->palette[i * 3];
        /* BGRX8888, matching pack_bgrx8888() in src/fb.c. */
        g_lut[i] = ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
    }
}

/*
 * Nearest-neighbour scale of the canvas onto `fb`, centred, aspect preserved.
 *
 * The step is 16.16 fixed point for the same reason everything else in this
 * tree is (src/fixed_math.h): the kernel compiles -mno-sse, so there is no
 * float to divide with.  Source columns are stepped rather than recomputed
 * per pixel because this runs ~10 times a second over ~600k destination
 * pixels of MMIO.
 */
static void blit(framebuffer_t *fb, const gif_t *g,
                 uint32_t dx0, uint32_t dy0, uint32_t dw, uint32_t dh) {
    uint32_t step_x = ((uint32_t)g->width  << 16) / dw;
    uint32_t step_y = ((uint32_t)g->height << 16) / dh;

    uint32_t sy = 0;
    for (uint32_t y = 0; y < dh; y++) {
        const uint8_t *src = &g_canvas[(sy >> 16) * (uint32_t)g->width];
        uint32_t *dst = (uint32_t *)(fb->addr + (dy0 + y) * fb->pitch + dx0 * 4);
        uint32_t sx = 0;
        for (uint32_t x = 0; x < dw; x++) {
            dst[x] = g_lut[src[sx >> 16]];
            sx += step_x;
        }
        sy += step_y;
    }
}

/* A byte waiting in the PS/2 controller's output buffer, i.e. somebody is
 * hitting a key.  Polled, not interrupt-driven: IRQ1 is still masked at the
 * PIC this early and kbd_init() has not run, so nothing else is competing for
 * port 0x60 -- and kbd_init() drains stale bytes itself when it does run. */
static int key_pressed(void) {
    if ((inb(0x64) & 0x01) == 0) {
        return 0;
    }
    (void)inb(0x60);
    return 1;
}

int boot_anim_play(framebuffer_t *fb, const uint8_t *data, size_t size,
                   uint32_t loops, uint32_t max_ms) {
    if (fb == NULL || fb->fmt != FB_PIXFMT_BGRX8888 || data == NULL || size == 0) {
        return BOOT_ANIM_ENOENT;
    }

    int rc = gif_open(&g_gif, data, size);
    if (rc != GIF_OK) {
        serial_print("boot_anim: not a usable GIF (rc=");
        serial_print_hex64((uint64_t)(int64_t)rc);
        serial_print(")\n");
        return BOOT_ANIM_EDECODE;
    }

    /* Fit-and-centre.  min() of the two scales preserves the aspect ratio;
     * the result is allowed to be an upscale, since a 498x477 asset on a
     * 1024x768 screen would otherwise sit in the middle looking like a
     * thumbnail. */
    uint32_t scale_x = ((uint32_t)fb->width  << 16) / g_gif.width;
    uint32_t scale_y = ((uint32_t)fb->height << 16) / g_gif.height;
    uint32_t scale   = (scale_x < scale_y) ? scale_x : scale_y;
    uint32_t dw = ((uint32_t)g_gif.width  * scale) >> 16;
    uint32_t dh = ((uint32_t)g_gif.height * scale) >> 16;
    if (dw == 0 || dh == 0) {
        return BOOT_ANIM_ENOENT;
    }
    if (dw > fb->width)  dw = fb->width;
    if (dh > fb->height) dh = fb->height;
    uint32_t dx0 = (fb->width  - dw) / 2;
    uint32_t dy0 = (fb->height - dh) / 2;

    /* Save IF and enable interrupts for the duration -- see boot_anim.h. */
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    __asm__ volatile ("sti");

    fb_clear(fb, 0, 0, 0);

    uint32_t start    = kernel_get_ticks_ms();
    uint32_t deadline = start;
    uint32_t frames   = 0;

    for (uint32_t loop = 0; loop < loops; loop++) {
        gif_canvas_clear(&g_gif, g_canvas);

        for (;;) {
            rc = gif_next_frame(&g_gif, g_canvas);
            if (rc != GIF_OK) {
                break;
            }
            build_lut(&g_gif);
            blit(fb, &g_gif, dx0, dy0, dw, dh);
            frames++;

            /* Deadline-driven rather than "sleep the frame delay": the blit
             * costs real time, and sleeping the full delay after it would
             * stretch every loop by that cost. */
            deadline += (g_gif.delay_ms != 0) ? g_gif.delay_ms : 100u;
            kernel_sleep_until_ms(deadline);

            if (key_pressed() ||
                (kernel_get_ticks_ms() - start) >= max_ms) {
                loop = loops;   /* ends the outer loop too */
                break;
            }
        }

        if (rc < 0) {
            serial_print("boot_anim: frame decode failed (rc=");
            serial_print_hex64((uint64_t)(int64_t)rc);
            serial_print(")\n");
            break;
        }
        gif_rewind(&g_gif);
    }

    fb_clear(fb, 0, 0, 0);

    if ((rflags & (1u << 9)) == 0) {
        __asm__ volatile ("cli");
    }

    serial_print("boot_anim: played ");
    serial_print_hex64(frames);
    serial_print(" frames\n");

    return (frames > 0) ? BOOT_ANIM_OK : BOOT_ANIM_EDECODE;
}
