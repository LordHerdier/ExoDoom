/*
 * pcm_mixer.c — software PCM mixer: N voices summed into one stereo stream
 * (SCRUM-212).
 *
 * See src/pcm_mixer.h for the contract and the reasoning behind it.  What
 * follows is the arithmetic, which is the whole of the file.
 */

#include "pcm_mixer.h"

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

        dst[f * PCM_MIXER_CHANNELS]     = clip16(acc_l);
        dst[f * PCM_MIXER_CHANNELS + 1] = clip16(acc_r);
    }
}
