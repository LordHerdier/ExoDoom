/*
 * test_doom_midi_k.c — SCRUM-215's acceptance tests for the MUS/MIDI song
 * reader and sequencer.
 *
 * Same two halves as test_doom_dmx_k.c, for the same reasons.  The synthetic
 * half builds songs byte by byte, because the inputs that must be *rejected*
 * (a track length that runs past the lump, a varlen with no terminator) do not
 * exist as files to point at, and because exact event times are only
 * assertable against a song whose every delta is known.  The real half opens
 * every `D_*` lump in the mounted freedoom2.wad and asserts the count rather
 * than spot-checking one, since a reader that handles 34 of 35 plays silence
 * on one level.
 */

#include "kunit.h"

#include "libos_doom/doom_midi.h"
#include "doom_wad.h"
#include "doom_wad_disk.h"
#include "string.h"
#include "wad.h"

#include <stddef.h>
#include <stdint.h>

/* ---- freedoom2 v0.13.0 reference data ---------------------------------- */

#define D_LUMP_COUNT     35u
#define D_LUMP_BYTES     1258992u
#define D_OPENIN_BYTES   83172u

/* ---- a tiny SMF builder ------------------------------------------------ */

#define BUF_LEN 512u

static uint8_t  buf[BUF_LEN];
static uint32_t blen;

static void emit(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n && blen < BUF_LEN; i++) {
        buf[blen++] = p[i];
    }
}

#define EMIT(...) do { const uint8_t b_[] = { __VA_ARGS__ }; emit(b_, sizeof(b_)); } while (0)

static void smf_begin(uint16_t format, uint16_t ntrks, uint16_t division)
{
    blen = 0;
    EMIT('M', 'T', 'h', 'd', 0, 0, 0, 6,
         (uint8_t)(format >> 8), (uint8_t)format,
         (uint8_t)(ntrks >> 8), (uint8_t)ntrks,
         (uint8_t)(division >> 8), (uint8_t)division);
}

/* Starts an MTrk; returns the offset of its length field for trk_end(). */
static uint32_t trk_begin(void)
{
    EMIT('M', 'T', 'r', 'k', 0, 0, 0, 0);
    return blen - 4u;
}

static void trk_end(uint32_t len_off)
{
    uint32_t len = blen - len_off - 4u;
    buf[len_off + 0] = (uint8_t)(len >> 24);
    buf[len_off + 1] = (uint8_t)(len >> 16);
    buf[len_off + 2] = (uint8_t)(len >> 8);
    buf[len_off + 3] = (uint8_t)len;
}

#define EOT(delta) EMIT((delta), 0xFF, 0x2F, 0x00)

/* Drain every event due within `ms` into ev[]; returns the count. */
static doom_midi_event_t ev[64];

static int run(doom_midi_song_t *s, uint32_t ms)
{
    return doom_midi_advance(s, ms, ev, 64);
}

/* ---- open: rejections --------------------------------------------------- */

static void test_open_rejects_null_and_short(void)
{
    doom_midi_song_t s;

    smf_begin(0, 1, 96);
    CU_ASSERT_EQUAL(doom_midi_open(NULL, 100, &s), DOOM_MIDI_EINVAL);
    CU_ASSERT_EQUAL(doom_midi_open(buf, 100, NULL), DOOM_MIDI_EINVAL);
    CU_ASSERT_EQUAL(doom_midi_open(buf, 0, &s), DOOM_MIDI_EINVAL);
    CU_ASSERT_EQUAL(doom_midi_open(buf, 3, &s), DOOM_MIDI_EINVAL);
    /* Magic present, header not: nothing to bounds-check against. */
    CU_ASSERT_EQUAL(doom_midi_open(buf, 8, &s), DOOM_MIDI_ETRUNC);
}

static void test_open_rejects_bad_magic_and_header(void)
{
    doom_midi_song_t s;
    uint32_t t;

    smf_begin(0, 1, 96);
    t = trk_begin(); EOT(0); trk_end(t);
    uint32_t n = blen;

    buf[0] = 'X';
    CU_ASSERT_EQUAL(doom_midi_open(buf, n, &s), DOOM_MIDI_EFORMAT);
    buf[0] = 'M';

    buf[9] = 2;   /* format 2: independent sequences, not a song */
    CU_ASSERT_EQUAL(doom_midi_open(buf, n, &s), DOOM_MIDI_EFORMAT);
    buf[9] = 0;

    buf[11] = 0;  /* zero tracks */
    CU_ASSERT_EQUAL(doom_midi_open(buf, n, &s), DOOM_MIDI_EFORMAT);
    buf[11] = 2;  /* format 0 with two tracks: rejected before they are read */
    CU_ASSERT_EQUAL(doom_midi_open(buf, n, &s), DOOM_MIDI_EFORMAT);
    buf[11] = 1;

    buf[12] = 0x80; /* SMPTE division */
    CU_ASSERT_EQUAL(doom_midi_open(buf, n, &s), DOOM_MIDI_EFORMAT);
    buf[12] = 0; buf[13] = 0;   /* division 0 */
    CU_ASSERT_EQUAL(doom_midi_open(buf, n, &s), DOOM_MIDI_EFORMAT);
    buf[13] = 96;

    CU_ASSERT_EQUAL(doom_midi_open(buf, n, &s), DOOM_MIDI_OK);
}

