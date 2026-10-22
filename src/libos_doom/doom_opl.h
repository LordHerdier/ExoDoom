#pragma once

#include <stdint.h>

#include "libos_doom/doom_genmidi.h"
#include "libos_doom/doom_midi.h"
#include "nuked_opl3/opl3.h"

/*
 * doom_opl — the MIDI-to-OPL3 driver and its 48 kHz renderer (SCRUM-217).
 *
 * The third stage of the music path (docs/drivers/hda.md §12.10):
 *
 *   doom_midi (events)  ─┐
 *                        ├─> doom_opl ─> Nuked-OPL3 ─> 48 kHz s16 stereo
 *   doom_genmidi (instr) ┘                              (pcm_mixer music voice)
 *
 * It owns one emulated YMF262 in OPL3 mode -- 18 two-operator channels -- and
 * decides which of them plays which note.  It knows nothing about time: a
 * caller applies the events that have come due with doom_opl_event() and then
 * renders however many frames that interval is worth with doom_opl_render().
 * Ring 3 only, beside its two producers in src/libos_doom/; a TESTING kernel
 * compiles it in docker/scripts/build.sh step 3b3 so the doom_opl suite can
 * drive it from ring 0.  Integer only -- that is what makes the second half
 * true, since the kernel is -mno-sse.
 *
 * ── Behaviour, after DMX as chocolate-doom's i_oplmusic.c reproduces it ──
 *
 * One OPL channel per sounding note, two for a DOOM_GENMIDI_FLAG_2VOICE
 * instrument (the second detuned by fine_tune).  When all 18 are busy a
 * note-on steals: a second voice of a two-voice note if there is one, since
 * those are the non-essential half, otherwise the oldest note.  A second voice
 * never steals -- it is skipped instead.  Pitch is the GENMIDI base_note_offset
 * (or fixed_note) through DMX's frequency curve, with pitch bend at 32 steps
 * per semitone (MSB only, as Doom).  Level is DMX's volume curve over note
 * velocity × channel volume (CC7) × expression (CC11) × the master music
 * volume, written into the carrier's (and, additive, the modulator's) output
 * level.  Channel 9 is percussion, one GENMIDI record per GM note 35..81.
 *
 * ── Memory ──────────────────────────────────────────────────────────────
 *
 * Caller-allocated and allocation-free, like src/pcm_mixer.c.  sizeof is
 * dominated by opl3_chip's 1024-entry write buffer (~20 KB), which this driver
 * never uses but cannot remove without editing the vendored source; the
 * object belongs in static storage or a heap, never on the 16 KiB kernel
 * stack.
 */

#define DOOM_OPL_NATIVE_HZ   49716u   /* YMF262: 14.31818 MHz / 288 */
#define DOOM_OPL_OUTPUT_HZ   48000u   /* == PCM_MIXER_RATE_HZ; the test asserts it */
#define DOOM_OPL_VOICES      18u
#define DOOM_OPL_MIDI_CHANS  16u
#define DOOM_OPL_PERC_CHAN   9u       /* MIDI channel 10, zero-based */

#define DOOM_OPL_OK          0
#define DOOM_OPL_EINVAL     -1        /* NULL synth or table */

typedef struct {
    uint8_t              in_use;
    uint8_t              midi_chan;
    uint8_t              key;          /* the event's note: what a note-off names */
    uint8_t              note;         /* what sounds: key, or the fixed note */
    uint8_t              velocity;
    uint8_t              second;       /* plays instr.voice[1] */
    uint8_t              car_level;    /* register 0x40 values last written */
    uint8_t              mod_level;
    uint16_t             freq;         /* block << 10 | F-number, key bit clear */
    uint32_t             age;          /* allocation stamp; lowest is oldest */
    doom_genmidi_instr_t instr;
} doom_opl_voice_t;

typedef struct {
    uint8_t program;
    uint8_t volume;      /* CC7, default 100 */
    uint8_t expression;  /* CC11, default 127 */
    uint8_t pan;         /* register 0xC0 output bits: 0x10, 0x20 or 0x30 */
    int8_t  bend;        /* pitch-bend MSB - 64 */
} doom_opl_chan_t;

typedef struct {
    opl3_chip         chip;
    doom_genmidi_t    genmidi;
    doom_opl_voice_t  voices[DOOM_OPL_VOICES];
    doom_opl_chan_t   chans[DOOM_OPL_MIDI_CHANS];
    uint32_t          next_age;
    uint8_t           master;       /* 0..127 */

    /* Resampler: an exact rational step of NATIVE/OUTPUT native samples per
     * output frame, interpolating linearly between prev and cur. */
    uint32_t          acc;          /* 0..OUTPUT_HZ-1: position between prev and cur */
    int16_t           prev[2];
    int16_t           cur[2];
    uint64_t          native_samples;  /* OPL3_Generate calls, ever */
} doom_opl_t;

/* Reset the chip into OPL3 mode with every channel silent, all MIDI channels
 * at their defaults (program 0, volume 100, centred) and master volume 127.
 * `genmidi` is copied by value; the lump it points into must outlive `s`. */
int  doom_opl_init(doom_opl_t *s, const doom_genmidi_t *genmidi);

/* Apply one channel-voice event: note on/off, controller, program change,
 * pitch bend.  Aftertouch and channel pressure are ignored. */
void doom_opl_event(doom_opl_t *s, const doom_midi_event_t *ev);

/* Doom's master music volume, 0..127 (larger is clamped).  Applied to every
 * sounding voice at once.  0 renders digital silence while the chip keeps
 * running, so a fade back up resumes mid-envelope rather than restarting. */
void doom_opl_set_volume(doom_opl_t *s, uint8_t volume);

/* Key off every voice -- a stop or song change.  Releases still ring out. */
void doom_opl_all_notes_off(doom_opl_t *s);

/* Render `nframes` interleaved stereo frames at DOOM_OPL_OUTPUT_HZ. */
void doom_opl_render(doom_opl_t *s, int16_t *frames, uint32_t nframes);

/* Diagnostics, for the tests and for anyone judging the voice budget. */
uint32_t doom_opl_voices_in_use(const doom_opl_t *s);
/* OPL channels whose operators the emulator itself has keyed on. */
uint32_t doom_opl_channels_keyed(const doom_opl_t *s);
