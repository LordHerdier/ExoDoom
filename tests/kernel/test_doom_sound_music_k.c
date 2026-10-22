/*
 * test_doom_sound_music.c — Doom's music module over OPL3 and the music
 * stream (SCRUM-220).
 *
 * Drives src/libos_doom/doom_sound_music.c from ring 0 the way Doom drives the
 * music_module_t: RegisterSong -> PlaySong -> Poll x N -> StopSong, against the
 * real mixer and the real freedoom2 lumps.  The clock is hand-advanced (every
 * call takes `now_ms`), interrupts stay off, and the "DAC" is
 * pcm_mixer_render() called by the test -- so how much the module writes is
 * decided by the ring's room alone, which is the claim under test.
 *
 * Not reachable from here: the -EXO_ENODEV latch (this suite requires the
 * controller), and the table in src/doom_sound.c (ring 3 only).
 */

#include "kunit.h"
#include "libos_doom/doom_sound_music.h"
#include "doom_wad.h"
#include "doom_wad_disk.h"
#include "hda.h"
#include "pcm_mixer.h"
#include "syscall_sound.h"
#include "wad.h"

#include <stddef.h>
#include <stdint.h>

#define SINK_FRAMES 4096u
static int16_t g_sink[SINK_FRAMES * PCM_MIXER_CHANNELS];

/* A one-note SMF, division 96 (default 120 BPM -> 5.2 ms per tick), program 0
 * and middle C.  The note-off comes after 48 ticks (250 ms) in SHORT_SONG and
 * after 384 (2 s) in LONG_SONG. */
static const uint8_t SHORT_SONG[] = {
    'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,96,
    'M','T','r','k', 0,0,0,15,
    0x00,0xC0,0x00,  0x00,0x90,0x3C,0x64,  0x30,0x80,0x3C,0x00,  0x00,0xFF,0x2F,0x00,
};
static const uint8_t LONG_SONG[] = {
    'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,96,
    'M','T','r','k', 0,0,0,16,
    0x00,0xC0,0x00,  0x00,0x90,0x3C,0x64,  0x83,0x00,0x80,0x3C,0x00,  0x00,0xFF,0x2F,0x00,
};

/* 0 if the IWAD is mounted afterwards.  Reported rather than swallowed: a test
 * that unmounts the WAD and fails to put it back would otherwise leave every
 * later test in the suite without a GENMIDI, failing for the wrong reason. */
static int mount_real_wad(void)
{
    uint64_t paddr = 0, size = 0;

    if (doom_wad_disk_get(&paddr, &size) != 0)
        return -1;
    return doom_wad_mount((const void *)(uintptr_t)paddr, (uint32_t)size);
}

static void quiesce(void)
{
    doom_music_reset();
    hda_pcm_stop();
    pcm_mixer_reset();
}

int suite_doom_sound_music_init(void)
{
    (void)mount_real_wad();   /* test_preconditions reports a failure by name */
    quiesce();
    return 0;
}

int suite_doom_sound_music_cleanup(void)
{
    quiesce();
    doom_wad_unmount();
    return 0;
}

/* The test's DAC: consume `frames` from the mixer; returns 1 if any sample
 * was non-zero. */
static int drain(uint32_t frames)
{
    int loud = 0;

    while (frames > 0) {
        uint32_t n = frames < SINK_FRAMES ? frames : SINK_FRAMES;

        pcm_mixer_render(g_sink, n);
        for (uint32_t i = 0; i < n * PCM_MIXER_CHANNELS; i++)
            if (g_sink[i] != 0)
                loud = 1;
        frames -= n;
    }
    return loud;
}

static void *register_bytes(const uint8_t *b, size_t n)
{
    return doom_music_register(b, (int)n);
}

static void test_preconditions(void)
{
    uint32_t size = 0;
    const doom_wad_t *w = doom_wad_mounted();

    CU_ASSERT_TRUE(hda_present());
    CU_ASSERT_PTR_NOT_NULL(w);
    if (w != NULL)
        CU_ASSERT_PTR_NOT_NULL(wad_find_lump(&w->wad, "D_OPENIN", &size));
    CU_ASSERT_TRUE(DOOM_MUSIC_TARGET_FRAMES <= DOOM_MUSIC_RING_FRAMES);
    /* The ring-3 build restates the ring size (it cannot include a driver
     * header) and sizes its pacing from it; pin it to both kernel constants so
     * a change to the ring cannot silently skew every fill calculation. */
    CU_ASSERT_EQUAL(DOOM_MUSIC_RING_FRAMES, PCM_MIXER_MUSIC_FRAMES);
    CU_ASSERT_EQUAL(DOOM_MUSIC_RING_FRAMES, SOUND_STREAM_MAX_FRAMES);
}

