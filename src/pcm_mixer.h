#pragma once

#include <stdint.h>

#include "doom_dmx.h"

/*
 * pcm_mixer — software PCM mixer: N voices summed into one stereo stream
 * (SCRUM-212).
 *
 * Doom plays several sound effects at once -- src/doom/s_sound.c's
 * snd_channels is 8 -- and the Intel HDA output stream (src/hda.c, SCRUM-210)
 * is one cyclic buffer of interleaved 16-bit stereo frames at a fixed
 * 48 kHz.  Something has to turn the former into the latter.  This is that
 * something: a fixed set of voices, each reading a decoded DMX lump
 * (src/doom_dmx.c, SCRUM-211) at its own rate, summed and clipped into
 * frames the controller can DMA.
 *
 * ── No hardware in here, deliberately ──────────────────────────────────
 *
 * Nothing in this file touches a register, a port or an interrupt, and it
 * includes neither hda.h nor pit.h.  That is the same split
 * src/doom_sfx_tone.c has from src/speaker.c, and it is what lets
 * tests/kernel/test_pcm_mixer_k.c assert every sample this code produces
 * without a controller being present.  The device side -- who calls
 * pcm_mixer_render(), how often, and into what -- is hda_pcm_start() and
 * the refill inside hda_irq_handler().
 *
 * ── Resampling is mandatory, not optional ──────────────────────────────
 *
 * DMX lumps are widely described as "always 11025 Hz".  They are not: across
 * the 109 `DS*` lumps in the pinned freedoom2 v0.13.0 there are 67 at 22050,
 * 38 at 11025, 2 at 17990, 1 at 16000 and 1 at 44100 (tests/kernel/
 * test_doom_dmx_k.c asserts that histogram, and src/doom_dmx.h explains why
 * the decoder reports each lump's rate rather than normalising it).  The
 * output is pinned at PCM_MIXER_RATE_HZ.  So a mixer that assumed one input
 * rate would play two thirds of Doom's effects at half speed, an octave
 * down.  Every voice therefore carries its own 16.16 fixed-point phase
 * accumulator, stepped by rate_hz / PCM_MIXER_RATE_HZ.
 *
 * Interpolation between the two samples either side of the phase is linear.
 * Point sampling would be two integer operations cheaper and audibly worse:
 * an 11 kHz lump stretched to 48 kHz by repetition aliases into a buzz on
 * top of the effect.
 *
 * ── Integer only ───────────────────────────────────────────────────────
 *
 * Every C file under src/ compiles -mno-sse -mno-sse2 (see CLAUDE.md), so a
 * `float` here would not build -- the same wall that made src/fixed_math.c
 * fixed-point.  Phases are 16.16, sums are int32_t, and the only division is
 * by a constant.
 *
 * ── Zero copy ──────────────────────────────────────────────────────────
 *
 * A voice holds the `const uint8_t *` out of doom_dmx_t and reads through it
 * for its whole lifetime.  Those bytes are inside the identity-mapped IWAD
 * module and are read-only for the life of the mount (src/doom_wad.h), so
 * there is nothing to allocate here and nothing to free.  The corollary is
 * that **a voice must not outlive its mount**: doom_wad_unmount() while a
 * voice is sounding leaves it reading a stale pointer, so a caller that
 * unmounts calls pcm_mixer_reset() first.
 */

/* Matches src/doom/s_sound.c's `snd_channels = 8`.  That is a convention
 * this constant follows, not a coupling it can express: snd_channels is a
 * runtime variable in a translation unit that does not link into a shipped
 * kernel (nothing under src/doom/ does -- see src/doom_dmx.h on why the sfx
 * lookup is name-based for the same reason). */
#define PCM_MIXER_VOICES     8u

/* The output rate, matching HDA_SAMPLE_RATE_HZ.  Restated rather than
 * included from hda.h so this file stays hardware-free; the two are asserted
 * equal in tests/kernel/test_hda_pcm_k.c, which sees both. */
#define PCM_MIXER_RATE_HZ    48000u

/* Output channels per frame.  The HDA stream's format is fixed at 2
 * (HDA_BYTES_PER_FRAME = 4), and DMX lumps are mono, so every voice is
 * spread across both. */
#define PCM_MIXER_CHANNELS   2u