static void test_open_rejects_too_many_tracks(void)
{
    doom_midi_song_t s;

    smf_begin(1, DOOM_MIDI_MAX_TRACKS + 1u, 96);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_ETOOMANY);
}

static void test_open_rejects_track_past_lump(void)
{
    doom_midi_song_t s;
    uint32_t t;

    smf_begin(0, 1, 96);
    t = trk_begin(); EMIT(0x00, 0x90, 60, 100); EOT(10); trk_end(t);

    /* The MTrk length claims more than the lump holds. */
    buf[t + 3] += 1;
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_ETRUNC);
    buf[t + 3] -= 1;

    /* The header promises two tracks and the lump has one (format 1, since
     * format 0 with two is refused before any track is read). */
    buf[9]  = 1;
    buf[11] = 2;
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_ETRUNC);
}

static void test_open_rejects_malformed_events(void)
{
    doom_midi_song_t s;
    uint32_t t;

    /* Delta with no terminating byte, cut off by the end of the track. */
    smf_begin(0, 1, 96);
    t = trk_begin(); EMIT(0x81, 0x81); trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_ETRUNC);

    /* A five-byte varlen is malformed, not merely long. */
    smf_begin(0, 1, 96);
    t = trk_begin(); EMIT(0x81, 0x81, 0x81, 0x81, 0x01, 0x90, 60, 100); EOT(0); trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_EFORMAT);

    /* Note-on missing its velocity byte at the end of the track. */
    smf_begin(0, 1, 96);
    t = trk_begin(); EMIT(0x00, 0x90, 60); trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_ETRUNC);

    /* Sysex claiming more bytes than remain. */
    smf_begin(0, 1, 96);
    t = trk_begin(); EMIT(0x00, 0xF0, 0x7F, 0x01); trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_ETRUNC);

    /* Meta whose length runs past the track. */
    smf_begin(0, 1, 96);
    t = trk_begin(); EMIT(0x00, 0xFF, 0x03, 0x10, 'a'); trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_ETRUNC);

    /* No end-of-track. */
    smf_begin(0, 1, 96);
    t = trk_begin(); EMIT(0x00, 0x90, 60, 100); trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_ETRUNC);

    /* Data byte with the high bit set. */
    smf_begin(0, 1, 96);
    t = trk_begin(); EMIT(0x00, 0x90, 0x90, 100); EOT(0); trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_EFORMAT);

    /* Running status with nothing to run on. */
    smf_begin(0, 1, 96);
    t = trk_begin(); EMIT(0x00, 60, 100); EOT(0); trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_EFORMAT);

    /* Tempo of zero would divide time by zero. */
    smf_begin(0, 1, 96);
    t = trk_begin(); EMIT(0x00, 0xFF, 0x51, 0x03, 0, 0, 0); EOT(0); trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_EFORMAT);
}

static void test_every_truncation_is_rejected(void)
{
    doom_midi_song_t s;
    uint32_t t;

    smf_begin(1, 2, 96);
    t = trk_begin(); EMIT(0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20); EOT(0); trk_end(t);
    t = trk_begin(); EMIT(0x00, 0x90, 60, 100, 0x60, 0x80, 60, 0); EOT(0); trk_end(t);

    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);
    doom_midi_close(&s);

    /* A prefix of a valid song can only be valid if it is all of it: the
     * track lengths in the headers point past any shorter lump.  Every cut
     * must come back as an error code, never as OK and never as a crash. */
    uint32_t full = blen;
    uint32_t bad = 0;
    for (uint32_t n = 4; n < full; n++) {
        if (doom_midi_open(buf, n, &s) == DOOM_MIDI_OK) {
            bad++;
        }
    }
    CU_ASSERT_EQUAL(bad, 0u);
}

static void test_open_leaves_out_untouched_on_failure(void)
{
    doom_midi_song_t s;
    memset(&s, 0xA5, sizeof(s));

    smf_begin(0, 1, 96);
    CU_ASSERT_NOT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);

    const uint8_t *p = (const uint8_t *)&s;
    uint32_t dirty = 0;
    for (uint32_t i = 0; i < sizeof(s); i++) {
        dirty += (p[i] != 0xA5);
    }
    CU_ASSERT_EQUAL(dirty, 0u);
}

/* ---- sequencing --------------------------------------------------------- */

