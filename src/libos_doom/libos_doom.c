/*
 * libos_doom.c — Doom, running as a real ring-3 LibOS (SCRUM-66).
 *
 * This is the convergence point the ticket describes: the vendored engine
 * (src/doom/, SCRUM-63/-64), the libc shim (SCRUM-51/-65), the WAD mount
 * (SCRUM-73) and the doomgeneric timer half (SCRUM-74) are linked together
 * here into one ring-3 image that the kernel embeds and the shell launches,
 * the same way src/libos_snake/ and src/libos_wad_viewer/ are.
 *
 * Modeled on src/libos_wad_viewer/libos_wad_viewer.c, which is the other
 * target that needs a WAD staged before launch: sys_launch_doom()
 * (src/syscall_launch.c) maps the multiboot WAD module into this address
 * space and patches g_doom_params (src/doomgeneric_exo.c) with its address
 * and length via libos_launch_patch_params(), which is where DG_Init picks
 * it up.
 *
 * ── What this file owes the link ──────────────────────────────────────
 *
 * doomgeneric fixes six DG_* platform callbacks. Three already exist in
 * src/doomgeneric_exo.c -- DG_Init (SCRUM-73), DG_GetTicksMs and DG_SleepMs
 * (SCRUM-74). The other three are exactly what docker/scripts/link-doom.sh
 * has been carrying in its ALLOWED list as the last undefined symbols in the
 * tree, and they have to be defined here or nothing links:
 *
 *   DG_DrawFrame       SCRUM-77 -- the 640x400 -> framebuffer blit. DONE:
 *                                  it maps the framebuffer on its first
 *                                  call and scales every frame into it.
 *   DG_GetKey          SCRUM-79 -- exo_kbd_poll + Doom keycode translation
 *                                  (whose translation table is SCRUM-40)
 *   DG_SetWindowTitle  no windows here; a serial line
 *
 * DG_GetKey is still a stub with the honest shape -- it reports "no key" and
 * says so on serial exactly once, so a boot log shows which half of the port
 * is missing rather than looking like a hang. Note that this costs less than
 * it sounds: Doom's attract mode (title screen, then demo playback, then the
 * credits) runs with no input at all, so the picture is live and moving
 * without it. What is missing is the menu and the ability to start a game.
 *
 * ── Entry convention ──────────────────────────────────────────────────
 *
 * libos_doom_main() carries __attribute__((section(".text.entry"))) so it
 * lands at offset 0 of the linked .text blob regardless of source order --
 * libos_build_image() always treats the base of the code blob as the entry
 * point rather than looking up a symbol.
 *
 * That attribute is doing real work here rather than belt-and-braces. This
 * is the one ring-3 target whose source list does NOT lead with the file
 * defining its entry point: src/doomgeneric_exo.c goes first instead,
 * because libos_launch_patch_params() writes the WAD params through offset 0
 * of the .data blob and g_doom_params has to be what lives there. Two
 * offsets, two files, two mechanisms -- see the source-order comment on this
 * target's build_ring3_link_target() call in docker/scripts/build.sh.
 */

#include "exo_syscall.h"
#include "doomgeneric_exo.h"
#include "libos_fb.h"
#include "doom/doomgeneric.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

void libos_doom_main(void) __attribute__((section(".text.entry")));

/* Declared here rather than taken from doom/doomgeneric.h, which does not
 * list it: doomgeneric_Tick() is defined in src/doom/d_main.c and is the
 * per-frame half of the engine/platform split (see libos_doom_main below). */
void doomgeneric_Tick(void);

/*
 * Doom's own argv. doomgeneric_Create() stashes these in myargc/myargv for
 * M_CheckParm(), which every -foo option in the engine reads through. There
 * is no command line to inherit here, so it is the program name alone:
 * M_CheckParm returns 0 for everything, which is the vanilla default path.
 *
 * Non-const because myargv is `char **` and M_FindResponseFile() writes
 * through it.
 */
