#pragma once

/*
 * sse_probe_result.h — the result encoding the ring-3 SSE probes hand back
 * through libos_return(), shared between the probes and the suite that reads
 * them (SCRUM-177).
 *
 * tests/kernel/sse_ring3_probe.s, tests/kernel/sse_irq_probe.s and
 * tests/kernel/test_sse_k.c all need to agree on these numbers.  They live
 * in one header rather than three hand-copied literals for the same reason
 * LIBOS_RETURN_SYSCALL_NUM does (src/libos_launch.h): the .s probes are run
 * through the C preprocessor before assembly (docker/scripts/build.sh's
 * `-x assembler-with-cpp` loop), so both sides can reference the same macro.
 *
 * Everything an assembler cannot parse belongs under `#ifndef __ASSEMBLER__`
 * and everything a C compiler cannot parse -- the .macro blocks below --
 * under `#ifdef __ASSEMBLER__`, the same split src/libos_launch.h uses.
 */

/*
 * A probe's result is MARKER + a code, never a bare code: a launched context
 * that never got as far as libos_return() cannot produce a value in this
 * range by accident, so "0" as a result means "the probe never ran", not
 * "everything passed".
 */
#define SSE_PROBE_MARKER            0x55E00000

#define SSE_PROBE_R_OK              0   /* every check passed               */
#define SSE_PROBE_R_BAD_ARITH       1   /* addss gave the wrong answer      */
#define SSE_PROBE_R_BAD_TICKS       2   /* exo_get_ticks returned an error  */
#define SSE_PROBE_R_BAD_XMM_BASE    3   /* + n: %xmmN did not survive (0-15) */

/*
 * The 64-bit value seeded into each XMM register, plus that register's
 * index.  Both halves are non-zero on purpose, so a clobber that only wrote
 * part of the register still fails the comparison.
 *
 * Only the low 64 bits of each XMM register are seeded and checked: `movq`
 * is the widest XMM<->GP move there is, and the ring-3 probes deliberately
 * have no data region to stage a 128-bit value through (they are launched
 * with data_len = bss_len = 0).  That is not a real gap -- anything the
 * kernel could plausibly do to an XMM register, up to and including the
 * full-width movaps GCC emits for a struct copy, writes the low half too.
 */
#define SSE_PROBE_XMM_PATTERN       0xD00DFACE5A5A0000

#ifdef __ASSEMBLER__

/*
 * SSE_SEED_XMM <xmm>, <index> — load this register's distinct pattern.
 * Clobbers %rbx.
 */
.macro SSE_SEED_XMM x, idx
    movabsq $(SSE_PROBE_XMM_PATTERN + \idx), %rbx
    movq    %rbx, \x
.endm

/*
 * SSE_CHECK_XMM <xmm>, <index> — verify it still holds that pattern.
 * Clobbers %rbx and %r8.
 *
 * On a mismatch this loads the failing result into %rdi and jumps forward to
 * the nearest `9:` label, which every probe using these macros defines once,
 * immediately before its libos_return() call.  GNU as's numeric labels
 * resolve by position, so the `1:` each expansion defines for its own
 * success path cannot be confused with any other expansion's.
 */
.macro SSE_CHECK_XMM x, idx
    movq    \x, %r8
    movabsq $(SSE_PROBE_XMM_PATTERN + \idx), %rbx
    cmpq    %rbx, %r8
    je      1f
    movq    $(SSE_PROBE_MARKER + SSE_PROBE_R_BAD_XMM_BASE + \idx), %rdi
    jmp     9f
1:
.endm

/* All sixteen, in order.  GCC allocates xmm0-xmm15 freely under the SysV
 * ABI, so a kernel path that clobbered "just one scratch register" could
 * have picked any of them -- checking a subset would be checking luck. */
.macro SSE_SEED_ALL_XMM
    SSE_SEED_XMM %xmm0,  0
    SSE_SEED_XMM %xmm1,  1
    SSE_SEED_XMM %xmm2,  2
    SSE_SEED_XMM %xmm3,  3
    SSE_SEED_XMM %xmm4,  4
    SSE_SEED_XMM %xmm5,  5
    SSE_SEED_XMM %xmm6,  6
    SSE_SEED_XMM %xmm7,  7
    SSE_SEED_XMM %xmm8,  8
    SSE_SEED_XMM %xmm9,  9
    SSE_SEED_XMM %xmm10, 10
    SSE_SEED_XMM %xmm11, 11
    SSE_SEED_XMM %xmm12, 12
    SSE_SEED_XMM %xmm13, 13
    SSE_SEED_XMM %xmm14, 14
    SSE_SEED_XMM %xmm15, 15
.endm

.macro SSE_CHECK_ALL_XMM
    SSE_CHECK_XMM %xmm0,  0
    SSE_CHECK_XMM %xmm1,  1
    SSE_CHECK_XMM %xmm2,  2
    SSE_CHECK_XMM %xmm3,  3
    SSE_CHECK_XMM %xmm4,  4
    SSE_CHECK_XMM %xmm5,  5
    SSE_CHECK_XMM %xmm6,  6
    SSE_CHECK_XMM %xmm7,  7
    SSE_CHECK_XMM %xmm8,  8
    SSE_CHECK_XMM %xmm9,  9
    SSE_CHECK_XMM %xmm10, 10
    SSE_CHECK_XMM %xmm11, 11
    SSE_CHECK_XMM %xmm12, 12
    SSE_CHECK_XMM %xmm13, 13
    SSE_CHECK_XMM %xmm14, 14
    SSE_CHECK_XMM %xmm15, 15
.endm

#endif /* __ASSEMBLER__ */
