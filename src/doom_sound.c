/*
 * doom_sound.c — Doom's sound_module_t over the PC speaker (SCRUM-101).
 * See doom_sound.h for the design.
 *
 * Same #ifdef EXO_KERNEL split as src/doomgeneric_exo.c:
 *   - NOT EXO_KERNEL (the ring-3 libos_doom target): the speaker is reached
 *     through the ordinary exo_sound_tone/exo_sound_stop stubs, and the
 *     sound_module_t/music_module_t that src/doom/i_sound.c links against
 *     are defined at the bottom of this file.
 *   - EXO_KERNEL (built into the kernel, driven from ring 0 by
 *     tests/kernel/test_doom_sound_k.c): the same syscalls through
 *     exo_syscall_dispatch() -- a `syscall` instruction from ring 0 would
 *     not come back -- so the test exercises the real handlers and the real
 *     speaker, not a mock.
 */

#include "doom_sound.h"
#include "doom_sfx_tone.h"
#include "exo_syscall.h"

#ifdef EXO_KERNEL
#include "syscall.h"
#endif

#include <stddef.h>
#include <stdint.h>

static int64_t hw_tone(uint32_t freq_hz, uint32_t dur_ms)
{
#ifdef EXO_KERNEL
    return exo_syscall_dispatch(EXO_SYS_SOUND_TONE, freq_hz, dur_ms,
                                0, 0, 0, 0);
#else
    return exo_sound_tone(freq_hz, dur_ms);
#endif
}

static void hw_stop(void)
{
#ifdef EXO_KERNEL
    (void)exo_syscall_dispatch(EXO_SYS_SOUND_STOP, 0, 0, 0, 0, 0, 0);
#else
    (void)exo_sound_stop();
#endif
}

/* The one voice. `tone == NULL` means idle. */
static const doom_sfx_tone_t *tone;
static int      cur_sfx;
static int      cur_channel;
static int      cur_priority;
static uint8_t  cur_step;
static uint32_t step_end_ms;

static void go_idle(void)
{
    tone = NULL;
    cur_sfx = 0;
    cur_channel = -1;
}

/* Start step `i` of the current sequence at `now_ms`, or go idle if there is
 * no such step or the kernel refused it (a refused step would otherwise
 * leave the voice held by a sound nobody can hear). */
static void play_step(uint8_t i, uint32_t now_ms)
{
    if (i >= tone->n_steps) {
        go_idle();
        return;
    }

    const doom_sfx_step_t *s = &tone->step[i];
    if (hw_tone(s->freq_hz, s->dur_ms) != 0) {
        go_idle();
        return;
    }

    cur_step = i;
    step_end_ms = now_ms + s->dur_ms;
}

void doom_sound_reset(void)
{
    if (tone != NULL)
        hw_stop();
    go_idle();
}

void doom_sound_update(uint32_t now_ms)
{
    /* Catch up by as many steps as are due, so a long frame does not
     * replay steps whose time has already passed. */
    while (tone != NULL && (int32_t)(now_ms - step_end_ms) >= 0) {
        uint32_t due = step_end_ms;
        uint8_t next = (uint8_t)(cur_step + 1);

        if (next >= tone->n_steps) {
            go_idle();
            return;
        }
        /* If the next step would itself already be over, skip it rather
         * than start a tone that is late for its whole length. */
        if ((int32_t)(now_ms - (due + tone->step[next].dur_ms)) >= 0) {
            cur_step = next;
            step_end_ms = due + tone->step[next].dur_ms;
            continue;
        }
        play_step(next, now_ms);
        return;
    }
}

int doom_sound_start(int sfx_id, int channel, int priority, int vol,
                     uint32_t now_ms)
{
    const doom_sfx_tone_t *t = doom_sfx_tone(sfx_id);

    doom_sound_update(now_ms);

    if (t == NULL || vol <= 0)
        return channel;

    /* Doom's rule: lower priority number = more important. Equal takes
     * over too, so a rapid-fire weapon restarts its own sound. */
    if (tone != NULL && priority > cur_priority)
        return channel;

    tone = t;
    cur_sfx = sfx_id;
    cur_channel = channel;
    cur_priority = priority;
    play_step(0, now_ms);
    return channel;
}

void doom_sound_stop(int channel)
{
    if (tone != NULL && channel == cur_channel) {
        hw_stop();
        go_idle();
    }
}

int doom_sound_is_playing(int channel, uint32_t now_ms)
{
    doom_sound_update(now_ms);
    return tone != NULL && channel == cur_channel;
}