static void test_default_tempo_is_120_bpm(void)
{
    doom_midi_song_t s;
    uint32_t t;

    /* No tempo meta anywhere: 96 ticks of a 96-division song is one quarter
     * note, which at 120 BPM is exactly 500 ms. */
    smf_begin(0, 1, 96);
    t = trk_begin(); EMIT(0x00, 0x90, 60, 100, 0x60, 0x80, 60, 0); EOT(0); trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);

    CU_ASSERT_EQUAL(run(&s, 0), 1);
    CU_ASSERT_EQUAL(ev[0].status, 0x90);
    CU_ASSERT_EQUAL(ev[0].data1, 60);
    CU_ASSERT_EQUAL(ev[0].data2, 100);
    CU_ASSERT_EQUAL(ev[0].time_us, 0u);

    CU_ASSERT_EQUAL(run(&s, 499), 0);   /* 1 ms early */
    CU_ASSERT_EQUAL(run(&s, 1), 1);
    CU_ASSERT_EQUAL(ev[0].status, 0x80);
    CU_ASSERT_EQUAL(ev[0].time_us, 500000u);

    /* The end-of-track sits on the same tick as the note-off, so it was
     * consumed in the same call. */
    CU_ASSERT_TRUE(doom_midi_finished(&s));
    CU_ASSERT_EQUAL(run(&s, 1), 0);
    doom_midi_close(&s);
}

static void test_tempo_change_shifts_later_events(void)
{
    doom_midi_song_t s;
    uint32_t t;

    /* Tick 0 note-on; tick 96 tempo -> 250000 us/qn (240 BPM); tick 192
     * note-off.  The second quarter note is half as long as the first. */
    smf_begin(0, 1, 96);
    t = trk_begin();
    EMIT(0x00, 0x90, 60, 100);
    EMIT(0x60, 0xFF, 0x51, 0x03, 0x03, 0xD0, 0x90);
    EMIT(0x60, 0x80, 60, 0);
    EOT(0);
    trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);

    CU_ASSERT_EQUAL(run(&s, 1000), 2);
    CU_ASSERT_EQUAL(ev[0].time_us, 0u);
    CU_ASSERT_EQUAL(ev[1].status, 0x80);
    CU_ASSERT_EQUAL(ev[1].time_us, 500000u + 250000u);
    doom_midi_close(&s);
}

static void test_odd_division_does_not_drift(void)
{
    doom_midi_song_t s;
    uint32_t t;

    /* 7 ticks per quarter note does not divide 500000 evenly.  Fifty notes a
     * tick apart must still land exactly where 50 ticks * 500000 / 7 puts
     * them, which only holds if the remainder is carried, not rounded away. */
    smf_begin(0, 1, 7);
    t = trk_begin();
    for (int i = 0; i < 50; i++) {
        EMIT(0x01, 0x90, 60, 100);
    }
    EOT(0);
    trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);

    CU_ASSERT_EQUAL(run(&s, 60000), 50);
    CU_ASSERT_EQUAL(ev[49].time_us, (uint64_t)50 * 500000u / 7u);
    doom_midi_close(&s);
}

static void test_running_status_sysex_and_meta_are_handled(void)
{
    doom_midi_song_t s;
    uint32_t t;

    smf_begin(0, 1, 96);
    t = trk_begin();
    EMIT(0x00, 0xFF, 0x03, 0x04, 'n', 'a', 'm', 'e');   /* track name: skipped */
    EMIT(0x00, 0xC1, 5);                                /* program change: 1 data byte */
    EMIT(0x00, 0xF0, 0x03, 0x7E, 0x01, 0xF7);           /* sysex: skipped */
    EMIT(0x00, 0x91, 64, 90);                           /* note-on ch1 */
    EMIT(0x00, 66, 91);                                 /* running status */
    EMIT(0x00, 0xFF, 0x7F, 0x00);                       /* sequencer-specific, empty */
    EMIT(0x00, 68, 92);                                 /* running status survived a meta? */
    EOT(0);
    trk_end(t);

    /* The spec says a meta cancels running status; real files ignore that
     * and so does the reader, so the last note is still note-on ch1. */
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);
    CU_ASSERT_EQUAL(run(&s, 0), 4);
    CU_ASSERT_EQUAL(ev[3].status, 0x91);
    CU_ASSERT_EQUAL(ev[3].data1, 68);
    CU_ASSERT_EQUAL(ev[3].data2, 92);
    doom_midi_close(&s);

    /* A sysex still cancels it: running status straight after one is
     * malformed. */
    smf_begin(0, 1, 96);
    t = trk_begin();
    EMIT(0x00, 0x91, 64, 90);
    EMIT(0x00, 0xF0, 0x01, 0xF7);
    EMIT(0x00, 66, 91);
    EOT(0);
    trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_EFORMAT);

    /* The first song with the status restated. */
    smf_begin(0, 1, 96);
    t = trk_begin();
    EMIT(0x00, 0xFF, 0x03, 0x04, 'n', 'a', 'm', 'e');
    EMIT(0x00, 0xC1, 5);
    EMIT(0x00, 0xF0, 0x03, 0x7E, 0x01, 0xF7);
    EMIT(0x00, 0x91, 64, 90);
    EMIT(0x00, 66, 91);
    EMIT(0x00, 0xFF, 0x7F, 0x00);
    EMIT(0x00, 0x91, 68, 92);
    EOT(0);
    trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);

    CU_ASSERT_EQUAL(run(&s, 0), 4);
    CU_ASSERT_EQUAL(ev[0].status, 0xC1);
    CU_ASSERT_EQUAL(ev[0].data1, 5);
    CU_ASSERT_EQUAL(ev[0].data2, 0);
    CU_ASSERT_EQUAL(ev[1].status, 0x91);
    CU_ASSERT_EQUAL(ev[1].data1, 64);
    CU_ASSERT_EQUAL(ev[2].status, 0x91);
    CU_ASSERT_EQUAL(ev[2].data1, 66);
    CU_ASSERT_EQUAL(ev[2].data2, 91);
    CU_ASSERT_EQUAL(ev[3].data1, 68);
    doom_midi_close(&s);
}

