/*
 * doom_sound_pcm.c — Doom's sound effects as their real PCM samples
 * (SCRUM-214).  See doom_sound_pcm.h for the design.
 *
 * ── The EXO_KERNEL split, and why this one is not symmetric ────────────
 *
 * src/doom_sound.c reaches the speaker through exo_syscall_dispatch() in a
 * kernel TU and through the inline `syscall` stub in the ring-3 one, and the
 * two are otherwise identical.  Here they cannot be, and the asymmetry is
 * forced rather than chosen:
 *
 *   - Ring 3 (the libos_doom target) calls exo_sound_pcm.  Its sample pointer
 *     is inside the mapped WAD window, which is what #27 requires.
 *   - Ring 0 (this file compiled into the kernel, driven by
 *     tests/kernel/test_doom_sound_pcm_k.c) cannot.  The mounted WAD is at an
 *     identity-mapped physical address far below EXO_USER_VA_BASE, so #27
 *     would refuse every call with -EXO_EFAULT -- correctly: a LibOS window
 *     check is exactly what it is for.  So the kernel side calls
 *     pcm_mixer_start() and its siblings, the layer immediately beneath #27,
 *     and skips only the window check and the staging copy that the check
 *     exists to make safe.  Skipping the copy is right on that side for the
 *     reason src/doom_dmx.h gives: the WAD is kernel-mapped and read-only for
 *     the life of the mount, so the mixer may read it in place.
 *
 * What that leaves un-proven from ring 0 is that #27 accepts the arguments
 * this file computes.  test_doom_sound_pcm_k.c closes that separately, by
 * staging a real lump's bytes into a page mapped in the LibOS window and
 * dispatching #27 with them.
 */

#include "doom_sound_pcm.h"
#include "doom_dmx.h"
#include "doom_wad.h"
#include "exo_syscall.h"

#ifdef EXO_KERNEL
#include "syscall.h"
#include "pcm_mixer.h"
#include "hda.h"
#endif

#include <stddef.h>
#include <stdint.h>

/*
 * One row per Doom channel.  Sized past the mixer's voice count on purpose:
 * the index is Doom's channel number, bounded by snd_channels (8 by default,
 * src/doom/s_sound.c, but a config variable, so a WAD's config could raise
 * it).  A row is 8 bytes and a voice is the scarce thing; an out-of-range
 * channel still has to be rejected rather than wrapped, since two channels
 * sharing a row would let one stop the other's sound.
 */
#define PCM_CHANNELS 16

typedef struct {
    int      voice;    /* mixer/syscall handle, or -1 when the row is free */
    uint32_t end_ms;   /* when the sound reaches its nominal end           */
} channel_t;

static channel_t g_chan[PCM_CHANNELS];
static int       g_init;      /* 0 until the rows have been cleared once   */
static int       g_no_device; /* latched by a -EXO_ENODEV from the kernel  */

/* ── The one layer that differs between ring 0 and ring 3 ──────────────── */

static int64_t hw_pcm(const uint8_t *samples, uint32_t num_samples,
                      uint32_t rate_hz, int vol, int sep, int priority)
{
#ifdef EXO_KERNEL
    if (!hda_present())
        return -EXO_ENODEV;

    /* The stream is started lazily here for the same reason sys_sound_pcm()
     * does it: a stream running with every voice silent costs ~47 interrupts
     * a second forever (docs/drivers/hda.md §12). */
    if (!hda_pcm_is_streaming() && hda_pcm_start() != HDA_OK)
        return -EXO_ENODEV;

    doom_dmx_t pcm = {
        .format      = DOOM_DMX_FORMAT_PCM,
        .rate_hz     = rate_hz,
        .samples     = samples,
        .num_samples = num_samples,
    };

    int handle = pcm_mixer_start(&pcm, vol, sep, priority);
    if (handle == PCM_MIXER_ENOVOICE)
        return -EXO_EBUSY;
    if (handle < 0)
        return -EXO_EINVAL;
    return handle;
#else
    return exo_sound_pcm(samples, num_samples, rate_hz, vol, sep, priority);
#endif
}

static void hw_pcm_stop(int handle)
{
#ifdef EXO_KERNEL
    pcm_mixer_stop(handle);
#else
    (void)exo_sound_pcm_stop(handle);
#endif
}

static void hw_pcm_params(int handle, int vol, int sep)
{
#ifdef EXO_KERNEL
    (void)pcm_mixer_set_params(handle, vol, sep);
#else
    (void)exo_sound_pcm_params(handle, vol, sep);
#endif
}

