/*
 * pcm_mixer.c — software PCM mixer: N voices summed into one stereo stream
 * (SCRUM-212).
 *
 * See src/pcm_mixer.h for the contract and the reasoning behind it.  What
 * follows is the arithmetic, which is the whole of the file.
 */

#include "pcm_mixer.h"
#include "string.h"

#include <stddef.h>
#include <stdint.h>

/* ── Voice state ───────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *samples;     /* into the mounted WAD; never owned here   */
    uint32_t       num_samples;
    uint32_t       step;        /* 16.16 phase increment per output frame   */
    uint32_t       phase;       /* 16.16 index into samples[]               */
    int32_t        gain_l;      /* 0..256, Q8: 256 == doom_dmx_to_s16 scale */
    int32_t        gain_r;
    int            priority;    /* Doom's rule: LOWER number = MORE important */
    uint32_t       generation;  /* bumped on every (re)start of this slot   */
    uint8_t        active;
} voice_t;

static voice_t g_voice[PCM_MIXER_VOICES];

/*
 * The music voice (SCRUM-218): one ring of interleaved stereo frames in
 * storage the caller owns.  See src/pcm_mixer.h for why it is not a ninth
 * voice_t.
 *
 * `rd` and `count` are the whole of the ring's state -- the write position
 * is derived, (rd + count) mod cap, rather than stored.  That is not thrift.
 * With a separate write index there are two fields a wrap has to keep
 * consistent with the count and three ways to get it wrong by one, which in
 * a ring is a tick once per lap; with one, "where does the next frame go"
 * and "how many are there" cannot disagree.
 */
typedef struct {
    int16_t  *ring;       /* NULL while no ring is attached                  */
    uint32_t  cap;        /* frames the ring holds                           */
    uint32_t  rd;         /* next frame pcm_mixer_render() will take         */
    uint32_t  count;      /* frames queued                                   */
    int32_t   gain;       /* Q8, 0..256: 256 passes samples through exactly  */
    uint32_t  underruns;  /* frames rendered as silence while playing        */
    uint8_t   playing;    /* written to since the last open or flush         */
} music_t;

#define MUSIC_GAIN_UNITY 256

static music_t g_music = { NULL, 0, 0, 0, MUSIC_GAIN_UNITY, 0, 0 };

/*
 * Handle packing.  The generation occupies bits 31:8 and the voice index
 * bits 7:0, so a handle is always non-negative for any index under 256 and
 * any generation under 2^23.  A generation that wrapped could alias a handle
 * held across 2^24 reuses of the same slot; at Doom's rate of a few sounds a
 * second that is years of continuous play, and the alternative (never
 * reusing a slot) is not available with a fixed voice table.
 */
#define HANDLE_INDEX_BITS 8
#define HANDLE_INDEX_MASK ((1u << HANDLE_INDEX_BITS) - 1u)
#define HANDLE_GEN_MASK   0x7FFFFFu

static inline int handle_make(uint32_t index, uint32_t generation)
{
    return (int)(((generation & HANDLE_GEN_MASK) << HANDLE_INDEX_BITS)
                 | (index & HANDLE_INDEX_MASK));
}

/*
 * Resolve a handle to its voice, or NULL.  The generation check is what
 * makes a *stale* handle -- one whose voice has since finished and been
 * handed to a different sound -- harmless rather than silently effective on
 * somebody else's effect.
 */
static voice_t *handle_voice(int handle)
{
    if (handle < 0) {
        return NULL;
    }
    uint32_t index = (uint32_t)handle & HANDLE_INDEX_MASK;
    if (index >= PCM_MIXER_VOICES) {
        return NULL;
    }
    voice_t *v = &g_voice[index];
    uint32_t gen = ((uint32_t)handle >> HANDLE_INDEX_BITS) & HANDLE_GEN_MASK;
    if (!v->active || (v->generation & HANDLE_GEN_MASK) != gen) {
        return NULL;
    }
    return v;
}

/* ── Interrupt discipline ──────────────────────────────────────────────── */

