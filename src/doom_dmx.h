#pragma once

#include <stdint.h>

#include "wad.h"

/*
 * doom_dmx — Doom's DMX sound lumps (`DS*`), decoded (SCRUM-211).
 *
 * Doom's sound effects live in the WAD as DMX lumps: an 8-byte header
 * (2-byte format tag, 2-byte sample rate, 4-byte sample count) followed by
 * that many raw 8-bit *unsigned* PCM samples, mono.  Nothing in this tree
 * has ever read one.  src/wad.c knows about flats, map markers and map
 * lumps; src/doom_sfx_tone.c (SCRUM-99) plays a hand-written square-wave
 * caricature of each effect through the PC speaker precisely *because* the
 * real samples were unreachable.  This is the front end that makes them
 * reachable, and the blocker it lifts is the software PCM mixer that would
 * feed SCRUM-210's HDA stream something other than a synthesized tone.
 *
 * ── Zero copy, by necessity ────────────────────────────────────────────
 *
 * doom_dmx_t.samples points *into* the mounted WAD.  That is the same
 * reasoning src/doom_wad.h spells out for the mount itself: the IWAD is a
 * GRUB module the kernel already identity-maps, it is ~28 MiB, and there is
 * nowhere to copy it to.  So there is no allocation here, nothing to free,
 * and the bytes are read-only for the life of the mount.  A caller that
 * needs a mutable or differently-typed buffer asks for one explicitly via
 * doom_dmx_to_s16().
 *
 * ── The padding is real and must be dropped ────────────────────────────
 *
 * The DMX library skipped the first and last 16 samples of every lump
 * (reason lost; chocolate-doom's i_sdlsound.c does the same and says as
 * much).  They are not silence -- DSPISTOL's raw body starts 145, 144, 141,
 * ... and its first *audible* sample is 156 -- so a decoder that keeps them
 * plays a click at both ends of every effect.  doom_dmx_parse() strips them,
 * which is why num_samples is the header count minus 32 rather than the
 * count.
 *
 * ── The sample rate is REPORTED, never assumed ─────────────────────────
 *
 * It is widely repeated that DMX lumps are "always 11025 Hz".  They are not.
 * Across the 109 `DS*` lumps in the pinned freedoom2 v0.13.0: 67 at 22050,
 * 38 at 11025, 2 at 17990, 1 at 16000, 1 at 44100 (tests/kernel/
 * test_doom_dmx_k.c asserts that histogram).  Meanwhile the HDA stream is
 * pinned at HDA_SAMPLE_RATE_HZ = 48000, 16-bit, 2 channels (src/hda.h).  So
 * resampling is unavoidable somewhere, and a decoder that quietly assumed
 * one rate would make every 22050 Hz effect -- two thirds of them -- play at
 * half speed an octave down.  Rate conversion belongs to the mixer; this
 * layer's job is to hand it the truth.
 *
 * ── Why the sfx lookup is name-based ───────────────────────────────────
 *
 * The obvious API would take a sfxenum_t.  It cannot: that enum's mapping to
 * lump names lives in src/doom/sounds.c's S_sfx[], and nothing under
 * src/doom/ links into build/exodoom except tables.c under TESTING (see
 * docker/scripts/build.sh's step 3b).  This file is picked up by build.sh's
 * `src/*.c` glob in *every* build, so a reference to S_sfx[] would be an
 * undefined symbol in a shipped kernel.  Hence doom_dmx_find_sfx() takes the
 * sfxinfo_t::name string, which is exactly what Doom's own
 * sound_module_t::GetSfxLumpNum already has in hand; the enum leg is proven
 * in the test, which does get sounds.c compiled for it.
 */

/* The only format tag Doom ever wrote.  Checked rather than ignored: a
 * non-3 tag means the lump is not PCM at all, and reading its body as
 * samples would emit noise instead of failing. */
#define DOOM_DMX_FORMAT_PCM   3u

#define DOOM_DMX_HEADER_BYTES 8u
#define DOOM_DMX_PAD_SAMPLES  16u   /* dropped at EACH end; see above */

typedef struct {
    uint16_t       format;      /* == DOOM_DMX_FORMAT_PCM on success */
    uint32_t       rate_hz;     /* as recorded, NOT normalised */
    const uint8_t *samples;     /* into the WAD; padding already skipped */
    uint32_t       num_samples; /* header count - 2 * DOOM_DMX_PAD_SAMPLES */
} doom_dmx_t;

/*
 * Result codes.  Distinct values rather than a bare -1, for the reason
 * src/doom_wad.h gives for its own set: each one sends an investigation
 * somewhere different.  "no such lump" points at the WAD's contents or a
 * mis-spelled sfx name; "the header lied about its length" points at a
 * truncated or non-DMX lump; "not tag 3" points at a WAD carrying something
 * else entirely in the DS namespace.
 */
#define DOOM_DMX_OK         0
#define DOOM_DMX_ENOENT    -1  /* no lump of that name in the directory */
#define DOOM_DMX_EINVAL    -2  /* NULL, shorter than a header, or count past the lump */
#define DOOM_DMX_EFORMAT   -3  /* format tag is not DOOM_DMX_FORMAT_PCM */
#define DOOM_DMX_ETOOSHORT -4  /* nothing left once the padding is stripped */

/*
 * Parse [lump, lump + lump_size) as a DMX lump.
 *
 * On DOOM_DMX_OK, *out describes it; on every failure *out is left
 * untouched, so a caller that reuses one doom_dmx_t across a loop of lumps
 * cannot mistake a rejected lump's leftovers for a decoded one.
 */
int doom_dmx_parse(const void *lump, uint32_t lump_size, doom_dmx_t *out);

/* Find `lump_name` in `wad` (exact, case-sensitive -- WAD directory names
 * are upper case) and parse it.  DOOM_DMX_ENOENT if there is no such lump. */
int doom_dmx_find(const wad_t *wad, const char *lump_name, doom_dmx_t *out);

/* As doom_dmx_find(), for a Doom sfx name as it appears in sfxinfo_t::name
 * ("pistol", lower case): the "DS" prefix and the upper-casing are applied
 * here.  DOOM_DMX_EINVAL for a NULL or empty name. */
int doom_dmx_find_sfx(const wad_t *wad, const char *sfx_name, doom_dmx_t *out);

/*
 * "pistol" -> "DSPISTOL".  Writes at most 8 characters plus a NUL into
 * out[9]; a name longer than 6 characters is truncated, because an 8-byte
 * WAD directory name has no room for more and Doom's own sfx names are all
 * within 6.  Exposed (rather than kept static) so a caller that wants to
 * check a lump's presence without decoding it, or that is reporting on
 * serial, does not have to rebuild the convention.
 */
void doom_dmx_lump_name(const char *sfx_name, char out[9]);

/*
 * Convert to the signed 16-bit PCM a mixer sums in, writing at most
 * dst_samples samples and returning how many were written.
 *
 * This is a format change only -- no resampling, no volume, no stereo
 * spread.  DMX's unsigned midpoint 128 maps to exactly 0 so that summing
 * several voices introduces no per-voice DC offset; the scale is a plain
 * 8-bit left shift, so 255 -> +32512 and 0 -> -32768 (slightly asymmetric,
 * as an unsigned 8-bit range inevitably is, and without the rounding error
 * a multiply would add).
 */
uint32_t doom_dmx_to_s16(const doom_dmx_t *dmx, int16_t *dst, uint32_t dst_samples);

/* Human-readable form of a result code, for a serial report.  Never NULL,
 * including for an unrecognised code. */
const char *doom_dmx_strerror(int rc);