/* Doom's own ranges, passed through unchanged so a future caller in
 * s_sound.c's shape needs no conversion: volume 0..127 (sfxinfo volume),
 * separation 0..255 with 128 = centred (S_StartSound's `sep`). */
#define PCM_MIXER_VOL_MAX    127
#define PCM_MIXER_SEP_CENTRE 128
#define PCM_MIXER_SEP_MAX    255

/* Phase accumulator fraction width.  16 bits of fraction over a sample
 * index: a lump is at most a few hundred thousand samples, so the integer
 * half cannot overflow a uint32_t at any DMX rate. */
#define PCM_MIXER_PHASE_BITS 16
#define PCM_MIXER_PHASE_ONE  (1u << PCM_MIXER_PHASE_BITS)

/*
 * Handles.  A handle is a voice index plus a generation counter, not a bare
 * index: voices are reused, and a caller holding an index alone could stop
 * or query a *different* effect that has since taken the slot.  The
 * generation makes that stale handle detectable instead of silently
 * effective.  Negative values are errors, so a handle is always >= 0.
 */
#define PCM_MIXER_ENOVOICE  (-1)  /* all voices busy with more important work */
#define PCM_MIXER_EINVAL    (-2)  /* NULL, empty, or an unplayable rate       */

/* Forget every voice.  Does not touch any buffer -- silence appears on the
 * next pcm_mixer_render(), which is the only thing that writes samples. */
void pcm_mixer_reset(void);

/*
 * Start `dmx` on a voice and return its handle, or a negative code above.
 *
 * vol is 0..127 and scales the voice linearly; 0 is admitted and simply
 * contributes nothing (it still occupies a voice, because Doom's own channel
 * bookkeeping expects a started sound to be startable).  sep is 0..255,
 * 128 centred, 0 hard left, 255 hard right.  priority is Doom's
 * sfxinfo_t::priority convention -- **a LOWER number is MORE important**,
 * exactly as src/doom_sound.h documents for the one-voice speaker path.
 *
 * When every voice is busy, the least important one (highest priority
 * number, tie-broken by whichever is furthest through its sample) is taken
 * over -- but only if the newcomer is at least as important as it.  A less
 * important sound is refused with PCM_MIXER_ENOVOICE and **nothing already
 * playing is disturbed**.  That refusal is the whole of "without one
 * starving the other": the alternative, always stealing, lets a stream of
 * incidental noises chop up the sound the player is actually listening for.
 *
 * `dmx` is not copied; see the zero-copy note above.
 */
int pcm_mixer_start(const doom_dmx_t *dmx, int vol, int sep, int priority);

/* Stop `handle`'s voice if it is still that voice's sound.  A stale or
 * out-of-range handle is ignored. */
void pcm_mixer_stop(int handle);

/* Stop every voice.  Unlike pcm_mixer_reset(), generations are preserved, so
 * outstanding handles go stale rather than becoming ambiguous. */
void pcm_mixer_stop_all(void);

/* 1 while `handle` names a voice that is still sounding, 0 for a finished,
 * stolen, stopped or stale handle. */
int pcm_mixer_is_playing(int handle);

/* How many voices are currently sounding, 0..PCM_MIXER_VOICES.  For tests
 * and for a serial report. */
uint32_t pcm_mixer_active_voices(void);

/*
 * Render `frames` interleaved stereo frames into `dst`, advancing every
 * voice by that much and retiring the ones that run out.
 *
 * `dst` holds frames * PCM_MIXER_CHANNELS int16_t samples and is written in
 * full -- frames no voice reaches are written as silence, so a caller never
 * has to pre-zero the buffer and a stale previous fill can never be replayed.
 *
 * Voices are summed in int32_t and clipped once per sample, saturating at
 * INT16_MIN/INT16_MAX.  Saturating rather than wrapping matters more than it
 * looks: eight full-scale effects do exceed the range, and a wrap turns a
 * loud moment into a full-amplitude sign flip -- a crack far louder than the
 * sound that caused it.
 *
 * Called from hda_irq_handler() with interrupts off, so it must not block,
 * allocate or print; it does none of those.  Cost is O(frames x active
 * voices) integer multiply-adds.
 */
void pcm_mixer_render(int16_t *dst, uint32_t frames);
