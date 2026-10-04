/*
 * test_pcm_mixer_music_k.c — the mixer's streaming music voice (SCRUM-218).
 *
 * Hardware-free for the same reason test_pcm_mixer_k.c is: src/pcm_mixer.c
 * includes no device header, so the real mixer can be driven against static
 * arrays and every sample it writes asserted.  Nothing here needs a
 * controller, and nothing here is allowed to assume one.
 *
 * The voice is a ring that a producer fills and pcm_mixer_render() drains,
 * so nearly every way it can be wrong is a way of losing track of a frame:
 * playing one twice, skipping one, playing something stale after a gap, or
 * getting the wrap off by one -- which in a ring is not a one-off glitch but
 * a tick once per lap, forever.  None of those can be heard in CI.  So the
 * music fed in below is not a tone but a COUNTER: every frame carries its
 * own index, differently on each channel, and what comes out is compared
 * against the frame that should be there.  A repeat, a skip, a swap of left
 * and right or a stale frame all show up as a wrong number in a known
 * position.
 *
 * The ring is whatever size pcm_mixer_music_open() is given, which is what
 * makes that affordable: most cases use sixteen frames, so a "lap" is
 * sixteen frames long and a few hundred laps cost nothing.
 */

#include "kunit.h"
#include "pcm_mixer.h"
#include "doom_dmx.h"

#include <stdint.h>

/* ── Fixtures ──────────────────────────────────────────────────────────── */

#define RENDER_FRAMES 64u
static int16_t g_out[RENDER_FRAMES * PCM_MIXER_CHANNELS];
static int16_t g_ref[RENDER_FRAMES * PCM_MIXER_CHANNELS];

/* A ring small enough that wrapping is the common case, not the rare one. */
#define SMALL_RING 16u
static int16_t g_ring[SMALL_RING * PCM_MIXER_CHANNELS];

/* And the real one.  Static: 48 KiB is three times the kernel stack. */
static int16_t g_full_ring[PCM_MIXER_MUSIC_FRAMES * PCM_MIXER_CHANNELS];

/* Scratch for building what gets written. */
static int16_t g_src[RENDER_FRAMES * PCM_MIXER_CHANNELS];

/*
 * Frame `i` of the test signal.  Multiplying by an odd constant and masking
 * walks the range in an order with no short period, so neighbouring frames
 * differ, a frame from a lap ago differs, and the two channels differ from
 * each other.  Kept to +/-16384 so it can be summed with an sfx voice
 * without reaching the clipper unless a test means it to.
 */
static int16_t sig_l(uint32_t i)
{
    return (int16_t)((int32_t)((i * 7919u) & 0x7FFFu) - 16384);
}

static int16_t sig_r(uint32_t i)
{
    return (int16_t)((int32_t)(((i * 104729u) >> 2) & 0x7FFFu) - 16384);
}

/* Fill g_src with frames [first, first + n) of the signal. */
static void make_src(uint32_t first, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        g_src[k * PCM_MIXER_CHANNELS]     = sig_l(first + k);
        g_src[k * PCM_MIXER_CHANNELS + 1] = sig_r(first + k);
    }
}

static void poison(int16_t *buf, uint32_t samples)
{
    for (uint32_t i = 0; i < samples; i++) {
        buf[i] = (int16_t)0x5A5A;
    }
}

/* The mixer's own volume mapping, restated so a test can say what a level
 * SHOULD do rather than read it back from the code under test. */
static int32_t q8_for(int vol)
{
    return (vol * 256 + 63) / PCM_MIXER_VOL_MAX;
}

static int16_t clip(int32_t v)
{
    if (v > 32767)  return (int16_t)32767;
    if (v < -32768) return (int16_t)(-32768);
    return (int16_t)v;
}

/* Same builder test_pcm_mixer_k.c uses: these bodies are not lumps. */
static doom_dmx_t dmx_of(const uint8_t *samples, uint32_t n, uint32_t rate_hz)
{
    doom_dmx_t d;
    d.format      = DOOM_DMX_FORMAT_PCM;
    d.rate_hz     = rate_hz;
    d.samples     = samples;
    d.num_samples = n;
    return d;
}

/* 160 -> (160 - 128) << 8 = 8192: loud enough to see, far from the clipper. */
static const uint8_t g_mid[RENDER_FRAMES] = {
    160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160,
    160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160,
    160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160,
    160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160,
};

