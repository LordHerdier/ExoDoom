#pragma once

#include "page_alloc.h"   /* page_owner_t */

#include <stdint.h>

/*
 * syscall_sound.h — exo_sound_tone / exo_sound_stop handlers (SCRUM-100).
 *
 * Binds #17/#18 in front of the PC speaker driver (src/speaker.c, SCRUM-98),
 * the same split syscall_serial.c has against serial.c: the driver knows how
 * to beep, this file knows what a LibOS may ask for and answers in -EXO_E*
 * terms. It is also the *only* way a LibOS reaches the speaker -- ring 3 has
 * IOPL 0 and no I/O permission bitmap (src/tss.c), so a direct `outb` to
 * 0x42/0x43/0x61 takes #GP; tests/kernel/test_syscall_sound_k.c proves both.
 *
 * Call from kernel_main after syscall_init() and ahead of the TESTING
 * branch, like every other syscall *_init(); after speaker_init() too.
 */

/* Longest tone one exo_sound_tone call may request. Every tone the syscall
 * starts ends on its own (dur_ms == 0, the driver's "until stopped", is not
 * offered to ring 3), so a LibOS that dies mid-effect -- or never calls
 * exo_sound_stop -- cannot leave the speaker on. 10 s is far past any Doom
 * effect; the bound is there to make "ends on its own" mean something.
 * Exposed so the test asserts the real limit. */
#define SOUND_TONE_MAX_MS 10000u

/*
 * The most 8-bit samples one exo_sound_pcm call may queue (SCRUM-213).
 *
 * Unlike SOUND_TONE_MAX_MS above, this bound is about memory rather than
 * time: the samples are copied into kernel pages (see syscall_sound.c on why
 * they cannot be played in place from ring 3), so `num_samples` decides how
 * many pages one syscall takes, and PCM_MIXER_VOICES of them can be
 * outstanding at once.  192 KiB is 48 pages, 1.5 MiB across all eight voices.
 *
 * The floor under that number is a real sound effect, not a round figure: the
 * largest DS* lump in the pinned freedoom2 v0.13.0 is DSBOSSIT at 154,358
 * bytes, so a cap under ~151 KiB would reject a lump Doom genuinely plays and
 * would have to be raised again by whoever wires the sound module up.
 *
 * It also bounds the copy itself, which runs with interrupts off like every
 * syscall body (`syscall`'s FMASK clears IF) -- the same reasoning behind
 * SERIAL_WRITE_MAX_LEN and EXO_DISK_MAX_SECTORS.  192 KiB is a few hundred
 * microseconds of memcpy, under one PIT tick at 1000 Hz.
 *
 * Exposed so the test asserts the real limit rather than a restated literal.
 */
#define SOUND_PCM_MAX_SAMPLES 196608u

void syscall_sound_init(void);

/* Silence the speaker if `owner` started the tone now sounding, stop every
 * mixer voice `owner` started, and return the kernel pages those voices were
 * playing out of; otherwise do nothing. Called from exo_exit
 * (src/syscall_exit.c) so an exiting LibOS does not leave its last effect
 * playing into whoever runs next -- and, since SCRUM-213, so it does not leak
 * its staging pages either. */
void syscall_sound_release(page_owner_t owner);

/* How many mixer voices `owner` currently holds a staging buffer for, and how
 * many kernel pages those buffers occupy. Both are for
 * tests/kernel/test_syscall_sound_pcm_k.c, which has no other way to see that
 * a finished voice's pages really were reaped rather than leaked. */
uint32_t syscall_sound_pcm_slots_used(void);
uint32_t syscall_sound_pcm_pages_used(void);