static char  arg0[] = "exodoom";
static char *doom_argv[] = { arg0, NULL };

/* One-shot serial note, so an unimplemented callback says so once rather
 * than on every one of Doom's 35 frames a second. */
static int warned_getkey;

/*
 * The framebuffer, mapped once on the first frame (SCRUM-77).
 *
 * fb_state is a tri-state rather than a "mapped" flag so that a failed map
 * is remembered: DG_DrawFrame() is called 35 times a second and retrying a
 * failing exo_fb_acquire() at that rate would bury the serial log in the
 * same message forever.
 */
#define FB_UNTRIED 0
#define FB_READY   1
#define FB_FAILED  (-1)

static libos_fb_t fb;
static int        fb_state = FB_UNTRIED;

/*
 * Blit Doom's frame to the screen (SCRUM-77).
 *
 * ── Why this is a copy and not a conversion ───────────────────────────
 *
 * doomgeneric renders into DG_ScreenBuffer as 32-bit pixels whose channel
 * offsets Doom itself reports at startup: red 16, green 8, blue 0, alpha 24.
 * Little-endian, that is B,G,R,X byte order in memory -- which is exactly
 * the framebuffer's own BGRX8888 layout (empirically confirmed on QEMU; see
 * CLAUDE.md). So no channel swizzle is needed and each pixel moves as one
 * 32-bit word. The ticket title calls the source "ARGB", which is true of
 * the *word* on a little-endian machine and would be misleading as a byte
 * order.
 *
 * ── Why a full-screen stretch is the CORRECT aspect, not a distortion ──
 *
 * 640x400 into 1024x768 is 1.6x horizontally and 1.92x vertically, which
 * looks like it ought to be wrong. It is not. Doom renders 320x200 (which
 * doomgeneric has already doubled to 640x400 -- "Auto-scaling factor: 2" in
 * its startup log) and 320x200 was designed for a 4:3 display, i.e. with
 * non-square pixels 1.2x taller than wide. 1024x768 is exactly 4:3. So
 * stretching to fill it reproduces the intended geometry, and it is
 * preserving the 16:10 pixel grid that would squash the picture.
 *
 * ── Scaling ───────────────────────────────────────────────────────────
 *
 * Nearest-neighbour, with a Bresenham-style accumulator per axis rather
 * than a multiply-and-divide per pixel: the source index advances by a
 * whole pixel whenever the accumulator passes the destination width, so
 * the inner loop is an add, a compare and a 32-bit store. SCRUM-78 is the
 * ticket for going faster than that if it turns out to matter.
 *
 * Written against fb.width/fb.height/fb.pitch rather than 1024/768/4096:
 * the geometry comes from exo_fb_acquire at runtime and GRUB is free to
 * hand us a different mode.
 */
