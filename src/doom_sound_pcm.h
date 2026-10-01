#ifndef DOOM_SOUND_PCM_H
#define DOOM_SOUND_PCM_H

#include <stdint.h>

/*
 * doom_sound_pcm.h — Doom's sound effects as their real PCM samples
 * (SCRUM-214).
 *
 * This is the other half of src/doom_sound.h. That file plays a hand-written
 * square-wave caricature of each effect through the PC speaker (SCRUM-99/101)
 * because the WAD's actual samples were unreachable; they are reachable now --
 * decoded by src/doom_dmx.c (SCRUM-211), mixed by src/pcm_mixer.c (SCRUM-212),
 * played by src/hda.c (SCRUM-210) and offered to ring 3 as exo_sound_pcm
 * (#27, SCRUM-213). What was missing was the layer that turns "Doom wants to
 * play sfx N on channel C" into one of those calls, and that is all this file
 * is.
 *
 * ── Which of the two paths owns I_StartSound ───────────────────────────
 *
 * Only one can. src/doom_sound.c's sound_module_t tries this one first and
 * falls back to the speaker sequencer when it answers "not started", and the
 * rule is deliberately a runtime one rather than a build-time #ifdef:
 *
 *   - no WAD mounted, or no DS* lump for this effect -> 0, the caller plays a
 *     tone. A WAD without a given effect is a real possibility (PWADs, and
 *     Doom's own S_sfx[] carries entries no IWAD defines), and it should cost
 *     that one effect, not all sound.
 *   - exo_sound_pcm answers -EXO_ENODEV -> this machine has no audio
 *     controller at all. That is latched (doom_sound_pcm_available() goes 0
 *     for the rest of the run) so the port degrades to the speaker in one
 *     step rather than paying a failed syscall per effect forever.
 *
 * ── Zero copy, all the way to the mixer ────────────────────────────────
 *
 * The samples handed to #27 point straight into the WAD the kernel mapped at
 * LIBOS_WAD_VADDR. Nothing is copied on this side, and nothing needs to be:
 * that window is inside [EXO_USER_VA_BASE, EXO_USER_VA_END) and #27 checks
 * its buffer read-only, so a DMX lump is already a valid argument exactly as
 * doom_dmx_parse() reports it -- rate included, since the mixer resamples per
 * voice. The kernel then copies into its own pages, for the reasons
 * docs/syscall_spec.md §3.5b gives; that copy is the syscall's, not ours.
 *
 * ── Why "is it still playing" is a clock and not a syscall ─────────────
 *
 * s_sound.c retires a channel when I_SoundIsPlaying says false, and there is
 * no syscall that answers it -- deliberately, because there does not need to
 * be one. A sound's length is num_samples/rate seconds, known at the moment
 * it starts, so the answer is arithmetic on DG_GetTicksMs(). A syscall would
 * cost a ring transition per channel per tic to learn something already
 * known. The cost of the cheap version is that a voice the mixer stole early
 * still reads as playing until its nominal end, which retires the channel
 * slightly late -- the same direction of error Doom's own SDL back end has.
 *
 * ── Why plain arguments, not sfxinfo_t ─────────────────────────────────
 *
 * Same reason src/doom_sound.h gives: everything below takes the sfx *name*
 * (which is what doom_dmx_find_sfx() matches on), the channel, and the
 * current time as arguments, so tests/kernel/test_doom_sound_pcm_k.c can
 * drive the whole of it from ring 0 against the real mixer and the real
 * controller with a hand-advanced clock. The sfxinfo_t glue lives in
 * src/doom_sound.c and is compiled only into the ring-3 target.
 */

/* Forget every channel and stop every voice this module started. */
void doom_sound_pcm_reset(void);

/* 0 once a -EXO_ENODEV from the kernel has latched this path off; 1 while PCM
 * playback is still worth attempting. Reset by doom_sound_pcm_reset(). */
int doom_sound_pcm_available(void);

/*
 * Start `sfx_name`'s DS* lump on `channel`.
 *
 * Returns 1 if this path has taken the effect on and the caller must NOT play
 * a tone for it, 0 if it has not (no device, no WAD, no such lump, or the
 * kernel refused for a reason a retry would not fix) and the caller should
 * fall back.
 *
 * `vol` is Doom's 0..127 and `sep` its 0..255 with 128 centred; both are
 * passed through untouched, since the mixer speaks exactly those ranges.
 * `priority` is sfxinfo_t::priority, where a LOWER number is MORE important.
 *
 * A channel that already holds a voice has it stopped first: Doom reuses a
 * channel without always calling I_StopSound.
 *
 * One outcome returns 1 without a voice: the mixer refusing a sound less
 * important than everything already playing (-EXO_EBUSY). The effect *was*
 * handled -- by being dropped, which is the mixer's own non-starvation rule --
 * so a tone would be a second, louder answer to a request that was already
 * declined. doom_sound_pcm_is_playing() reports false for that channel and
 * s_sound.c retires it, the same convention src/doom_sound.h documents for a
 * tone that lost the speaker.
 */
int doom_sound_pcm_start(const char *sfx_name, int channel, int priority,
                         int vol, int sep, uint32_t now_ms);

/* Stop `channel`'s voice if it holds one. */
void doom_sound_pcm_stop(int channel);

/* 1 while `channel`'s sound has not yet reached its nominal end. */
int doom_sound_pcm_is_playing(int channel, uint32_t now_ms);

/* Re-place `channel`'s voice: `vol`/`sep` as in doom_sound_pcm_start(). A
 * channel holding no voice is ignored. */
void doom_sound_pcm_params(int channel, int vol, int sep);

/* 1 if `channel` currently holds a voice on this path -- what
 * src/doom_sound.c asks to decide whether stop/is_playing/params belong here
 * or to the speaker sequencer. For tests too. */
int doom_sound_pcm_owns(int channel);

#endif
