#ifndef DOOM_SOUND_H
#define DOOM_SOUND_H

#include <stdint.h>

/*
 * doom_sound.h — Doom's sound_module_t over the PC speaker (SCRUM-101).
 *
 * Doom's s_sound.c mixes up to eight channels of PCM; the PC speaker is one
 * square wave. This is the policy layer between the two: a single-voice
 * sequencer that plays a sound effect's SCRUM-99 tone sequence
 * (src/doom_sfx_tone.c) one step at a time through exo_sound_tone
 * (SCRUM-100), and decides which effect gets the voice when several want
 * it.
 *
 * ── Never blocks the game loop ─────────────────────────────────────────
 *
 * Nothing here waits. Each step is started with exo_sound_tone(freq, dur),
 * which returns at once while the kernel's IRQ0 ends the tone on time
 * (SCRUM-98); doom_sound_update() -- called from I_UpdateSound, once per
 * pass of Doom's main loop -- only notices a step's end and starts the next
 * one. A late update cannot leave a note stuck on (the kernel already
 * stopped it); it can only open a short gap before the next step.
 *
 * ── Who gets the one voice ─────────────────────────────────────────────
 *
 * Doom's own rule, from s_sound.c's S_GetChannel(): a LOWER sfxinfo_t
 * priority number is MORE important. A new effect takes the voice if
 * nothing is sounding or its priority number is <= the current one's;
 * otherwise it is dropped. Dropping is reported the way s_sound.c already
 * understands -- the handle comes back, but doom_sound_is_playing() says
 * false for it, and s_sound.c retires the channel on its next update.
 *
 * ── Why the core takes an sfx id and a clock, not an sfxinfo_t ─────────
 *
 * The functions below are the whole of the logic, and they take the
 * sfxenum_t id and the current time as plain arguments so that
 * tests/kernel/test_doom_sound_k.c can drive them from ring 0 against the
 * real speaker, with a hand-advanced clock. The sound_module_t glue that
 * turns an sfxinfo_t pointer into an id (pointer arithmetic against Doom's
 * S_sfx[], which only the Doom link has) and reads DG_GetTicksMs() is a few
 * lines compiled only into the ring-3 target.
 */

/* Forget all state and silence anything this module started. */
void doom_sound_reset(void);

/* Start sfx_id on `channel`. Returns `channel` (Doom's handle) whether the
 * effect won the voice or was dropped -- see doom_sound_is_playing(). vol
 * is Doom's 0..127; 0 plays nothing (the speaker has no volume, so
 * "silent" is the only level that can be honoured). */
int doom_sound_start(int sfx_id, int channel, int priority, int vol,
                     uint32_t now_ms);

/* Stop `channel`'s effect if it is the one sounding. */
void doom_sound_stop(int channel);

/* 1 while `channel`'s effect still holds the voice and has steps left. */
int doom_sound_is_playing(int channel, uint32_t now_ms);

/* Advance the sequence: start the next step once the current one's time is
 * up, or finish. Cheap when nothing is due; call every frame. */
void doom_sound_update(uint32_t now_ms);

/* The sfx id holding the voice, or 0 (sfx_None) if none, and which of its
 * steps is sounding. For tests. */
int doom_sound_current_sfx(void);
int doom_sound_current_step(void);

#endif