/*
 * pcm_mixer_render() runs inside hda_irq_handler(); every other entry point
 * runs in ordinary kernel context.  So the mutators have to shut interrupts
 * out for the few instructions in which a voice is half-written -- otherwise
 * a completion interrupt landing between "samples = ..." and "active = 1"
 * renders a voice with a length of zero and a live pointer, or worse the
 * reverse.  Same primitive, and the same reason, as src/hda.c's own
 * irq_save()/irq_restore().
 *
 * This is RFLAGS manipulation, not a device access, so it does not make this
 * file hardware-dependent: there is still nothing here for a test to have to
 * mock.
 */
static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    if (flags & (1u << 9)) {   /* IF was set on entry */
        __asm__ volatile ("sti" ::: "memory");
    }
}

/* ── Gain ──────────────────────────────────────────────────────────────── */

/*
 * Doom's own panning law, from the separation arithmetic in its sound back
 * end (chocolate-doom's i_sdlsound.c I_UpdateSoundParams, which the vendored
 * tree's i_sound.h describes the same way): the separation is squared, so
 * the volume falls off away from a channel rather than linearly, and a
 * centred sound sits at about three quarters of full on *both* channels
 * rather than half on each.  Reproduced rather than invented, so a sound
 * panned by a future caller in s_sound.c's shape is placed where Doom places
 * it.
 *
 * `vol` is 0..127 and `sep` 0..255.  The result is a Q8 multiplier, 0..256,
 * where 256 reproduces doom_dmx_to_s16()'s scaling exactly.  That identity
 * is what tests/kernel/test_pcm_mixer_k.c pins a full-volume HARD-PANNED
 * voice against; a centred one sits at about three quarters of it.
 */
static void gain_for(int vol, int sep, int32_t *out_l, int32_t *out_r)
{
    if (vol < 0) {
        vol = 0;
    } else if (vol > PCM_MIXER_VOL_MAX) {
        vol = PCM_MIXER_VOL_MAX;
    }
    if (sep < 0) {
        sep = 0;
    } else if (sep > PCM_MIXER_SEP_MAX) {
        sep = PCM_MIXER_SEP_MAX;
    }

    int32_t s = sep + 1;                              /* 1..256            */
    int32_t left  = vol - ((vol * s * s) >> 16);
    s -= 257;                                         /* -256..-1          */
    int32_t right = vol - ((vol * s * s) >> 16);

    if (left < 0) {
        left = 0;
    }
    if (right < 0) {
        right = 0;
    }

    /*
     * 0..127 -> 0..256.  Scaled by 256/127 and rounded, so vol == 127 with
     * a hard pan gives exactly 256 and reproduces the unscaled conversion;
     * the +63 is the round-to-nearest term for a divide by 127.
     */
    *out_l = (left  * 256 + 63) / PCM_MIXER_VOL_MAX;
    *out_r = (right * 256 + 63) / PCM_MIXER_VOL_MAX;
}

/* ── Public API ────────────────────────────────────────────────────────── */

void pcm_mixer_reset(void)
{
    uint64_t flags = irq_save();
    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        g_voice[i].samples     = NULL;
        g_voice[i].num_samples = 0;
        g_voice[i].step        = 0;
        g_voice[i].phase       = 0;
        g_voice[i].gain_l      = 0;
        g_voice[i].gain_r      = 0;
        g_voice[i].priority    = 0;
        g_voice[i].generation  = 0;
        g_voice[i].active      = 0;
    }

    /* "Forget every voice" includes the one that is not in the table: the
     * ring is detached (never freed -- it is the caller's) and the level
     * goes back to full, so nothing a previous user of the mixer set up can
     * colour the next one's output. */
    g_music.ring      = NULL;
    g_music.cap       = 0;
    g_music.rd        = 0;
    g_music.count     = 0;
    g_music.gain      = MUSIC_GAIN_UNITY;
    g_music.underruns = 0;
    g_music.playing   = 0;
    irq_restore(flags);
}

void pcm_mixer_stop_all(void)
{
    uint64_t flags = irq_save();
    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        g_voice[i].active = 0;
    }
    irq_restore(flags);
}