int doom_sound_current_sfx(void)
{
    return tone != NULL ? cur_sfx : 0;
}

int doom_sound_current_step(void)
{
    return tone != NULL ? cur_step : -1;
}

/* ── sound_module_t / music_module_t for src/doom/i_sound.c ───────────── */

#ifndef EXO_KERNEL

#include "doom/i_sound.h"
#include "doom/sounds.h"

uint32_t DG_GetTicksMs(void);

/* Referenced by i_sound.c's I_BindSoundVariables() once FEATURE_SOUND is
 * on (they belong to chocolate-doom's SDL mixer, which is not vendored).
 * Nothing reads them here; they exist so the config binding resolves. */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

/* Every device Doom's config can name. snd_sfxdevice defaults to
 * SNDDEVICE_SB (i_sound.c) and there is no config file to change it, so a
 * list of just SNDDEVICE_PCSPEAKER would never be selected. Whatever the
 * setting says, the PC speaker is the one output this machine has. */
static snddevice_t exo_sound_devices[] = {
    SNDDEVICE_PCSPEAKER, SNDDEVICE_ADLIB, SNDDEVICE_SB, SNDDEVICE_PAS,
    SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_GENMIDI, SNDDEVICE_AWE32,
};

static boolean exo_snd_init(boolean use_sfx_prefix)
{
    (void)use_sfx_prefix;
    doom_sound_reset();
    return true;
}

static void exo_snd_shutdown(void)
{
    doom_sound_reset();
}

/* No PCM is ever read, so there is no lump to find. Any non-negative value
 * stops s_sound.c asking again; W_GetNumForName would I_Error on a WAD
 * without the DS lump. */
static int exo_snd_get_lump(sfxinfo_t *sfx)
{
    (void)sfx;
    return 0;
}

static void exo_snd_update(void)
{
    doom_sound_update(DG_GetTicksMs());
}

static void exo_snd_update_params(int channel, int vol, int sep)
{
    (void)channel; (void)vol; (void)sep;   /* one mono voice, no volume */
}

static int exo_snd_start(sfxinfo_t *sfx, int channel, int vol, int sep)
{
    (void)sep;
    return doom_sound_start((int)(sfx - S_sfx), channel, sfx->priority, vol,
                            DG_GetTicksMs());
}

static void exo_snd_stop(int channel)
{
    doom_sound_stop(channel);
}

static boolean exo_snd_is_playing(int channel)
{
    return doom_sound_is_playing(channel, DG_GetTicksMs()) ? true : false;
}

sound_module_t DG_sound_module = {
    .sound_devices     = exo_sound_devices,
    .num_sound_devices = (int)(sizeof(exo_sound_devices) /
                               sizeof(exo_sound_devices[0])),
    .Init              = exo_snd_init,
    .Shutdown          = exo_snd_shutdown,
    .GetSfxLumpNum     = exo_snd_get_lump,
    .Update            = exo_snd_update,
    .UpdateSoundParams = exo_snd_update_params,
    .StartSound        = exo_snd_start,
    .StopSound         = exo_snd_stop,
    .SoundIsPlaying    = exo_snd_is_playing,
    .CacheSounds       = NULL,   /* nothing to cache */
};

/* Music: the one voice belongs to sound effects. FEATURE_SOUND makes
 * i_sound.c install this module unconditionally and call every pointer
 * except Poll without a NULL check, so each is a real no-op. */
static boolean mus_init(void) { return true; }
static void    mus_shutdown(void) { }
static void    mus_set_volume(int volume) { (void)volume; }
static void    mus_pause(void) { }
static void    mus_resume(void) { }
static void   *mus_register(void *data, int len) { (void)data; (void)len; return NULL; }
static void    mus_unregister(void *handle) { (void)handle; }
static void    mus_play(void *handle, boolean looping) { (void)handle; (void)looping; }
static void    mus_stop(void) { }
static boolean mus_is_playing(void) { return false; }

music_module_t DG_music_module = {
    .sound_devices     = NULL,
    .num_sound_devices = 0,
    .Init              = mus_init,
    .Shutdown          = mus_shutdown,
    .SetMusicVolume    = mus_set_volume,
    .PauseMusic        = mus_pause,
    .ResumeMusic       = mus_resume,
    .RegisterSong      = mus_register,
    .UnRegisterSong    = mus_unregister,
    .PlaySong          = mus_play,
    .StopSong          = mus_stop,
    .MusicIsPlaying    = mus_is_playing,
    .Poll              = NULL,
};

#endif /* !EXO_KERNEL */
