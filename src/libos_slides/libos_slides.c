/*
 * libos_slides.c — slide-deck viewer LibOS, ring 3.
 *
 * Presents a deck (src/exo_slides.h) inside ExoDoom itself. Decks are
 * rendered to pixels on the build host (tools/mkslides/pptx2exs.sh) and
 * written onto the program volume next to the packages by
 * docker/scripts/build.sh, so everything this program does is LibOS policy
 * over three exokernel resources: sectors of the program volume, the
 * framebuffer and the keyboard ring.
 *
 * Startup: bind the program volume (the same extent the shell's `run` binds),
 * mount ExoFS, list the root for *.exs decks, let the user pick one if there
 * is more than one, read the whole deck into the LibOS heap, then unmount and
 * release the disk. The disk is never held while presenting, so the shell's
 * filesystem commands keep working behind a Ctrl+Tab.
 *
 * Presenting: each slide is RLE-decoded into one heap buffer the size of the
 * deck's pixels, then nearest-neighbour fitted and centred onto the
 * framebuffer (16.16 fixed point, as in the fun/gif-boot branch's
 * boot_anim.c -- this target, like the kernel, builds -mno-sse). A deck
 * rendered at the screen's width is a 1:1 copy with a black letterbox.
 *
 * Keys: Right/Down/Space/Enter/N next, Left/Up/Backspace/P previous,
 * digits then Enter jump to that slide, Q/Esc exit.
 *
 * libos_slides_main() carries __attribute__((section(".text.entry"))) and
 * this file must be listed first in build.sh's source list for the target --
 * same convention as libos_clock.c / libos_voldemo.c.
 */

#include "doom_wad_disk.h"   /* LIBOS_WAD_DISK_BASE_LBA: the program volume */
#include "exo_slides.h"
#include "exo_syscall.h"
#include "exo_volume.h"
#include "libos_fb.h"
#include "libos_heap.h"
#include "libos_fs/exofs.h"
#include "fb.h"
#include "fb_console.h"
#include "ps2.h"   /* ps2_key_t only */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

__attribute__((section(".text.entry")))
void libos_slides_main(void);

/* How many decks the picker lists; any more are ignored (and logged). */
#define SLIDES_MAX_DECKS 16
#define SLIDES_NAME_MAX  64

/* .data, not .rodata -- build_ring3_link_target's objcopy step errors on an
 * empty .data extraction (see libos_clock.c). */
static char slides_banner[] = "ExoDoom Slides\n";
static char slides_ext[]    = ".exs";

static char slides_names[SLIDES_MAX_DECKS][SLIDES_NAME_MAX];

static framebuffer_t g_fb;
static fb_console_t  g_con;

/* ---- small helpers ------------------------------------------------------ */

static void write_u32(fb_console_t *con, uint32_t val)
{
    if (val == 0) { fbcon_write(con, "0"); return; }
    char buf[11];
    buf[10] = '\0';
    int i = 10;
    while (val > 0) { buf[--i] = (char)('0' + (val % 10)); val /= 10; }
    fbcon_write(con, &buf[i]);
}

static void slides_exit(void)
{
    exo_syscall1(EXO_SYS_EXIT, 0);
    for (;;) { exo_yield(); }
}

/* See libos_clock.c: an unthrottled exo_kbd_poll() loop keeps IF masked
 * often enough to lose PIT ticks. */
static void slides_backoff(void)
{
    for (volatile uint32_t spin = 0; spin < 20000; spin++) {
        __asm__ volatile ("pause");
    }
}

/* Next key-down event; blocks. */
static uint8_t slides_wait_key(void)
{
    for (;;) {
        exo_kbd_event_t ev;
        if (exo_kbd_poll(&ev) > 0 && ev.pressed) {
            return ev.key;
        }
        slides_backoff();
    }
}

