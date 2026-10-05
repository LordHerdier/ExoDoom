/*
 * sound_pcm_probe.s — ring-3 code for test_syscall_sound_pcm_k.c (SCRUM-213).
 *
 * The acceptance clause of the ticket: a LibOS queues a decoded sample
 * through exo_sound_pcm and it comes out of the audio device. This is the
 * ring-3 half -- one real `syscall` for EXO_SYS_SOUND_PCM, whose samples are
 * the image's own data blob (the test hands libos_build_image() a generated
 * square wave as `data`, so LIBOS_LAUNCH_DATA_VADDR *is* the buffer), and
 * whose genuine return value comes back through libos_return().
 *
 * The buffer is deliberately the LibOS's own mapped data page rather than
 * anything the kernel arranged: that is what makes the test prove the
 * validation path (exo_range_in_user_window + exo_user_range_mapped against
 * the LibOS's own CR3) rather than bypass it.
 *
 * Position-independent (copied to LIBOS_LAUNCH_CODE_VADDR by
 * libos_build_image()): no internal jump/call, no RIP-relative reference, and
 * its data is reached through the fixed LIBOS_LAUNCH_DATA_VADDR immediate --
 * the SCRUM-50 entry convention.
 */

#include "exo_syscall.h"
#include "libos_launch.h"

.code64

.set SYS_LIBOS_RETURN, LIBOS_RETURN_SYSCALL_NUM
.set SYS_SOUND_PCM,    EXO_SYS_SOUND_PCM

/* Kept in step with the test's own PROBE_* constants, which assert the values
 * it expects to see land on the voice. */
.set PROBE_SAMPLES,    2048
.set PROBE_RATE_HZ,    11025
.set PROBE_VOL,        127
.set PROBE_SEP,        128
.set PROBE_PRIORITY,   64

.global sound_pcm_probe
.global sound_pcm_probe_end
sound_pcm_probe:
    movq $SYS_SOUND_PCM, %rax
    movq $LIBOS_LAUNCH_DATA_VADDR, %rdi   /* the square wave, in our data  */
    movq $PROBE_SAMPLES, %rsi
    movq $PROBE_RATE_HZ, %rdx
    movq $PROBE_VOL, %r10                 /* 4th arg is R10, not RCX       */
    movq $PROBE_SEP, %r8
    movq $PROBE_PRIORITY, %r9
    syscall

    movq %rax, %rdi                       /* exo_sound_pcm's real result   */
    movq $SYS_LIBOS_RETURN, %rax
    syscall
sound_pcm_probe_end:
