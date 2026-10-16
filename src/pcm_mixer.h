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
#define PCM_MIXER_OK        0     /* for the entry points that return no handle */
#define PCM_MIXER_ENOVOICE  (-1)  /* all voices busy with more important work */
#define PCM_MIXER_EINVAL    (-2)  /* NULL, empty, or an unplayable rate       */
#define PCM_MIXER_EBUSY     (-3)  /* the music voice already has a ring       */

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

/*
 * Re-place a voice that is already sounding: recompute its gains from `vol`
 * and `sep` -- same ranges, same panning law as pcm_mixer_start -- and leave
 * everything else alone (SCRUM-214).
 *
 * This exists because Doom retunes live sounds.  I_UpdateSoundParams is
 * called from S_UpdateSounds once a tic for every active channel, so a sound
 * started to the player's left has to follow the player as they turn; a mixer
 * that could only place a voice at the moment it started would freeze each
 * effect at the geometry it was fired in.
 *
 * Deliberately NOT a re-admission: priority, phase and the sample pointer are
 * untouched, so this changes how a voice is *heard*, never who holds it or
 * where it has got to.  A voice that has finished, been stolen or was never
 * started answers PCM_MIXER_ENOVOICE and nothing happens -- Doom retunes
 * channels whose end it has not noticed yet, so that is an ordinary outcome
 * rather than a caller error.  PCM_MIXER_OK otherwise.
 */
int pcm_mixer_set_params(int handle, int vol, int sep);

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
 *
 * The music voice below is summed into the same accumulators, before that
 * one clip.
 */
void pcm_mixer_render(int16_t *dst, uint32_t frames);

/*
 * ── The music voice (SCRUM-218) ────────────────────────────────────────
 *
 * Everything above mixes 8-bit mono DMX lumps of known, finite length, read
 * in place from the IWAD.  Music is none of those things: it is 16-bit
 * stereo, already at PCM_MIXER_RATE_HZ, continuous for minutes, and made a
 * chunk at a time by a synthesiser running in ring 3 (SCRUM-217).  So it is
 * a second KIND of voice rather than a ninth entry in the table:
 *
 *   - ONE voice, outside the sfx table.  It takes no sfx slot and no
 *     priority, so pcm_mixer_start()'s stealing can never reach it: a sound
 *     effect must not be able to silence the soundtrack.
 *   - Its source is a RING of interleaved stereo frames that a producer
 *     fills and pcm_mixer_render() drains.  No resampling -- the synthesiser
 *     resamples on its side, where the cost is visible to the application
 *     spending it.
 *   - Its OWN volume.  Doom has separate sfxVolume and musicVolume sliders,
 *     and I_SetMusicVolume has to move this and nothing else.
 *
 * ── The mixer does not own the ring ────────────────────────────────────
 *
 * pcm_mixer_music_open() is handed the storage.  That keeps this file what
 * its header says it is -- nothing here allocates or frees -- and leaves
 * the pages with whoever can account for them: src/syscall_sound.c takes
 * them from the PMM when a LibOS binds the stream and gives them back when
 * it stops or exits (SCRUM-219).  The corollary is the same as the sfx
 * voices' zero-copy note: close the voice BEFORE freeing its ring.
 *
 * ── How big, and why ───────────────────────────────────────────────────
 *
 * 48 kHz x 2 channels x 2 bytes is 192 KB a second, so every 100 ms of
 * buffer is ~19 KB -- nearly five pages -- of kernel memory held for as
 * long as a song plays.  The ring exists to cover the gap between two
 * refills by the producer, which is one frame of Doom's loop: ~29 ms at 35
 * tics a second, more when a frame runs long.
 *
 *   too small   any hitch in that loop is longer than the buffer, the ring
 *               runs dry and the gap is audible.  At 12 pages a refill can
 *               arrive eight tics late before that happens.
 *   too large   the soundtrack trails the game by the fill level, and a
 *               producer that keeps it topped up has committed that much
 *               audio it can no longer take back: a pause or a song change
 *               is late by however much was queued.
 *
 * PCM_MIXER_MUSIC_PAGES = 12 is 12288 frames, 256 ms.  It is a starting
 * point with a way to judge it, not a derived constant:
 * pcm_mixer_music_underruns() counts every frame that had to be rendered as
 * silence for want of data, so "is this enough?" has a number for an answer.
 * Raising it is this one line; the cost is the pages and the lag.
 *
 * Nothing in this file requires that size.  The ring is whatever
 * pcm_mixer_music_open() is given, which is how the tests exercise a wrap
 * with sixteen frames rather than twelve thousand.
 *
 * ── Underrun is silence, not a stall ───────────────────────────────────
 *
 * pcm_mixer_render() runs inside hda_irq_handler() with interrupts off, so
 * an empty ring cannot be waited on: there is nothing to wait FOR until the
 * producer runs again, and it cannot run while this is.  A frame with no
 * music renders the music voice as zero, the sfx voices carry on unaffected,
 * and the frame is counted.
 *
 * Only a voice that is actually playing can underrun.  One that has been
 * opened but not yet written to, or stopped with pcm_mixer_music_flush(), is
 * idle -- silent on purpose -- and is not counted, or the counter would
 * measure how long nothing was playing rather than how often something was
 * late.
 *
 * ── Who calls what ─────────────────────────────────────────────────────
 *
 * pcm_mixer_render() is the consumer and runs in interrupt context.  Every
 * other function below is the producer side and runs in ordinary kernel
 * context -- a syscall body, or a test -- never from an interrupt handler.
 * They guard their shared state the way the sfx mutators do.
 */

