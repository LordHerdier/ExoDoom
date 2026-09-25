#ifndef DOOM_SFX_TONE_H
#define DOOM_SFX_TONE_H

#include <stdint.h>

/*
 * doom_sfx_tone.h — Doom SFX id -> PC speaker tone sequence (SCRUM-99).
 *
 * Doom's sound effects are 8-bit PCM lumps; the PC speaker (src/speaker.c,
 * SCRUM-98) is a single square wave. There is no faithful conversion, so
 * this is a hand-made table instead: each sfxenum_t id gets a short run of
 * (frequency, duration) steps chosen to be recognisable for what it stands
 * for -- a low falling boom for the shotgun, a rising sweep for a door
 * opening, a rough up-down growl for an imp's sight call. SCRUM-101's
 * sound_module_t plays the steps back one by one through exo_sound_tone.
 *
 * Why steps rather than a single pair per SFX: one steady tone per effect
 * collapses most of Doom's ~100 sounds into "a beep of some pitch", and the
 * acceptance criterion is that they are told apart. Two to four steps is
 * enough for direction (rising door vs falling door) and texture.
 *
 * The table is indexed by sfxenum_t via designated initialisers in
 * doom_sfx_tone.c, so it follows src/doom/sounds.h's ordinals rather than
 * restating them -- reordering that enum cannot silently mis-map a sound.
 * Like src/doom_keymap.c it is a pure lookup with no state and no syscalls,
 * which is what lets tests/kernel/test_doom_sfx_tone_k.c check every entry
 * from ring 0 while it ships inside the ring-3 Doom LibOS.
 */

#define DOOM_SFX_MAX_STEPS 4

typedef struct {
    uint16_t freq_hz;   /* within the speaker's 19..20000 Hz range */
    uint16_t dur_ms;    /* > 0 */
} doom_sfx_step_t;

typedef struct {
    uint8_t         n_steps;   /* 1..DOOM_SFX_MAX_STEPS */
    doom_sfx_step_t step[DOOM_SFX_MAX_STEPS];
} doom_sfx_tone_t;

/* The tone sequence for sfx_id, or NULL for sfx_None or an id outside
 * sfxenum_t. Every real SFX has one. */
const doom_sfx_tone_t *doom_sfx_tone(int sfx_id);

/* Sum of a sequence's step durations. */
uint32_t doom_sfx_tone_total_ms(const doom_sfx_tone_t *t);

#endif
