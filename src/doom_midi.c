/*
 * doom_midi.c — the MUS/MIDI song reader and sequencer (SCRUM-215).
 *
 * See src/doom_midi.h for the two input formats, why MUS conversion arrives
 * through a hook, and why every read is bounds-checked against the lump.
 */

#include "doom_midi.h"

#include <stddef.h>
#include <stdint.h>

static doom_midi_mus_convert_fn g_convert;
static doom_midi_mus_release_fn g_release;

void doom_midi_set_mus_converter(doom_midi_mus_convert_fn convert,
                                 doom_midi_mus_release_fn release)
{
    g_convert = convert;
    g_release = release;
}

/* SMF is big-endian; byte-wise reads for the alignment reason doom_dmx.c
 * gives (a lump starts at an arbitrary byte offset in the WAD). */
static uint16_t rd_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* ---- per-track event decoding ------------------------------------------ */

/*
 * Read a variable-length quantity at t->cur, never past t->end.  SMF caps it
 * at four bytes (28 bits); a fifth continuation byte is malformed, not long.
 */
static int track_read_varlen(doom_midi_track_t *t, uint32_t *value)
{
    uint32_t v = 0;

    for (int i = 0; i < 4; i++) {
        if (t->cur >= t->end) {
            return DOOM_MIDI_ETRUNC;
        }
        uint8_t b = *t->cur++;
        v = (v << 7) | (uint32_t)(b & 0x7Fu);
        if ((b & 0x80u) == 0) {
            *value = v;
            return DOOM_MIDI_OK;
        }
    }
    return DOOM_MIDI_EFORMAT;
}

/* Consume the delta time ahead of the next event.  A track that runs out
 * here never wrote its end-of-track meta. */
static int track_read_delta(doom_midi_track_t *t)
{
    uint32_t delta;
    int rc = track_read_varlen(t, &delta);
    if (rc != DOOM_MIDI_OK) {
        return rc;
    }
    if (t->next_tick + delta < t->next_tick) {
        return DOOM_MIDI_EFORMAT;
    }
    t->next_tick += delta;
    return DOOM_MIDI_OK;
}

static int track_start(doom_midi_track_t *t)
{
    t->cur       = t->start;
    t->next_tick = 0;
    t->running   = 0;
    t->done      = 0;
    return track_read_delta(t);
}

typedef enum { EV_CHANNEL, EV_TEMPO, EV_EOT, EV_SKIP } ev_kind_t;

typedef struct {
    ev_kind_t kind;
    uint8_t   status, data1, data2;
    uint32_t  tempo_us;
} step_t;

/*
 * Decode the event at t->cur and, unless it is end-of-track, the delta that
 * follows it.  Shared by the open-time validation pass and by advance(), so
 * what was validated is exactly what is later executed.
 */
static int track_step(doom_midi_track_t *t, step_t *r)
{
    if (t->cur >= t->end) {
        return DOOM_MIDI_ETRUNC;
    }

    uint8_t status = *t->cur;

    if (status >= 0x80u) {
        t->cur++;
    } else if (t->running != 0) {
        status = t->running;    /* running status: this byte is data1 */
    } else {
        return DOOM_MIDI_EFORMAT;
    }

    r->kind = EV_SKIP;

    if (status < 0xF0u) {
        t->running = status;

        uint32_t need = ((status & 0xF0u) == 0xC0u || (status & 0xF0u) == 0xD0u) ? 1u : 2u;
        if ((uint32_t)(t->end - t->cur) < need) {
            return DOOM_MIDI_ETRUNC;
        }
        uint8_t d1 = t->cur[0];
        uint8_t d2 = need == 2u ? t->cur[1] : 0;
        if ((d1 & 0x80u) || (d2 & 0x80u)) {
            return DOOM_MIDI_EFORMAT;
        }
        t->cur += need;

        r->kind   = EV_CHANNEL;
        r->status = status;
        r->data1  = d1;
        r->data2  = d2;
    } else if (status == 0xF0u || status == 0xF7u) {
        t->running = 0;

        uint32_t len;
        int rc = track_read_varlen(t, &len);
        if (rc != DOOM_MIDI_OK) {
            return rc;
        }
        if (len > (uint32_t)(t->end - t->cur)) {
            return DOOM_MIDI_ETRUNC;
        }
        t->cur += len;
    } else if (status == 0xFFu) {
        /* Running status is left alone.  The SMF spec says a meta cancels
         * it, but files that carry on with running status after a track name
         * or marker are common, and players accept them; rejecting the whole
         * song over it would silence a PWAD's music for no gain.  A sysex
         * still cancels it, as above. */
        if (t->cur >= t->end) {
            return DOOM_MIDI_ETRUNC;
        }
        uint8_t type = *t->cur++;

        uint32_t len;
        int rc = track_read_varlen(t, &len);
        if (rc != DOOM_MIDI_OK) {
            return rc;
        }
        if (len > (uint32_t)(t->end - t->cur)) {
            return DOOM_MIDI_ETRUNC;
        }

        if (type == 0x2Fu) {
            if (len != 0) {
                return DOOM_MIDI_EFORMAT;
            }
            t->done = 1;
            r->kind = EV_EOT;
            return DOOM_MIDI_OK;        /* nothing follows an end-of-track */
        }
        if (type == 0x51u) {
            if (len != 3) {
                return DOOM_MIDI_EFORMAT;
            }
            r->kind     = EV_TEMPO;
            r->tempo_us = ((uint32_t)t->cur[0] << 16) | ((uint32_t)t->cur[1] << 8) |
                          (uint32_t)t->cur[2];
            if (r->tempo_us == 0) {
                return DOOM_MIDI_EFORMAT;   /* would divide time by zero */
            }
        }
        t->cur += len;
    } else {
        return DOOM_MIDI_EFORMAT;   /* F1-F6, F8-FE: not valid in a file */
    }

    return track_read_delta(t);
}