/* Say what went wrong and wait for Q/Esc; there is nothing to retry. */
static void slides_fail(const char *what, int64_t rc)
{
    printf("slides: %s (%d)\n", what, (int)rc);
    fbcon_set_color(&g_con, 255, 90, 90, 0, 0, 0);
    fbcon_write(&g_con, what);
    fbcon_write(&g_con, " (");
    if (rc < 0) {
        fbcon_write(&g_con, "-");
        rc = -rc;
    }
    write_u32(&g_con, (uint32_t)rc);
    fbcon_write(&g_con, ")\n");
    fbcon_set_color(&g_con, 140, 140, 140, 0, 0, 0);
    fbcon_write(&g_con, "Q/Esc to exit\n");
    for (;;) {
        uint8_t k = slides_wait_key();
        if (k == KEY_Q || k == KEY_ESC) {
            slides_exit();
        }
    }
}

static int slides_has_ext(const char *name)
{
    size_t n = 0, e = sizeof(slides_ext) - 1;
    while (name[n] != '\0') n++;
    if (n <= e) return 0;
    for (size_t i = 0; i < e; i++) {
        char c = name[n - e + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != slides_ext[i]) return 0;
    }
    return 1;
}

/* ---- loading ------------------------------------------------------------ */

/* Fill slides_names[] with the decks in the volume root. Volume mounted. */
static uint32_t slides_find_decks(void)
{
    exofs_dir_t dir;
    uint32_t count = 0;
    if (exofs_opendir("/", &dir) != 0) {
        return 0;
    }
    for (;;) {
        exofs_dirinfo_t info;
        if (exofs_readdir(&dir, &info) <= 0) {
            break;
        }
        if ((info.attributes & EXOFS_ATTR_DIRECTORY) || !slides_has_ext(info.name)) {
            continue;
        }
        size_t n = 0;
        while (info.name[n] != '\0') n++;
        if (n + 2 > SLIDES_NAME_MAX) {
            printf("slides: skipping %s (name too long)\n", info.name);
            continue;
        }
        if (count == SLIDES_MAX_DECKS) {
            printf("slides: more than %d decks; ignoring %s\n",
                   SLIDES_MAX_DECKS, info.name);
            continue;
        }
        slides_names[count][0] = '/';
        for (size_t i = 0; i <= n; i++) {
            slides_names[count][i + 1] = info.name[i];
        }
        count++;
    }
    exofs_closedir(&dir);
    return count;
}

/* Read all of `path` into a fresh heap buffer. Volume mounted. */
static int slides_read_file(const char *path, uint8_t **out, uint32_t *out_len)
{
    exofs_stat_t st;
    int rc = exofs_stat(path, &st);
    if (rc != 0) {
        return rc;
    }
    if (st.size == 0) {
        return -EXO_EINVAL;
    }
    uint8_t *buf = libos_heap_alloc(st.size);
    if (buf == NULL) {
        return -EXO_ENOMEM;
    }

    exofs_file_t f;
    rc = exofs_open(path, EXOFS_O_READ, &f);
    uint32_t have = 0;
    if (rc == 0) {
        while (have < st.size) {
            int64_t got = exofs_read(&f, buf + have, st.size - have);
            if (got <= 0) {
                rc = got < 0 ? (int)got : -EXO_EIO;
                break;
            }
            have += (uint32_t)got;
        }
        exofs_close(&f);
    }
    if (rc != 0) {
        libos_heap_free(buf);
        return rc;
    }
    *out = buf;
    *out_len = have;
    return 0;
}