static void test_register_play_poll_stop(void)
{
    uint32_t size = 0;
    const doom_wad_t *w = doom_wad_mounted();
    const uint8_t *lump = w ? wad_find_lump(&w->wad, "D_OPENIN", &size) : NULL;

    CU_ASSERT_PTR_NOT_NULL(lump);
    if (lump == NULL || !hda_present())
        return;
    quiesce();

    void *h = doom_music_register(lump, (int)size);
    CU_ASSERT_PTR_NOT_NULL(h);
    doom_music_play(h, 1, 0);
    CU_ASSERT_TRUE(doom_music_is_playing());

    int loud = 0;
    uint32_t now = 0;
    for (int i = 0; i < 40; i++) {
        doom_music_poll(now);
        loud |= drain(1400);   /* ~one 35 fps frame of audio */
        now += 29;
    }
    CU_ASSERT_TRUE(loud);
    CU_ASSERT_TRUE(pcm_mixer_music_is_open());
    CU_ASSERT_EQUAL(pcm_mixer_music_underruns(), 0u);
    CU_ASSERT_TRUE(doom_music_is_playing());

    doom_music_stop();
    CU_ASSERT_FALSE(doom_music_is_playing());
    CU_ASSERT_FALSE(pcm_mixer_music_is_open());
    doom_music_unregister(h);
    quiesce();
}

static void test_pacing_follows_ring_room(void)
{
    if (!hda_present())
        return;
    quiesce();

    void *h = register_bytes(LONG_SONG, sizeof(LONG_SONG));
    CU_ASSERT_PTR_NOT_NULL(h);
    doom_music_play(h, 1, 0);

    /* Empty ring: one poll fills toward the target, bounded per poll. */
    doom_music_poll(0);
    uint32_t fill = pcm_mixer_music_fill();
    CU_ASSERT_EQUAL(fill, DOOM_MUSIC_MAX_POLL_FRAMES);
    doom_music_poll(0);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), DOOM_MUSIC_TARGET_FRAMES);

    /* Full to target: a further poll does NO work, however often it is asked. */
    uint64_t rendered = doom_music_frames_rendered();
    for (int i = 0; i < 10; i++)
        doom_music_poll(10 + i);
    CU_ASSERT_EQUAL(doom_music_frames_rendered(), rendered);

    /* Consume 3000 frames: the next poll renders exactly that much back. */
    (void)drain(3000);
    doom_music_poll(100);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), DOOM_MUSIC_TARGET_FRAMES);
    CU_ASSERT_EQUAL(doom_music_frames_rendered(), rendered + 3000u);

    /* Audio written == audio rendered: the song clock cannot drift. */
    uint64_t consumed = 3000;
    CU_ASSERT_EQUAL(doom_music_frames_rendered(),
                    consumed + pcm_mixer_music_fill());

    /* A long stall (everything drained, 500 ms of wall time): catch-up is
     * spread over polls, never past the ring. */
    (void)drain(pcm_mixer_music_fill());
    doom_music_poll(600);
    CU_ASSERT_TRUE(pcm_mixer_music_fill() <= DOOM_MUSIC_MAX_POLL_FRAMES);
    doom_music_poll(600);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), DOOM_MUSIC_TARGET_FRAMES);

    quiesce();
}

#ifdef AUDIO_PROFILE
static void test_timing_profile(void)
{
    if (!hda_present())
        return;
    quiesce();

    void *h = register_bytes(LONG_SONG, sizeof(LONG_SONG));
    doom_music_play(h, 1, 0);
    for (int i = 0; i < 40; i++) {
        doom_music_poll(i * 29);
        (void)drain(1400);
    }

    doom_music_prof_t p;
    doom_music_prof_get(&p);

    CU_ASSERT_EQUAL(p.polls, 39u);            /* 40 polls, 39 gaps */
    CU_ASSERT_EQUAL(p.dt_min, 29u);
    CU_ASSERT_EQUAL(p.dt_max, 29u);
    CU_ASSERT_EQUAL(p.dt_hist[1], 39u);       /* all in the <=40 ms bucket */
    CU_ASSERT_EQUAL(p.starved, 0u);
    CU_ASSERT_TRUE(p.fill_max <= DOOM_MUSIC_RING_FRAMES);
    /* One note-on per pass of the 2 s song, applied at its own output frame:
     * under one sample (21 us) late, not a render step (5.3 ms). */
    CU_ASSERT_TRUE(p.notes >= 1u);
    CU_ASSERT_TRUE(p.late_max < 22u);

    doom_music_play(h, 1, 0);                 /* a new song starts clean */
    doom_music_prof_get(&p);
    CU_ASSERT_EQUAL(p.polls, 0u);
    CU_ASSERT_EQUAL(p.notes, 0u);

    doom_music_stop();
    doom_music_unregister(h);
    quiesce();
}

#endif /* AUDIO_PROFILE */

