/*
 * test_doom_opl_k.c — SCRUM-217's acceptance tests for the OPL3 synthesizer
 * and the MIDI-to-OPL voice allocator.
 *
 * Two kinds of instrument.  A synthetic GENMIDI lump holds instruments chosen
 * so the output is predictable from first principles -- a pure sine at a known
 * pitch, an instant attack, an instant release -- so pitch, silence and voice
 * accounting are asserted against physics rather than against a recording.
 * The real freedoom2 GENMIDI then supplies the bit-stability checksum and the
 * render-cost measurement, which are about the instruments Doom actually
 * plays.  The checksum is a recorded value: it catches a regression, it cannot
 * prove the first recording right -- the synthetic tests are what do that.
 */

#include "kunit.h"

#include "libos_doom/doom_genmidi.h"
#include "libos_doom/doom_midi.h"
#include "libos_doom/doom_opl.h"
#include "doom_wad.h"
#include "doom_wad_disk.h"
#include "pcm_mixer.h"
#include "pit.h"
#include "serial.h"
#include "string.h"
#include "wad.h"

#include <stddef.h>
#include <stdint.h>

/* Static: doom_opl_t is ~20 KB and a second of stereo is 192 KB, against a
 * 16 KiB kernel stack. */
static doom_opl_t     synth;
static int16_t        frames[DOOM_OPL_OUTPUT_HZ * 2];
static uint8_t        lump[DOOM_GENMIDI_LUMP_BYTES];
static doom_genmidi_t synthetic;

/* ---- the synthetic instrument table ------------------------------------ */

enum {
    PROG_SINE    = 0,   /* single voice: sine carrier, silent modulator */
    PROG_DOUBLE  = 1,   /* the same sine, FLAG_2VOICE */
    PERC_FIXED   = 36,  /* GM note 36 (record 129): fixed note 60 */
};

/* One 16-byte voice.  Carrier: EG-type sustain, multiplier 1 (0x21); attack
 * rate 15, decay 0; sustain level 0, release 15; sine.  Modulator: attack
 * rate 0 and level 0x3f, so its envelope never leaves full attenuation and
 * it contributes nothing to the phase.  Connection FM (feedback byte 0). */
static void put_sine_voice(uint8_t *v, int16_t base_note_offset)
{
    static const uint8_t mod[6] = { 0x21, 0x00, 0x0f, 0x00, 0x00, 0x3f };
    static const uint8_t car[6] = { 0x21, 0xf0, 0x0f, 0x00, 0x00, 0x00 };

    memcpy(v, mod, 6);
    v[6] = 0x00;
    memcpy(v + 7, car, 6);
    v[13] = 0;
    v[14] = (uint8_t)((uint16_t)base_note_offset & 0xffu);
    v[15] = (uint8_t)((uint16_t)base_note_offset >> 8);
}

static void put_record(uint32_t index, uint16_t flags, uint8_t fixed_note)
{
    uint8_t *r = lump + DOOM_GENMIDI_HEADER_BYTES + index * DOOM_GENMIDI_INSTR_BYTES;

    r[0] = (uint8_t)(flags & 0xffu);
    r[1] = (uint8_t)(flags >> 8);
    r[2] = 128;                     /* fine_tune: no detune */
    r[3] = fixed_note;
    /* -12, as freedoom's own records carry: DMX's frequency curve sits an
     * octave high, so it is note + offset that lands on concert pitch. */
    put_sine_voice(r + 4, -12);
    put_sine_voice(r + 20, -12);
}

static void build_synthetic(void)
{
    memset(lump, 0, sizeof(lump));
    memcpy(lump, DOOM_GENMIDI_MAGIC, DOOM_GENMIDI_HEADER_BYTES);
    for (uint32_t i = 0; i < DOOM_GENMIDI_NUM_INSTRS; i++) {
        put_record(i, 0, 0);
    }
    put_record(PROG_DOUBLE, DOOM_GENMIDI_FLAG_2VOICE, 0);
    put_record(DOOM_GENMIDI_NUM_PROGRAMS + (PERC_FIXED - DOOM_GENMIDI_PERC_FIRST),
               DOOM_GENMIDI_FLAG_FIXED, 60);
    CU_ASSERT_EQUAL(doom_genmidi_parse(lump, sizeof(lump), &synthetic),
                    DOOM_GENMIDI_OK);
}

/* ---- helpers ------------------------------------------------------------ */

