/*
 * test_pcm_mixer_k.c — software PCM mixer arithmetic (SCRUM-212).
 *
 * Every test here is hardware-free by construction: src/pcm_mixer.c includes
 * no device header, so this suite drives the real mixer against synthetic
 * doom_dmx_t descriptors over static arrays and asserts the exact samples it
 * produces.  The controller side -- the BDL split, the refill cadence and the
 * "frame timing is unaffected" half of the ticket's acceptance -- is
 * test_hda_pcm_k.c's job, because only that needs a real controller.
 *
 * Splitting it this way is what makes the arithmetic testable at all.  A
 * mixer written directly into hda.c's interrupt handler could only ever be
 * checked by "SDnLPIB moved and it did not crash", which is exactly the
 * assertion that would miss the bug this ticket is most exposed to: a phase
 * step computed the wrong way round, playing two thirds of Doom's effects an
 * octave low (see src/pcm_mixer.h and test_doom_dmx_k.c's rate histogram).
 */

#include "kunit.h"
#include "pcm_mixer.h"
#include "doom_dmx.h"

#include <stdint.h>

/* ── Fixtures ──────────────────────────────────────────────────────────── */

/*
 * Render buffers.  Static rather than on the stack: the kernel stack is
 * 16 KiB (CLAUDE.md) and a few hundred stereo frames of int16_t is already a
 * meaningful slice of it.
 */
#define RENDER_FRAMES 512u
static int16_t g_out[RENDER_FRAMES * PCM_MIXER_CHANNELS];

/* A short ramp covering the full unsigned 8-bit range at both ends, so a
 * sign-extension slip in the (u8 - 128) conversion shows up as a wrap rather
 * than as a small error. */
static const uint8_t g_ramp[9] = { 0, 32, 64, 96, 128, 160, 192, 224, 255 };

/* Constant full-positive and full-negative bodies, for the clipping tests:
 * summing several of these is the only way to exceed int16_t. */
static const uint8_t g_max[64] = {
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};
static const uint8_t g_min[64] = { 0 };   /* 0 -> -128 -> -32768 at full scale */

/* Mid-level: 160 -> (160 - 128) << 8 = 8192, so several of these sum without
 * reaching the clipper.  Needed wherever the assertion is about the mix
 * itself rather than about saturation. */
static const uint8_t g_mid[16] = {
    160, 160, 160, 160, 160, 160, 160, 160,
    160, 160, 160, 160, 160, 160, 160, 160,
};

/* Build a descriptor without going through doom_dmx_parse(): these bodies are
 * not lumps and have no header, and the mixer's contract is over the parsed
 * struct rather than over the bytes a lump carries. */
static doom_dmx_t dmx_of(const uint8_t *samples, uint32_t n, uint32_t rate_hz)
{
    doom_dmx_t d;
    d.format      = DOOM_DMX_FORMAT_PCM;
    d.rate_hz     = rate_hz;
    d.samples     = samples;
    d.num_samples = n;
    return d;
}

static void clear_out(void)
{
    for (uint32_t i = 0; i < RENDER_FRAMES * PCM_MIXER_CHANNELS; i++) {
        g_out[i] = (int16_t)0x5A5A;   /* poison, so "written in full" is real */
    }
}

/* Every test starts from a known-empty mixer: this is process-global state,
 * and a voice left sounding by one test would mix into the next one's
 * assertions. */
static int suite_init(void)
{
    pcm_mixer_reset();
    return 0;
}

static int suite_cleanup(void)
{
    pcm_mixer_reset();
    return 0;
}

/* ── Silence and buffer hygiene ────────────────────────────────────────── */

static void test_no_voices_renders_silence(void)
{
    pcm_mixer_reset();
    clear_out();
    pcm_mixer_render(g_out, RENDER_FRAMES);

    /* Not just "starts with zeros": the whole buffer, both channels, because
     * the contract is that a caller never has to pre-zero and a previous
     * fill can never be replayed. */
    int nonzero = 0;
    for (uint32_t i = 0; i < RENDER_FRAMES * PCM_MIXER_CHANNELS; i++) {
        if (g_out[i] != 0) {
            nonzero++;
        }
    }
    CU_ASSERT_EQUAL(nonzero, 0);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);
}

