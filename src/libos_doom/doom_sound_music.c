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
/* Longest release we will wait out after the last event: OPL pads decay over
 * a second or two, and a song must not hold the stream open forever. */
#define TAIL_MAX_FRAMES (2u * OUT_HZ)

static doom_opl_t        g_opl;
static doom_genmidi_t    g_genmidi;
static doom_midi_song_t  g_song;
static int               g_song_open;

static int16_t  g_buf[BATCH_FRAMES * 2];
static uint32_t g_pend_off;       /* frames of g_buf still to be accepted */
static uint32_t g_pend_n;

static int      g_playing;
static int      g_paused;
static int      g_dead;           /* a stream error latched music off */
static int      g_looping;
static int      g_ending;         /* last event consumed (or reader error): rendering the tail */
static uint32_t g_tail;           /* frames rendered since g_ending */
static int      g_bound;          /* a write has bound the stream */
static int      g_synth_ok;       /* g_opl has been initialised   */
static uint64_t g_frames;         /* frames rendered since PlaySong */
static uint64_t g_song_ms;        /* song clock handed to doom_midi_advance */
static doom_midi_event_t g_ev[EVENT_BATCH];   /* fetched, not yet applied */
static uint32_t g_ev_i, g_ev_n;
static int      g_ev_err;         /* the reader failed: the song ends */

/* Timing profile (see doom_sound_music.h).  Plain counters, a few adds per
 * poll or per note: cheap enough to leave on. */
static doom_music_prof_t g_prof;
static uint32_t          g_prof_last_poll;
static int               g_prof_have_last;

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

/* How late a note-on is applied against where the song says it belongs, at
 * output frame `at`.  Events are applied at their own frame (render_step), so
 * this is the sub-sample rounding; it is here to keep that claim measured. */
static void prof_note(const doom_midi_event_t *ev, uint64_t at)
{
    if ((ev->status & 0xF0u) != 0x90u || ev->data2 == 0)
        return;

    uint64_t at_us = at * 1000000u / OUT_HZ;
    uint32_t late  = at_us > ev->time_us ? (uint32_t)(at_us - ev->time_us) : 0;
    uint32_t b     = late < 1000u ? 0 : late < 2000u ? 1 : late < 4000u ? 2 : 3;

    if (g_prof.notes == 0 || late < g_prof.late_min)
        g_prof.late_min = late;
    if (late > g_prof.late_max)
        g_prof.late_max = late;
    g_prof.late_sum += late;
    g_prof.late_hist[b]++;
    g_prof.notes++;
}

static void prof_poll(uint32_t now_ms, uint32_t fill)
{
    if (g_prof_have_last) {
        uint32_t dt = now_ms - g_prof_last_poll;
        uint32_t b  = dt <= 20 ? 0 : dt <= 40 ? 1 : dt <= 80 ? 2 :
                      dt <= 170 ? 3 : 4;

        if (g_prof.polls == 0 || dt < g_prof.dt_min)
            g_prof.dt_min = dt;
        if (dt > g_prof.dt_max)
            g_prof.dt_max = dt;
        g_prof.dt_sum += dt;
        g_prof.dt_hist[b]++;
        g_prof.polls++;
    }
    g_prof_last_poll = now_ms;
    g_prof_have_last = 1;

    if (g_prof.fills == 0 || fill < g_prof.fill_min)
        g_prof.fill_min = fill;
    if (fill > g_prof.fill_max)
        g_prof.fill_max = fill;
    g_prof.fill_sum += fill;
    g_prof.fills++;
    if (fill == 0 && g_frames > 0)
        g_prof.starved++;
}

/* Render `n` frames into `dst`, counting them. */
static void render_span(int16_t *dst, uint32_t n)
{
    doom_opl_render(&g_opl, dst, n);
    g_frames += n;
    if (g_ending)
        g_tail += n;
}

/* Pull the next events off the song: everything due by `target_ms`, which may
 * run up to a millisecond past the step being rendered.  Those stay queued in
 * g_ev and are applied at their own frame, in this step or a later one.
 * Returns 0 with the queue empty when nothing more is due. */