static void ev(uint8_t status, uint8_t d1, uint8_t d2)
{
    doom_midi_event_t e = { 0, status, d1, d2 };
    doom_opl_event(&synth, &e);
}

static void start_synthetic(void)
{
    build_synthetic();
    CU_ASSERT_EQUAL(doom_opl_init(&synth, &synthetic), DOOM_OPL_OK);
}

static uint32_t ms_frames(uint32_t ms) { return ms * (DOOM_OPL_OUTPUT_HZ / 1000u); }

static int all_zero(const int16_t *f, uint32_t nframes)
{
    for (uint32_t i = 0; i < 2u * nframes; i++) {
        if (f[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int32_t peak(const int16_t *f, uint32_t nframes)
{
    int32_t p = 0;
    for (uint32_t i = 0; i < 2u * nframes; i++) {
        int32_t a = f[i] < 0 ? -(int32_t)f[i] : f[i];
        if (a > p) {
            p = a;
        }
    }
    return p;
}

/* Upward zero crossings on one channel of an interleaved buffer. */
static uint32_t rising_crossings(const int16_t *f, uint32_t nframes, int ch)
{
    uint32_t n = 0;
    for (uint32_t i = 1; i < nframes; i++) {
        n += (f[2 * (i - 1) + ch] < 0 && f[2 * i + ch] >= 0);
    }
    return n;
}

static uint32_t fnv1a(const int16_t *f, uint32_t nframes)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < 2u * nframes; i++) {
        uint16_t s = (uint16_t)f[i];
        h = (h ^ (s & 0xffu)) * 16777619u;
        h = (h ^ (s >> 8)) * 16777619u;
    }
    return h;
}

/* Index of the voice playing (chan, key), or DOOM_OPL_VOICES. */
static uint32_t voice_for(uint8_t chan, uint8_t key, uint8_t second)
{
    for (uint32_t v = 0; v < DOOM_OPL_VOICES; v++) {
        const doom_opl_voice_t *vo = &synth.voices[v];
        if (vo->in_use && vo->midi_chan == chan && vo->key == key &&
            vo->second == second) {
            return v;
        }
    }
    return DOOM_OPL_VOICES;
}

/* ---- synthetic: rate, pitch, silence ------------------------------------ */

static void test_rate_constant_matches_mixer(void)
{
    CU_ASSERT_EQUAL(DOOM_OPL_OUTPUT_HZ, PCM_MIXER_RATE_HZ);
    CU_ASSERT_EQUAL(DOOM_OPL_NATIVE_HZ, 49716u);
}

static void test_init_rejects_null(void)
{
    doom_genmidi_t empty = { NULL };

    CU_ASSERT_EQUAL(doom_opl_init(NULL, &synthetic), DOOM_OPL_EINVAL);
    CU_ASSERT_EQUAL(doom_opl_init(&synth, NULL), DOOM_OPL_EINVAL);
    CU_ASSERT_EQUAL(doom_opl_init(&synth, &empty), DOOM_OPL_EINVAL);
}

static void test_idle_chip_is_digital_silence(void)
{
    start_synthetic();
    doom_opl_render(&synth, frames, ms_frames(100));
    CU_ASSERT(all_zero(frames, ms_frames(100)));
}

/* The resampler's exact rational step: a second of output is 48000 frames
 * and consumes 49716 native samples, not 49713 (a truncated 16.16 step) and
 * not 48000 (no resampling at all).  Checked in pieces of uneven size, since
 * a step that only came out right for whole seconds would be no step. */
static void test_resampler_counts_native_samples(void)
{
    start_synthetic();

    doom_opl_render(&synth, frames, ms_frames(10));
    CU_ASSERT(synth.native_samples == 497u || synth.native_samples == 498u);

    static const uint32_t pieces[] = { 1, 7, 479, 4800, 13, 42220 };
    uint32_t total = ms_frames(10);
    for (uint32_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++) {
        doom_opl_render(&synth, frames, pieces[i]);
        total += pieces[i];
    }
    CU_ASSERT_EQUAL(total, 48000u);
    CU_ASSERT(synth.native_samples == 49716u);
    CU_ASSERT_EQUAL(synth.acc, 0u);
}

/* Note 69 + base offset -12 is A4: 440 rising zero crossings a second, on
 * both outputs (the channel is centred).  This is the frequency curve, the
 * block/F-number split, the key-on write and the resampler all at once. */
static void test_a440_has_440_crossings(void)
{
    start_synthetic();
    ev(0x90, 69, 127);
    doom_opl_render(&synth, frames, ms_frames(50));     /* past the attack */
    doom_opl_render(&synth, frames, ms_frames(1000));

    uint32_t l = rising_crossings(frames, ms_frames(1000), 0);
    uint32_t r = rising_crossings(frames, ms_frames(1000), 1);
    CU_ASSERT(l >= 439u && l <= 441u);
    CU_ASSERT(r >= 439u && r <= 441u);
    CU_ASSERT(peak(frames, ms_frames(1000)) > 1000);
}

/* A whole semitone of bend (MSB 96: +32 curve steps) programs exactly the
 * register value of the next note up, and centring it restores the first. */
static void test_pitch_bend_moves_a_semitone(void)
{
    start_synthetic();
    ev(0x90, 70, 100);
    uint16_t up_one = synth.voices[voice_for(0, 70, 0)].freq;
    ev(0x80, 70, 0);

    ev(0x90, 69, 100);
    uint32_t v = voice_for(0, 69, 0);
    CU_ASSERT(v < DOOM_OPL_VOICES);
    uint16_t base = synth.voices[v].freq;
    ev(0xe0, 0, 96);
    CU_ASSERT_EQUAL(synth.voices[v].freq, up_one);
    ev(0xe0, 0, 64);
    CU_ASSERT_EQUAL(synth.voices[v].freq, base);
}

/* Key off, release rate 15: the output reaches exactly 0 on both channels and
 * stays there -- not a 1-LSB DC level, and not the -1 square wave a fully
 * attenuated operator's one's-complement sine leaves behind. */
static void test_release_reaches_digital_silence(void)
{
    start_synthetic();
    ev(0x90, 60, 127);
    doom_opl_render(&synth, frames, ms_frames(100));
    CU_ASSERT(peak(frames, ms_frames(100)) > 1000);

    ev(0x80, 60, 0);
    CU_ASSERT_EQUAL(doom_opl_voices_in_use(&synth), 0u);
    CU_ASSERT_EQUAL(doom_opl_channels_keyed(&synth), 0u);
    doom_opl_render(&synth, frames, ms_frames(200));    /* the release */
    doom_opl_render(&synth, frames, ms_frames(500));
    CU_ASSERT(all_zero(frames, ms_frames(500)));
}

/* Velocity 0 is a note-off, as running-status MIDI files use it. */
static void test_velocity_zero_is_note_off(void)
{
    start_synthetic();
    ev(0x90, 60, 127);
    CU_ASSERT_EQUAL(doom_opl_voices_in_use(&synth), 1u);
    ev(0x90, 60, 0);
    CU_ASSERT_EQUAL(doom_opl_voices_in_use(&synth), 0u);
    CU_ASSERT_EQUAL(doom_opl_channels_keyed(&synth), 0u);
}

static void test_all_notes_off_silences_everything(void)
{
    start_synthetic();
    for (uint8_t c = 0; c < 4; c++) {
        ev((uint8_t)(0x90 | c), (uint8_t)(60 + c), 100);
    }
    ev(0xb0, 123, 0);                           /* CC123 on channel 0 only */
    CU_ASSERT_EQUAL(doom_opl_voices_in_use(&synth), 3u);

    doom_opl_all_notes_off(&synth);
    CU_ASSERT_EQUAL(doom_opl_voices_in_use(&synth), 0u);
    CU_ASSERT_EQUAL(doom_opl_channels_keyed(&synth), 0u);
    doom_opl_render(&synth, frames, ms_frames(300));
    doom_opl_render(&synth, frames, ms_frames(100));
    CU_ASSERT(all_zero(frames, ms_frames(100)));
}

/* ---- synthetic: allocation and stealing --------------------------------- */

static void test_18_notes_use_18_channels_then_19th_steals_oldest(void)
{
    start_synthetic();

    for (uint8_t k = 0; k < 18; k++) {
        ev(0x90, (uint8_t)(40 + k), 100);
    }
    CU_ASSERT_EQUAL(doom_opl_voices_in_use(&synth), 18u);
    CU_ASSERT_EQUAL(doom_opl_channels_keyed(&synth), 18u);

    /* Each note on its own OPL channel: no two voices share a key. */
    uint32_t seen = 0;
    for (uint8_t k = 0; k < 18; k++) {
        uint32_t v = voice_for(0, (uint8_t)(40 + k), 0);
        CU_ASSERT(v < DOOM_OPL_VOICES);
        if (v < DOOM_OPL_VOICES) {
            seen |= 1u << v;
        }
    }
    CU_ASSERT_EQUAL(seen, (1u << 18) - 1u);

    /* The 19th takes the oldest note's channel (key 40, voice 0). */
    uint32_t oldest = voice_for(0, 40, 0);
    ev(0x90, 58, 100);
    CU_ASSERT_EQUAL(doom_opl_voices_in_use(&synth), 18u);
    CU_ASSERT_EQUAL(voice_for(0, 40, 0), DOOM_OPL_VOICES);
    CU_ASSERT_EQUAL(voice_for(0, 58, 0), oldest);

    /* The stolen note's own note-off must not release its successor. */
    ev(0x80, 40, 0);
    CU_ASSERT_EQUAL(doom_opl_voices_in_use(&synth), 18u);
    CU_ASSERT_EQUAL(doom_opl_channels_keyed(&synth), 18u);

    /* And every note-off together leaves nothing keyed on in the chip. */
    for (uint8_t k = 41; k <= 58; k++) {
        ev(0x80, k, 0);
    }
    CU_ASSERT_EQUAL(doom_opl_voices_in_use(&synth), 0u);
    CU_ASSERT_EQUAL(doom_opl_channels_keyed(&synth), 0u);
}

/* A FLAG_2VOICE note takes two channels; when the chip is full, a new note
 * takes the oldest *second* voice before any whole note, and its own second
 * voice is dropped rather than stealing again. */
static void test_double_voice_takes_two_and_yields_second_first(void)
{
    start_synthetic();
    ev(0xc0, PROG_DOUBLE, 0);

    ev(0x90, 60, 100);
    CU_ASSERT_EQUAL(doom_opl_voices_in_use(&synth), 2u);
    CU_ASSERT(voice_for(0, 60, 0) < DOOM_OPL_VOICES);
    CU_ASSERT(voice_for(0, 60, 1) < DOOM_OPL_VOICES);

    for (uint8_t k = 61; k < 69; k++) {
        ev(0x90, k, 100);
    }
    CU_ASSERT_EQUAL(doom_opl_voices_in_use(&synth), 18u);

    ev(0x90, 69, 100);
    CU_ASSERT_EQUAL(doom_opl_voices_in_use(&synth), 18u);
    CU_ASSERT(voice_for(0, 69, 0) < DOOM_OPL_VOICES);
    CU_ASSERT_EQUAL(voice_for(0, 69, 1), DOOM_OPL_VOICES);
    CU_ASSERT(voice_for(0, 60, 0) < DOOM_OPL_VOICES);       /* kept its primary */
    CU_ASSERT_EQUAL(voice_for(0, 60, 1), DOOM_OPL_VOICES);  /* lost its second */

    doom_opl_all_notes_off(&synth);
    CU_ASSERT_EQUAL(doom_opl_channels_keyed(&synth), 0u);
}

/* Channel 9 plays the percussion record for its key; a FLAG_FIXED record
 * sounds its fixed note whatever the key, and a key outside 35..81 is
 * dropped. */
static void test_percussion_uses_fixed_note_records(void)
{
    start_synthetic();

    ev(0x99, PERC_FIXED, 100);
    uint32_t v = voice_for(DOOM_OPL_PERC_CHAN, PERC_FIXED, 0);
    CU_ASSERT(v < DOOM_OPL_VOICES);
    if (v < DOOM_OPL_VOICES) {
        CU_ASSERT_EQUAL(synth.voices[v].note, 60);
    }

    ev(0x99, 30, 100);
    ev(0x99, 82, 100);
    CU_ASSERT_EQUAL(doom_opl_voices_in_use(&synth), 1u);
}

/* ---- synthetic: levels -------------------------------------------------- */

static void test_channel_volume_and_expression_scale_level(void)
{
    start_synthetic();
    ev(0x90, 60, 127);
    uint32_t v = voice_for(0, 60, 0);
    CU_ASSERT(v < DOOM_OPL_VOICES);

    ev(0xb0, 7, 127);
    uint8_t loud = synth.voices[v].car_level & 0x3fu;
    ev(0xb0, 11, 64);
    uint8_t half = synth.voices[v].car_level & 0x3fu;
    ev(0xb0, 11, 0);
    uint8_t none = synth.voices[v].car_level & 0x3fu;

    CU_ASSERT(loud < half);             /* attenuation grows as level falls */
    CU_ASSERT(half < none);
    CU_ASSERT_EQUAL(none, 0x3f);

    ev(0xb0, 121, 0);                   /* reset controllers: expression back */
    CU_ASSERT_EQUAL(synth.voices[v].car_level & 0x3fu, loud);
}

/* Master volume 0 is digital silence while the note keeps sounding in the
 * chip, and turning it back up brings the note back without a new key-on. */
static void test_master_volume_zero_is_silence(void)
{
    start_synthetic();
    ev(0x90, 60, 127);
    doom_opl_set_volume(&synth, 0);
    doom_opl_render(&synth, frames, ms_frames(100));
    CU_ASSERT(all_zero(frames, ms_frames(100)));
    CU_ASSERT_EQUAL(doom_opl_channels_keyed(&synth), 1u);

    doom_opl_set_volume(&synth, 127);
    doom_opl_render(&synth, frames, ms_frames(100));
    CU_ASSERT(peak(frames, ms_frames(100)) > 1000);
}

/* ---- the real freedoom2 GENMIDI ----------------------------------------- */

static int real_genmidi(doom_genmidi_t *g)
{
    const doom_wad_t *w = doom_wad_mounted();
    return w != NULL ? doom_genmidi_find(&w->wad, g) : DOOM_GENMIDI_ENOENT;
}

int suite_doom_opl_init(void)
{
    uint64_t paddr = 0, size = 0;

    if (doom_wad_disk_get(&paddr, &size) == 0) {
        (void)doom_wad_mount((const void *)(uintptr_t)paddr, (uint32_t)size);
    }
    return 0;
}

int suite_doom_opl_cleanup(void)
{
    doom_wad_unmount();
    return 0;
}

/*
 * Program 0 (freedoom's acoustic grand, a two-voice record), middle C at full
 * velocity, 100 ms.  The checksum was recorded from this implementation; a
 * change to it means the synth's output changed, which is either a bug or a
 * deliberate change that should re-record it here with a reason.
 */
#define REAL_PIANO_C4_FNV1A  0xe9c13c5bu

static void test_real_piano_note_is_bit_stable(void)
{
    doom_genmidi_t g;

    CU_ASSERT_EQUAL(real_genmidi(&g), DOOM_GENMIDI_OK);
    if (real_genmidi(&g) != DOOM_GENMIDI_OK) {
        return;
    }
    CU_ASSERT_EQUAL(doom_opl_init(&synth, &g), DOOM_OPL_OK);
    ev(0x90, 60, 127);
    CU_ASSERT_EQUAL(doom_opl_voices_in_use(&synth), 2u);
    doom_opl_render(&synth, frames, ms_frames(100));

    uint32_t h = fnv1a(frames, ms_frames(100));
    serial_print("doom_opl: program 0 note 60, 100 ms: fnv1a 0x");
    serial_print_hex(h);
    serial_print("\n");
    CU_ASSERT(peak(frames, ms_frames(100)) > 1000);
    CU_ASSERT_EQUAL(h, REAL_PIANO_C4_FNV1A);
}

/*
 * The epic's risk, measured: one second of D_OPENIN (freedoom2's largest
 * music lump), played the way Doom will play it -- advance the song a 35 Hz
 * tic at a time, apply that tic's events, render that tic's frames.  Printed,
 * not asserted: the number is wall clock under whatever QEMU accelerator ran
 * the suite, and a budget assertion would fail on a slow CI host rather than
 * on a slow synth.  What is asserted is that it was real work: events
 * arrived, voices sounded, the frames were not silence, and the clock moved.
 */
#define TICS_PER_SEC 35u

static void test_real_render_cost_of_d_openin(void)
{
    const doom_wad_t *w = doom_wad_mounted();
    doom_genmidi_t g;
    uint32_t size = 0;
    const uint8_t *song_lump = w != NULL ? wad_find_lump(&w->wad, "D_OPENIN", &size) : NULL;

    CU_ASSERT_PTR_NOT_NULL(song_lump);
    CU_ASSERT_EQUAL(real_genmidi(&g), DOOM_GENMIDI_OK);
    if (song_lump == NULL || real_genmidi(&g) != DOOM_GENMIDI_OK) {
        return;
    }

    static doom_midi_song_t song;
    static doom_midi_event_t evs[256];
    CU_ASSERT_EQUAL(doom_midi_open(song_lump, size, &song), DOOM_MIDI_OK);
    CU_ASSERT_EQUAL(doom_opl_init(&synth, &g), DOOM_OPL_OK);

    uint32_t events = 0, peak_voices = 0, done = 0;
    int32_t  loudest = 0;

    /* Real time, as test_doom_sound_k.c does it: the suite runs with
     * interrupts off, and the PIT's ms counter only moves with them on. */
    __asm__ volatile ("sti");
    uint32_t t0 = kernel_get_ticks_ms();

    for (uint32_t t = 0; t < TICS_PER_SEC; t++) {
        uint32_t from = t * 1000u / TICS_PER_SEC, to = (t + 1u) * 1000u / TICS_PER_SEC;
        int n;
        while ((n = doom_midi_advance(&song, to - from, evs, 256)) > 0) {
            for (int i = 0; i < n; i++) {
                doom_opl_event(&synth, &evs[i]);
            }
            events += (uint32_t)n;
            from = to;                      /* any remainder: zero more time */
            if (n < 256) {
                break;
            }
        }
        CU_ASSERT(n >= 0);
        uint32_t v = doom_opl_voices_in_use(&synth);
        peak_voices = v > peak_voices ? v : peak_voices;

        uint32_t nf = ms_frames(to) - done;
        doom_opl_render(&synth, frames, nf);
        int32_t p = peak(frames, nf);
        loudest = p > loudest ? p : loudest;
        done += nf;
    }

    uint32_t elapsed = kernel_get_ticks_ms() - t0;
    __asm__ volatile ("cli");
    doom_midi_close(&song);

    serial_print("doom_opl: 1000 ms of D_OPENIN (");
    serial_print_dec(events);
    serial_print(" events, peak ");
    serial_print_dec(peak_voices);
    serial_print(" voices) rendered in ");
    serial_print_dec(elapsed);
    serial_print(" ms = ");
    serial_print_dec(elapsed * 10u / TICS_PER_SEC / 10u);
    serial_print(".");
    serial_print_dec(elapsed * 10u / TICS_PER_SEC % 10u);
    serial_print(" ms per 28.57 ms tic\n");

    CU_ASSERT_EQUAL(done, 48000u);
    CU_ASSERT(elapsed > 0u);            /* else the clock was not running */
    CU_ASSERT(events > 0u);
    CU_ASSERT(peak_voices > 0u);
    CU_ASSERT(loudest > 1000);
}

void suite_doom_opl_tests(CU_pSuite s)
{
    CU_add_test(s, "output rate is the mixer's, native is 49716",
                test_rate_constant_matches_mixer);
    CU_add_test(s, "init rejects NULL synth and table",
                test_init_rejects_null);
    CU_add_test(s, "an idle chip renders digital silence",
                test_idle_chip_is_digital_silence);
    CU_add_test(s, "48000 frames consume exactly 49716 native samples",
                test_resampler_counts_native_samples);
    CU_add_test(s, "note 69 sounds 440 Hz on both channels",
                test_a440_has_440_crossings);
    CU_add_test(s, "a +1 semitone bend programs the next note's frequency",
                test_pitch_bend_moves_a_semitone);
    CU_add_test(s, "release ends in exact digital silence",
                test_release_reaches_digital_silence);
    CU_add_test(s, "note-on with velocity 0 is note-off",
                test_velocity_zero_is_note_off);
    CU_add_test(s, "CC123 and all_notes_off key everything off",
                test_all_notes_off_silences_everything);
    CU_add_test(s, "18 notes take 18 channels; the 19th steals the oldest",
                test_18_notes_use_18_channels_then_19th_steals_oldest);
    CU_add_test(s, "two-voice notes take two channels and yield the second first",
                test_double_voice_takes_two_and_yields_second_first);
    CU_add_test(s, "percussion plays fixed-note records, drops out-of-range keys",
                test_percussion_uses_fixed_note_records);
    CU_add_test(s, "CC7/CC11 scale the carrier level; CC121 resets expression",
                test_channel_volume_and_expression_scale_level);
    CU_add_test(s, "master volume 0 is digital silence and recovers",
                test_master_volume_zero_is_silence);

    CU_add_test(s, "a real piano note renders bit-stable PCM",
                test_real_piano_note_is_bit_stable);
    CU_add_test(s, "render cost of one second of D_OPENIN",
                test_real_render_cost_of_d_openin);
}