int pcm_mixer_start(const doom_dmx_t *dmx, int vol, int sep, int priority)
{
    if (dmx == NULL || dmx->samples == NULL || dmx->num_samples == 0) {
        return PCM_MIXER_EINVAL;
    }

    /*
     * The phase increment, 16.16.  Computed in 64 bits because
     * rate_hz << 16 overflows a uint32_t above 65535 Hz, and a DMX lump is
     * free to claim any rate its header likes.
     */
    uint32_t step = (uint32_t)(((uint64_t)dmx->rate_hz << PCM_MIXER_PHASE_BITS)
                               / PCM_MIXER_RATE_HZ);
    if (step == 0) {
        /* A rate under ~0.73 Hz: the phase would never reach the second
         * sample, so the voice would hold one value forever rather than
         * play.  Rejected here instead of wedging a voice. */
        return PCM_MIXER_EINVAL;
    }

    int32_t gain_l, gain_r;
    gain_for(vol, sep, &gain_l, &gain_r);

    uint64_t flags = irq_save();

    /* A free voice first, lowest index, so a mostly-idle mixer uses the same
     * slots and a test can reason about which handle it got. */
    voice_t *chosen = NULL;
    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        if (!g_voice[i].active) {
            chosen = &g_voice[i];
            break;
        }
    }

    if (chosen == NULL) {
        /*
         * Everything is busy.  Find the least important voice -- the highest
         * priority number, tie-broken by whichever is furthest through its
         * sample, since that one has the least left to lose.
         */
        voice_t *worst = &g_voice[0];
        for (uint32_t i = 1; i < PCM_MIXER_VOICES; i++) {
            voice_t *v = &g_voice[i];
            if (v->priority > worst->priority
                || (v->priority == worst->priority && v->phase > worst->phase)) {
                worst = v;
            }
        }

        /*
         * Steal only if the newcomer is at least as important, Doom's own
         * rule (src/doom_sound.h, S_GetChannel()).  Refusing is the point:
         * a mixer that always stole would let a stream of incidental noises
         * chop up whatever the player is listening for, which is exactly the
         * starvation this ticket's acceptance is about.  Nothing playing is
         * touched on this path.
         */
        if (priority > worst->priority) {
            irq_restore(flags);
            return PCM_MIXER_ENOVOICE;
        }
        chosen = worst;
    }

    chosen->samples     = dmx->samples;
    chosen->num_samples = dmx->num_samples;
    chosen->step        = step;
    chosen->phase       = 0;
    chosen->gain_l      = gain_l;
    chosen->gain_r      = gain_r;
    chosen->priority    = priority;
    chosen->generation++;
    chosen->active      = 1;

    int handle = handle_make((uint32_t)(chosen - g_voice), chosen->generation);
    irq_restore(flags);
    return handle;
}

void pcm_mixer_stop(int handle)
{
    uint64_t flags = irq_save();
    voice_t *v = handle_voice(handle);
    if (v != NULL) {
        v->active = 0;
    }
    irq_restore(flags);
}

int pcm_mixer_set_params(int handle, int vol, int sep)
{
    /* Computed before the lock: gain_for() is pure arithmetic on the
     * arguments, and the critical section only has to cover the two stores
     * pcm_mixer_render() reads. */
    int32_t gain_l, gain_r;
    gain_for(vol, sep, &gain_l, &gain_r);

    uint64_t flags = irq_save();
    voice_t *v = handle_voice(handle);
    if (v == NULL) {
        irq_restore(flags);
        return PCM_MIXER_ENOVOICE;
    }

    v->gain_l = gain_l;
    v->gain_r = gain_r;
    irq_restore(flags);
    return PCM_MIXER_OK;
}

int pcm_mixer_is_playing(int handle)
{
    uint64_t flags = irq_save();
    int playing = (handle_voice(handle) != NULL);
    irq_restore(flags);
    return playing;
}

uint32_t pcm_mixer_active_voices(void)
{
    uint32_t n = 0;
    uint64_t flags = irq_save();
    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        if (g_voice[i].active) {
            n++;
        }
    }
    irq_restore(flags);
    return n;
}

/* Saturate rather than wrap.  Eight full-scale effects really do exceed the
 * range, and a wrap turns the loudest moment into a full-amplitude sign flip
 * -- a crack far louder than the sound that caused it. */
static inline int16_t clip16(int32_t v)
{
    if (v > 32767) {
        return (int16_t)32767;
    }
    if (v < -32768) {
        return (int16_t)(-32768);
    }
    return (int16_t)v;
}