static void test_render_null_is_ignored(void)
{
    pcm_mixer_reset();
    pcm_mixer_render(NULL, RENDER_FRAMES);   /* must not fault */
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);
}

/* ── Scaling: the identity with SCRUM-211's converter ──────────────────── */

/*
 * A voice at exactly the output rate, full volume, hard left, reproduces
 * doom_dmx_to_s16() sample for sample.  That is the anchor for every other
 * scaling assertion here: if this drifts, the mixer and the decoder disagree
 * about what a sample *is*, and no amount of correct resampling would help.
 *
 * Hard left rather than centred because Doom's panning law puts a centred
 * sound at about three quarters of full on both channels (see gain_for() in
 * src/pcm_mixer.c) -- 256/256 is only reached with the pan fully to one side.
 */
static void test_unity_rate_matches_dmx_to_s16(void)
{
    pcm_mixer_reset();
    doom_dmx_t d = dmx_of(g_ramp, 9, PCM_MIXER_RATE_HZ);

    int16_t reference[9];
    CU_ASSERT_EQUAL(doom_dmx_to_s16(&d, reference, 9), 9u);

    int h = pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0 /* hard left */, 0);
    CU_ASSERT_TRUE(h >= 0);

    clear_out();
    pcm_mixer_render(g_out, 9);

    for (uint32_t i = 0; i < 9; i++) {
        CU_ASSERT_EQUAL(g_out[i * PCM_MIXER_CHANNELS], reference[i]);
    }
    /* Hard left really is hard: the right channel is silent. */
    for (uint32_t i = 0; i < 9; i++) {
        CU_ASSERT_EQUAL(g_out[i * PCM_MIXER_CHANNELS + 1], 0);
    }
}

static void test_centre_pan_is_equal_and_quieter(void)
{
    pcm_mixer_reset();
    doom_dmx_t d = dmx_of(g_max, 64, PCM_MIXER_RATE_HZ);

    CU_ASSERT_TRUE(pcm_mixer_start(&d, PCM_MIXER_VOL_MAX,
                                   PCM_MIXER_SEP_CENTRE, 0) >= 0);
    clear_out();
    pcm_mixer_render(g_out, 8);

    int32_t l = g_out[0];
    int32_t r = g_out[1];
    /*
     * Balanced to within a couple of percent, not bit-identical: Doom's
     * squared-separation law is symmetric about 127.5, and the separation is
     * an integer, so PCM_MIXER_SEP_CENTRE == 128 sits a hair to the right of
     * true centre.  That is Doom's own arithmetic rather than a rounding
     * choice made here, and the imbalance is ~1.5% -- below audibility, and
     * asserted rather than hidden.
     */
    int32_t diff = (l > r) ? (l - r) : (r - l);
    CU_ASSERT_TRUE(diff * 32 < l);
    /* Audibly present, but below the full-scale 32512 a hard pan gives. */
    CU_ASSERT_TRUE(l > 16000 && l < 32512);
    CU_ASSERT_TRUE(r > 16000 && r < 32512);
}

static void test_zero_volume_is_silent_but_occupies_a_voice(void)
{
    pcm_mixer_reset();
    doom_dmx_t d = dmx_of(g_max, 64, PCM_MIXER_RATE_HZ);

    int h = pcm_mixer_start(&d, 0, PCM_MIXER_SEP_CENTRE, 0);
    CU_ASSERT_TRUE(h >= 0);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 1u);
    CU_ASSERT_TRUE(pcm_mixer_is_playing(h));

    clear_out();
    pcm_mixer_render(g_out, 8);
    for (uint32_t i = 0; i < 8 * PCM_MIXER_CHANNELS; i++) {
        CU_ASSERT_EQUAL(g_out[i], 0);
    }
}

/* ── Resampling ────────────────────────────────────────────────────────── */

/*
 * The one assertion the ticket's own premise would have got wrong.  A lump at
 * rate R occupies num_samples * 48000 / R output frames, so an 11025 Hz
 * effect lasts more than four times its sample count and a 44100 Hz one
 * barely stretches at all.  A mixer that ignored rate_hz would render every
 * one of these in exactly num_samples frames.
 *
 * The bound is +/-1 frame: the phase is 16.16 and the last frame is whichever
 * one the accumulator lands on, not a separately computed count.
 */