static void test_loop_and_end(void)
{
    if (!hda_present())
        return;
    quiesce();

    /* 250 ms song, 12000 frames; run for ~1 s of audio. */
    void *h = register_bytes(SHORT_SONG, sizeof(SHORT_SONG));
    doom_music_play(h, 1, 0);
    for (int i = 0; i < 40; i++) {
        doom_music_poll(i * 29);
        (void)drain(1400);
    }
    CU_ASSERT_TRUE(doom_music_is_playing());   /* looped, still going */

    doom_music_play(h, 0, 0);
    int loud = 0;
    /* Up to the tail cap (2 s) past the 250 ms song; it ends sooner if the
     * release decays to digital silence first. */
    for (int i = 0; i < 120 && doom_music_is_playing(); i++) {
        doom_music_poll(i * 29);
        loud |= drain(1400);
    }
    CU_ASSERT_TRUE(loud);
    CU_ASSERT_FALSE(doom_music_is_playing());  /* ended on its own */
    /* The note-off is at 250 ms (12000 frames); the release is rendered past
     * it rather than cut at the key-off, but not for longer than the cap. */
    CU_ASSERT_TRUE(doom_music_frames_rendered() > 12000u);
    CU_ASSERT_TRUE(doom_music_frames_rendered() <= 12000u + 2u * 48000u + 4096u);

    quiesce();
}

static void test_pause_silences_and_resume_continues(void)
{
    if (!hda_present())
        return;
    quiesce();

    void *h = register_bytes(LONG_SONG, sizeof(LONG_SONG));
    doom_music_play(h, 0, 0);
    int loud = 0;
    for (int i = 0; i < 4; i++) {
        doom_music_poll(i * 29);
        loud |= drain(1000);
    }
    CU_ASSERT_TRUE(loud);                      /* the note is sounding */
    doom_music_poll(200);
    CU_ASSERT_TRUE(pcm_mixer_music_fill() > 0);

    /* 4000 frames were heard and the rest of the 8192 queued were not:
     * pausing takes the song back to 4000, not leaves it at the render
     * position, so resuming does not skip the dropped stretch. */
    CU_ASSERT_TRUE(doom_music_frames_rendered() > 4000u);
    doom_music_pause(300);
    CU_ASSERT_EQUAL(doom_music_frames_rendered(), 4000u);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 0u);       /* queued audio dropped */
    CU_ASSERT_FALSE(drain(2048));                      /* ...and nothing rings */
    doom_music_poll(300);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 0u);       /* paused: no work */

    doom_music_resume(400);
    doom_music_poll(400);
    CU_ASSERT_TRUE(pcm_mixer_music_fill() > 0);
    CU_ASSERT_TRUE(doom_music_is_playing());

    quiesce();
}

static void test_volume_reaches_the_stream(void)
{
    quiesce();
    doom_music_set_volume(40);
    CU_ASSERT_EQUAL(pcm_mixer_music_volume(), 40);
    doom_music_set_volume(127);
    CU_ASSERT_EQUAL(pcm_mixer_music_volume(), 127);
    doom_music_set_volume(999);
    CU_ASSERT_EQUAL(pcm_mixer_music_volume(), PCM_MIXER_VOL_MAX);
    doom_music_set_volume(-5);
    CU_ASSERT_EQUAL(pcm_mixer_music_volume(), 0);
    doom_music_set_volume(127);
    quiesce();
}

static void test_bad_input_costs_only_that_song(void)
{
    static const uint8_t junk[16] = { 1, 2, 3 };

    quiesce();
    CU_ASSERT_PTR_NULL(doom_music_register(NULL, 10));
    CU_ASSERT_PTR_NULL(doom_music_register(junk, 0));
    CU_ASSERT_PTR_NULL(doom_music_register(junk, (int)sizeof(junk)));

    doom_music_play(NULL, 1, 0);               /* no handle: nothing plays */
    CU_ASSERT_FALSE(doom_music_is_playing());
    doom_music_poll(0);                        /* and polling is harmless */
    CU_ASSERT_FALSE(pcm_mixer_music_is_open());
    CU_ASSERT_TRUE(doom_music_available());

    /* No WAD, so no GENMIDI: that song is silent, the next one is not. */
    doom_wad_unmount();
    void *h = register_bytes(SHORT_SONG, sizeof(SHORT_SONG));
    CU_ASSERT_PTR_NOT_NULL(h);
    doom_music_play(h, 0, 0);
    CU_ASSERT_FALSE(doom_music_is_playing());
    CU_ASSERT_TRUE(doom_music_available());

    CU_ASSERT_EQUAL(mount_real_wad(), 0);
    CU_ASSERT_PTR_NOT_NULL(doom_wad_mounted());
    if (hda_present()) {
        doom_music_play(h, 0, 0);
        CU_ASSERT_TRUE(doom_music_is_playing());
    }
    quiesce();
}

void suite_doom_sound_music_tests(CU_pSuite s)
{
    CU_add_test(s, "WAD, controller and D_OPENIN are present", test_preconditions);
    CU_add_test(s, "RegisterSong -> PlaySong -> Poll xN -> StopSong",
                test_register_play_poll_stop);
    CU_add_test(s, "poll renders only what the ring has room for",
                test_pacing_follows_ring_room);
#ifdef AUDIO_PROFILE
    CU_add_test(s, "timing profile counts polls, fill and note lateness",
                test_timing_profile);
#endif
    CU_add_test(s, "loops, and ends on its own when not looping", test_loop_and_end);
    CU_add_test(s, "pause silences held notes; resume continues",
                test_pause_silences_and_resume_continues);
    CU_add_test(s, "volume reaches the stream", test_volume_reaches_the_stream);
    CU_add_test(s, "bad input costs only that song", test_bad_input_costs_only_that_song);
}
