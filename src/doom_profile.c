/*
 * doom_profile.c — per-subsystem frame-time profiling (SCRUM-87).
 *
 * See src/doom_profile.h for what this is and why it is not in the vendored
 * tree. The accumulation math itself is unchanged from SCRUM-78's original
 * single-counter version in src/libos_doom/libos_doom.c: accumulate frames,
 * total ms and max ms until a window closes, report, reset. This file just
 * keeps DOOM_PROF_COUNT independent copies of that state instead of one.
 */

#include "doom_profile.h"

#include "stdio.h" /* printf -- already #ifdef EXO_KERNEL-split internally,
                     * same as every other caller in this codebase */

/* ~1s at Doom's 35 tics/sec -- unchanged from SCRUM-78's PROF_WINDOW_FRAMES,
 * chosen so a window gets enough ms-granularity samples to average into
 * sub-ms precision (see doom_profile.h and the SCRUM-78 writeup in
 * docs/drivers/framebuffer.md for why a single before/after pair can't). */
#define DOOM_PROF_WINDOW_FRAMES 35

typedef struct {
    uint32_t frames;
    uint32_t total_ms;
    uint32_t max_ms;
} doom_prof_counter_t;

static const char *const doom_prof_slot_name[DOOM_PROF_COUNT] = {
    [DOOM_PROF_SIM]      = "sim",
    [DOOM_PROF_SOUND]    = "sound",
    [DOOM_PROF_RENDER]   = "render",
    [DOOM_PROF_BLIT]     = "blit",
    [DOOM_PROF_TOTAL]    = "total",
    [DOOM_PROF_TIC_WAIT] = "tic_wait",
    [DOOM_PROF_TIC_RUN]  = "tic_run",
};

static doom_prof_counter_t doom_prof_counters[DOOM_PROF_COUNT];

/* Test-only: mirrors doom_panic_in_progress()/doom_panic_reset()'s pattern
 * of exposing real state to a suite rather than the suite reimplementing
 * the average/window math and hoping it agrees. */
void doom_profile_test_state(doom_prof_slot_t slot, uint32_t *frames,
                              uint32_t *total_ms, uint32_t *max_ms)
{
    if (slot < 0 || slot >= DOOM_PROF_COUNT) {
        return;
    }

    if (frames != NULL) {
        *frames = doom_prof_counters[slot].frames;
    }
    if (total_ms != NULL) {
        *total_ms = doom_prof_counters[slot].total_ms;
    }
    if (max_ms != NULL) {
        *max_ms = doom_prof_counters[slot].max_ms;
    }
}

void doom_profile_reset_all(void)
{
    doom_prof_slot_t slot;

    for (slot = 0; slot < DOOM_PROF_COUNT; slot++) {
        doom_prof_counters[slot].frames   = 0;
        doom_prof_counters[slot].total_ms = 0;
        doom_prof_counters[slot].max_ms   = 0;
    }
}

void doom_profile_mark(doom_prof_slot_t slot, uint32_t dt_ms)
{
    doom_prof_counter_t *c;

    if (slot < 0 || slot >= DOOM_PROF_COUNT) {
        return;
    }

    c = &doom_prof_counters[slot];

    c->frames++;
    c->total_ms += dt_ms;
    if (dt_ms > c->max_ms) {
        c->max_ms = dt_ms;
    }

    if (c->frames < DOOM_PROF_WINDOW_FRAMES) {
        return;
    }

    /* Tenths of a ms via integer math -- same as SCRUM-78's original. */
    uint32_t avg_x10 = (c->total_ms * 10) / c->frames;

    printf("libos_doom: %s avg %u.%ums max %ums over %u frames\n",
           doom_prof_slot_name[slot], (unsigned)(avg_x10 / 10),
           (unsigned)(avg_x10 % 10), (unsigned)c->max_ms,
           (unsigned)c->frames);

    c->frames   = 0;
    c->total_ms = 0;
    c->max_ms   = 0;
}
