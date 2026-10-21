#pragma once

#include <stdint.h>

/*
 * doom_midi — Doom's music lumps (`D_*`) as a timed stream of MIDI events
 * (SCRUM-215).
 *
 * The counterpart to src/doom_dmx.c on the music side, and the first link of
 * the chain SCRUM-208 scopes: this layer decides *what note happens when*
 * and nothing about how it sounds.  The GENMIDI patch decoder (SCRUM-216) and
 * the OPL3 synthesizer (SCRUM-217) consume what comes out of here.
 *
 * ── Two input formats, sniffed rather than assumed ─────────────────────
 *
 * Commercial Doom IWADs store music as DMX MUS; the pinned freedoom2 v0.13.0
 * stores raw Standard MIDI Files (all 35 `D_*` lumps begin `MThd`).  A reader
 * that assumed MUS would play nothing on the WAD this project boots with, and
 * one that assumed MIDI would break on retail Doom, so doom_midi_open() looks
 * at the first four bytes: `MThd` is parsed in place, `MUS\x1a` is converted
 * first.
 *
 * ── Why MUS conversion is a hook, not a call ───────────────────────────
 *
 * The converter is the vendored src/doom/mus2mid.c, which allocates through
 * Doom's zone allocator.  This file is picked up by build.sh's `src/` C-file glob
 * in *every* kernel build, where nothing from src/doom/ links (the same
 * constraint src/doom_dmx.h spells out for S_sfx[]), so it cannot name
 * mus2mid().  src/libos_doom/doom_midi_mus.c is the one file that does; it exists only
 * in the libos_doom link and the TESTING kernel (a subdirectory, so the glob
 * never sees it), and installs itself here via
 * doom_midi_set_mus_converter().  A MUS lump with no converter installed is
 * DOOM_MIDI_ENOCONV, not silence.
 *
 * ── Zero copy, no allocation ───────────────────────────────────────────
 *
 * A parsed `MThd` song points *into* the lump (which points into the mounted
 * WAD) and keeps per-track cursors in a fixed array, so there is nothing to
 * free on that path.  Only a converted MUS lump owns memory, and
 * doom_midi_close() releases it.
 *
 * ── Every read is bounds-checked against the lump ──────────────────────
 *
 * These bytes are inside the read-only mapped IWAD, and a runaway parse walks
 * into the neighbouring lumps.  So doom_midi_open() validates every track end
 * to end *before* returning success -- a song that opens can be advanced
 * without a further bounds failure -- and the cursor helpers in doom_midi.c
 * refuse to step past a track's end rather than trusting a length field.
 *
 * ── Time ───────────────────────────────────────────────────────────────
 *
 * Event times are exact integer microseconds: ticks are converted with the
 * tempo in force *at that point in the song* and the sub-microsecond
 * remainder is carried, so a long song does not drift.  No floating point
 * (the kernel is -mno-sse and so is anything linked beside it).  A song with
 * no tempo meta runs at 120 BPM (500000 us per quarter note), per the SMF
 * spec.
 */

#define DOOM_MIDI_MAX_TRACKS       32u
#define DOOM_MIDI_DEFAULT_TEMPO_US 500000u   /* 120 BPM */

/*
 * Result codes.  Distinct values rather than a bare -1 for the reason
 * src/doom_dmx.h gives: each points an investigation somewhere different.
 */
#define DOOM_MIDI_OK         0
#define DOOM_MIDI_EINVAL    -1  /* NULL argument, or a lump too short to name a format */
#define DOOM_MIDI_EFORMAT   -2  /* bad magic / header / event, or SMPTE division */
#define DOOM_MIDI_ETRUNC    -3  /* a chunk, event or track ran past its end */
#define DOOM_MIDI_ETOOMANY  -4  /* more tracks than DOOM_MIDI_MAX_TRACKS */
#define DOOM_MIDI_ENOCONV   -5  /* MUS lump, but no converter installed */
#define DOOM_MIDI_ECONV     -6  /* the MUS converter refused the lump */

/* One channel-voice event.  `status` keeps the channel in its low nibble
 * (0x90 | ch for note-on), exactly as it appeared on the wire; `data2` is 0
 * for the two-byte events (program change, channel pressure). */