static void test_varlen_delta_widths(void)
{
    doom_midi_song_t s;
    uint32_t t;

    /* Deltas of 1, 2, 3 and 4 varlen bytes: 0x7F, 0x80, 0x4000, 0x200000. */
    smf_begin(0, 1, 1);
    t = trk_begin();
    EMIT(0x7F, 0x90, 1, 1);
    EMIT(0x81, 0x00, 0x90, 2, 1);
    EMIT(0x81, 0x80, 0x00, 0x90, 3, 1);
    EMIT(0x81, 0x80, 0x80, 0x00, 0x90, 4, 1);
    EOT(0);
    trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);

    /* Division 1 and the default tempo: one tick is 500000 us. */
    int n = doom_midi_advance(&s, 0xFFFFFFFFu, ev, 64);
    CU_ASSERT_EQUAL(n, 4);
    uint64_t tick = 0x7Fu;
    CU_ASSERT_EQUAL(ev[0].time_us, tick * 500000u);
    tick += 0x80u;
    CU_ASSERT_EQUAL(ev[1].time_us, tick * 500000u);
    tick += 0x4000u;
    CU_ASSERT_EQUAL(ev[2].time_us, tick * 500000u);
    tick += 0x200000u;
    CU_ASSERT_EQUAL(ev[3].time_us, tick * 500000u);
    doom_midi_close(&s);
}

static void test_format1_tracks_merge_in_time_order(void)
{
    doom_midi_song_t s;
    uint32_t t;

    smf_begin(1, 3, 96);
    t = trk_begin();    /* conductor: tempo only */
    EMIT(0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20);
    EOT(0);
    trk_end(t);
    t = trk_begin();    /* ticks 0, 48, 96 */
    EMIT(0x00, 0x90, 10, 1, 0x30, 0x90, 11, 1, 0x30, 0x90, 12, 1);
    EOT(0);
    trk_end(t);
    t = trk_begin();    /* ticks 24, 48 (ties with track 1), 72 */
    EMIT(0x18, 0x91, 20, 1, 0x18, 0x91, 21, 1, 0x18, 0x91, 22, 1);
    EOT(0);
    trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);

    CU_ASSERT_EQUAL(run(&s, 5000), 6);
    static const uint8_t order[6] = { 10, 20, 11, 21, 22, 12 };
    for (int i = 0; i < 6; i++) {
        CU_ASSERT_EQUAL(ev[i].data1, order[i]);
    }
    /* Tie at tick 48: the lower-numbered track plays first. */
    CU_ASSERT_EQUAL(ev[2].time_us, ev[3].time_us);
    CU_ASSERT_EQUAL(ev[2].data1, 11);
    for (int i = 1; i < 6; i++) {
        CU_ASSERT_TRUE(ev[i].time_us >= ev[i - 1].time_us);
    }
    doom_midi_close(&s);
}

static void test_advance_hands_back_a_full_buffer_in_pieces(void)
{
    doom_midi_song_t s;
    uint32_t t;

    smf_begin(0, 1, 96);
    t = trk_begin();
    for (int i = 0; i < 10; i++) {
        EMIT(0x00, 0x90, (uint8_t)(40 + i), 1);
    }
    EOT(0);
    trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);

    /* Ten events due at once, room for four: nothing is dropped. */
    CU_ASSERT_EQUAL(doom_midi_advance(&s, 0, ev, 4), 4);
    CU_ASSERT_EQUAL(ev[0].data1, 40);
    CU_ASSERT_EQUAL(doom_midi_advance(&s, 0, ev, 4), 4);
    CU_ASSERT_EQUAL(ev[0].data1, 44);
    CU_ASSERT_EQUAL(doom_midi_advance(&s, 0, ev, 4), 2);
    CU_ASSERT_EQUAL(ev[1].data1, 49);
    doom_midi_close(&s);
}

