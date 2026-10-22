#pragma once

#include <stdint.h>

/*
 * doom_sound_music — Doom's music_module_t over the OPL3 synth and the music
 * stream (SCRUM-220).
 *
 * The last link of the chain SCRUM-208 scopes:
 *
 *   D_* lump -> doom_midi -> doom_opl (+ GENMIDI) -> 48 kHz s16 stereo
 *            -> exo_sound_stream_write (#32) -> kernel ring -> pcm_mixer music voice
 *
 * src/doom_sound.c holds only the sfxinfo-style glue (the music_module_t
 * table) and calls these.  The logic is here so it can be driven from ring 0
 * by tests/kernel/test_doom_sound_music_k.c, the way src/doom_sound_pcm.c is
 * for effects; like doom_midi.c and doom_opl.c it lives in src/libos_doom/
 * (ring 3, plus an explicit TESTING-kernel step in docker/scripts/build.sh).
 *
 * ── The EXO_KERNEL split ───────────────────────────────────────────────
 *
 * Ring 3 reaches the stream through exo_sound_stream_*.  A ring-0 build (the
 * test kernel) cannot -- its buffers are not inside the LibOS window #32
 * requires -- so it calls pcm_mixer_music_* directly, the layer beneath the
 * syscalls, over a static ring.  See src/doom_sound_pcm.c for the same split.
 *
 * ── Pacing ─────────────────────────────────────────────────────────────
 *
 * The ring, not the frame rate, sets how much work a doom_music_poll() does:
 * it renders only what the stream has room for, up to a fill target, so a slow
 * frame is caught up on the next one rather than dropping audio, and a fast
 * one does no work.  The song clock is the count of frames *rendered*, not
 * the wall clock, so the music cannot drift against the 48 kHz output.  Wall
 * time is used for one thing only: ring 3 cannot ask how much room there is
 * (the only answer is a write's result), so it estimates the drain since the
 * last write and lets the next write's result correct it.
 *
 * ── Degradation ────────────────────────────────────────────────────────
 *
 * No `#ifdef` and no build switch.  No WAD, no GENMIDI, or an unparseable
 * lump costs that song only.  Any error from the stream (-EXO_ENODEV on a
 * machine with no HDA controller, -EXO_EBUSY, ...) latches music off for the
 * run: a failed syscall every frame would cost more than the silence.
 */

/* Ring capacity in frames: PCM_MIXER_MUSIC_FRAMES, which the kernel build
 * asserts, restated so ring 3 need not include a driver header. */
#define DOOM_MUSIC_RING_FRAMES   12288u
/* Keep this much queued ahead of the DAC (170 ms), no more: a deeper fill is
 * latency on a stop or a pause and a longer hitch on a song's first poll. */
#define DOOM_MUSIC_TARGET_FRAMES 8192u
/* Upper bound of audio rendered by one poll (85 ms), so catching up after a
 * stall is spread over a few frames instead of one long one. */
#define DOOM_MUSIC_MAX_POLL_FRAMES 4096u

/* RegisterSong: open a `D_*` lump (MUS or MIDI).  NULL if it is neither or the
 * lump is damaged.  Registering again replaces the previous song. */
void *doom_music_register(const void *lump, int len);
void  doom_music_unregister(void *handle);

/* PlaySong: reset the synth, rewind and start.  A NULL handle, or no GENMIDI in
 * the mounted WAD, plays nothing. */
void doom_music_play(void *handle, int looping, uint32_t now_ms);

/* StopSong: notes off, stream stopped and released. */
void doom_music_stop(void);

/* PauseMusic/ResumeMusic.  Pause silences held notes AND drops the queued
 * audio, so it is immediate rather than up to 170 ms late, and puts the song
 * back to where the listener had got to -- not where rendering had got to --
 * so resuming does not skip the dropped stretch. */
void doom_music_pause(uint32_t now_ms);
void doom_music_resume(uint32_t now_ms);

/* 0..127 (Doom's S_SetMusicVolume range).  Reaches the stream level at once,
 * with or without a song. */
void doom_music_set_volume(int volume);

/* 1 while a song is playing (or paused); 0 after it ends, is stopped, or music
 * has latched off. */
int  doom_music_is_playing(void);

/* The per-frame hook: advance the song and keep the stream fed. */
void doom_music_poll(uint32_t now_ms);

/* 1 until a stream error has latched music off. */
int  doom_music_available(void);

/* Stop, drop the song and clear every latch.  Shutdown, and test isolation. */
void doom_music_reset(void);

/* Diagnostics, for tests. */
uint64_t doom_music_frames_rendered(void);

/* ── Timing profile ─────────────────────────────────────────────────────
 *
 * Where music pacing goes wrong, measured per song (reset by PlaySong):
 *
 *   dt_*     wall ms between polls.  The ring holds 12288 frames (256 ms) and
 *            is topped up to 170 ms, so a gap past ~170 ms starves it.
 *   fill_*   frames queued at the start of a poll; `starved` counts polls that
 *            found it empty.  Exact in ring 0, an estimate in ring 3.
 *   late_*   microseconds a note-on is applied after its song time.  This is
 *            the render-step (5.3 ms) and whole-ms clock quantization, and is
 *            independent of how well the ring is kept fed.
 *   cost_*   wall ms one poll took (caller-measured, 1 ms resolution).
 *   underruns  frames the mixer had to fill with silence (ring 0 only).
 */
typedef struct {
    uint32_t polls, dt_min, dt_max, dt_sum;
    uint32_t dt_hist[5];       /* <=20, <=40, <=80, <=170, >170 ms */
    uint32_t fills, fill_min, fill_max, fill_sum, starved;
    uint32_t notes, late_min, late_max, late_sum;
    uint32_t late_hist[4];     /* <1, <2, <4, >=4 ms */
    uint32_t costs, cost_max, cost_sum;
    uint32_t underruns;
} doom_music_prof_t;

void doom_music_prof_reset(void);
void doom_music_prof_get(doom_music_prof_t *out);
/* The caller times doom_music_poll() and reports it here. */
void doom_music_prof_note_cost(uint32_t ms);
