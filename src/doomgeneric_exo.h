#pragma once

/*
 * ExoDoom's doomgeneric platform layer (SCRUM-74).
 *
 * This header exists to pull in doomgeneric's own declarations rather than
 * restate them. The six DG_* prototypes are doomgeneric's to define, and
 * src/doom/doomgeneric.h is where it defines them -- copying the two this
 * port implements into a hand-written declaration here would be exactly the
 * "hand-copied literal that can silently drift" that build.sh's SCRUM-50
 * comment argues against for syscall numbers, with the same consequence: a
 * re-vendor changes the signature, nothing complains, and the mismatch shows
 * up as a wrong argument at runtime.
 *
 * The quoted include resolves against this file's own directory (src/), so
 * it needs no -I of its own. doomgeneric.h itself includes <stdlib.h> and
 * <stdint.h>, which is why the kernel's C compile in
 * docker/scripts/build.sh passes -I src -- see the comment there.
 *
 * Implemented in src/doomgeneric_exo.c:
 *     uint32_t DG_GetTicksMs(void);
 *     void     DG_SleepMs(uint32_t ms);
 *
 * Not implemented yet, and intentionally left undefined rather than stubbed:
 *     DG_Init            SCRUM-73
 *     DG_DrawFrame       needs exo_fb_acquire + the blit
 *     DG_GetKey          needs the keyboard syscall
 *     DG_SetWindowTitle  no windows; a no-op or a serial line
 */

#include "doom/doomgeneric.h"
