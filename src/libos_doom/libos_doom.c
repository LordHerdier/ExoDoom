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
 *   DG_GetKey          SCRUM-79 -- exo_kbd_poll + Doom keycode translation.
 *                                  DONE: it drains the kernel's key ring
 *                                  each tic through doom_keymap_translate()
 *                                  (src/doom_keymap.c, SCRUM-40).
 *   DG_SetWindowTitle  no windows here; a serial line
 *
 * With both of those in, all six DG_* callbacks are real and Doom is
 * playable: the menu responds, a game starts, and the movement/fire/use
 * keys reach G_BuildTiccmd like they would anywhere else.
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
#include "doom_keymap.h"
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
 * dst_x -> src_x / dst_y -> src_y nearest-neighbour lookup tables (SCRUM-78).
 *
 * The per-axis Bresenham walk SCRUM-77 used to redo on every row of every
 * frame depends only on fb.width/fb.height (via exo_fb_acquire) and
 * DOOMGENERIC_RESX/RESY -- never on pixel content -- so it produces the exact
 * same sequence of source indices every single time. Built once, right after
 * the framebuffer maps successfully, and reused for the life of the process;
 * see build_scale_lut() below for how they're filled.
 *
 * Sized to a ceiling well above any VESA mode this project has ever seen
 * (1024x768 today); DG_DrawFrame bails out rather than index past them if
 * exo_fb_acquire ever hands back something larger.
 */
#define LIBOS_DOOM_FB_MAX_W 3840
#define LIBOS_DOOM_FB_MAX_H 2160

static uint32_t x_lut[LIBOS_DOOM_FB_MAX_W];
static uint32_t y_lut[LIBOS_DOOM_FB_MAX_H];

/*
 * Fill `lut[0 .. dst_n)` with the nearest-neighbour source index for each
 * destination index, scaling src_n -> dst_n. Same Bresenham-style
 * accumulator SCRUM-77 ran inline in the per-pixel loop -- an add, a compare
 * and a store per destination index -- just run once per axis instead of
 * once per axis per row per frame.
 */
static void build_scale_lut(uint32_t *lut, uint32_t dst_n, uint32_t src_n)
{
    uint32_t src_i = 0;
    uint32_t acc = 0;

    for (uint32_t dst_i = 0; dst_i < dst_n; dst_i++) {
        lut[dst_i] = src_i;

        acc += src_n;
        while (acc >= dst_n) {
            acc -= dst_n;
            src_i++;
        }
        /* Only reachable when the destination axis is NARROWER than Doom's
         * buffer, where the accumulator can step past the last source index
         * on the final destination index. Cheap insurance against a mode
         * nobody has tried rather than a condition seen at 1024x768. */
        if (src_i >= src_n) {
            src_i = src_n - 1;
        }
    }
}

/*
 * Profiling counters for the acceptance criterion (SCRUM-78): "frame blit
 * takes <5ms measured via serial profiling". The PIT backing DG_GetTicksMs()
 * is 1ms-resolution and a single frame is expected to be well under that, so
 * a single before/after pair would mostly just print "0ms" or "1ms" and
 * prove nothing. Accumulating over many frames and reporting the average
 * gets sub-ms precision out of ms-granularity ticks -- same approach
 * docs/drivers/framebuffer.md §9 used to measure SCRUM-162's scroll fix.
 * The max is tracked separately because an average can hide a single slow
 * frame, and the acceptance criterion is about worst case, not mean case.
 */
#define PROF_WINDOW_FRAMES 35 /* ~1s at Doom's 35 tics/sec */

static uint32_t prof_frames   = 0;
static uint32_t prof_total_ms = 0;
static uint32_t prof_max_ms   = 0;

static void profile_report(uint32_t dt_ms)
{
    prof_frames++;
    prof_total_ms += dt_ms;
    if (dt_ms > prof_max_ms) {
        prof_max_ms = dt_ms;
    }

    if (prof_frames < PROF_WINDOW_FRAMES) {
        return;
    }

    /* Tenths of a ms via integer math -- no float needed for a ratio this
     * simple, and this file has no other reason to touch one. */
    uint32_t avg_x10 = (prof_total_ms * 10) / prof_frames;

    printf("libos_doom: blit avg %u.%ums max %ums over %u frames\n",
           (unsigned)(avg_x10 / 10), (unsigned)(avg_x10 % 10),
           (unsigned)prof_max_ms, (unsigned)prof_frames);

    prof_frames   = 0;
    prof_total_ms = 0;
    prof_max_ms   = 0;
}