static void test_looping_restarts_at_end_of_track(void)
{
    doom_midi_song_t s;
    uint32_t t;

    /* One note at tick 0, end-of-track at tick 96: a 500 ms loop. */
    smf_begin(0, 1, 96);
    t = trk_begin(); EMIT(0x00, 0x90, 60, 100); EOT(0x60); trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);
    doom_midi_set_looping(&s, 1);

    /* Passes start at 0, 500 and 1000 ms, and the times say so: one clock
     * for the whole song, not one per pass. */
    CU_ASSERT_EQUAL(run(&s, 0), 1);
    CU_ASSERT_EQUAL(ev[0].time_us, 0u);
    CU_ASSERT_EQUAL(run(&s, 500), 1);
    CU_ASSERT_EQUAL(ev[0].time_us, 500000u);
    CU_ASSERT_EQUAL(run(&s, 500), 1);
    CU_ASSERT_EQUAL(ev[0].time_us, 1000000u);
    CU_ASSERT_EQUAL(run(&s, 100), 0);
    CU_ASSERT_EQUAL(s.loops, 2u);
    CU_ASSERT_FALSE(doom_midi_finished(&s));
    doom_midi_close(&s);
}

static void test_loop_restores_the_default_tempo(void)
{
    doom_midi_song_t s;
    uint32_t t;

    /* Note A at tick 0, tempo -> 250000 at tick 96, note B at tick 192.  On
     * the first pass B lands at 500 + 250 = 750 ms.  If the second pass kept
     * the faster tempo the change would fire at 250 ms and B at 500 ms. */
    smf_begin(0, 1, 96);
    t = trk_begin();
    EMIT(0x00, 0x90, 60, 100);
    EMIT(0x60, 0xFF, 0x51, 0x03, 0x03, 0xD0, 0x90);
    EMIT(0x60, 0x90, 62, 100);
    EOT(0);
    trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);
    doom_midi_set_looping(&s, 1);

    CU_ASSERT_EQUAL(run(&s, 750), 3);        /* A, B, then A again */
    CU_ASSERT_EQUAL(ev[1].time_us, 750000u);
    CU_ASSERT_EQUAL(s.loops, 1u);

    CU_ASSERT_EQUAL(run(&s, 749), 0);        /* a leaked tempo would show B here */
    CU_ASSERT_EQUAL(run(&s, 1), 2);          /* B, then the third pass's A */
    CU_ASSERT_EQUAL(ev[0].data1, 62);
    CU_ASSERT_EQUAL(ev[0].time_us, 1500000u);
    CU_ASSERT_EQUAL(ev[1].data1, 60);
    CU_ASSERT_EQUAL(ev[1].time_us, 1500000u);
    doom_midi_close(&s);
}

static void test_overtaken_passes_are_skipped(void)
{
    doom_midi_song_t s;
    uint32_t t;

    /* The 500 ms one-note loop again, advanced 1100 ms in one call.  The pass
     * at 500 ms is wholly in the past by the time the first one ends, so it is
     * stepped over: the note at 0, then the pass at 1000 ms, and no replay of
     * the one in between. */
    smf_begin(0, 1, 96);
    t = trk_begin(); EMIT(0x00, 0x90, 60, 100); EOT(0x60); trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);
    doom_midi_set_looping(&s, 1);

    CU_ASSERT_EQUAL(run(&s, 1100), 2);
    CU_ASSERT_EQUAL(ev[0].time_us, 0u);
    CU_ASSERT_EQUAL(ev[1].time_us, 1000000u);
    CU_ASSERT_EQUAL(s.loops, 2u);

    CU_ASSERT_EQUAL(run(&s, 400), 1);        /* and it carries on from there */
    CU_ASSERT_EQUAL(ev[0].time_us, 1500000u);
    CU_ASSERT_EQUAL(s.loops, 3u);
    doom_midi_close(&s);

    /* A 1 us loop with no channel events, handed ~49 days at once.  Replaying
     * pass by pass would never fill `max` and spin ~4e15 times; skipping
     * makes it one step. */
    smf_begin(0, 1, 1);
    t = trk_begin();
    EMIT(0x00, 0xFF, 0x51, 0x03, 0x00, 0x00, 0x01);
    EOT(0x01);
    trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);
    doom_midi_set_looping(&s, 1);

    CU_ASSERT_EQUAL(run(&s, 0xFFFFFFFFu), 0);
    CU_ASSERT_FALSE(doom_midi_finished(&s));
    doom_midi_close(&s);
}

static void test_empty_song_does_not_spin_when_looping(void)
{
    doom_midi_song_t s;
    uint32_t t;

    smf_begin(0, 1, 96);
    t = trk_begin(); EOT(0); trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);
    doom_midi_set_looping(&s, 1);

    /* Zero length: nothing to repeat.  Must return, not loop forever. */
    CU_ASSERT_EQUAL(run(&s, 1000), 0);
    CU_ASSERT_TRUE(doom_midi_finished(&s));
    doom_midi_close(&s);
}