/* 255 -> 32512, the largest an sfx voice can be: two of them exceed int16. */
static const uint8_t g_max[RENDER_FRAMES] = {
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

static int suite_init(void)
{
    pcm_mixer_reset();
    return 0;
}

static int suite_cleanup(void)
{
    /* Detach before anything else could free or reuse the storage. */
    pcm_mixer_reset();
    return 0;
}

/* ── A voice with no ring ──────────────────────────────────────────────── */

static void test_closed_voice_is_silent_and_takes_nothing(void)
{
    pcm_mixer_reset();

    CU_ASSERT_EQUAL(pcm_mixer_music_is_open(), 0);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 0u);
    CU_ASSERT_EQUAL(pcm_mixer_music_space(), 0u);

    make_src(0, 8);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 8), 0u);

    poison(g_out, RENDER_FRAMES * PCM_MIXER_CHANNELS);
    pcm_mixer_render(g_out, RENDER_FRAMES);

    int nonzero = 0;
    for (uint32_t i = 0; i < RENDER_FRAMES * PCM_MIXER_CHANNELS; i++) {
        if (g_out[i] != 0) nonzero++;
    }
    CU_ASSERT_EQUAL(nonzero, 0);
    CU_ASSERT_EQUAL(pcm_mixer_music_underruns(), 0u);

    /* The producer-side calls are all harmless with nothing attached. */
    pcm_mixer_music_flush();
    pcm_mixer_music_close();
    CU_ASSERT_EQUAL(pcm_mixer_music_is_open(), 0);
}

static void test_open_validates_and_is_exclusive(void)
{
    pcm_mixer_reset();

    CU_ASSERT_EQUAL(pcm_mixer_music_open(NULL, SMALL_RING), PCM_MIXER_EINVAL);
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, 0), PCM_MIXER_EINVAL);
    CU_ASSERT_EQUAL(pcm_mixer_music_is_open(), 0);

    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);
    CU_ASSERT_EQUAL(pcm_mixer_music_is_open(), 1);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 0u);
    CU_ASSERT_EQUAL(pcm_mixer_music_space(), SMALL_RING);

    /* One music voice.  A second ring is refused rather than swapped in,
     * and the first is left exactly as it was. */
    make_src(0, 4);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 4), 4u);
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_full_ring, PCM_MIXER_MUSIC_FRAMES),
                    PCM_MIXER_EBUSY);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 4u);
    CU_ASSERT_EQUAL(pcm_mixer_music_space(), SMALL_RING - 4u);

    pcm_mixer_music_close();
    CU_ASSERT_EQUAL(pcm_mixer_music_is_open(), 0);
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_full_ring, PCM_MIXER_MUSIC_FRAMES),
                    PCM_MIXER_OK);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 0u);
}

/* ── What comes out ────────────────────────────────────────────────────── */

/*
 * At full volume the music voice is a bit-exact pass-through, both channels,
 * extremes included.  This is the anchor for everything else here, as the
 * unity-rate test is for the sfx voices: if a frame is not reproduced
 * exactly when nothing else is happening, no later assertion about mixing
 * or volume means anything.
 */
static void test_full_volume_passes_frames_through_exactly(void)
{
    pcm_mixer_reset();
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);

    static const int16_t frames[6 * 2] = {
        0, 0,
        1, -1,
        32767, -32768,          /* full scale, opposite signs per channel */
        -32768, 32767,
        12345, -23456,
        -1, 1,
    };
    CU_ASSERT_EQUAL(pcm_mixer_music_write(frames, 6), 6u);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 6u);

    poison(g_out, RENDER_FRAMES * PCM_MIXER_CHANNELS);
    pcm_mixer_render(g_out, 6);

    for (uint32_t i = 0; i < 6 * 2; i++) {
        CU_ASSERT_EQUAL(g_out[i], frames[i]);
    }
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 0u);
    CU_ASSERT_EQUAL(pcm_mixer_music_underruns(), 0u);
}

/*
 * Music and sound effects in the same frame: summed, then clipped ONCE.
 *
 * The first half is the plain sum.  The second is the case that tells one
 * clip from two.  Two full-scale sfx voices are 65024 on the left -- over
 * the top on their own -- and the music is at full negative.  The true sum
 * is 32256 and that is what must come out.  A mixer that clipped the sfx
 * bus before adding the music would produce 32767 - 32768 = -1 instead: a
 * loud moment turned into silence.
 */
