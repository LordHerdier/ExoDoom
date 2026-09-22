#pragma once

#include <stdint.h>

/*
 * doom_profile — per-subsystem frame-time profiling (SCRUM-87).
 *
 * SCRUM-78 added a windowed avg/max timer for exactly one thing, the
 * DG_DrawFrame blit, as three static counters private to
 * src/libos_doom/libos_doom.c. SCRUM-87's acceptance criterion is the whole
 * frame ("avg/max frame time per frame; target <30ms for 33+ fps"), broken
 * down per subsystem, so that one-off counter is generalized here into a
 * small array of identical counters, one per named slot, with one shared
 * windowing/reporting implementation instead of N copies of it.
 *
 * ── Why this lives in src/ and not in src/doom/ ─────────────────────────
 *
 * Same split as src/doom_panic.c (SCRUM-83): src/doom/ is vendored verbatim
 * (SCRUM-63) so a re-vendor stays a clean drop-in, and the patch to
 * src/doom/d_main.c's doomgeneric_Tick() that calls doom_profile_mark() is
 * kept to the smallest thing that reports timing and nothing more. Keeping
 * the counters and the averaging math here, outside the vendored tree, is
 * also what lets tests/kernel/test_doom_profile_k.c drive them from ring 0.
 *
 * ── What each slot measures ─────────────────────────────────────────────
 *
 * DOOM_PROF_SIM/_SOUND/_RENDER bracket the three calls doomgeneric_Tick()
 * (src/doom/d_main.c) makes every frame: TryRunTics(), S_UpdateSounds(),
 * and D_Display(). DOOM_PROF_BLIT is DG_DrawFrame's own body -- unchanged
 * math from SCRUM-78, just reported through this module instead of a
 * private counter -- and is nested inside DOOM_PROF_RENDER's window rather
 * than sequential with it, because D_Display() is what calls I_FinishUpdate()
 * -> DG_DrawFrame(). DOOM_PROF_TOTAL wraps the whole of doomgeneric_Tick()
 * and is the slot that answers the ticket's literal acceptance criterion.
 */
typedef enum {
    DOOM_PROF_SIM = 0,
    DOOM_PROF_SOUND,
    DOOM_PROF_RENDER,
    DOOM_PROF_BLIT,
    DOOM_PROF_TOTAL,
    DOOM_PROF_COUNT,
} doom_prof_slot_t;

/*
 * Record one frame's elapsed time (milliseconds, from DG_GetTicksMs()) for
 * the given slot. Accumulates into that slot's own window, independent of
 * every other slot, and prints an "avg/max over N frames" line to serial
 * once the window fills -- see doom_profile.c for the cadence.
 */
void doom_profile_mark(doom_prof_slot_t slot, uint32_t dt_ms);

/*
 * Test-only: read a slot's raw window state (current frame count, total ms
 * and max ms accumulated since its last report) without waiting for the
 * window to close and print. Mirrors doom_panic_in_progress()'s pattern of
 * exposing real state to a suite instead of the suite re-deriving it. Any
 * out-pointer may be NULL. A slot outside [0, DOOM_PROF_COUNT) is a no-op.
 */
void doom_profile_test_state(doom_prof_slot_t slot, uint32_t *frames,
                              uint32_t *total_ms, uint32_t *max_ms);

/*
 * Test-only: zero every slot's window state. Mirrors doom_panic_reset() --
 * needed so one suite's marks don't leak into the next suite's assertions.
 */
void doom_profile_reset_all(void);