static void test_rewind_replays_from_the_start(void)
{
    doom_midi_song_t s;
    uint32_t t;

    smf_begin(0, 1, 96);
    t = trk_begin(); EMIT(0x00, 0x90, 60, 100); EOT(0); trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);

    CU_ASSERT_EQUAL(run(&s, 10), 1);
    CU_ASSERT_TRUE(doom_midi_finished(&s));
    doom_midi_rewind(&s);
    CU_ASSERT_FALSE(doom_midi_finished(&s));
    CU_ASSERT_EQUAL(run(&s, 0), 1);
    CU_ASSERT_EQUAL(ev[0].data1, 60);
    doom_midi_close(&s);
}

static void test_advance_rejects_bad_arguments(void)
{
    doom_midi_song_t s;
    uint32_t t;

    smf_begin(0, 1, 96);
    t = trk_begin(); EOT(0); trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &s), DOOM_MIDI_OK);

    CU_ASSERT_EQUAL(doom_midi_advance(NULL, 0, ev, 4), DOOM_MIDI_EINVAL);
    CU_ASSERT_EQUAL(doom_midi_advance(&s, 0, NULL, 4), DOOM_MIDI_EINVAL);
    CU_ASSERT_EQUAL(doom_midi_advance(&s, 0, NULL, 0), 0);
    doom_midi_close(&s);
}

/* ---- MUS ---------------------------------------------------------------- */

/*
 * A MUS lump with one note: key 60 (middle C) at volume 100 on channel 0, held
 * for 10 MUS ticks.  Header is the 14 bytes mus2mid expects with no
 * instrument list, then the score.
 */
static const uint8_t mus_lump[] = {
    'M', 'U', 'S', 0x1A,
    0x08, 0x00,         /* score length */
    0x0E, 0x00,         /* score start (right after this header) */
    0x01, 0x00,         /* primary channels */
    0x00, 0x00,         /* secondary channels */
    0x00, 0x00,         /* instrument count */
    0x90, 0xBC, 0x64, 0x0A,   /* last|play, ch0; key 60|has-volume; vol 100; delay 10 */
    0x80, 0x3C, 0x00,         /* last|release, ch0; key 60; delay 0 */
    0x60,                     /* score end */
};

/* A note with no volume byte (so the channel's last volume, 127 when fresh),
 * then score end; and a lump cut off right after a note's delay, which fails
 * with that delay still queued inside mus2mid. */
static const uint8_t mus_novol_lump[] = {
    'M', 'U', 'S', 0x1A, 0x04, 0x00, 0x0E, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x90, 0x3C, 0x00,         /* last|play, ch0; key 60, no volume; delay 0 */
    0x60,
};
static const uint8_t mus_cut_lump[] = {
    'M', 'U', 'S', 0x1A, 0x04, 0x00, 0x0E, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x90, 0xBC, 0x64, 0x0A,   /* volume 100, delay 10 -- then nothing */
};

static void test_mus_converts_to_its_midi_equivalent(void)
{
    doom_midi_song_t mus, mid;
    uint32_t t;

    CU_ASSERT_EQUAL(doom_midi_open(mus_lump, sizeof(mus_lump), &mus), DOOM_MIDI_OK);

    /* The same song as the MIDI file mus2mid emits for it: division 70 and
     * no tempo meta (so 120 BPM, which makes a MUS tick exactly 1/140 s), an
     * all-notes-off controller the first time a channel is used, then the two
     * notes 10 ticks apart. */
    smf_begin(0, 1, 70);
    t = trk_begin();
    EMIT(0x00, 0xB0, 0x7B, 0x00);
    EMIT(0x00, 0x90, 60, 100);
    EMIT(0x0A, 0x80, 60, 0);
    EOT(0);
    trk_end(t);
    CU_ASSERT_EQUAL(doom_midi_open(buf, blen, &mid), DOOM_MIDI_OK);

    doom_midi_event_t a[16], b[16];
    int na = doom_midi_advance(&mus, 10000, a, 16);
    int nb = doom_midi_advance(&mid, 10000, b, 16);

    CU_ASSERT_EQUAL(na, nb);
    CU_ASSERT_EQUAL(na, 3);
    for (int i = 0; i < na && i < nb; i++) {
        CU_ASSERT_EQUAL(a[i].status, b[i].status);
        CU_ASSERT_EQUAL(a[i].data1, b[i].data1);
        CU_ASSERT_EQUAL(a[i].data2, b[i].data2);
        CU_ASSERT_EQUAL(a[i].time_us, b[i].time_us);
    }
    CU_ASSERT_EQUAL(b[2].time_us, 10u * DOOM_MIDI_DEFAULT_TEMPO_US / 70u);
    doom_midi_close(&mus);
    doom_midi_close(&mid);
}