/* ── The music voice ───────────────────────────────────────────────────── */

int pcm_mixer_music_open(int16_t *ring, uint32_t frames)
{
    if (ring == NULL || frames == 0) {
        return PCM_MIXER_EINVAL;
    }

    uint64_t flags = irq_save();
    if (g_music.ring != NULL) {
        irq_restore(flags);
        return PCM_MIXER_EBUSY;
    }

    /* `ring` last: it is what pcm_mixer_render() tests, so the voice only
     * becomes visible to the interrupt once everything it will read through
     * that pointer is in place.  Interrupts are off here anyway; the order
     * is so the code is right without having to check that they are. */
    g_music.cap       = frames;
    g_music.rd        = 0;
    g_music.count     = 0;
    g_music.underruns = 0;
    g_music.playing   = 0;
    g_music.ring      = ring;
    irq_restore(flags);
    return PCM_MIXER_OK;
}

void pcm_mixer_music_close(void)
{
    uint64_t flags = irq_save();
    g_music.ring    = NULL;
    g_music.cap     = 0;
    g_music.rd      = 0;
    g_music.count   = 0;
    g_music.playing = 0;
    irq_restore(flags);
}

int pcm_mixer_music_is_open(void)
{
    return g_music.ring != NULL;
}

uint32_t pcm_mixer_music_write(const int16_t *src, uint32_t frames)
{
    if (src == NULL || frames == 0) {
        return 0;
    }

    /*
     * Snapshot where the free region starts and how big it is, then copy
     * with the lock DROPPED and take it again only to publish.
     *
     * That is safe because of who else can run.  The producer side is never
     * re-entered (see the header), so the only thing that can interleave is
     * pcm_mixer_render(), and all it does to the ring is consume from the
     * front: `rd` advances and `count` falls by the same amount.  The write
     * position, rd + count, is unchanged by that, and the region past it is
     * one render never reads -- so the frames can be copied in at leisure.
     * Until `count` is raised they are not part of the stream, and the
     * raise is a single store under the lock.
     *
     * The alternative, holding interrupts off across the memcpy, would put a
     * 48 KB copy inside a critical section whose only purpose is to make two
     * words consistent.
     */
    uint64_t flags = irq_save();
    int16_t *ring  = g_music.ring;
    uint32_t cap   = g_music.cap;
    uint32_t space = cap - g_music.count;
    uint32_t wr    = g_music.rd + g_music.count;
    irq_restore(flags);

    if (ring == NULL) {
        return 0;
    }
    if (wr >= cap) {
        wr -= cap;
    }
    if (frames > space) {
        frames = space;
    }
    if (frames == 0) {
        return 0;
    }

    /* Up to two pieces: to the end of the storage, then from its start. */
    uint32_t first = cap - wr;
    if (first > frames) {
        first = frames;
    }
    memcpy(ring + (size_t)wr * PCM_MIXER_CHANNELS, src,
           (size_t)first * PCM_MIXER_CHANNELS * sizeof(int16_t));
    memcpy(ring, src + (size_t)first * PCM_MIXER_CHANNELS,
           (size_t)(frames - first) * PCM_MIXER_CHANNELS * sizeof(int16_t));

    flags = irq_save();
    g_music.count  += frames;
    g_music.playing = 1;
    irq_restore(flags);
    return frames;
}

void pcm_mixer_music_flush(void)
{
    uint64_t flags = irq_save();
    g_music.rd      = 0;
    g_music.count   = 0;
    g_music.playing = 0;
    irq_restore(flags);
}

void pcm_mixer_music_set_volume(int vol)
{
    if (vol < 0) {
        vol = 0;
    } else if (vol > PCM_MIXER_VOL_MAX) {
        vol = PCM_MIXER_VOL_MAX;
    }

    /* The same 0..127 -> Q8 mapping gain_for() ends with, without the
     * panning law: the music arrives already stereo, so there is nothing to
     * place.  Full volume is exactly 256, which is what makes it a bit-exact
     * pass-through.  One aligned store, so no lock: render sees the old
     * level or the new one, and either is a level somebody asked for. */
    g_music.gain = (vol * MUSIC_GAIN_UNITY + 63) / PCM_MIXER_VOL_MAX;
}