static void test_music_sums_with_sfx_under_one_clip(void)
{
    pcm_mixer_reset();
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);

    /* An sfx voice hard left at unity: 8192 on the left, nothing on the
     * right (see gain_for() in src/pcm_mixer.c). */
    doom_dmx_t mid = dmx_of(g_mid, RENDER_FRAMES, PCM_MIXER_RATE_HZ);
    CU_ASSERT_TRUE(pcm_mixer_start(&mid, PCM_MIXER_VOL_MAX, 0, 64) >= 0);

    static const int16_t music[4 * 2] = {
        1000, 2000,             /* plain sum                              */
        -9000, -3000,           /* music pulling the other way            */
        30000, 30000,           /* 38192 on the left: clips high          */
        -32768, -32768,         /* -24576 on the left, full negative right */
    };
    CU_ASSERT_EQUAL(pcm_mixer_music_write(music, 4), 4u);

    pcm_mixer_render(g_out, 4);
    CU_ASSERT_EQUAL(g_out[0], 8192 + 1000);
    CU_ASSERT_EQUAL(g_out[1], 2000);
    CU_ASSERT_EQUAL(g_out[2], 8192 - 9000);
    CU_ASSERT_EQUAL(g_out[3], -3000);
    CU_ASSERT_EQUAL(g_out[4], 32767);
    CU_ASSERT_EQUAL(g_out[5], 30000);
    CU_ASSERT_EQUAL(g_out[6], 8192 - 32768);
    CU_ASSERT_EQUAL(g_out[7], -32768);

    /* One clip, not two. */
    pcm_mixer_reset();
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);

    doom_dmx_t max = dmx_of(g_max, RENDER_FRAMES, PCM_MIXER_RATE_HZ);
    CU_ASSERT_TRUE(pcm_mixer_start(&max, PCM_MIXER_VOL_MAX, 0, 64) >= 0);
    CU_ASSERT_TRUE(pcm_mixer_start(&max, PCM_MIXER_VOL_MAX, 0, 64) >= 0);

    static const int16_t low[1 * 2] = { -32768, 0 };
    CU_ASSERT_EQUAL(pcm_mixer_music_write(low, 1), 1u);

    pcm_mixer_render(g_out, 1);
    CU_ASSERT_EQUAL(g_out[0], 2 * 32512 - 32768);
    CU_ASSERT_EQUAL(g_out[1], 0);
}

/*
 * The music level moves the music and nothing else.
 *
 * The same sfx voice and the same music frames are rendered at four levels.
 * Each frame of each render must equal the sfx-only reference plus the music
 * scaled by that level -- so the sfx contribution is the same number in all
 * four, to the bit, and at level 0 the output IS the reference.
 */
static void test_volume_moves_only_the_music(void)
{
    static const int levels[4] = { PCM_MIXER_VOL_MAX, 80, 17, 0 };
    doom_dmx_t mid = dmx_of(g_mid, RENDER_FRAMES, PCM_MIXER_RATE_HZ);

    /* The reference: that sfx voice, centred, with no music voice at all. */
    pcm_mixer_reset();
    CU_ASSERT_TRUE(pcm_mixer_start(&mid, 100, PCM_MIXER_SEP_CENTRE, 64) >= 0);
    pcm_mixer_render(g_ref, SMALL_RING);

    for (uint32_t n = 0; n < 4; n++) {
        pcm_mixer_reset();
        CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);
        pcm_mixer_music_set_volume(levels[n]);
        CU_ASSERT_EQUAL(pcm_mixer_music_volume(), levels[n]);

        CU_ASSERT_TRUE(pcm_mixer_start(&mid, 100, PCM_MIXER_SEP_CENTRE, 64) >= 0);

        make_src(100, SMALL_RING);
        CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, SMALL_RING), SMALL_RING);
        pcm_mixer_render(g_out, SMALL_RING);

        int32_t g = q8_for(levels[n]);
        int wrong = 0;
        for (uint32_t i = 0; i < SMALL_RING * PCM_MIXER_CHANNELS; i++) {
            int32_t want = (int32_t)g_ref[i] + (((int32_t)g_src[i] * g) >> 8);
            if (g_out[i] != clip(want)) wrong++;
        }
        CU_ASSERT_EQUAL(wrong, 0);
    }

    /* And the level is the voice's, not the ring's: it survives a close and
     * a fresh open, because Doom sets it before any song exists. */
    pcm_mixer_reset();
    pcm_mixer_music_set_volume(40);
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);
    CU_ASSERT_EQUAL(pcm_mixer_music_volume(), 40);
    pcm_mixer_music_close();
    CU_ASSERT_EQUAL(pcm_mixer_music_volume(), 40);
}