typedef struct {
    uint64_t time_us;   /* on the song clock: since open/rewind, across loops */
    uint8_t  status;
    uint8_t  data1;
    uint8_t  data2;
} doom_midi_event_t;

typedef struct {
    const uint8_t *start;       /* first byte of the track's event data */
    const uint8_t *end;         /* one past its last byte */
    const uint8_t *cur;         /* next event, just after its delta time */
    uint32_t       next_tick;   /* absolute tick of the event at `cur` */
    uint8_t        running;     /* running-status byte, 0 if none */
    uint8_t        done;        /* end-of-track consumed */
} doom_midi_track_t;

typedef struct {
    doom_midi_track_t tracks[DOOM_MIDI_MAX_TRACKS];
    uint32_t          ntracks;
    uint32_t          division;     /* ticks per quarter note */

    /* Playback position. */
    uint32_t          tempo_us;     /* us per quarter note, now */
    uint32_t          frac;         /* tick*tempo remainder, in 1/division us */
    uint32_t          last_tick;    /* tick of the last event processed */
    uint64_t          us;           /* last event processed, from the pass start */
    uint64_t          pass_us;      /* the current pass's start, on the song clock */
    uint64_t          clock_us;     /* time handed to advance() so far */

    int               looping;
    int               finished;     /* every track ended and not looping */
    uint32_t          loops;        /* completed passes, when looping */

    /* Set only for a converted MUS lump. */
    void            (*release)(void *handle);
    void             *handle;
} doom_midi_song_t;

/*
 * MUS converter hook.  On success `convert` points *midi and *midi_size at a
 * complete SMF and sets *handle to whatever `release` needs to free it; the
 * buffer must stay valid until release(handle).  Returns 0 or non-zero.
 */
typedef int  (*doom_midi_mus_convert_fn)(const void *mus, uint32_t mus_size,
                                         const uint8_t **midi, uint32_t *midi_size,
                                         void **handle);
typedef void (*doom_midi_mus_release_fn)(void *handle);

void doom_midi_set_mus_converter(doom_midi_mus_convert_fn convert,
                                 doom_midi_mus_release_fn release);

/* Installs the mus2mid-backed converter.  Defined by
 * src/libos_doom/doom_midi_mus.c, so it exists only in the libos_doom link and
 * the TESTING kernel; a shipped kernel must not call it. */
void doom_midi_mus_install(void);

/*
 * Open [lump, lump + lump_size) as a MUS or MIDI music lump.
 *
 * On DOOM_MIDI_OK the song is positioned at its start, not looping.  On every
 * failure *out is left untouched and nothing is held, so there is nothing to
 * close.  `lump` need only stay valid for as long as the song does.
 */
int  doom_midi_open(const void *lump, uint32_t lump_size, doom_midi_song_t *out);
void doom_midi_close(doom_midi_song_t *song);

/* Doom's PlaySong(handle, looping).  Takes effect at the next end of song. */
void doom_midi_set_looping(doom_midi_song_t *song, int looping);

/* Back to the start, tempo reset, clock zeroed. */
void doom_midi_rewind(doom_midi_song_t *song);

/*
 * Advance the song clock by `elapsed_ms` and write the channel events that
 * have come due, in order, to out[0..max).  Returns how many were written,
 * or a negative DOOM_MIDI_* code.  If `max` filled before the interval was
 * consumed, the rest are returned by the next call -- nothing is dropped.
 *
 * Tempo, end-of-track, sysex and the other meta events are consumed here and
 * never surfaced.  When looping, the song restarts at the end of its longest
 * track; otherwise doom_midi_finished() turns true.
 *
 * Event times are on one clock that does not reset at a loop, so they never go
 * backwards within or across calls.  A pass that `elapsed_ms` overtakes
 * entirely -- after a long stall, say -- is skipped rather than replayed: the
 * song lands where it would have been, without a burst of stale notes, and a
 * short loop costs the same as a long one.
 */
int  doom_midi_advance(doom_midi_song_t *song, uint32_t elapsed_ms,
                       doom_midi_event_t *out, uint32_t max);

int  doom_midi_finished(const doom_midi_song_t *song);

/* Human-readable form of a result code.  Never NULL. */
const char *doom_midi_strerror(int rc);
