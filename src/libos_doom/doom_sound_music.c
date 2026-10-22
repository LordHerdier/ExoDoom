/*
 * doom_sound_music.c — Doom's music over OPL3 and the music stream
 * (SCRUM-220).  See doom_sound_music.h for the design.
 */

#include "libos_doom/doom_sound_music.h"
#include "libos_doom/doom_genmidi.h"
#include "libos_doom/doom_midi.h"
#include "libos_doom/doom_opl.h"
#include "doom_wad.h"
#include "exo_syscall.h"

#ifdef EXO_KERNEL
#include "pcm_mixer.h"
#include "hda.h"
_Static_assert(DOOM_MUSIC_RING_FRAMES == PCM_MIXER_MUSIC_FRAMES,
               "doom_sound_music.h restates the mixer's music ring size");
#endif

#include <stddef.h>
#include <stdint.h>

#define OUT_HZ        DOOM_OPL_OUTPUT_HZ
#define BATCH_FRAMES  1024u   /* one stream write */
#define STEP_FRAMES   256u    /* song-clock granularity: 5.3 ms, finer than DMX's 140 Hz */
#define EVENT_BATCH   64u

static doom_opl_t        g_opl;
static doom_genmidi_t    g_genmidi;
static int               g_genmidi_ok;
static doom_midi_song_t  g_song;
static int               g_song_open;

static int16_t  g_buf[BATCH_FRAMES * 2];
static uint32_t g_pend_off;       /* frames of g_buf still to be accepted */
static uint32_t g_pend_n;

static int      g_playing;
static int      g_paused;
static int      g_dead;           /* a stream error latched music off */
static int      g_bound;          /* a write has bound the stream */
static int      g_synth_ok;       /* g_opl has been initialised   */
static uint64_t g_frames;         /* frames rendered since PlaySong */
static uint64_t g_song_ms;        /* song clock handed to doom_midi_advance */

/* Ring 3's room estimate: the last write's answer, plus the drain since. */
static uint32_t g_space;
static uint32_t g_space_ms;

/* ── The one layer that differs between ring 0 and ring 3 ──────────────── */

#ifdef EXO_KERNEL
static int16_t g_ring[DOOM_MUSIC_RING_FRAMES * 2];

/* Same contract as exo_sound_stream_write: frames-accepted | room-left << 32,
 * or a negative -EXO_* code. */
static int64_t hw_write(const int16_t *src, uint32_t frames)
{
    if (!pcm_mixer_music_is_open()) {
        if (!hda_present())
            return -EXO_ENODEV;
        if (pcm_mixer_music_open(g_ring, DOOM_MUSIC_RING_FRAMES) != PCM_MIXER_OK)
            return -EXO_ENOMEM;
        if (!hda_pcm_is_streaming() && hda_pcm_start() != HDA_OK) {
            pcm_mixer_music_close();
            return -EXO_ENODEV;
        }
    }
    uint32_t taken = pcm_mixer_music_write(src, frames);
    return (int64_t)(((uint64_t)pcm_mixer_music_space() << 32) | taken);
}

static void hw_stop(void)
{
    pcm_mixer_music_flush();
    pcm_mixer_music_close();
}

static void hw_volume(int v) { pcm_mixer_music_set_volume(v); }

/* Exact in ring 0; the test drains the ring by hand. */
static uint32_t hw_room(uint32_t now_ms)
{
    (void)now_ms;
    return pcm_mixer_music_is_open() ? pcm_mixer_music_space()
                                     : DOOM_MUSIC_RING_FRAMES;
}
#else
static int64_t hw_write(const int16_t *src, uint32_t frames)
{
    return exo_sound_stream_write(src, frames);
}

static void hw_stop(void)       { (void)exo_sound_stream_stop(); }
static void hw_volume(int v)    { (void)exo_sound_stream_volume(v); }

static uint32_t hw_room(uint32_t now_ms)
{
    uint64_t drained = (uint64_t)(now_ms - g_space_ms) * (OUT_HZ / 1000u);
    uint64_t room = (uint64_t)g_space + drained;
    return room > DOOM_MUSIC_RING_FRAMES ? DOOM_MUSIC_RING_FRAMES : (uint32_t)room;
}
#endif

/* ── Song clock and rendering ──────────────────────────────────────────── */

/* Bring the song up to the audio rendered so far, then render `n` frames. */
static void render_step(int16_t *dst, uint32_t n)
{
    doom_midi_event_t ev[EVENT_BATCH];

    if (g_playing && !doom_midi_finished(&g_song)) {
        uint64_t target = g_frames * 1000u / OUT_HZ;
        uint32_t elapsed = (uint32_t)(target - g_song_ms);
        int k;

        g_song_ms = target;
        do {
            k = doom_midi_advance(&g_song, elapsed, ev, EVENT_BATCH);
            elapsed = 0;
            for (int i = 0; i < k; i++)
                doom_opl_event(&g_opl, &ev[i]);
        } while (k == (int)EVENT_BATCH);

        if (k < 0 || doom_midi_finished(&g_song))
            doom_opl_all_notes_off(&g_opl);   /* let the releases ring out */
    }

    doom_opl_render(&g_opl, dst, n);
    g_frames += n;
}