static void test_volume_is_clamped_and_round_trips(void)
{
    pcm_mixer_reset();
    CU_ASSERT_EQUAL(pcm_mixer_music_volume(), PCM_MIXER_VOL_MAX);

    int wrong = 0;
    for (int v = 0; v <= PCM_MIXER_VOL_MAX; v++) {
        pcm_mixer_music_set_volume(v);
        if (pcm_mixer_music_volume() != v) wrong++;
    }
    CU_ASSERT_EQUAL(wrong, 0);

    pcm_mixer_music_set_volume(-5);
    CU_ASSERT_EQUAL(pcm_mixer_music_volume(), 0);
    pcm_mixer_music_set_volume(9999);
    CU_ASSERT_EQUAL(pcm_mixer_music_volume(), PCM_MIXER_VOL_MAX);
}

/* ── Running dry ───────────────────────────────────────────────────────── */

/*
 * A ring that runs out mid-render: the frames it had come out, the rest are
 * exact silence, and each silent frame is counted.  Then data resumes, and
 * what plays is the NEW data from its first frame -- not a replay of
 * anything from before the gap, and not a frame skipped to "catch up".
 */
static void test_starved_ring_is_silent_counted_and_recovers(void)
{
    pcm_mixer_reset();
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);

    make_src(0, 10);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 10), 10u);

    poison(g_out, RENDER_FRAMES * PCM_MIXER_CHANNELS);
    pcm_mixer_render(g_out, 16);

    int wrong = 0;
    for (uint32_t f = 0; f < 16; f++) {
        int16_t want_l = f < 10 ? sig_l(f) : 0;
        int16_t want_r = f < 10 ? sig_r(f) : 0;
        if (g_out[f * 2] != want_l || g_out[f * 2 + 1] != want_r) wrong++;
    }
    CU_ASSERT_EQUAL(wrong, 0);
    CU_ASSERT_EQUAL(pcm_mixer_music_underruns(), 6u);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 0u);

    /* Still dry: more silence, more counted. */
    pcm_mixer_render(g_out, 4);
    CU_ASSERT_EQUAL(g_out[0], 0);
    CU_ASSERT_EQUAL(g_out[7], 0);
    CU_ASSERT_EQUAL(pcm_mixer_music_underruns(), 10u);

    /* Data again.  Frames 500..504 -- nothing like what was there before. */
    make_src(500, 5);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 5), 5u);

    poison(g_out, RENDER_FRAMES * PCM_MIXER_CHANNELS);
    pcm_mixer_render(g_out, 8);

    wrong = 0;
    for (uint32_t f = 0; f < 8; f++) {
        int16_t want_l = f < 5 ? sig_l(500 + f) : 0;
        int16_t want_r = f < 5 ? sig_r(500 + f) : 0;
        if (g_out[f * 2] != want_l || g_out[f * 2 + 1] != want_r) wrong++;
    }
    CU_ASSERT_EQUAL(wrong, 0);
    CU_ASSERT_EQUAL(pcm_mixer_music_underruns(), 13u);
}

/*
 * Sound effects are not the music's problem.  With the ring dry, an sfx
 * voice renders exactly what it would have rendered with no music voice at
 * all -- an underrun must cost the soundtrack and nothing else.
 */
static void test_underrun_does_not_disturb_sfx(void)
{
    doom_dmx_t mid = dmx_of(g_mid, RENDER_FRAMES, PCM_MIXER_RATE_HZ);

    pcm_mixer_reset();
    CU_ASSERT_TRUE(pcm_mixer_start(&mid, 90, 40, 64) >= 0);
    pcm_mixer_render(g_ref, 32);

    pcm_mixer_reset();
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);

    /* One frame of digital silence: enough to make the voice "playing", so
     * the 31 frames after it are genuine underruns, without adding anything
     * to the output that would have to be subtracted back out. */
    static const int16_t silence[2] = { 0, 0 };
    CU_ASSERT_EQUAL(pcm_mixer_music_write(silence, 1), 1u);
    CU_ASSERT_TRUE(pcm_mixer_start(&mid, 90, 40, 64) >= 0);
    pcm_mixer_render(g_out, 32);

    int wrong = 0;
    for (uint32_t i = 0; i < 32 * PCM_MIXER_CHANNELS; i++) {
        if (g_out[i] != g_ref[i]) wrong++;
    }
    CU_ASSERT_EQUAL(wrong, 0);
    CU_ASSERT_EQUAL(pcm_mixer_music_underruns(), 31u);
}