int pcm_mixer_music_volume(void)
{
    /* The inverse of the mapping above, rounded the same way, so a level
     * that was set reads back as itself across the whole range. */
    return (g_music.gain * PCM_MIXER_VOL_MAX + MUSIC_GAIN_UNITY / 2)
           / MUSIC_GAIN_UNITY;
}

uint32_t pcm_mixer_music_fill(void)
{
    uint64_t flags = irq_save();
    uint32_t n = g_music.count;
    irq_restore(flags);
    return n;
}

uint32_t pcm_mixer_music_space(void)
{
    uint64_t flags = irq_save();
    uint32_t n = g_music.cap - g_music.count;
    irq_restore(flags);
    return n;
}

uint32_t pcm_mixer_music_underruns(void)
{
    uint64_t flags = irq_save();
    uint32_t n = g_music.underruns;
    irq_restore(flags);
    return n;
}

void pcm_mixer_render(int16_t *dst, uint32_t frames)
{
    if (dst == NULL) {
        return;
    }

    for (uint32_t f = 0; f < frames; f++) {
        int32_t acc_l = 0;
        int32_t acc_r = 0;

        for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
            voice_t *v = &g_voice[i];
            if (!v->active) {
                continue;
            }

            uint32_t idx = v->phase >> PCM_MIXER_PHASE_BITS;
            if (idx >= v->num_samples) {
                v->active = 0;
                continue;
            }

            /*
             * Linear interpolation between samples idx and idx+1, in the
             * signed domain: (u8 - 128) has to be signed *before* it is
             * scaled or the low half of the range wraps -- the same trap
             * doom_dmx_to_s16() notes.
             *
             * The last sample interpolates against itself rather than
             * reading samples[num_samples].  That byte is inside the mapped
             * IWAD, so an overread would not fault; it would quietly mix one
             * sample of the next lump in the directory, which is worse than
             * a fault because nothing would ever report it.
             */
            int32_t a = (int32_t)v->samples[idx] - 128;
            int32_t b = (idx + 1 < v->num_samples)
                        ? (int32_t)v->samples[idx + 1] - 128
                        : a;
            int32_t frac = (int32_t)(v->phase & (PCM_MIXER_PHASE_ONE - 1u));

            /*
             * Scale to doom_dmx_to_s16()'s domain, (s - 128) << 8, keeping 8
             * bits of the interpolation: (b - a) * frac >> 8 is
             * (b - a) * frac / 65536 << 8.  Range -32768..32512.
             */
            int32_t s = (a << 8) + (((b - a) * frac) >> 8);

            /* Q8 gain, so a full-volume hard-panned voice reproduces the
             * unscaled conversion exactly.  s * 256 >> 8 cannot overflow an
             * int32_t (peak 32768 * 256 = 2^23). */
            acc_l += (s * v->gain_l) >> 8;
            acc_r += (s * v->gain_r) >> 8;

            v->phase += v->step;
        }

        /*
         * The music voice, into the same accumulators and so under the same
         * single clip below.  One frame in, one frame out -- it is already
         * at PCM_MIXER_RATE_HZ, so there is no phase to step.
         *
         * `rd` wraps by comparison rather than by `% cap`: the ring is
         * whatever size it was opened with, not a power of two, and this is
         * once per frame inside an interrupt handler.
         *
         * An empty ring contributes nothing and the frame is counted, but
         * only while the voice is playing -- see the header on why an idle
         * one is not an underrun.  There is nothing else to do about it
         * here: this runs with interrupts off, and the producer cannot run
         * until it returns.
         */
        if (g_music.ring != NULL) {
            if (g_music.count != 0) {
                const int16_t *m = g_music.ring
                                   + (size_t)g_music.rd * PCM_MIXER_CHANNELS;

                acc_l += ((int32_t)m[0] * g_music.gain) >> 8;
                acc_r += ((int32_t)m[1] * g_music.gain) >> 8;

                if (++g_music.rd == g_music.cap) {
                    g_music.rd = 0;
                }
                g_music.count--;
            } else if (g_music.playing) {
                g_music.underruns++;
            }
        }

        dst[f * PCM_MIXER_CHANNELS]     = clip16(acc_l);
        dst[f * PCM_MIXER_CHANNELS + 1] = clip16(acc_r);
    }
}
