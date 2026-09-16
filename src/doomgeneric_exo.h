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
 *     void     DG_Init(void);         SCRUM-73
 *     uint32_t DG_GetTicksMs(void);   SCRUM-74
 *     void     DG_SleepMs(uint32_t ms);
 *
 * Not implemented yet, and intentionally left undefined rather than stubbed:
 *     DG_DrawFrame       needs exo_fb_acquire + the blit
 *     DG_GetKey          needs the keyboard syscall
 *     DG_SetWindowTitle  no windows; a no-op or a serial line
 */

#include "doom/doomgeneric.h"

/*
 * DG_Init's outcome (SCRUM-73).
 *
 * doomgeneric fixes DG_Init's signature as `void DG_Init()`, so there is
 * nowhere to return a result -- and DG_Init deliberately does not halt on
 * failure, because a missing or malformed IWAD is what Doom's own startup
 * (d_iwad.c, then W_AddFile) exists to diagnose, with more engine context
 * than this layer has. See DG_Init's own comment.
 *
 * Returns DG_INIT_NOT_RUN before DG_Init() has been called, otherwise one of
 * the DOOM_WAD_* codes from src/doom_wad.h (DOOM_WAD_OK on success).
 *
 * This is also how tests/kernel/test_dg_init_k.c observes DG_Init from ring
 * 0, which is why there is no #ifdef splitting the tested path from the
 * shipped one.
 */
#define DG_INIT_NOT_RUN 1 /* distinct from every DOOM_WAD_* code, all <= 0 */

int dg_init_result(void);