/*
 * Idle is not starved.  A voice that has been opened but never written to,
 * or that has been stopped, is silent ON PURPOSE -- counting those frames
 * would make the counter measure how long nothing was playing instead of
 * how often something was late, and it would be useless for judging the
 * ring size, which is the only thing it is for.
 */
static void test_idle_voice_is_not_an_underrun(void)
{
    pcm_mixer_reset();
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);

    pcm_mixer_render(g_out, RENDER_FRAMES);          /* never written to */
    CU_ASSERT_EQUAL(pcm_mixer_music_underruns(), 0u);

    make_src(0, 6);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 6), 6u);
    pcm_mixer_render(g_out, 6);                      /* exactly consumed  */
    CU_ASSERT_EQUAL(pcm_mixer_music_underruns(), 0u);

    pcm_mixer_music_flush();                         /* stopped           */
    pcm_mixer_render(g_out, RENDER_FRAMES);
    CU_ASSERT_EQUAL(pcm_mixer_music_underruns(), 0u);

    /* Written to again, so playing again -- and now running dry counts. */
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 2), 2u);
    pcm_mixer_render(g_out, 5);
    CU_ASSERT_EQUAL(pcm_mixer_music_underruns(), 3u);

    /* The count belongs to one open: still readable after the close, so
     * whoever is tearing the stream down can report it, and started afresh
     * by the next open. */
    pcm_mixer_music_close();
    CU_ASSERT_EQUAL(pcm_mixer_music_underruns(), 3u);
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);
    CU_ASSERT_EQUAL(pcm_mixer_music_underruns(), 0u);
}

/* ── The wrap ──────────────────────────────────────────────────────────── */

/*
 * One render that crosses the end of the storage.
 *
 * Twelve frames in, eight out, twelve more in: the second write starts at
 * slot 12 of 16, so four frames go at the end and eight at the start.  The
 * render that follows reads slots 8..15 and then 0..7 -- across the seam in
 * the middle of one call -- and must produce frames 8..23 of the signal in
 * order.  The classic mistakes here are a write that runs off the end of the
 * storage, and a read that repeats or drops the frame at the seam.
 */
static void test_wrap_mid_render_is_continuous(void)
{
    pcm_mixer_reset();

    /* Guard words either side, so "ran off the end" is a failed assertion
     * rather than a corrupted neighbour. */
    static int16_t guarded[(SMALL_RING + 2) * PCM_MIXER_CHANNELS];
    poison(guarded, (SMALL_RING + 2) * PCM_MIXER_CHANNELS);
    int16_t *ring = guarded + PCM_MIXER_CHANNELS;

    CU_ASSERT_EQUAL(pcm_mixer_music_open(ring, SMALL_RING), PCM_MIXER_OK);

    make_src(0, 12);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 12), 12u);
    pcm_mixer_render(g_out, 8);

    make_src(12, 12);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 12), 12u);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 16u);
    CU_ASSERT_EQUAL(pcm_mixer_music_space(), 0u);

    poison(g_out, RENDER_FRAMES * PCM_MIXER_CHANNELS);
    pcm_mixer_render(g_out, 16);

    int wrong = 0;
    for (uint32_t f = 0; f < 16; f++) {
        if (g_out[f * 2] != sig_l(8 + f) || g_out[f * 2 + 1] != sig_r(8 + f)) {
            wrong++;
        }
    }
    CU_ASSERT_EQUAL(wrong, 0);
    CU_ASSERT_EQUAL(pcm_mixer_music_underruns(), 0u);

    CU_ASSERT_EQUAL(guarded[0], (int16_t)0x5A5A);
    CU_ASSERT_EQUAL(guarded[1], (int16_t)0x5A5A);
    CU_ASSERT_EQUAL(guarded[(SMALL_RING + 1) * 2], (int16_t)0x5A5A);
    CU_ASSERT_EQUAL(guarded[(SMALL_RING + 1) * 2 + 1], (int16_t)0x5A5A);
}