/*
 * Blit Doom's frame to the screen (SCRUM-77, LUTs + profiling SCRUM-78).
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
 * Nearest-neighbour. SCRUM-77 walked a Bresenham-style accumulator per axis
 * inline in the per-pixel loop; SCRUM-78 hoists that walk out into
 * x_lut/y_lut (build_scale_lut() above), built once when the framebuffer
 * first maps rather than redone on every row of every frame, so the inner
 * loop here is just two lookups and a 32-bit store.
 *
 * ── Row-duplicate skip ──────────────────────────────────────────────────
 *
 * y_lut maps many consecutive dst rows to the SAME src row whenever the
 * vertical scale factor is above 1x (768/400 = 1.92x here, and it only gets
 * larger at bigger framebuffer resolutions -- more duplication, not less).
 * When `y_lut[dy] == y_lut[dy - 1]`, row dy is byte-for-byte identical to
 * the row just written: same source row, same x_lut. Rather than redo the
 * two-lookup-per-pixel walk against DG_ScreenBuffer, that row is produced by
 * copying the previous destination row's already-computed bytes instead --
 * one sequential read of adjacent memory and a store per pixel, versus two
 * independent array lookups (x_lut, then DG_ScreenBuffer at a
 * scale-dependent, non-sequential offset) and a store. Still one store per
 * pixel either way -- every row still has to actually reach the framebuffer
 * -- but the source side gets cheaper and more cache-friendly as the fraction
 * of duplicate rows grows with resolution.
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

        /* x_lut/y_lut are fixed-size arrays sized for a ceiling no mode this
         * project has used comes close to -- bail loudly rather than index
         * past them if that ever changes. */
        if (fb.width > LIBOS_DOOM_FB_MAX_W || fb.height > LIBOS_DOOM_FB_MAX_H) {
            fb_state = FB_FAILED;
            printf("libos_doom: framebuffer %ux%u exceeds the %ux%u scale "
                   "LUT budget -- running blind.\n",
                   (unsigned)fb.width, (unsigned)fb.height,
                   (unsigned)LIBOS_DOOM_FB_MAX_W, (unsigned)LIBOS_DOOM_FB_MAX_H);
            return;
        }

        build_scale_lut(x_lut, fb.width, DOOMGENERIC_RESX);
        build_scale_lut(y_lut, fb.height, DOOMGENERIC_RESY);

        fb_state = FB_READY;
        printf("libos_doom: framebuffer %ux%u, pitch %u -- scaling %ux%u\n",
               (unsigned)fb.width, (unsigned)fb.height, (unsigned)fb.pitch,
               (unsigned)DOOMGENERIC_RESX, (unsigned)DOOMGENERIC_RESY);
    }

    if (fb_state != FB_READY || DG_ScreenBuffer == NULL) {
        return;
    }

    uint32_t t0 = DG_GetTicksMs();

    const uint32_t dst_w = fb.width;
    const uint32_t dst_h = fb.height;

    uint32_t *prev_dst_row = NULL;

    for (uint32_t dy = 0; dy < dst_h; dy++) {
        uint32_t *dst_row = (uint32_t *)((uint8_t *)fb.vaddr +
                                         (size_t)dy * fb.pitch);

        if (prev_dst_row != NULL && y_lut[dy] == y_lut[dy - 1]) {
            for (uint32_t dx = 0; dx < dst_w; dx++) {
                dst_row[dx] = prev_dst_row[dx];
            }
        } else {
            const pixel_t *src_row = DG_ScreenBuffer +
                                     (size_t)y_lut[dy] * DOOMGENERIC_RESX;

            for (uint32_t dx = 0; dx < dst_w; dx++) {
                dst_row[dx] = (uint32_t)src_row[x_lut[dx]];
            }
        }

        prev_dst_row = dst_row;
    }

    profile_report(DG_GetTicksMs() - t0);
}

/*
 * Hand Doom one key event, if the kernel has one queued (SCRUM-79).
 *
 * Contract, from doomgeneric's only caller (I_GetEvent, src/doom/i_input.c):
 * return non-zero having filled *pressed and *key, or 0 when nothing is
 * left. That caller is a `while (DG_GetKey(&pressed, &key))` loop, so it
 * drains everything available each tic.
 *
 * ── Why `key` is already a Doom keycode ───────────────────────────────
 *
 * Because doomgeneric asks for one. i_input.c's TranslateKey() -- the hook
 * that would map a platform scancode into Doom's keycode space -- is
 * `return key;` in this tree, its lookup-table body commented out upstream.
 * So whatever DG_GetKey reports IS what reaches event.data1, and what the
 * menu, the binding layer and the cheat matcher compare against.
 * doom_keymap_translate() (src/doom_keymap.c, SCRUM-40) is that mapping and
 * its output lands here unmodified.
 *
 * Two things downstream lean on choices SCRUM-40 made, worth naming since
 * they are what makes this a few lines rather than a subsystem:
 *
 *   - UpdateShiftStatus() (i_input.c) counts shift by comparing against
 *     Doom's KEY_RSHIFT. The keymap folds both PS/2 shifts onto that one
 *     keycode, so either physical key moves the counter -- which is what
 *     makes GetTypedChar()'s shiftxform[] upshifting work when typing a
 *     savegame name.
 *   - Letters arrive lowercase, which is what the cheat matcher
 *     (st_stuff.c) and the menu's y/n prompts compare against.
 *
 * ── Unmapped keys are dropped, not reported ───────────────────────────
 *
 * `continue` rather than `return 0`: a key with no Doom meaning must not
 * end the caller's drain loop, or one unmapped keypress would stall every
 * event queued behind it until the next tic. Dropping it and looking again
 * keeps the queue moving.
 *
 * Ctrl+Tab never arrives here at all -- src/ps2.c swallows it as the LibOS
 * switch hotkey (SCRUM-111) before the event is queued, so Doom does not see
 * a spurious TAB (which would toggle the automap) on every switch.
 */
int DG_GetKey(int *pressed, unsigned char *key)
{
    exo_kbd_event_t ev;

    /* `== 1` rather than `> 0`: 0 means the ring was empty and a negative is
     * -EXO_EFAULT. Neither is an event, and both end the drain. */
    while (exo_kbd_poll(&ev) == 1) {
        unsigned char doom_key = doom_keymap_translate(ev.key);

        if (doom_key == DOOM_KEY_NONE) {
            continue;
        }

        if (pressed != NULL) {
            *pressed = ev.pressed ? 1 : 0;
        }
        if (key != NULL) {
            *key = doom_key;
        }
        return 1;
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