/* ---- open / close ------------------------------------------------------ */

static int open_smf(const uint8_t *d, uint32_t size, doom_midi_song_t *s)
{
    if (size < 14u) {
        return DOOM_MIDI_ETRUNC;
    }
    if (d[0] != 'M' || d[1] != 'T' || d[2] != 'h' || d[3] != 'd') {
        return DOOM_MIDI_EFORMAT;
    }

    uint32_t hlen = rd_be32(d + 4);
    if (hlen < 6u || hlen > size - 8u) {
        return DOOM_MIDI_EFORMAT;
    }

    uint16_t format = rd_be16(d + 8);
    uint16_t ntrks  = rd_be16(d + 10);
    uint16_t div    = rd_be16(d + 12);

    if (format > 1u || ntrks == 0 || div == 0 || (div & 0x8000u)) {
        return DOOM_MIDI_EFORMAT;   /* format 2, or SMPTE timing */
    }
    if (format == 0u && ntrks != 1u) {
        return DOOM_MIDI_EFORMAT;   /* format 0 is one track by definition */
    }
    if (ntrks > DOOM_MIDI_MAX_TRACKS) {
        return DOOM_MIDI_ETOOMANY;
    }

    /* `s` is doom_midi_open()'s scratch song, not the caller's: filling it
     * in place and failing halfway leaves nothing visible. */
    s->ntracks  = 0;
    s->division = div;

    uint32_t pos = 8u + hlen;
    while (s->ntracks < ntrks) {
        if (size - pos < 8u) {
            return DOOM_MIDI_ETRUNC;
        }
        uint32_t len = rd_be32(d + pos + 4);
        if (len > size - pos - 8u) {
            return DOOM_MIDI_ETRUNC;
        }
        if (d[pos] == 'M' && d[pos + 1] == 'T' && d[pos + 2] == 'r' && d[pos + 3] == 'k') {
            doom_midi_track_t *t = &s->tracks[s->ntracks++];
            t->start = d + pos + 8u;
            t->end   = t->start + len;
        }
        pos += 8u + len;
    }

    /* Validate every track end to end on a scratch copy, so that a song which
     * opens can be advanced without meeting a malformed byte later. */
    for (uint32_t i = 0; i < s->ntracks; i++) {
        doom_midi_track_t t = s->tracks[i];
        int rc = track_start(&t);
        step_t r;
        while (rc == DOOM_MIDI_OK && !t.done) {
            rc = track_step(&t, &r);
        }
        if (rc != DOOM_MIDI_OK) {
            return rc;
        }
    }

    s->looping = 0;
    s->release = NULL;
    s->handle  = NULL;
    doom_midi_rewind(s);
    return DOOM_MIDI_OK;
}

int doom_midi_open(const void *lump, uint32_t lump_size, doom_midi_song_t *out)
{
    const uint8_t *d = lump;
    doom_midi_song_t s;     /* copied to *out only on success */
    int rc;

    if (d == NULL || out == NULL || lump_size < 4u) {
        return DOOM_MIDI_EINVAL;
    }

    if (d[0] == 'M' && d[1] == 'U' && d[2] == 'S' && d[3] == 0x1Au) {
        if (g_convert == NULL || g_release == NULL) {
            return DOOM_MIDI_ENOCONV;
        }

        const uint8_t *midi = NULL;
        uint32_t       midi_size = 0;
        void          *handle = NULL;

        if (g_convert(lump, lump_size, &midi, &midi_size, &handle) != 0) {
            return DOOM_MIDI_ECONV;
        }

        rc = open_smf(midi, midi_size, &s);
        if (rc != DOOM_MIDI_OK) {
            g_release(handle);
            return rc;
        }
        s.release = g_release;
        s.handle  = handle;
    } else {
        rc = open_smf(d, lump_size, &s);
        if (rc != DOOM_MIDI_OK) {
            return rc;
        }
    }

    *out = s;
    return DOOM_MIDI_OK;
}