/* Write g_buf[g_pend_off .. +g_pend_n).  Returns 0 if the stream is done for. */
static int submit(uint32_t now_ms)
{
    int64_t rc = hw_write(g_buf + g_pend_off * 2, g_pend_n);

    if (rc < 0) {
        g_dead = 1;
        g_pend_n = 0;
        return 0;
    }
    uint32_t taken = (uint32_t)((uint64_t)rc & 0xFFFFFFFFu);

    g_bound    = 1;
    g_space    = (uint32_t)((uint64_t)rc >> 32);
    g_space_ms = now_ms;
    g_pend_off += taken;
    g_pend_n   -= taken;
    return 1;
}

/* ── Public API ────────────────────────────────────────────────────────── */

void *doom_music_register(const void *lump, int len)
{
    if (lump == NULL || len <= 0)
        return NULL;

    if (g_song_open) {
        doom_music_stop();
        doom_midi_close(&g_song);
        g_song_open = 0;
    }
    if (doom_midi_open(lump, (uint32_t)len, &g_song) != DOOM_MIDI_OK)
        return NULL;

    g_song_open = 1;
    return &g_song;
}

void doom_music_unregister(void *handle)
{
    if (handle != &g_song || !g_song_open)
        return;
    doom_music_stop();
    doom_midi_close(&g_song);
    g_song_open = 0;
}

void doom_music_play(void *handle, int looping, uint32_t now_ms)
{
    doom_music_stop();

    if (handle != &g_song || !g_song_open || g_dead)
        return;

    if (!g_genmidi_ok) {
        const doom_wad_t *w = doom_wad_mounted();

        if (w == NULL || doom_genmidi_find(&w->wad, &g_genmidi) != DOOM_GENMIDI_OK)
            return;
        g_genmidi_ok = 1;
    }
    if (doom_opl_init(&g_opl, &g_genmidi) != DOOM_OPL_OK)
        return;
    g_synth_ok = 1;

    doom_midi_rewind(&g_song);
    doom_midi_set_looping(&g_song, looping);
    g_frames   = 0;
    g_song_ms  = 0;
    g_pend_off = g_pend_n = 0;
    g_space    = DOOM_MUSIC_RING_FRAMES;
    g_space_ms = now_ms;
    g_paused   = 0;
    g_playing  = 1;
}

void doom_music_stop(void)
{
    if (g_synth_ok)
        doom_opl_all_notes_off(&g_opl);
    if (g_bound && !g_dead)
        hw_stop();
    g_bound = 0;
    g_playing = g_paused = 0;
    g_pend_off = g_pend_n = 0;
}

void doom_music_pause(void)
{
    if (!g_playing || g_paused)
        return;
    /* Keys off the synth AND drops what is queued: with up to 170 ms in the
     * ring, notes-off alone would leave the chord sounding until it drained. */
    doom_opl_all_notes_off(&g_opl);
    if (g_bound && !g_dead)
        hw_stop();
    g_bound = 0;
    g_pend_off = g_pend_n = 0;
    g_paused = 1;
}

void doom_music_resume(uint32_t now_ms)
{
    if (!g_paused)
        return;
    g_paused   = 0;
    g_space    = DOOM_MUSIC_RING_FRAMES;   /* the stream was emptied */
    g_space_ms = now_ms;
}

void doom_music_set_volume(int volume)
{
    if (volume < 0)
        volume = 0;
    if (volume > 127)
        volume = 127;
    hw_volume(volume);
}

int doom_music_is_playing(void)
{
    return g_playing;
}

void doom_music_poll(uint32_t now_ms)
{
    if (!g_playing || g_paused || g_dead)
        return;

    /* A short write last time left frames rendered but not queued. */
    if (g_pend_n && !submit(now_ms))
        return;
    if (g_pend_n)
        return;

    uint32_t room = hw_room(now_ms);
    uint32_t fill = DOOM_MUSIC_RING_FRAMES - room;

    if (fill >= DOOM_MUSIC_TARGET_FRAMES)
        return;

    uint32_t want = DOOM_MUSIC_TARGET_FRAMES - fill;
    if (want > DOOM_MUSIC_MAX_POLL_FRAMES)
        want = DOOM_MUSIC_MAX_POLL_FRAMES;

    while (want > 0 && g_playing) {
        uint32_t n = want < BATCH_FRAMES ? want : BATCH_FRAMES;

        for (uint32_t done = 0; done < n; done += STEP_FRAMES) {
            uint32_t step = n - done < STEP_FRAMES ? n - done : STEP_FRAMES;
            render_step(g_buf + done * 2, step);
        }
        g_pend_off = 0;
        g_pend_n   = n;
        if (!submit(now_ms) || g_pend_n)
            return;
        want -= n;

        /* Song over and not looping: the tail is queued, so we are done. */
        if (doom_midi_finished(&g_song))
            g_playing = 0;
    }
}

int doom_music_available(void)
{
    return !g_dead;
}

void doom_music_reset(void)
{
    doom_music_stop();
    if (g_song_open) {
        doom_midi_close(&g_song);
        g_song_open = 0;
    }
    g_dead = 0;
    g_synth_ok = 0;
    g_genmidi_ok = 0;
    g_frames = g_song_ms = 0;
}

uint64_t doom_music_frames_rendered(void)
{
    return g_frames;
}