/* The recommended ring, in pages, bytes and stereo frames.  See above. */
#define PCM_MIXER_MUSIC_PAGES   12u
#define PCM_MIXER_MUSIC_BYTES   (PCM_MIXER_MUSIC_PAGES * 4096u)
#define PCM_MIXER_MUSIC_FRAMES  (PCM_MIXER_MUSIC_BYTES / \
                                 (PCM_MIXER_CHANNELS * 2u))

/*
 * Give the music voice a ring of `frames` stereo frames at `ring` (so
 * frames * PCM_MIXER_CHANNELS int16_t), empty it and clear the underrun
 * count.  The volume is NOT reset: it belongs to the voice, not the ring,
 * and Doom sets it before the first song exists.
 *
 * PCM_MIXER_OK, PCM_MIXER_EINVAL for a NULL ring or zero frames, or
 * PCM_MIXER_EBUSY if a ring is already attached -- there is one music voice,
 * and silently swapping its storage would strand whoever owns the old one.
 */
int pcm_mixer_music_open(int16_t *ring, uint32_t frames);

/* Detach the ring.  After this returns pcm_mixer_render() will not read it
 * again, so the caller may free it.  Harmless when nothing is attached. */
void pcm_mixer_music_close(void);

/* 1 while a ring is attached. */
int pcm_mixer_music_is_open(void);

/*
 * Queue up to `frames` interleaved stereo frames from `src` and return how
 * many were taken -- which is fewer than offered, possibly zero, when the
 * ring is short of room.  It never blocks and never overwrites audio that
 * has not been played: a full ring is the producer's cue to come back
 * later, and the alternative (dropping the oldest) is a skip in the song.
 *
 * Returns 0 with no ring attached.  Frames already queued are untouched by
 * a short write, and what was accepted will be played in order after them.
 */
uint32_t pcm_mixer_music_write(const int16_t *src, uint32_t frames);

/*
 * Stop: drop everything queued, now.  The next rendered frame is silent
 * rather than the tail of whatever was buffered, and the voice goes idle,
 * so the silence that follows is not counted as an underrun.  The ring
 * stays attached and the next write starts it again.
 */
void pcm_mixer_music_flush(void);

/* Set the music level, 0..PCM_MIXER_VOL_MAX, clamped.  Linear, and
 * PCM_MIXER_VOL_MAX passes samples through bit for bit.  Affects only the
 * music voice; persists across open/close; pcm_mixer_reset() puts it back
 * to full. */
void pcm_mixer_music_set_volume(int vol);
int  pcm_mixer_music_volume(void);

/* Frames queued and not yet played, and frames a write would be accepted
 * right now.  They sum to the ring's size while one is attached and are both
 * 0 otherwise.  A snapshot: render may have drained more by the time the
 * caller looks at the answer, so a write can only ever find MORE room than
 * pcm_mixer_music_space() promised, never less. */
uint32_t pcm_mixer_music_fill(void);
uint32_t pcm_mixer_music_space(void);

/* Frames rendered as silence because a playing voice had nothing queued,
 * since the last pcm_mixer_music_open().  Still readable after a close. */
uint32_t pcm_mixer_music_underruns(void);