void doom_midi_close(doom_midi_song_t *song)
{
    if (song == NULL) {
        return;
    }
    if (song->release != NULL) {
        song->release(song->handle);
    }
    song->release = NULL;
    song->handle  = NULL;
    song->ntracks = 0;
}

/* ---- sequencer --------------------------------------------------------- */

/* Back to tick 0 without touching the caller-visible clock. */
static void restart_pass(doom_midi_song_t *s)
{
    for (uint32_t i = 0; i < s->ntracks; i++) {
        (void)track_start(&s->tracks[i]);   /* validated at open */
    }
    s->tempo_us  = DOOM_MIDI_DEFAULT_TEMPO_US;
    s->frac      = 0;
    s->last_tick = 0;
    s->us        = 0;
}

void doom_midi_rewind(doom_midi_song_t *s)
{
    if (s == NULL) {
        return;
    }
    restart_pass(s);
    s->pass_us  = 0;
    s->clock_us = 0;
    s->finished = 0;
    s->loops    = 0;
}

void doom_midi_set_looping(doom_midi_song_t *s, int looping)
{
    if (s != NULL) {
        s->looping = looping != 0;
    }
}

int doom_midi_finished(const doom_midi_song_t *s)
{
    return s == NULL || s->finished;
}

/* The unfinished track whose next event comes first; ties go to the lower
 * index, which keeps a format-1 file's conductor track ahead of the notes it
 * shares a tick with.  -1 once every track has ended. */
static int pick_track(const doom_midi_song_t *s)
{
    int best = -1;

    for (uint32_t i = 0; i < s->ntracks; i++) {
        const doom_midi_track_t *t = &s->tracks[i];
        if (t->done) {
            continue;
        }
        if (best < 0 || t->next_tick < s->tracks[best].next_tick) {
            best = (int)i;
        }
    }
    return best;
}

int doom_midi_advance(doom_midi_song_t *s, uint32_t elapsed_ms,
                      doom_midi_event_t *out, uint32_t max)
{
    if (s == NULL || (out == NULL && max != 0)) {
        return DOOM_MIDI_EINVAL;
    }

    s->clock_us += (uint64_t)elapsed_ms * 1000u;

    uint32_t n = 0;

    while (n < max && !s->finished) {
        int ti = pick_track(s);

        if (ti < 0) {
            /* Every track has played its end-of-track.  A song whose whole
             * length is zero would loop forever inside this call, so it ends
             * instead -- it has nothing to repeat. */
            uint64_t len = s->us;
            if (!s->looping || len == 0) {
                s->finished = 1;
                break;
            }
            s->pass_us += len;
            s->loops++;

            /* Every pass is identical (restart_pass() resets the tempo), so
             * the ones the clock has already run past can be stepped over in
             * one go instead of replayed event by event. */
            uint64_t skip = (s->clock_us - s->pass_us) / len;
            s->pass_us += skip * len;
            s->loops   += (uint32_t)skip;

            restart_pass(s);
            continue;
        }

        doom_midi_track_t *t = &s->tracks[ti];

        /* When would this event fire, at the tempo in force right now?  The
         * remainder is kept in 1/division us so no time is lost to rounding. */
        uint64_t acc = (uint64_t)(t->next_tick - s->last_tick) * s->tempo_us + s->frac;
        uint64_t at  = s->us + acc / s->division;

        if (s->pass_us + at > s->clock_us) {
            break;
        }

        s->us        = at;
        s->frac      = (uint32_t)(acc % s->division);
        s->last_tick = t->next_tick;

        step_t r;
        int rc = track_step(t, &r);
        if (rc != DOOM_MIDI_OK) {
            return rc;
        }

        if (r.kind == EV_CHANNEL) {
            out[n].time_us = s->pass_us + s->us;
            out[n].status  = r.status;
            out[n].data1   = r.data1;
            out[n].data2   = r.data2;
            n++;
        } else if (r.kind == EV_TEMPO) {
            s->tempo_us = r.tempo_us;
        }
    }

    return (int)n;
}

const char *doom_midi_strerror(int rc)
{
    switch (rc) {
    case DOOM_MIDI_OK:
        return "ok";
    case DOOM_MIDI_EINVAL:
        return "bad argument, or lump too short to identify";
    case DOOM_MIDI_EFORMAT:
        return "not a usable MIDI file (bad header, event, or SMPTE timing)";
    case DOOM_MIDI_ETRUNC:
        return "chunk, event or track runs past its end";
    case DOOM_MIDI_ETOOMANY:
        return "more tracks than the reader supports";
    case DOOM_MIDI_ENOCONV:
        return "MUS lump but no MUS converter installed";
    case DOOM_MIDI_ECONV:
        return "MUS converter rejected the lump";
    default:
        return "unknown error";
    }
}