/* Arrow-key menu over slides_names[0..count). Returns the chosen index. */
static uint32_t slides_pick(uint32_t count)
{
    uint32_t sel = 0;
    for (;;) {
        fbcon_clear(&g_con);
        fbcon_set_color(&g_con, 100, 220, 255, 0, 0, 0);
        fbcon_write(&g_con, slides_banner);
        fbcon_set_color(&g_con, 140, 140, 140, 0, 0, 0);
        fbcon_write(&g_con, "Up/Down to choose, Enter to present, Q/Esc to exit\n\n");
        for (uint32_t i = 0; i < count; i++) {
            if (i == sel) {
                fbcon_set_color(&g_con, 255, 220, 80, 0, 0, 0);
                fbcon_write(&g_con, "  > ");
            } else {
                fbcon_set_color(&g_con, 220, 220, 220, 0, 0, 0);
                fbcon_write(&g_con, "    ");
            }
            fbcon_write(&g_con, slides_names[i] + 1);
            fbcon_write(&g_con, "\n");
        }

        uint8_t k = slides_wait_key();
        if (k == KEY_UP && sel > 0) {
            sel--;
        } else if (k == KEY_DOWN && sel + 1 < count) {
            sel++;
        } else if (k == KEY_ENTER || k == KEY_SPACE) {
            return sel;
        } else if (k == KEY_Q || k == KEY_ESC) {
            slides_exit();
        }
    }
}

/* Bind and mount the program volume, choose a deck, read it, and give the
 * disk back. Returns only with a deck in the heap. */
static void slides_load_deck(uint8_t **deck, uint32_t *deck_len, const char **name)
{
    int64_t rc = exo_disk_acquire(LIBOS_WAD_DISK_BASE_LBA,
                                  EXO_VOLUME_REGION_BASE_LBA -
                                      LIBOS_WAD_DISK_BASE_LBA);
    if (rc != 0) {
        slides_fail("cannot bind the program volume", rc);
    }
    int mrc = exofs_mount(LIBOS_WAD_DISK_BASE_LBA);
    if (mrc != 0) {
        exo_disk_release();
        slides_fail("cannot mount the program volume", mrc);
    }

    uint32_t count = slides_find_decks();
    if (count == 0) {
        exofs_unmount();
        exo_disk_release();
        slides_fail("no .exs decks on the program volume -- see tools/mkslides",
                    -EXO_ENOENT);
    }

    uint32_t pick = 0;
    if (count > 1) {
        /* Keep the volume mounted across the menu: nothing else can need
         * the program volume while this context is in the foreground. */
        pick = slides_pick(count);
    }

    fbcon_clear(&g_con);
    fbcon_set_color(&g_con, 220, 220, 220, 0, 0, 0);
    fbcon_write(&g_con, "loading ");
    fbcon_write(&g_con, slides_names[pick] + 1);
    fbcon_write(&g_con, " ...\n");

    int frc = slides_read_file(slides_names[pick], deck, deck_len);
    exofs_unmount();
    exo_disk_release();
    if (frc != 0) {
        slides_fail("cannot read the deck", frc);
    }
    *name = slides_names[pick] + 1;
}

/* ---- presenting --------------------------------------------------------- */

typedef struct {
    uint32_t dx0, dy0, dw, dh;     /* destination rectangle on screen */
    uint32_t step_x, step_y;       /* source pixels per screen pixel, 16.16 */
} slides_fit_t;

/* Fit-and-centre, aspect preserved; up- or downscaled to fill the screen. */
static int slides_fit(const exo_slides_t *d, slides_fit_t *fit)
{
    uint32_t sx = (g_fb.width  << 16) / d->width;
    uint32_t sy = (g_fb.height << 16) / d->height;
    uint32_t s  = sx < sy ? sx : sy;
    uint32_t dw = (uint32_t)(((uint64_t)d->width  * s) >> 16);
    uint32_t dh = (uint32_t)(((uint64_t)d->height * s) >> 16);
    if (dw == 0 || dh == 0) {
        return -1;
    }
    if (dw > g_fb.width)  dw = g_fb.width;
    if (dh > g_fb.height) dh = g_fb.height;
    fit->dw = dw;
    fit->dh = dh;
    fit->dx0 = (g_fb.width  - dw) / 2;
    fit->dy0 = (g_fb.height - dh) / 2;
    fit->step_x = (d->width  << 16) / dw;
    fit->step_y = (d->height << 16) / dh;
    return 0;
}