/*
 * The same property, for a long time and at awkward sizes.
 *
 * Writes and renders of mismatched lengths, none of which divides the ring,
 * for a few hundred laps.  The test keeps its own count of how many frames
 * have gone in and how many have come out, and every rendered frame must be
 * the next one of the signal -- or silence, exactly when the ring was dry.
 * An off-by-one at the seam would show up within the first lap and then
 * once every lap after it.
 */
static void test_frames_come_out_in_order_across_many_laps(void)
{
    static const uint32_t write_sizes[5]  = { 7, 3, 11, 5, 13 };
    static const uint32_t render_sizes[4] = { 5, 9, 2, 8 };

    pcm_mixer_reset();
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);

    uint32_t produced = 0;      /* frames the ring has accepted          */
    uint32_t consumed = 0;      /* frames that have been played          */
    uint32_t starved  = 0;      /* frames that had to be silence         */
    uint32_t wrong    = 0;
    uint32_t short_writes = 0;

    for (uint32_t step = 0; step < 1500; step++) {
        uint32_t want = write_sizes[step % 5];
        make_src(produced, want);

        uint32_t room = pcm_mixer_music_space();
        uint32_t took = pcm_mixer_music_write(g_src, want);
        if (took != (want < room ? want : room)) wrong++;
        if (took < want) short_writes++;
        produced += took;

        uint32_t n = render_sizes[step % 4];
        uint32_t avail = produced - consumed;
        pcm_mixer_render(g_out, n);

        for (uint32_t f = 0; f < n; f++) {
            int16_t want_l = f < avail ? sig_l(consumed + f) : 0;
            int16_t want_r = f < avail ? sig_r(consumed + f) : 0;
            if (g_out[f * 2] != want_l || g_out[f * 2 + 1] != want_r) wrong++;
        }
        if (n > avail) {
            starved  += n - avail;
            consumed += avail;
        } else {
            consumed += n;
        }
    }

    CU_ASSERT_EQUAL(wrong, 0u);
    CU_ASSERT_EQUAL(pcm_mixer_music_underruns(), starved);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), produced - consumed);

    /* It has to have actually exercised what it claims to: hundreds of
     * laps, with both a full ring and an empty one met on the way. */
    CU_ASSERT_TRUE(consumed > 200u * SMALL_RING);
    CU_ASSERT_TRUE(short_writes > 0u);
    CU_ASSERT_TRUE(starved > 0u);
}

/* ── A full ring ───────────────────────────────────────────────────────── */

/*
 * A write that does not fit is taken as far as it goes and says so.  It
 * never blocks -- the caller is a syscall body with interrupts off -- and it
 * never overwrites audio that has not been played, which would be a skip in
 * the song.  The fill level is what lets the producer size its next write
 * without a second question.
 */
static void test_short_write_reports_what_fit(void)
{
    pcm_mixer_reset();
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);

    make_src(0, 20);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 20), SMALL_RING);
    CU_ASSERT_EQUAL(pcm_mixer_music_space(), 0u);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), SMALL_RING);

    /* Full: nothing more, and nothing lost. */
    make_src(900, 4);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 4), 0u);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), SMALL_RING);

    pcm_mixer_render(g_out, 5);
    CU_ASSERT_EQUAL(pcm_mixer_music_space(), 5u);

    make_src(16, 20);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 20), 5u);

    /* What is queued is frames 5..20 of the signal: the original sixteen
     * minus the five played, then the five that fit. The refused frames
     * 900.. appear nowhere. */
    pcm_mixer_render(g_out, SMALL_RING);
    int wrong = 0;
    for (uint32_t f = 0; f < SMALL_RING; f++) {
        if (g_out[f * 2] != sig_l(5 + f) || g_out[f * 2 + 1] != sig_r(5 + f)) {
            wrong++;
        }
    }
    CU_ASSERT_EQUAL(wrong, 0);

    CU_ASSERT_EQUAL(pcm_mixer_music_write(NULL, 4), 0u);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 0), 0u);
}

/*
 * The recommended ring is what its name says: twelve pages, 12288 stereo
 * frames, 256 ms at the output rate -- and it holds exactly that many.
 */