static void test_mus_conversions_do_not_share_state(void)
{
    doom_midi_song_t s;
    doom_midi_event_t e[8];

    /* Leave volume 100 on channel 0, then a failed conversion with 10 ticks
     * queued.  Neither may reach the next song. */
    CU_ASSERT_EQUAL(doom_midi_open(mus_lump, sizeof(mus_lump), &s), DOOM_MIDI_OK);
    doom_midi_close(&s);
    CU_ASSERT_EQUAL(doom_midi_open(mus_cut_lump, sizeof(mus_cut_lump), &s),
                    DOOM_MIDI_ECONV);

    CU_ASSERT_EQUAL(doom_midi_open(mus_novol_lump, sizeof(mus_novol_lump), &s),
                    DOOM_MIDI_OK);
    int n = doom_midi_advance(&s, 1000, e, 8);
    int seen = 0;
    for (int i = 0; i < n; i++) {
        if ((e[i].status & 0xF0) == 0x90) {
            CU_ASSERT_EQUAL(e[i].data2, 127);    /* not the previous song's 100 */
            CU_ASSERT_EQUAL(e[i].time_us, 0u);   /* not 10 ticks late */
            seen++;
        }
    }
    CU_ASSERT_EQUAL(seen, 1);
    doom_midi_close(&s);
}

static void test_garbage_mus_is_rejected(void)
{
    doom_midi_song_t s;
    uint8_t bad[sizeof(mus_lump)];

    /* Score start pointing past the lump. */
    memcpy(bad, mus_lump, sizeof(bad));
    bad[6] = 0xF0;
    CU_ASSERT_NOT_EQUAL(doom_midi_open(bad, sizeof(bad), &s), DOOM_MIDI_OK);

    /* Cut off in the middle of the score. */
    CU_ASSERT_NOT_EQUAL(doom_midi_open(mus_lump, 17, &s), DOOM_MIDI_OK);
}

/* ---- the real freedoom2 lumps ------------------------------------------ */

static const wad_t *real_wad(void)
{
    const doom_wad_t *w = doom_wad_mounted();
    return w != NULL ? &w->wad : NULL;
}

int suite_doom_midi_init(void)
{
    uint64_t paddr = 0, size = 0;

    if (doom_wad_disk_get(&paddr, &size) == 0) {
        (void)doom_wad_mount((const void *)(uintptr_t)paddr, (uint32_t)size);
    }
    return 0;
}

int suite_doom_midi_cleanup(void)
{
    doom_wad_unmount();
    return 0;
}

static void test_real_wad_is_mounted(void)
{
    CU_ASSERT_PTR_NOT_NULL(real_wad());
}

static void test_all_real_music_lumps_parse(void)
{
    const wad_t *w = real_wad();
    if (w == NULL) {
        CU_ASSERT_PTR_NOT_NULL(w);
        return;
    }

    uint32_t count = 0, bytes = 0, largest = 0, smf = 0, failed = 0, unfinished = 0;
    static doom_midi_event_t evs[256];

    for (uint32_t i = 0; i < w->numlumps; i++) {
        const uint8_t *e = w->dir_data + (size_t)i * 16u;
        if (e[8] != 'D' || e[9] != '_') {
            continue;
        }

        char name[9];
        memcpy(name, e + 8, 8);
        name[8] = '\0';

        uint32_t size = 0;
        const uint8_t *lump = wad_find_lump(w, name, &size);
        if (lump == NULL) {
            failed++;
            continue;
        }

        count++;
        bytes += size;
        if (size > largest) {
            largest = size;
        }
        if (size >= 4 && lump[0] == 'M' && lump[1] == 'T' && lump[2] == 'h' && lump[3] == 'd') {
            smf++;
        }

        doom_midi_song_t s;
        if (doom_midi_open(lump, size, &s) != DOOM_MIDI_OK) {
            failed++;
            continue;
        }

        /* Play it to the end in one go.  Every event must come out in time
         * order and the song must actually finish. */
        uint64_t last = 0;
        int      bad_order = 0;
        int      err = 0;
        uint32_t guard = 0;
        uint32_t ms = 0xFFFFFFFFu;

        while (!doom_midi_finished(&s) && guard++ < 100000u) {
            int n = doom_midi_advance(&s, ms, evs, 256);
            ms = 0;
            if (n < 0) {
                err = 1;
                break;
            }
            for (int k = 0; k < n; k++) {
                if (evs[k].time_us < last) {
                    bad_order = 1;
                }
                last = evs[k].time_us;
            }
        }
        if (err || bad_order) {
            failed++;
        }
        if (!doom_midi_finished(&s)) {
            unfinished++;
        }
        doom_midi_close(&s);
    }

    CU_ASSERT_EQUAL(count, D_LUMP_COUNT);
    CU_ASSERT_EQUAL(bytes, D_LUMP_BYTES);
    CU_ASSERT_EQUAL(largest, D_OPENIN_BYTES);
    CU_ASSERT_EQUAL(smf, D_LUMP_COUNT);     /* freedoom2 ships raw MIDI */
    CU_ASSERT_EQUAL(failed, 0u);
    CU_ASSERT_EQUAL(unfinished, 0u);
}

