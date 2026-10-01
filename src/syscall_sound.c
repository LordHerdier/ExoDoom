#include "syscall_sound.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "speaker.h"

#include <stdint.h>

/*
 * syscall_sound.c — the sound syscalls (SCRUM-100).
 *
 * The speaker is one voice, so there is no binding table in the style of
 * fb_binding.c/disk_binding.c: acquiring it up front would only let one
 * LibOS lock every other one out of making a sound at all. The policy is
 * last-tone-wins, and the only ownership kept is who started the tone that
 * is sounding now -- which is what decides whether exo_sound_stop may cut it
 * and whether exo_exit should.
 *
 * No locking: syscalls run with IF cleared by IA32_FMASK, and the one piece
 * of state IRQ0 also touches (the driver's deadline) is the driver's to
 * guard.
 */

static page_owner_t tone_holder = PAGE_OWNER_FREE;

/* #17 — start a tone at `freq` Hz for `dur_ms` ms and return at once.
 *   0              tone started (replacing any tone already sounding)
 *   -EXO_EINVAL    freq outside [SPEAKER_MIN_HZ, SPEAKER_MAX_HZ], or dur_ms
 *                  outside [1, SOUND_TONE_MAX_MS]; the speaker is untouched
 * Backs the Doom sound module's StartSound (docs/syscall_spec.md §3.2 #17). */
static int64_t sys_sound_tone(uint64_t freq, uint64_t dur_ms, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;

    /* Range-check the full 64-bit values before narrowing, or a freq of
     * 2^32 + 440 would truncate to a perfectly valid 440. */
    if (freq < SPEAKER_MIN_HZ || freq > SPEAKER_MAX_HZ)
        return -EXO_EINVAL;
    if (dur_ms == 0 || dur_ms > SOUND_TONE_MAX_MS)
        return -EXO_EINVAL;

    if (speaker_tone((uint32_t)freq, (uint32_t)dur_ms) != SPEAKER_OK)
        return -EXO_EINVAL;

    tone_holder = syscall_current_context();
    return 0;
}

/* #18 — silence the speaker now.
 *   0              silenced, or nothing was sounding
 *   -EXO_EPERM     a different context's tone is sounding; left alone
 * Backs the Doom sound module's StopSound (docs/syscall_spec.md §3.2 #18). */
static int64_t sys_sound_stop(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!speaker_is_playing())
        return 0;
    if (tone_holder != syscall_current_context())
        return -EXO_EPERM;

    speaker_stop();
    return 0;
}

void syscall_sound_release(page_owner_t owner)
{
    if (speaker_is_playing() && tone_holder == owner)
        speaker_stop();
}

void syscall_sound_init(void)
{
    tone_holder = PAGE_OWNER_FREE;
    exo_syscall_register(EXO_SYS_SOUND_TONE, sys_sound_tone);
    exo_syscall_register(EXO_SYS_SOUND_STOP, sys_sound_stop);
}