static void assert_duration_for_rate(uint32_t rate_hz, uint32_t num_samples)
{
    pcm_mixer_reset();
    doom_dmx_t d = dmx_of(g_max, num_samples, rate_hz);
    CU_ASSERT_TRUE(pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 0) >= 0);

    uint32_t expected = (uint32_t)(((uint64_t)num_samples * PCM_MIXER_RATE_HZ)
                                   / rate_hz);

    /* Render one frame at a time and count how many carry signal. */
    uint32_t sounded = 0;
    for (uint32_t f = 0; f < RENDER_FRAMES; f++) {
        pcm_mixer_render(g_out, 1);
        if (g_out[0] != 0) {
            sounded++;
        }
    }

    CU_ASSERT_TRUE(sounded + 1 >= expected && sounded <= expected + 1);
    /* And it retired rather than looping. */
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);
}

static void test_duration_11025(void) { assert_duration_for_rate(11025u, 64u); }
static void test_duration_22050(void) { assert_duration_for_rate(22050u, 64u); }
static void test_duration_44100(void) { assert_duration_for_rate(44100u, 64u); }
static void test_duration_16000(void) { assert_duration_for_rate(16000u, 64u); }

/*
 * Upsampling a monotone ramp must stay monotone.  This is the shape test that
 * catches an interpolation whose fraction runs backwards -- a bug that leaves
 * the duration correct (so assert_duration_for_rate passes) while making
 * every waveform a staircase of little reversals, which is audible as buzz.
 */
static void test_upsampled_ramp_is_monotone(void)
{
    pcm_mixer_reset();
    doom_dmx_t d = dmx_of(g_ramp, 9, 11025u);
    CU_ASSERT_TRUE(pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 0) >= 0);

    clear_out();
    pcm_mixer_render(g_out, RENDER_FRAMES);

    int16_t prev = g_out[0];
    int regressions = 0;
    uint32_t f = 1;
    /* Only while the voice is still sounding; once it retires the buffer goes
     * to silence, which is a legitimate drop. */
    for (; f < RENDER_FRAMES; f++) {
        int16_t cur = g_out[f * PCM_MIXER_CHANNELS];
        if (cur == 0 && prev > 0) {
            break;      /* retired */
        }
        if (cur < prev) {
            regressions++;
        }
        prev = cur;
    }
    CU_ASSERT_EQUAL(regressions, 0);
    /* It really did stretch: 9 samples at 11025 Hz is ~39 output frames. */
    CU_ASSERT_TRUE(f > 30);
}

/* Interpolation must never read past the lump.  A one-byte overread lands
 * inside the mapped WAD rather than faulting, so the only way to catch it is
 * to assert the final frame's value -- the last sample interpolated against
 * itself, i.e. the sample unchanged. */
static void test_last_sample_does_not_overrun(void)
{
    pcm_mixer_reset();
    static const uint8_t two[2] = { 128, 255 };
    doom_dmx_t d = dmx_of(two, 2, PCM_MIXER_RATE_HZ);
    CU_ASSERT_TRUE(pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 0) >= 0);

    clear_out();
    pcm_mixer_render(g_out, 4);

    CU_ASSERT_EQUAL(g_out[0], 0);                      /* 128 -> 0          */
    CU_ASSERT_EQUAL(g_out[1 * PCM_MIXER_CHANNELS], (int16_t)((255 - 128) << 8));
    CU_ASSERT_EQUAL(g_out[2 * PCM_MIXER_CHANNELS], 0); /* retired, silence  */
    CU_ASSERT_EQUAL(g_out[3 * PCM_MIXER_CHANNELS], 0);
}

/* ── Summation and clipping ────────────────────────────────────────────── */

static void test_two_voices_sum_arithmetically(void)
{
    pcm_mixer_reset();
    /* Two mid-level voices sum to 16384 -- well inside int16_t, which is the
     * point: this checks the summation before clipping has anything to do. */
    doom_dmx_t d = dmx_of(g_mid, 16, PCM_MIXER_RATE_HZ);

    CU_ASSERT_TRUE(pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 0) >= 0);
    clear_out();
    pcm_mixer_render(g_out, 1);
    int16_t one = g_out[0];
    CU_ASSERT_EQUAL(one, (int16_t)8192);

    pcm_mixer_reset();
    CU_ASSERT_TRUE(pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 0) >= 0);
    CU_ASSERT_TRUE(pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 0) >= 0);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 2u);
    clear_out();
    pcm_mixer_render(g_out, 1);
    CU_ASSERT_EQUAL(g_out[0], (int16_t)(2 * 8192));
}

/*
 * Eight full-scale voices sum to about 8x int16_t's range.  The requirement
 * is not merely "it is large" but that it saturates: a wrap would flip the
 * sign, turning the loudest moment of the mix into a full-amplitude crack
 * that is louder than anything that caused it.  So the assertion is on the
 * sign as much as the magnitude.
 */
static void test_eight_voices_saturate_without_wrapping(void)
{
    pcm_mixer_reset();
    doom_dmx_t hi = dmx_of(g_max, 64, PCM_MIXER_RATE_HZ);
    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        CU_ASSERT_TRUE(pcm_mixer_start(&hi, PCM_MIXER_VOL_MAX, 0, 0) >= 0);
    }
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), PCM_MIXER_VOICES);

    clear_out();
    pcm_mixer_render(g_out, 8);
    for (uint32_t f = 0; f < 8; f++) {
        CU_ASSERT_EQUAL(g_out[f * PCM_MIXER_CHANNELS], (int16_t)32767);
    }

    /* And the negative rail, which clips against a different constant. */
    pcm_mixer_reset();
    doom_dmx_t lo = dmx_of(g_min, 64, PCM_MIXER_RATE_HZ);
    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        CU_ASSERT_TRUE(pcm_mixer_start(&lo, PCM_MIXER_VOL_MAX, 0, 0) >= 0);
    }
    clear_out();
    pcm_mixer_render(g_out, 8);
    for (uint32_t f = 0; f < 8; f++) {
        CU_ASSERT_EQUAL(g_out[f * PCM_MIXER_CHANNELS], (int16_t)(-32768));
    }
}

/* ── Voice allocation, stealing and starvation ─────────────────────────── */

static void test_all_voices_available(void)
{
    pcm_mixer_reset();
    doom_dmx_t d = dmx_of(g_max, 64, PCM_MIXER_RATE_HZ);
    int handles[PCM_MIXER_VOICES];

    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        handles[i] = pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 10);
        CU_ASSERT_TRUE(handles[i] >= 0);
    }
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), PCM_MIXER_VOICES);
    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        CU_ASSERT_TRUE(pcm_mixer_is_playing(handles[i]));
    }
    /* Distinct voices, not the same one handed out repeatedly. */
    for (uint32_t i = 1; i < PCM_MIXER_VOICES; i++) {
        CU_ASSERT_NOT_EQUAL(handles[i], handles[0]);
    }
}

/*
 * The ticket's "without one starving the other", stated as an assertion: a
 * less important sound arriving at a full mixer is refused, and every voice
 * already sounding keeps its handle.  A mixer that always stole would let a
 * stream of incidental noises chop up whatever the player is listening for.
 */
static void test_less_important_sound_is_refused(void)
{
    pcm_mixer_reset();
    doom_dmx_t d = dmx_of(g_max, 64, PCM_MIXER_RATE_HZ);
    int handles[PCM_MIXER_VOICES];

    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        handles[i] = pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 10 /* important */);
        CU_ASSERT_TRUE(handles[i] >= 0);
    }

    /* Higher number = less important, per Doom's rule. */
    CU_ASSERT_EQUAL(pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 99),
                    PCM_MIXER_ENOVOICE);

    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), PCM_MIXER_VOICES);
    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        CU_ASSERT_TRUE(pcm_mixer_is_playing(handles[i]));
    }
}

static void test_more_important_sound_steals_exactly_one(void)
{
    pcm_mixer_reset();
    doom_dmx_t d = dmx_of(g_max, 64, PCM_MIXER_RATE_HZ);
    int handles[PCM_MIXER_VOICES];

    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        handles[i] = pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 50);
        CU_ASSERT_TRUE(handles[i] >= 0);
    }

    int urgent = pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 1);
    CU_ASSERT_TRUE(urgent >= 0);
    CU_ASSERT_TRUE(pcm_mixer_is_playing(urgent));
    /* Still full -- it took a slot rather than adding one. */
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), PCM_MIXER_VOICES);

    /* Exactly one of the originals lost its voice. */
    uint32_t survivors = 0;
    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        if (pcm_mixer_is_playing(handles[i])) {
            survivors++;
        }
    }
    CU_ASSERT_EQUAL(survivors, PCM_MIXER_VOICES - 1u);
}

/* An equally important sound does get in: Doom's rule is "<=", not "<", and
 * the common case of the same effect retriggering depends on it. */
static void test_equal_priority_steals(void)
{
    pcm_mixer_reset();
    doom_dmx_t d = dmx_of(g_max, 64, PCM_MIXER_RATE_HZ);
    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        CU_ASSERT_TRUE(pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 20) >= 0);
    }
    CU_ASSERT_TRUE(pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 20) >= 0);
}

/* ── Handles ───────────────────────────────────────────────────────────── */

static void test_stop_silences_only_that_voice(void)
{
    pcm_mixer_reset();
    /* Mid-level, not full-scale: two full-scale voices would clip, and the
     * "half the sum" assertion below has to be about the mix rather than
     * about the clipper. */
    doom_dmx_t d = dmx_of(g_mid, 16, PCM_MIXER_RATE_HZ);

    int a = pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 0);
    int b = pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 0);
    CU_ASSERT_TRUE(a >= 0 && b >= 0);

    clear_out();
    pcm_mixer_render(g_out, 1);
    int16_t both = g_out[0];

    pcm_mixer_stop(a);
    CU_ASSERT_FALSE(pcm_mixer_is_playing(a));
    CU_ASSERT_TRUE(pcm_mixer_is_playing(b));
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 1u);

    clear_out();
    pcm_mixer_render(g_out, 1);
    CU_ASSERT_EQUAL(g_out[0], (int16_t)(both / 2));
}

/*
 * The reason handles carry a generation.  Stop a voice, start a different
 * sound (which reuses the slot), and the old handle must not be able to stop
 * or claim the new one -- otherwise a caller holding a finished handle
 * silently operates on somebody else's effect.
 */
static void test_stale_handle_is_rejected(void)
{
    pcm_mixer_reset();
    doom_dmx_t d = dmx_of(g_max, 64, PCM_MIXER_RATE_HZ);

    int old = pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 0);
    CU_ASSERT_TRUE(old >= 0);
    pcm_mixer_stop(old);

    int fresh = pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 0);
    CU_ASSERT_TRUE(fresh >= 0);
    CU_ASSERT_NOT_EQUAL(fresh, old);          /* same slot, new generation   */

    CU_ASSERT_FALSE(pcm_mixer_is_playing(old));
    pcm_mixer_stop(old);                      /* must not touch `fresh`      */
    CU_ASSERT_TRUE(pcm_mixer_is_playing(fresh));
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 1u);
}

static void test_retired_handle_reports_not_playing(void)
{
    pcm_mixer_reset();
    static const uint8_t one[1] = { 255 };
    doom_dmx_t d = dmx_of(one, 1, PCM_MIXER_RATE_HZ);

    int h = pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 0);
    CU_ASSERT_TRUE(h >= 0);
    CU_ASSERT_TRUE(pcm_mixer_is_playing(h));

    pcm_mixer_render(g_out, 4);               /* outlives its one sample     */
    CU_ASSERT_FALSE(pcm_mixer_is_playing(h));
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);
}

static void test_stop_all_and_reset(void)
{
    pcm_mixer_reset();
    doom_dmx_t d = dmx_of(g_max, 64, PCM_MIXER_RATE_HZ);
    int h = pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 0);
    CU_ASSERT_TRUE(pcm_mixer_start(&d, PCM_MIXER_VOL_MAX, 0, 0) >= 0);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 2u);

    pcm_mixer_stop_all();
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);
    CU_ASSERT_FALSE(pcm_mixer_is_playing(h));

    /* Bad handles are ignored rather than indexing out of the table. */
    pcm_mixer_stop(-1);
    pcm_mixer_stop(0x7FFFFFFF);
    CU_ASSERT_FALSE(pcm_mixer_is_playing(-1));
    CU_ASSERT_FALSE(pcm_mixer_is_playing(0x7FFFFFFF));
}

/* ── Rejections ────────────────────────────────────────────────────────── */

static void test_start_rejects_bad_input(void)
{
    pcm_mixer_reset();

    CU_ASSERT_EQUAL(pcm_mixer_start(NULL, PCM_MIXER_VOL_MAX, 0, 0),
                    PCM_MIXER_EINVAL);

    doom_dmx_t no_samples = dmx_of(NULL, 64, PCM_MIXER_RATE_HZ);
    CU_ASSERT_EQUAL(pcm_mixer_start(&no_samples, PCM_MIXER_VOL_MAX, 0, 0),
                    PCM_MIXER_EINVAL);

    doom_dmx_t empty = dmx_of(g_max, 0, PCM_MIXER_RATE_HZ);
    CU_ASSERT_EQUAL(pcm_mixer_start(&empty, PCM_MIXER_VOL_MAX, 0, 0),
                    PCM_MIXER_EINVAL);

    /* A rate whose 16.16 step rounds to zero would hold one sample forever
     * instead of playing, so it is refused rather than wedging a voice. */
    doom_dmx_t too_slow = dmx_of(g_max, 64, 0u);
    CU_ASSERT_EQUAL(pcm_mixer_start(&too_slow, PCM_MIXER_VOL_MAX, 0, 0),
                    PCM_MIXER_EINVAL);

    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);
}

/* Out-of-range vol/sep are clamped, not rejected: they come from Doom's own
 * bookkeeping, where refusing to play a sound is worse than playing it at the
 * nearest legal level. */
static void test_out_of_range_gain_is_clamped(void)
{
    pcm_mixer_reset();
    doom_dmx_t d = dmx_of(g_max, 64, PCM_MIXER_RATE_HZ);

    CU_ASSERT_TRUE(pcm_mixer_start(&d, 9999, -50, 0) >= 0);
    clear_out();
    pcm_mixer_render(g_out, 1);
    /* Clamped to vol 127, sep 0 -- the hard-left full-scale case. */
    CU_ASSERT_EQUAL(g_out[0], (int16_t)((255 - 128) << 8));
    CU_ASSERT_EQUAL(g_out[1], 0);
}

/* ── Registration ──────────────────────────────────────────────────────── */

void suite_pcm_mixer_tests(CU_pSuite s)
{
    CU_add_test(s, "no voices renders silence", test_no_voices_renders_silence);
    CU_add_test(s, "render(NULL) is ignored", test_render_null_is_ignored);
    CU_add_test(s, "unity rate matches doom_dmx_to_s16",
                test_unity_rate_matches_dmx_to_s16);
    CU_add_test(s, "centre pan is equal on both channels",
                test_centre_pan_is_equal_and_quieter);
    CU_add_test(s, "zero volume is silent but holds a voice",
                test_zero_volume_is_silent_but_occupies_a_voice);
    CU_add_test(s, "11025 Hz stretches to the output rate", test_duration_11025);
    CU_add_test(s, "22050 Hz stretches to the output rate", test_duration_22050);
    CU_add_test(s, "44100 Hz stretches to the output rate", test_duration_44100);
    CU_add_test(s, "16000 Hz stretches to the output rate", test_duration_16000);
    CU_add_test(s, "upsampled ramp stays monotone",
                test_upsampled_ramp_is_monotone);
    CU_add_test(s, "interpolation does not read past the lump",
                test_last_sample_does_not_overrun);
    CU_add_test(s, "two voices sum arithmetically",
                test_two_voices_sum_arithmetically);
    CU_add_test(s, "eight voices saturate without wrapping",
                test_eight_voices_saturate_without_wrapping);
    CU_add_test(s, "all voices available", test_all_voices_available);
    CU_add_test(s, "less important sound is refused, none disturbed",
                test_less_important_sound_is_refused);
    CU_add_test(s, "more important sound steals exactly one voice",
                test_more_important_sound_steals_exactly_one);
    CU_add_test(s, "equal priority steals", test_equal_priority_steals);
    CU_add_test(s, "stop silences only that voice",
                test_stop_silences_only_that_voice);
    CU_add_test(s, "stale handle is rejected", test_stale_handle_is_rejected);
    CU_add_test(s, "retired handle reports not playing",
                test_retired_handle_reports_not_playing);
    CU_add_test(s, "stop_all and bad handles", test_stop_all_and_reset);
    CU_add_test(s, "start rejects bad input", test_start_rejects_bad_input);
    CU_add_test(s, "out-of-range gain is clamped",
                test_out_of_range_gain_is_clamped);
}

int suite_pcm_mixer_init(void)    { return suite_init(); }
int suite_pcm_mixer_cleanup(void) { return suite_cleanup(); }
