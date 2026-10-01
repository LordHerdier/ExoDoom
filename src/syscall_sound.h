#pragma once

#include "page_alloc.h"   /* page_owner_t */

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

void syscall_sound_init(void);

/* Silence the speaker if `owner` started the tone now sounding; otherwise do
 * nothing. Called from exo_exit (src/syscall_exit.c) so an exiting LibOS
 * does not leave its last effect playing into whoever runs next. */
void syscall_sound_release(page_owner_t owner);