static void test_recommended_ring_holds_a_quarter_second(void)
{
    CU_ASSERT_EQUAL(PCM_MIXER_MUSIC_BYTES, PCM_MIXER_MUSIC_PAGES * 4096u);
    CU_ASSERT_EQUAL(PCM_MIXER_MUSIC_FRAMES * PCM_MIXER_CHANNELS
                    * sizeof(int16_t), PCM_MIXER_MUSIC_BYTES);
    CU_ASSERT_EQUAL(PCM_MIXER_MUSIC_FRAMES, 12288u);
    CU_ASSERT_EQUAL(PCM_MIXER_MUSIC_FRAMES * 1000u / PCM_MIXER_RATE_HZ, 256u);

    pcm_mixer_reset();
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_full_ring, PCM_MIXER_MUSIC_FRAMES),
                    PCM_MIXER_OK);

    uint32_t produced = 0;
    for (uint32_t i = 0; i < 400; i++) {
        make_src(produced, RENDER_FRAMES);
        uint32_t took = pcm_mixer_music_write(g_src, RENDER_FRAMES);
        produced += took;
        if (took < RENDER_FRAMES) break;
    }
    CU_ASSERT_EQUAL(produced, PCM_MIXER_MUSIC_FRAMES);
    CU_ASSERT_EQUAL(pcm_mixer_music_space(), 0u);

    /* And it plays back from the first frame to the last. */
    uint32_t wrong = 0;
    for (uint32_t done = 0; done < PCM_MIXER_MUSIC_FRAMES; done += RENDER_FRAMES) {
        pcm_mixer_render(g_out, RENDER_FRAMES);
        for (uint32_t f = 0; f < RENDER_FRAMES; f++) {
            if (g_out[f * 2] != sig_l(done + f) ||
                g_out[f * 2 + 1] != sig_r(done + f)) {
                wrong++;
            }
        }
    }
    CU_ASSERT_EQUAL(wrong, 0u);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 0u);
}

/* ── Stop ──────────────────────────────────────────────────────────────── */

/*
 * Stop means now.  Whatever was queued is dropped, the very next frame is
 * silent, and when playback starts again it starts with the new frames --
 * the dropped tail does not resurface.
 */
static void test_flush_drops_the_tail_immediately(void)
{
    pcm_mixer_reset();
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);

    make_src(0, 12);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 12), 12u);
    pcm_mixer_render(g_out, 4);
    CU_ASSERT_EQUAL(g_out[0], sig_l(0));
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 8u);

    pcm_mixer_music_flush();
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 0u);
    CU_ASSERT_EQUAL(pcm_mixer_music_space(), SMALL_RING);
    CU_ASSERT_EQUAL(pcm_mixer_music_is_open(), 1);

    poison(g_out, RENDER_FRAMES * PCM_MIXER_CHANNELS);
    pcm_mixer_render(g_out, 8);
    int nonzero = 0;
    for (uint32_t i = 0; i < 8 * PCM_MIXER_CHANNELS; i++) {
        if (g_out[i] != 0) nonzero++;
    }
    CU_ASSERT_EQUAL(nonzero, 0);

    make_src(700, 3);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 3), 3u);
    pcm_mixer_render(g_out, 3);
    CU_ASSERT_EQUAL(g_out[0], sig_l(700));
    CU_ASSERT_EQUAL(g_out[1], sig_r(700));
    CU_ASSERT_EQUAL(g_out[4], sig_l(702));
    CU_ASSERT_EQUAL(g_out[5], sig_r(702));
}

/*
 * After a close the mixer has let go of the storage: render no longer reads
 * it, so its owner can free it or scribble on it.  That is the contract
 * SCRUM-219 relies on to give the pages back.
 */
static void test_close_lets_go_of_the_ring(void)
{
    pcm_mixer_reset();
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);

    make_src(0, 8);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 8), 8u);
    pcm_mixer_music_close();

    /* The owner reuses it for something loud. */
    for (uint32_t i = 0; i < SMALL_RING * PCM_MIXER_CHANNELS; i++) {
        g_ring[i] = 32767;
    }

    poison(g_out, RENDER_FRAMES * PCM_MIXER_CHANNELS);
    pcm_mixer_render(g_out, SMALL_RING);
    int nonzero = 0;
    for (uint32_t i = 0; i < SMALL_RING * PCM_MIXER_CHANNELS; i++) {
        if (g_out[i] != 0) nonzero++;
    }
    CU_ASSERT_EQUAL(nonzero, 0);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, 8), 0u);

    /* pcm_mixer_reset() lets go as well -- "forget every voice" includes
     * the one that is not in the table. */
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);
    pcm_mixer_reset();
    CU_ASSERT_EQUAL(pcm_mixer_music_is_open(), 0);
}