static int fetch_events(uint32_t target_ms)
{
    uint32_t elapsed = target_ms > g_song_ms ? (uint32_t)(target_ms - g_song_ms) : 0;
    int k;

    g_song_ms += elapsed;
    k = doom_midi_advance(&g_song, elapsed, g_ev, EVENT_BATCH);
    g_ev_i = 0;
    g_ev_n = k > 0 ? (uint32_t)k : 0;
    if (k < 0)
        g_ev_err = 1;
    return g_ev_n != 0;
}

/* Render `n` frames, applying each song event at the frame it belongs to: the
 * span is split at the event's sample position instead of handing the synth
 * every due event at the start of the step.  (Steps of STEP_FRAMES snapped
 * notes to a 5.3 ms grid under a whole-ms clock, up to ~6 ms late and by a
 * different amount each time.) */
static void render_step(int16_t *dst, uint32_t n)
{
    uint32_t pos = 0;

    if (g_playing && !g_ending) {
        uint64_t end    = g_frames + n;
        uint64_t end_us = end * 1000000u / OUT_HZ;

        for (;;) {
            if (g_ev_i == g_ev_n && !fetch_events((uint32_t)((end_us + 999u) / 1000u)))
                break;

            const doom_midi_event_t *ev = &g_ev[g_ev_i];
            uint64_t at = (ev->time_us * OUT_HZ + 999999u) / 1000000u;   /* ceil */

            if (at >= end)
                break;                        /* a later step's */
            if (at > g_frames) {
                render_span(dst + pos * 2, (uint32_t)(at - g_frames));
                pos = (uint32_t)(at - (end - n));
            }
            prof_note(ev, g_frames);
            doom_opl_event(&g_opl, ev);
            g_ev_i++;
        }

        /* Every event consumed and the song over (or unreadable): the release
         * tail starts here.  Not while an event is still queued -- the final
         * note-offs can sit in g_ev past the flag. */
        if (g_ev_i == g_ev_n && (g_ev_err || doom_midi_finished(&g_song))) {
            g_ending = 1;
            g_tail   = 0;
            doom_opl_all_notes_off(&g_opl);
        }
    }

    if (pos < n)
        render_span(dst + pos * 2, n - pos);
}

/* Put the synth and the song where the listener is: `frames` into the song.
 * Used by pause, which throws away audio that was rendered but never heard;
 * resuming from the render position instead would skip that stretch, and any
 * note-on inside it would be lost while its note-off still landed.  Replaying
 * the events up to the target into a fresh synth restores programs, volumes
 * and the notes that should be sounding, without rendering a sample. */
static int resync_to(uint64_t frames)
{
    doom_midi_event_t ev[EVENT_BATCH];
    uint32_t elapsed;
    int k;

    if (doom_opl_init(&g_opl, &g_genmidi) != DOOM_OPL_OK)
        return -1;
    doom_midi_rewind(&g_song);
    doom_midi_set_looping(&g_song, g_looping);

    g_frames  = frames;
    g_ev_i = g_ev_n = 0;
    g_ev_err  = 0;
    g_song_ms = frames * 1000u / OUT_HZ;
    elapsed   = (uint32_t)g_song_ms;
    do {
        k = doom_midi_advance(&g_song, elapsed, ev, EVENT_BATCH);
        elapsed = 0;
        for (int i = 0; i < k; i++)
            doom_opl_event(&g_opl, &ev[i]);
    } while (k == (int)EVENT_BATCH);
    return k < 0 ? -1 : 0;
}

static int batch_is_silent(uint32_t frames)
{
    for (uint32_t i = 0; i < frames * 2; i++)
        if (g_buf[i] != 0)
            return 0;
    return 1;
}