static void slides_blit(const exo_slides_t *d, const slides_fit_t *fit,
                        const uint32_t *px)
{
    uint32_t sy = 0;
    for (uint32_t y = 0; y < fit->dh; y++) {
        const uint32_t *src = px + (sy >> 16) * d->width;
        uint32_t *dst = (uint32_t *)(g_fb.addr + (fit->dy0 + y) * g_fb.pitch) +
                        fit->dx0;
        uint32_t sx = 0;
        for (uint32_t x = 0; x < fit->dw; x++) {
            dst[x] = src[sx >> 16];
            sx += fit->step_x;
        }
        sy += fit->step_y;
    }
}

static void slides_show(const exo_slides_t *d, const slides_fit_t *fit,
                        uint32_t *px, uint32_t n)
{
    int rc = exo_slides_decode(d, n, px, d->width);
    if (rc != 0) {
        /* A bad slide in the middle of a talk: say so on serial, show what
         * decoded, and let the presenter move on. */
        printf("slides: slide %u: decode failed (%d)\n", (unsigned)(n + 1), rc);
    }
    slides_blit(d, fit, px);
    printf("slides: %u/%u\n", (unsigned)(n + 1), (unsigned)d->slide_count);
}

/* ---- entry point -------------------------------------------------------- */

__attribute__((section(".text.entry")))
void libos_slides_main(void)
{
    libos_fb_t libfb;
    if (libos_fb_map(&libfb) != 0 || libfb.vaddr == NULL) {
        for (;;) { exo_yield(); }
    }
    if (!fb_init_bgrx8888(&g_fb, (uintptr_t)libfb.vaddr, libfb.pitch,
                          libfb.width, libfb.height, libfb.bpp) ||
        !fbcon_init(&g_con, &g_fb)) {
        for (;;) { exo_yield(); }
    }

    uint8_t *deck_buf;
    uint32_t deck_len;
    const char *name;
    slides_load_deck(&deck_buf, &deck_len, &name);

    exo_slides_t deck;
    int rc = exo_slides_open(&deck, deck_buf, deck_len);
    if (rc != 0) {
        slides_fail("not a valid .exs deck", rc);
    }
    slides_fit_t fit;
    if (slides_fit(&deck, &fit) != 0) {
        slides_fail("deck does not fit the screen", -EXO_EINVAL);
    }
    uint32_t *px = libos_heap_alloc((size_t)deck.width * deck.height *
                                    sizeof(uint32_t));
    if (px == NULL) {
        slides_fail("out of memory for the slide buffer", -EXO_ENOMEM);
    }
    printf("slides: %s: %u slides %ux%u, shown at %ux%u\n", name,
           (unsigned)deck.slide_count, (unsigned)deck.width,
           (unsigned)deck.height, (unsigned)fit.dw, (unsigned)fit.dh);

    fb_clear(&g_fb, 0, 0, 0);
    uint32_t cur = 0;
    uint32_t jump = 0;     /* digits typed so far; 0 = none */
    slides_show(&deck, &fit, px, cur);

    for (;;) {
        uint8_t k = slides_wait_key();
        uint32_t next = cur;

        if (k >= KEY_0 && k <= KEY_9) {
            uint32_t j = jump * 10u + (uint32_t)(k - KEY_0);
            jump = j <= deck.slide_count ? j : 0;
            continue;
        }
        if (k == KEY_ENTER && jump != 0) {
            next = jump - 1;
        } else if (k == KEY_RIGHT || k == KEY_DOWN || k == KEY_SPACE ||
                   k == KEY_ENTER || k == KEY_N) {
            if (cur + 1 < deck.slide_count) next = cur + 1;
        } else if (k == KEY_LEFT || k == KEY_UP || k == KEY_BACKSPACE ||
                   k == KEY_P) {
            if (cur > 0) next = cur - 1;
        } else if (k == KEY_Q || k == KEY_ESC) {
            slides_exit();
        }
        jump = 0;

        if (next != cur) {
            cur = next;
            slides_show(&deck, &fit, px, cur);
        }
    }
}