/* ── The soundtrack is not an sfx voice ────────────────────────────────── */

/*
 * A sound effect must never silence the music.  Fill all eight sfx voices,
 * then start a ninth that is important enough to steal: it takes an sfx
 * voice, as it always did, and the music is still there in the next frame.
 * pcm_mixer_stop_all() -- the sfx-side "stop everything" -- leaves it alone
 * too.
 */
static void test_sfx_cannot_take_or_stop_the_music_voice(void)
{
    pcm_mixer_reset();
    CU_ASSERT_EQUAL(pcm_mixer_music_open(g_ring, SMALL_RING), PCM_MIXER_OK);

    /* Sfx voices that occupy a slot and contribute nothing (128 is the DMX
     * zero level), so what is left in the output is the music alone. */
    static uint8_t centre[RENDER_FRAMES];
    for (uint32_t i = 0; i < RENDER_FRAMES; i++) centre[i] = 128;

    doom_dmx_t zero = dmx_of(centre, RENDER_FRAMES, PCM_MIXER_RATE_HZ);
    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        CU_ASSERT_TRUE(pcm_mixer_start(&zero, 100, PCM_MIXER_SEP_CENTRE, 64) >= 0);
    }
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), PCM_MIXER_VOICES);

    make_src(0, SMALL_RING);
    CU_ASSERT_EQUAL(pcm_mixer_music_write(g_src, SMALL_RING), SMALL_RING);

    /* More important than everything playing: it steals an sfx voice. */
    CU_ASSERT_TRUE(pcm_mixer_start(&zero, 100, PCM_MIXER_SEP_CENTRE, 1) >= 0);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), PCM_MIXER_VOICES);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), SMALL_RING);

    pcm_mixer_render(g_out, 4);
    CU_ASSERT_EQUAL(g_out[0], sig_l(0));
    CU_ASSERT_EQUAL(g_out[1], sig_r(0));
    CU_ASSERT_EQUAL(g_out[6], sig_l(3));

    pcm_mixer_stop_all();
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);
    CU_ASSERT_EQUAL(pcm_mixer_music_is_open(), 1);

    pcm_mixer_render(g_out, 4);
    CU_ASSERT_EQUAL(g_out[0], sig_l(4));
    CU_ASSERT_EQUAL(g_out[7], sig_r(7));
}

/* ── Registration ──────────────────────────────────────────────────────── */

void suite_pcm_mixer_music_tests(CU_pSuite s)
{
    CU_add_test(s, "a closed voice is silent and takes nothing",
                test_closed_voice_is_silent_and_takes_nothing);
    CU_add_test(s, "open validates and is exclusive",
                test_open_validates_and_is_exclusive);
    CU_add_test(s, "full volume passes frames through exactly",
                test_full_volume_passes_frames_through_exactly);
    CU_add_test(s, "music sums with sfx under one clip",
                test_music_sums_with_sfx_under_one_clip);
    CU_add_test(s, "volume moves only the music",
                test_volume_moves_only_the_music);
    CU_add_test(s, "volume is clamped and round-trips",
                test_volume_is_clamped_and_round_trips);
    CU_add_test(s, "a starved ring is silent, counted, and recovers",
                test_starved_ring_is_silent_counted_and_recovers);
    CU_add_test(s, "an underrun does not disturb sfx",
                test_underrun_does_not_disturb_sfx);
    CU_add_test(s, "an idle voice is not an underrun",
                test_idle_voice_is_not_an_underrun);
    CU_add_test(s, "a wrap mid-render is continuous",
                test_wrap_mid_render_is_continuous);
    CU_add_test(s, "frames come out in order across many laps",
                test_frames_come_out_in_order_across_many_laps);
    CU_add_test(s, "a short write reports what fit",
                test_short_write_reports_what_fit);
    CU_add_test(s, "the recommended ring holds a quarter second",
                test_recommended_ring_holds_a_quarter_second);
    CU_add_test(s, "flush drops the tail immediately",
                test_flush_drops_the_tail_immediately);
    CU_add_test(s, "close lets go of the ring",
                test_close_lets_go_of_the_ring);
    CU_add_test(s, "sfx cannot take or stop the music voice",
                test_sfx_cannot_take_or_stop_the_music_voice);
}

int suite_pcm_mixer_music_init(void)    { return suite_init(); }
int suite_pcm_mixer_music_cleanup(void) { return suite_cleanup(); }
