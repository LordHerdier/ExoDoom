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
 *   DG_DrawFrame       SCRUM-77 -- the real 640x400 -> framebuffer blit
 *   DG_GetKey          SCRUM-79 -- exo_kbd_poll + Doom keycode translation
 *                                  (whose translation table is SCRUM-40)
 *   DG_SetWindowTitle  no windows here; a serial line
 *
 * Both SCRUM-77 and SCRUM-79 are *blocked by this ticket* on the board, so
 * this file deliberately does not implement them. They are stubs with the
 * honest shape -- DG_GetKey reports "no key", DG_DrawFrame drops the frame --
 * and each says so on serial exactly once, so a boot log shows which half of
 * the port is still missing rather than looking like a hang. Filling them in
 * is a two-function change against this file and touches nothing else.
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

/* One-shot serial notes, so an unimplemented callback says so once rather
 * than on every one of Doom's 35 frames a second. */
static int warned_drawframe;
static int warned_getkey;

/*
 * SCRUM-77 replaces this with the real blit: exo_fb_acquire + libos_fb_map()
 * for the framebuffer, then DG_ScreenBuffer (640x400 ARGB, allocated by
 * doomgeneric_Create) scaled into it. Until then the engine runs and simply
 * has nowhere to put the frame.
 */
void DG_DrawFrame(void)
{
    if (!warned_drawframe) {
        warned_drawframe = 1;
        printf("libos_doom: DG_DrawFrame is a stub (SCRUM-77) -- "
               "the engine is running, frames are being dropped.\n");
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
