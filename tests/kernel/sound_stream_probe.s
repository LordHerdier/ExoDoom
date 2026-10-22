/*
 * sound_stream_probe.s — ring-3 code for test_syscall_sound_stream_k.c
 * (SCRUM-219).
 *
 * One real `syscall` for EXO_SYS_SOUND_STREAM_WRITE, whose frames are the
 * image's own data blob (LIBOS_LAUNCH_DATA_VADDR), so the handler's bounds
 * and page-table checks run against the LibOS's own CR3.  The packed result
 * (accepted | free << 32) comes back through libos_return().
 *
 * Position-independent, as sound_pcm_probe.s: no internal jump/call, no
 * RIP-relative reference (the SCRUM-50 entry convention).
 */

#include "exo_syscall.h"
#include "libos_launch.h"

.code64

.set SYS_LIBOS_RETURN,   LIBOS_RETURN_SYSCALL_NUM
.set SYS_STREAM_WRITE,   EXO_SYS_SOUND_STREAM_WRITE

/* Kept in step with the test's PROBE_FRAMES. */
.set PROBE_FRAMES,       1024

.global sound_stream_probe
.global sound_stream_probe_end
sound_stream_probe:
    movq $SYS_STREAM_WRITE, %rax
    movq $LIBOS_LAUNCH_DATA_VADDR, %rdi
    movq $PROBE_FRAMES, %rsi
    syscall

    movq %rax, %rdi
    movq $SYS_LIBOS_RETURN, %rax
    syscall
sound_stream_probe_end:
