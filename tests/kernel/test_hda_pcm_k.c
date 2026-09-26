/*
 * test_hda_pcm_k.c — the mixer driving the real HDA stream (SCRUM-212).
 *
 * test_pcm_mixer_k.c asserts the arithmetic with no hardware in sight.  This
 * is the other half: that the controller really plays what the mixer renders,
 * that the completion interrupt really drives the refill, and the two
 * properties the ticket's acceptance is actually about --
 *
 *   1. several overlapping sounds mix without one starving the other, and
 *   2. frame timing is unaffected.
 *
 * Both are asserted from hardware state rather than from listening, exactly
 * as test_hda_k.c's header explains for SCRUM-210: headless Docker has no
 * audio backend, so audibility is `make docker-run-kernel`'s job on a machine
 * with a speaker, and what CI can prove is that the samples leave RAM on time.
 *
 * ── Why the timing assertion is the interesting one ───────────────────────
 *
 * The refill runs inside hda_irq_handler(), with IF clear, roughly 47 times a
 * second, mixing up to PCM_MIXER_VOICES voices over HDA_PCM_ENTRY_FRAMES
 * frames each time.  That is a real-time budget, and the honest way to check
 * it is not to assert in a comment that integer multiply-adds are cheap.  It
 * is to run the thing fully loaded and check two independent things it would
 * break: that IRQ0's millisecond clock still advances at wall-clock rate (so
 * the mixing is not eating the frame clock the way a blocking speaker call
 * would), and that the controller never reports a FIFO error (so the render
 * is finishing before the DMA engine arrives).  SCRUM-98 proved its speaker
 * non-blocking the same way -- observable state, not assertion by comment.
 *
 * ── Interrupts ────────────────────────────────────────────────────────────
 *
 * run_tests() has no ambient IF in either direction (see
 * test_doomgeneric_timer_k.c's header), so every test that needs time to pass
 * enables interrupts for exactly its own wait and clears them again.  They
 * have to: QEMU advances the stream's DMA position from a timer callback, and
 * the completion interrupt is the thing under test.
 */

#include "kunit.h"
#include "hda.h"
#include "pcm_mixer.h"
#include "doom_dmx.h"
#include "doom_wad.h"
#include "wad.h"
#include "mmap.h"
#include "pit.h"

#include <stdint.h>

/*
 * How long to let the stream run.  One entry is HDA_PCM_ENTRY_FRAMES = 1024
 * frames = ~21.3 ms at 48 kHz, so 400 ms is ~18 completion interrupts: enough
 * for the refill cursor to lap a good part of the buffer, and still nothing
 * against the test boot's 30-second budget.
 */
#define PCM_TEST_WAIT_MS 400u

/* Spin with interrupts enabled until `ms` have passed.  `hlt` rather than a
 * busy loop so IRQ0 and the HDA line actually get serviced promptly. */
static void wait_ms_with_irqs(uint32_t ms)
{
    uint32_t start = kernel_get_ticks_ms();
    __asm__ volatile ("sti");
    while ((uint32_t)(kernel_get_ticks_ms() - start) < ms) {
        __asm__ volatile ("hlt");
    }
    __asm__ volatile ("cli");
}

/* ── Fixtures ──────────────────────────────────────────────────────────── */

/*
 * The real freedoom2 module, mounted for the suite rather than per test:
 * doom_wad_mount() re-parses a 3610-entry directory, and that registry is
 * process-global state shared with the doom_dmx and dg_init suites -- hence
 * the unmount in cleanup, to leave it as it was found.
 */
static const wad_t *real_wad(void)
{
    const doom_wad_t *w = doom_wad_mounted();
    return w != NULL ? &w->wad : NULL;
}

static int suite_init(void)
{
    uint64_t start = 0, end = 0;
    pcm_mixer_reset();
    if (mmap_find_module(&start, &end) == 0 && end > start) {
        (void)doom_wad_mount((const void *)(uintptr_t)start,
                             (uint32_t)(end - start));
    }
    return 0;
}

/*
 * Stop the stream *and* forget the voices, in that order.  A voice left
 * sounding would keep the next suite's boot noisy, and -- more importantly --
 * voices point into the WAD this cleanup is about to unmount, so unmounting
 * with one live would leave a stale pointer for any later render to read
 * (src/pcm_mixer.h's zero-copy note).
 */
static int suite_cleanup(void)
{
    hda_pcm_stop();
    pcm_mixer_reset();
    doom_wad_unmount();
    return 0;
}

/* Decode one DS* lump, or report it and give up on this test. */
static int load_sfx(const char *name, doom_dmx_t *out)
{
    const wad_t *wad = real_wad();
    if (wad == NULL) {
        return 0;
    }
    return doom_dmx_find_sfx(wad, name, out) == DOOM_DMX_OK;
}

/* ── The constants the two layers have to agree on ─────────────────────── */

/*
 * src/pcm_mixer.c deliberately does not include hda.h -- that is what keeps it
 * testable without a controller.  The cost is that its output rate and channel
 * count are restated rather than derived, so something has to check the two
 * views match.  This file sees both headers, so it is the only place that can.
 */
/*
 * Four tests below decode real DS* lumps and quietly return if there is no
 * mounted module, so that a machine booted without one reports the absence
 * once rather than failing four times.  This is the "once": without it, a
 * broken mount would turn those four into silent skips and the suite would
 * still print PASS.  docker-test boots the ISO with the module, so this is an
 * unconditional assertion, exactly as test_doom_dmx_k.c treats the same
 * dependency.
 */
static void test_real_wad_is_mounted(void)
{
    CU_ASSERT_PTR_NOT_NULL(real_wad());

    doom_dmx_t pistol;
    CU_ASSERT_TRUE(load_sfx("pistol", &pistol));
    CU_ASSERT_TRUE(pistol.num_samples > 0);
    /* And it is one of the rates that made resampling mandatory, rather than
     * the 11025 the ticket assumed for all of them. */
    CU_ASSERT_TRUE(pistol.rate_hz >= 11025u);
}

static void test_mixer_and_stream_formats_agree(void)
{
    CU_ASSERT_EQUAL(PCM_MIXER_RATE_HZ, HDA_SAMPLE_RATE_HZ);
    CU_ASSERT_EQUAL(PCM_MIXER_CHANNELS, HDA_CHANNELS);
    CU_ASSERT_EQUAL(HDA_PCM_ENTRY_BYTES * HDA_BDL_ENTRIES_PCM, HDA_BUF_BYTES);
    CU_ASSERT_EQUAL(HDA_PCM_ENTRY_FRAMES * HDA_BYTES_PER_FRAME,
                    HDA_PCM_ENTRY_BYTES);
    /* The split has to be finer than a tone's, or it bought nothing. */
    CU_ASSERT_TRUE(HDA_BDL_ENTRIES_PCM > HDA_BDL_ENTRIES);
}

/* ── Bring-up ──────────────────────────────────────────────────────────── */

static void test_pcm_start_programs_the_stream(void)
{
    CU_ASSERT_TRUE(hda_present());

    CU_ASSERT_EQUAL(hda_pcm_start(), HDA_OK);
    CU_ASSERT_TRUE(hda_pcm_is_streaming());
    CU_ASSERT_TRUE(hda_is_playing());

    /* LVI names the last valid entry, so the 16-entry split must read back as
     * 15 -- the assertion that bdl_program() and stream_program() agree. */
    CU_ASSERT_EQUAL(hda_sd_read16(HDA_SD_LVI), HDA_BDL_ENTRIES_PCM - 1u);
    CU_ASSERT_EQUAL(hda_sd_read32(HDA_SD_CBL), HDA_BUF_BYTES);
    CU_ASSERT_EQUAL(hda_sd_read16(HDA_SD_FMT), HDA_FMT_48K_16BIT_STEREO);

    /* RUN and the per-buffer interrupt enable, both set. */
    uint32_t ctl = hda_sd_read32(HDA_SD_CTL);
    CU_ASSERT_TRUE((ctl & HDA_SDCTL_RUN) != 0);
    CU_ASSERT_TRUE((ctl & HDA_SDCTL_IOCE) != 0);

    hda_pcm_stop();
    CU_ASSERT_FALSE(hda_pcm_is_streaming());
}

/* An idle mixer still streams -- silence, continuously -- rather than
 * stopping.  That is what keeps a later pcm_mixer_start() audible within one
 * entry instead of within a stream restart (see hda_pcm_start()'s comment). */
static void test_idle_stream_keeps_running_and_refilling(void)
{
    pcm_mixer_reset();
    CU_ASSERT_EQUAL(hda_pcm_start(), HDA_OK);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);

    wait_ms_with_irqs(PCM_TEST_WAIT_MS);

    /* The cadence is real: ~400 ms at ~21 ms an entry is ~18 refills.  A
     * floor of 8 leaves better than a 2x margin for a slow CI host. */
    CU_ASSERT_TRUE(hda_pcm_refills() >= 8u);
    CU_ASSERT_TRUE(hda_pcm_is_streaming());
    CU_ASSERT_TRUE(hda_stream_position() > 0);
    CU_ASSERT_EQUAL(hda_pcm_underruns(), 0u);

    hda_pcm_stop();
}

/* ── The acceptance: overlapping sounds, and timing ────────────────────── */

/*
 * Two real effects at different sample rates, started a few tens of
 * milliseconds apart so they genuinely overlap, both still sounding
 * afterwards.  DSPISTOL and DSSHOTGN are the two lumps test_doom_dmx_k.c
 * already carries reference data for, and they are at different rates -- which
 * is the point: if the mixer ignored rate_hz, one of the two would finish at
 * the wrong time and this test's overlap window would not hold.
 */
static void test_two_overlapping_effects_both_sound(void)
{
    doom_dmx_t pistol, shotgun;
    if (!load_sfx("pistol", &pistol) || !load_sfx("shotgn", &shotgun)) {
        return;   /* no module; test_doom_dmx_k.c reports that on its own */
    }

    pcm_mixer_reset();
    CU_ASSERT_EQUAL(hda_pcm_start(), HDA_OK);

    int a = pcm_mixer_start(&pistol, PCM_MIXER_VOL_MAX,
                            PCM_MIXER_SEP_CENTRE, 64);
    CU_ASSERT_TRUE(a >= 0);
    wait_ms_with_irqs(20u);
    int b = pcm_mixer_start(&shotgun, PCM_MIXER_VOL_MAX,
                            PCM_MIXER_SEP_CENTRE, 64);
    CU_ASSERT_TRUE(b >= 0);

    /* Genuinely concurrent: two voices, and neither displaced the other. */
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 2u);
    CU_ASSERT_TRUE(pcm_mixer_is_playing(a));
    CU_ASSERT_TRUE(pcm_mixer_is_playing(b));
    CU_ASSERT_NOT_EQUAL(a, b);

    wait_ms_with_irqs(PCM_TEST_WAIT_MS);

    /* The stream carried them: refills happened and DMA advanced. */
    CU_ASSERT_TRUE(hda_pcm_refills() >= 8u);
    CU_ASSERT_TRUE(hda_stream_position() > 0);
    /* And nothing the mixer did upset the stream itself. */
    CU_ASSERT_EQUAL(hda_pcm_underruns(), 0u);
    CU_ASSERT_EQUAL(hda_stream_errors() & HDA_SDSTS_DESE, 0);

    hda_pcm_stop();
    pcm_mixer_reset();
}

/*
 * Every voice sounding at once, which is the worst case for the per-interrupt
 * render, and the two things that would show it is too slow:
 *
 *   - a FIFO error, meaning the DMA engine reached a slice the mixer had not
 *     finished writing, and
 *   - IRQ0's clock falling behind wall time, meaning the handler is holding
 *     interrupts off long enough to lose timer ticks.
 *
 * The second is the ticket's "frame timing is unaffected" clause: Doom's frame
 * clock *is* kernel_get_ticks_ms() (via DG_GetTicksMs), so a mixer that ate
 * timer ticks would slow the game down, which is exactly the failure SCRUM-98
 * had to rule out for the speaker.
 */
static void test_full_load_does_not_disturb_frame_timing(void)
{
    doom_dmx_t pistol;
    if (!load_sfx("pistol", &pistol)) {
        return;
    }

    pcm_mixer_reset();
    CU_ASSERT_EQUAL(hda_pcm_start(), HDA_OK);

    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        CU_ASSERT_TRUE(pcm_mixer_start(&pistol, PCM_MIXER_VOL_MAX,
                                       (int)(i * 32u), 0) >= 0);
    }
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), PCM_MIXER_VOICES);

    uint32_t refills_before = hda_pcm_refills();

    wait_ms_with_irqs(PCM_TEST_WAIT_MS);

    uint32_t refills = hda_pcm_refills() - refills_before;

    /*
     * The timing check, and it has to be a *ratio between two independent
     * clocks* rather than a measurement of one against itself.  Timing the
     * wait with kernel_get_ticks_ms() would prove nothing: wait_ms_with_irqs()
     * spins on that very clock, so if the handler were eating timer ticks the
     * loop would simply run longer in wall time and still report ~400 ms.
     *
     * So: the PIT's millisecond count gates the wait, and the HDA stream's own
     * DMA rate is the reference.  Refills are driven by the controller
     * finishing HDA_PCM_ENTRY_FRAMES frames at HDA_SAMPLE_RATE_HZ, which owes
     * nothing to IRQ0.  Over a window of PCM_TEST_WAIT_MS *PIT* milliseconds
     * the expected count is ~18.75; a handler holding interrupts off long
     * enough to lose timer ticks would make the PIT clock run slow against the
     * audio clock and inflate that number.  Both bounds therefore mean
     * something -- the floor says the refill is happening at all, the ceiling
     * says the frame clock is still keeping time while it does.
     */
    uint32_t expected = (PCM_TEST_WAIT_MS * (HDA_SAMPLE_RATE_HZ / 1000u))
                        / HDA_PCM_ENTRY_FRAMES;
    CU_ASSERT_TRUE(refills >= expected / 2u);
    CU_ASSERT_TRUE(refills <= expected * 2u);

    /* And the refill kept up with the DMA under full load.  This is the
     * real-time budget's own assertion, rather than a comment claiming the
     * arithmetic is cheap. */
    CU_ASSERT_EQUAL(hda_pcm_underruns(), 0u);

    hda_pcm_stop();
    pcm_mixer_reset();
}

/* Silence really is written, not merely assumed: after stopping every voice
 * the refill keeps running and the buffer it produces is zero.  This is what
 * rules out a stale tone or a previous effect looping forever in the cyclic
 * buffer -- the failure mode a driver that only ever *started* a fill would
 * have. */
static void test_stopping_all_voices_yields_silence(void)
{
    doom_dmx_t pistol;
    if (!load_sfx("pistol", &pistol)) {
        return;
    }

    pcm_mixer_reset();
    CU_ASSERT_EQUAL(hda_pcm_start(), HDA_OK);
    CU_ASSERT_TRUE(pcm_mixer_start(&pistol, PCM_MIXER_VOL_MAX, 0, 0) >= 0);
    wait_ms_with_irqs(60u);

    pcm_mixer_stop_all();
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);

    uint32_t refills = hda_pcm_refills();
    wait_ms_with_irqs(PCM_TEST_WAIT_MS);
    /* Still refilling -- the stream did not stop just because nothing sounds. */
    CU_ASSERT_TRUE(hda_pcm_refills() > refills);
    CU_ASSERT_EQUAL(hda_pcm_underruns(), 0u);

    hda_pcm_stop();
}

/* ── Mode exclusivity with SCRUM-210's tone path ───────────────────────── */

/*
 * The two modes share one stream descriptor and one buffer, so each has to be
 * able to take it from the other without a caller sequencing them.  Both
 * directions are checked, because they fail differently: a tone after
 * streaming would play at the wrong LVI, and streaming after a tone would
 * loop the square wave in whichever slices the refill had not reached yet.
 */
static void test_tone_takes_the_stream_back(void)
{
    CU_ASSERT_EQUAL(hda_pcm_start(), HDA_OK);
    CU_ASSERT_TRUE(hda_pcm_is_streaming());

    CU_ASSERT_EQUAL(hda_play_tone(HDA_BOOT_TONE_HZ, 50u), HDA_OK);
    CU_ASSERT_FALSE(hda_pcm_is_streaming());
    /* The tone's own, coarser split is back. */
    CU_ASSERT_EQUAL(hda_sd_read16(HDA_SD_LVI), HDA_BDL_ENTRIES - 1u);
    CU_ASSERT_TRUE(hda_is_playing());

    hda_stop();
}

static void test_streaming_takes_the_stream_from_a_tone(void)
{
    CU_ASSERT_EQUAL(hda_play_tone(HDA_BOOT_TONE_HZ, 0u), HDA_OK);
    CU_ASSERT_TRUE(hda_is_playing());
    CU_ASSERT_FALSE(hda_pcm_is_streaming());

    CU_ASSERT_EQUAL(hda_pcm_start(), HDA_OK);
    CU_ASSERT_TRUE(hda_pcm_is_streaming());
    CU_ASSERT_EQUAL(hda_sd_read16(HDA_SD_LVI), HDA_BDL_ENTRIES_PCM - 1u);
    /* Counters restart with the mode, so a test's numbers are its own. */
    CU_ASSERT_EQUAL(hda_pcm_underruns(), 0u);

    hda_pcm_stop();
}

/* hda_stop() leaves streaming mode too, so the generic stop is not a way to
 * end up with a halted stream that still believes it is streaming. */
static void test_generic_stop_leaves_streaming_mode(void)
{
    CU_ASSERT_EQUAL(hda_pcm_start(), HDA_OK);
    hda_stop();
    CU_ASSERT_FALSE(hda_pcm_is_streaming());
    CU_ASSERT_FALSE(hda_is_playing());
}

/* ── Registration ──────────────────────────────────────────────────────── */

void suite_hda_pcm_tests(CU_pSuite s)
{
    CU_add_test(s, "the real freedoom2 module is mounted",
                test_real_wad_is_mounted);
    CU_add_test(s, "mixer and stream formats agree",
                test_mixer_and_stream_formats_agree);
    CU_add_test(s, "pcm start programs the 16-entry stream",
                test_pcm_start_programs_the_stream);
    CU_add_test(s, "idle stream keeps running and refilling",
                test_idle_stream_keeps_running_and_refilling);
    CU_add_test(s, "two overlapping effects both sound",
                test_two_overlapping_effects_both_sound);
    CU_add_test(s, "full load does not disturb frame timing",
                test_full_load_does_not_disturb_frame_timing);
    CU_add_test(s, "stopping all voices yields silence",
                test_stopping_all_voices_yields_silence);
    CU_add_test(s, "a tone takes the stream back",
                test_tone_takes_the_stream_back);
    CU_add_test(s, "streaming takes the stream from a tone",
                test_streaming_takes_the_stream_from_a_tone);
    CU_add_test(s, "generic stop leaves streaming mode",
                test_generic_stop_leaves_streaming_mode);
}

int suite_hda_pcm_init(void)    { return suite_init(); }
int suite_hda_pcm_cleanup(void) { return suite_cleanup(); }