/* Write g_buf[g_pend_off .. +g_pend_n).  Returns 0 if the stream is done for. */
static int submit(uint32_t now_ms)
{
    int64_t rc = hw_write(g_buf + g_pend_off * 2, g_pend_n);

    if (rc < 0) {
        /* Latched off for the run, and this song with it: MusicIsPlaying() must
         * not keep answering yes for audio that can never come out. */
        g_dead = 1;
        g_pend_n = 0;
        g_playing = 0;
        doom_opl_all_notes_off(&g_opl);
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

    /* Looked up per song, not cached: g_genmidi points into the mounted WAD's
     * lump, and a cache would outlive an unmount or a remount.  It is a
     * directory lookup and a header check, once per level. */
    const doom_wad_t *w = doom_wad_mounted();

    if (w == NULL || doom_genmidi_find(&w->wad, &g_genmidi) != DOOM_GENMIDI_OK)
        return;
    /* Cleared first: a failed init leaves g_opl half-built, and stop/pause
     * must not key off a chip that was never brought up. */
    g_synth_ok = 0;
    if (doom_opl_init(&g_opl, &g_genmidi) != DOOM_OPL_OK)
        return;
    g_synth_ok = 1;

    doom_midi_rewind(&g_song);
    doom_midi_set_looping(&g_song, looping);
    g_looping  = looping;
    g_frames   = 0;
    g_song_ms  = 0;
    g_ev_i = g_ev_n = 0;
    g_ev_err   = 0;
    g_ending   = 0;
    g_tail     = 0;
    g_pend_off = g_pend_n = 0;
    g_space    = DOOM_MUSIC_RING_FRAMES;
    g_space_ms = now_ms;
    g_paused   = 0;
    g_playing  = 1;
    doom_music_prof_reset();
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

void doom_music_pause(uint32_t now_ms)
{
    if (!g_playing || g_paused)
        return;

    /* Rendered but not heard: what is in the ring, plus what a short write
     * left in g_buf.  An estimate in ring 3 (hw_room), exact in ring 0. */
    uint64_t unheard = (uint64_t)(DOOM_MUSIC_RING_FRAMES - hw_room(now_ms)) +
                       g_pend_n;
    uint64_t heard = g_frames > unheard ? g_frames - unheard : 0;

    /* Keys off the synth AND drops what is queued: with up to 170 ms in the
     * ring, notes-off alone would leave the chord sounding until it drained. */
    doom_opl_all_notes_off(&g_opl);
    if (g_bound && !g_dead)
        hw_stop();
    g_bound = 0;
    g_pend_off = g_pend_n = 0;

    /* In the release tail there is nothing left to resume: the song is over. */
    if (g_ending) {
        g_playing = 0;
        return;
    }
    if (resync_to(heard) != 0) {
        g_playing = 0;
        return;
    }
    g_paused = 1;
}

void doom_music_resume(uint32_t now_ms)
{
    if (!g_paused)
        return;
    g_paused   = 0;
    g_space    = DOOM_MUSIC_RING_FRAMES;   /* the stream was emptied */
    g_space_ms = now_ms;
    g_prof_have_last = 0;                  /* a pause is not a slow poll */
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

    prof_poll(now_ms, fill);

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

        /* Song over and not looping: keep feeding the stream until the
         * releases have decayed (a batch of exact digital silence, which the
         * synth guarantees once they have) or the tail has run its course.
         * Stopping at the key-off would cut a long release to a click. */
        if (g_ending && (g_tail >= TAIL_MAX_FRAMES || batch_is_silent(n)))
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
    g_frames = g_song_ms = 0;
}

uint64_t doom_music_frames_rendered(void)
{
    return g_frames;
}

/* ── Timing profile ────────────────────────────────────────────────────── */

void doom_music_prof_reset(void)
{
    g_prof = (doom_music_prof_t){ 0 };
    g_prof_have_last = 0;
}

void doom_music_prof_note_cost(uint32_t ms)
{
    if (ms > g_prof.cost_max)
        g_prof.cost_max = ms;
    g_prof.cost_sum += ms;
    g_prof.costs++;
}

void doom_music_prof_get(doom_music_prof_t *out)
{
    *out = g_prof;
#ifdef EXO_KERNEL
    out->underruns = pcm_mixer_music_underruns();
#endif
}