void DG_DrawFrame(void)
{
    if (fb_state == FB_UNTRIED) {
        int rc = libos_fb_map(&fb);

        if (rc != 0 || fb.vaddr == NULL) {
            fb_state = FB_FAILED;
            printf("libos_doom: framebuffer map failed (%d) -- running "
                   "blind.\n", rc);
            return;
        }

        /* Every path this port has is 32bpp: the kernel refuses to start a
         * framebuffer console on anything else, and fb_init_bgrx8888()
         * (which the other LibOS apps use) checks the same thing. Bail
         * rather than write garbage at a stride we guessed. */
        if (fb.bpp != 32) {
            fb_state = FB_FAILED;
            printf("libos_doom: framebuffer is %u bpp, need 32 -- running "
                   "blind.\n", (unsigned)fb.bpp);
            return;
        }

        fb_state = FB_READY;
        printf("libos_doom: framebuffer %ux%u, pitch %u -- scaling %ux%u\n",
               (unsigned)fb.width, (unsigned)fb.height, (unsigned)fb.pitch,
               (unsigned)DOOMGENERIC_RESX, (unsigned)DOOMGENERIC_RESY);
    }

    if (fb_state != FB_READY || DG_ScreenBuffer == NULL) {
        return;
    }

    const uint32_t dst_w = fb.width;
    const uint32_t dst_h = fb.height;

    uint32_t src_y = 0;
    uint32_t y_acc = 0;

    for (uint32_t dy = 0; dy < dst_h; dy++) {
        const pixel_t *src_row = DG_ScreenBuffer +
                                 (size_t)src_y * DOOMGENERIC_RESX;
        uint32_t      *dst_row = (uint32_t *)((uint8_t *)fb.vaddr +
                                              (size_t)dy * fb.pitch);

        uint32_t src_x = 0;
        uint32_t x_acc = 0;

        for (uint32_t dx = 0; dx < dst_w; dx++) {
            dst_row[dx] = (uint32_t)src_row[src_x];

            x_acc += DOOMGENERIC_RESX;
            while (x_acc >= dst_w) {
                x_acc -= dst_w;
                src_x++;
            }
            /* Only reachable when the framebuffer is NARROWER than Doom's
             * buffer, where the accumulator can step past the last column
             * on the final pixel. Cheap insurance against a mode nobody
             * has tried rather than a condition seen at 1024x768. */
            if (src_x >= DOOMGENERIC_RESX) {
                src_x = DOOMGENERIC_RESX - 1;
            }
        }

        y_acc += DOOMGENERIC_RESY;
        while (y_acc >= dst_h) {
            y_acc -= dst_h;
            src_y++;
        }
        if (src_y >= DOOMGENERIC_RESY) {
            src_y = DOOMGENERIC_RESY - 1;
        }
    }
}

/*
 * SCRUM-79 replaces this with exo_kbd_poll plus the Doom keycode translation
 * SCRUM-40 builds. Reporting "no key available" is the correct shape for a
 * platform with no input yet: doomgeneric's caller (D_ProcessEvents via
 * I_GetEvent) simply sees an empty queue.
 */
int DG_GetKey(int *pressed, unsigned char *key)
{
    (void)pressed;
    (void)key;

    if (!warned_getkey) {
        warned_getkey = 1;
        printf("libos_doom: DG_GetKey is a stub (SCRUM-79) -- "
               "no input will reach the engine.\n");
    }
    return 0;
}

/*
 * There is no window and no title bar. Doom calls this from I_SetWindowTitle
 * with the game description, which is genuinely useful on serial as a sign
 * that D_DoomMain got as far as identifying the IWAD.
 */
void DG_SetWindowTitle(const char *title)
{
    if (title != NULL) {
        printf("libos_doom: title: %s\n", title);
    }
}

void libos_doom_main(void)
{
    printf("libos_doom: entering doomgeneric_Create\n");

    /*
     * doomgeneric splits the game loop between the engine and the platform,
     * and this is the platform half.
     *
     * doomgeneric_Create() (src/doom/doomgeneric.c) runs D_DoomMain(), whose
     * D_DoomLoop() does all of the one-time setup -- TryRunTics(),
     * I_SetWindowTitle(), I_InitGraphics(), D_StartGameLoop() -- then calls
     * doomgeneric_Tick() exactly ONCE and returns (src/doom/d_main.c:458).
     * Every frame after that first one is the platform's job: upstream's own
     * main() is `doomgeneric_Create(...); for (;;) doomgeneric_Tick();` and
     * this is that loop.
     *
     * Worth being explicit about, because getting it wrong looks like
     * success. The first end-to-end launch of this target treated
     * doomgeneric_Create() as never-returning: Doom started up perfectly,
     * printed its banner, loaded the WAD, initialised the renderer, drew one
     * frame -- and then fell out of the bottom and exited, which reads like a
     * clean shutdown rather than a missing loop.
     *
     * Nothing here returns in normal operation. The exits are I_Quit and
     * I_Error, both of which go through doom_panic_begin() (SCRUM-83) and
     * halt without coming back.
     */
    doomgeneric_Create(1, doom_argv);

    for (;;) {
        doomgeneric_Tick();
    }
}