/* ── Channel bookkeeping ───────────────────────────────────────────────── */

static void clear_rows(void)
{
    for (int i = 0; i < PCM_CHANNELS; i++) {
        g_chan[i].voice  = -1;
        g_chan[i].end_ms = 0;
    }
    g_init = 1;
}

static channel_t *row_for(int channel)
{
    if (!g_init)
        clear_rows();
    if (channel < 0 || channel >= PCM_CHANNELS)
        return NULL;
    return &g_chan[channel];
}

/* Length in milliseconds, rounded up so a sound is never reported finished
 * while its last samples are still being rendered.  64-bit because
 * num_samples * 1000 overflows a uint32_t above ~4.3 million samples, which a
 * caller-supplied count could reach even though no DMX lump does. */
static uint32_t duration_ms(uint32_t num_samples, uint32_t rate_hz)
{
    if (rate_hz == 0)
        return 0;
    return (uint32_t)(((uint64_t)num_samples * 1000u + rate_hz - 1) / rate_hz);
}

/* ── Public API ────────────────────────────────────────────────────────── */

void doom_sound_pcm_reset(void)
{
    if (!g_init)
        clear_rows();

    for (int i = 0; i < PCM_CHANNELS; i++) {
        if (g_chan[i].voice >= 0)
            hw_pcm_stop(g_chan[i].voice);
    }

    clear_rows();
    g_no_device = 0;
}

int doom_sound_pcm_available(void)
{
    return !g_no_device;
}

int doom_sound_pcm_start(const char *sfx_name, int channel, int priority,
                         int vol, int sep, uint32_t now_ms)
{
    channel_t *row = row_for(channel);

    if (row == NULL || g_no_device)
        return 0;

    const doom_wad_t *w = doom_wad_mounted();
    if (w == NULL)
        return 0;

    doom_dmx_t dmx;
    if (doom_dmx_find_sfx(&w->wad, sfx_name, &dmx) != DOOM_DMX_OK)
        return 0;

    /* Before the new voice, not after: Doom reuses a channel without always
     * calling I_StopSound first, and a row overwritten with the new handle
     * would leave the old voice sounding with nobody able to name it. */
    if (row->voice >= 0) {
        hw_pcm_stop(row->voice);
        row->voice = -1;
    }

    int64_t rc = hw_pcm(dmx.samples, dmx.num_samples, dmx.rate_hz, vol, sep,
                        priority);

    if (rc >= 0) {
        row->voice  = (int)rc;
        row->end_ms = now_ms + duration_ms(dmx.num_samples, dmx.rate_hz);
        return 1;
    }

    if (rc == -EXO_ENODEV) {
        /* No controller on this machine.  Latched rather than retried per
         * effect: the answer cannot change while this LibOS runs, and Doom
         * asks several times a second. */
        g_no_device = 1;
        return 0;
    }

    if (rc == -EXO_EBUSY) {
        /* Refused as the least important sound in the room, with nothing
         * playing disturbed.  Handled, not failed -- see the header. */
        return 1;
    }

    /* Anything else (-EXO_EINVAL for a lump the mixer cannot play, -EXO_ENOMEM
     * for staging pages) is about this one effect, so this one effect falls
     * back to a tone. */
    return 0;
}

void doom_sound_pcm_stop(int channel)
{
    channel_t *row = row_for(channel);

    if (row == NULL || row->voice < 0)
        return;

    hw_pcm_stop(row->voice);
    row->voice  = -1;
    row->end_ms = 0;
}

int doom_sound_pcm_is_playing(int channel, uint32_t now_ms)
{
    channel_t *row = row_for(channel);

    if (row == NULL || row->voice < 0)
        return 0;

    /* Wrap-safe: the difference is compared as a signed quantity, so this
     * keeps working across the 32-bit millisecond rollover ~49 days in.  Same
     * idiom as doom_sound.c's step deadlines. */
    if ((int32_t)(now_ms - row->end_ms) >= 0) {
        row->voice  = -1;
        row->end_ms = 0;
        return 0;
    }

    return 1;
}

void doom_sound_pcm_params(int channel, int vol, int sep)
{
    channel_t *row = row_for(channel);

    if (row == NULL || row->voice < 0)
        return;

    hw_pcm_params(row->voice, vol, sep);
}

int doom_sound_pcm_owns(int channel)
{
    channel_t *row = row_for(channel);

    return (row != NULL && row->voice >= 0) ? 1 : 0;
}