static void test_truncated_real_lump_never_opens(void)
{
    const wad_t *w = real_wad();
    uint32_t size = 0;
    const uint8_t *lump = w != NULL ? wad_find_lump(w, "D_OPENIN", &size) : NULL;

    CU_ASSERT_PTR_NOT_NULL(lump);
    if (lump == NULL) {
        return;
    }

    doom_midi_song_t s;
    uint32_t opened = 0;

    /* The lump cut at a spread of points, including one byte short.  Each is
     * a view onto the same bytes with a smaller length, so a parser that read
     * past `size` would sail on into the rest of the WAD and call it a song. */
    for (uint32_t n = 4; n < size; n += 997u) {
        opened += (doom_midi_open(lump, n, &s) == DOOM_MIDI_OK);
    }
    opened += (doom_midi_open(lump, size - 1u, &s) == DOOM_MIDI_OK);

    CU_ASSERT_EQUAL(opened, 0u);
}

static void test_strerror_covers_every_code(void)
{
    CU_ASSERT_STRING_EQUAL(doom_midi_strerror(DOOM_MIDI_OK), "ok");
    int codes[] = { DOOM_MIDI_EINVAL, DOOM_MIDI_EFORMAT, DOOM_MIDI_ETRUNC,
                    DOOM_MIDI_ETOOMANY, DOOM_MIDI_ECONV };
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        CU_ASSERT_STRING_NOT_EQUAL(doom_midi_strerror(codes[i]), "unknown error");
    }
    CU_ASSERT_STRING_EQUAL(doom_midi_strerror(-999), "unknown error");
}

void suite_doom_midi_tests(CU_pSuite s)
{
    CU_add_test(s, "open rejects NULL and sub-header lumps",
                test_open_rejects_null_and_short);
    CU_add_test(s, "open rejects bad magic, format 2, SMPTE, bad track counts",
                test_open_rejects_bad_magic_and_header);
    CU_add_test(s, "open rejects more tracks than the reader holds",
                test_open_rejects_too_many_tracks);
    CU_add_test(s, "open rejects a track that runs past the lump",
                test_open_rejects_track_past_lump);
    CU_add_test(s, "open rejects malformed and truncated events",
                test_open_rejects_malformed_events);
    CU_add_test(s, "every truncation of a valid song is rejected",
                test_every_truncation_is_rejected);
    CU_add_test(s, "open leaves *out untouched on failure",
                test_open_leaves_out_untouched_on_failure);
    CU_add_test(s, "no tempo meta means 120 BPM",
                test_default_tempo_is_120_bpm);
    CU_add_test(s, "a tempo change shifts the events after it",
                test_tempo_change_shifts_later_events);
    CU_add_test(s, "an odd division carries its remainder",
                test_odd_division_does_not_drift);
    CU_add_test(s, "running status, sysex and metas are handled",
                test_running_status_sysex_and_meta_are_handled);
    CU_add_test(s, "delta times of 1 to 4 varlen bytes",
                test_varlen_delta_widths);
    CU_add_test(s, "format-1 tracks merge in time order",
                test_format1_tracks_merge_in_time_order);
    CU_add_test(s, "a full output buffer drops nothing",
                test_advance_hands_back_a_full_buffer_in_pieces);
    CU_add_test(s, "looping restarts at end-of-track",
                test_looping_restarts_at_end_of_track);
    CU_add_test(s, "a loop restores the default tempo",
                test_loop_restores_the_default_tempo);
    CU_add_test(s, "passes the clock overtakes are skipped, not replayed",
                test_overtaken_passes_are_skipped);
    CU_add_test(s, "an empty song does not spin when looping",
                test_empty_song_does_not_spin_when_looping);
    CU_add_test(s, "rewind replays from the start",
                test_rewind_replays_from_the_start);
    CU_add_test(s, "advance rejects bad arguments",
                test_advance_rejects_bad_arguments);
    CU_add_test(s, "MUS converts to the events of its MIDI equivalent",
                test_mus_converts_to_its_midi_equivalent);
    CU_add_test(s, "MUS conversions do not share state",
                test_mus_conversions_do_not_share_state);
    CU_add_test(s, "garbage MUS is rejected",
                test_garbage_mus_is_rejected);
    CU_add_test(s, "strerror names every result code",
                test_strerror_covers_every_code);

    CU_add_test(s, "the real freedoom2 module is mounted",
                test_real_wad_is_mounted);
    CU_add_test(s, "all 35 real D_* lumps open and play to the end",
                test_all_real_music_lumps_parse);
    CU_add_test(s, "a truncated real lump never opens",
                test_truncated_real_lump_never_opens);
}
