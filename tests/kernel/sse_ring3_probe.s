/*
 * sse_ring3_probe.s — SSE executed at CPL 3, and XMM state carried across a
 * real syscall (SCRUM-177).
 *
 * Two claims in one launch, because they need the same setup:
 *
 *   1. A LibOS can execute SSE instructions at all.  boot.s sets
 *      CR0/CR4 once, in ring 0, before kernel_main -- nothing re-programs
 *      them per privilege level, and CR4.OSFXSR is not a CPL-gated bit, so
 *      this ought to follow from the ring-0 half of the suite.  "Ought to"
 *      is why the probe exists: Doom runs in ring 3, and this is the ring
 *      the acceptance criterion actually cares about.
 *
 *   2. Every one of %xmm0-%xmm15 still holds what ring 3 put there after a
 *      round trip through src/syscall_entry.s.  That stub saves GP registers
 *      only -- no fxsave, no XMM spill -- which is safe precisely because
 *      every kernel object is compiled -mno-sse -mno-sse2 -mno-mmx and so
 *      cannot name an XMM register.  docs/syscall_spec.md §3.4a is the
 *      written form of that decision; this is the executable form, so a
 *      future edit that drops -mno-sse from the kernel's own CFLAGS fails a
 *      test instead of silently corrupting Doom's floats.
 *
 * exo_get_ticks (#5) is the syscall used: bound by syscall_pit_init() ahead
 * of kernel_main's TESTING branch, takes no arguments and touches no memory,
 * so nothing about the call can fail for a reason unrelated to what is being
 * measured.
 *
 * Preprocessed with the C preprocessor before assembly
 * (docker/scripts/build.sh), so SYS_GET_TICKS/SYS_LIBOS_RETURN and the
 * SSE_PROBE_* result codes below are the real macros rather than hand-copied
 * literals.  Same PIC rules as every other probe here: relative branches
 * within the copied blob only, no RIP-relative data reference (this probe has
 * no data region at all -- every constant it needs is an immediate).
 */

#include "exo_syscall.h"
#include "libos_launch.h"
#include "sse_probe_result.h"

.code64

.set SYS_GET_TICKS,    EXO_SYS_GET_TICKS
.set SYS_LIBOS_RETURN, LIBOS_RETURN_SYSCALL_NUM

/* IEEE-754 single-precision bit patterns.  Spelled out rather than written
 * as floats because this file is assembled, and because the same three
 * constants appear in test_sse_k.c's ring-0 cases -- where -mno-sse C cannot
 * write a float literal either. */
.set F_ONE,   0x3F800000        /* 1.0f */
.set F_TWO,   0x40000000        /* 2.0f */
.set F_THREE, 0x40400000        /* 3.0f */

.global sse_ring3_probe
.global sse_ring3_probe_end
sse_ring3_probe:
    /* ── 1. SSE arithmetic at CPL 3 ──────────────────────────────────
     * If CR4.OSFXSR were clear the addss below would raise #UD, which
     * idt_init() leaves on default_stub -- a bare iretq back to the
     * faulting instruction, so the probe would hang rather than reach the
     * compare.  test_sse_k.c asserts the control-register bits before ever
     * launching this, so that path is unreachable by the time we get here;
     * the compare guards against the instruction executing and being
     * *wrong*, not against it being absent. */
    movl   $F_ONE, %eax
    movd   %eax, %xmm0
    movl   $F_TWO, %eax
    movd   %eax, %xmm1
    addss  %xmm1, %xmm0
    movd   %xmm0, %eax
    cmpl   $F_THREE, %eax
    je     1f
    movq   $(SSE_PROBE_MARKER + SSE_PROBE_R_BAD_ARITH), %rdi
    jmp    9f
1:

    /* ── 2. Seed every XMM register with its own pattern ─────────────── */
    SSE_SEED_ALL_XMM

    /* ── 3. Cross the syscall boundary for real ──────────────────────── */
    movq   $SYS_GET_TICKS, %rax
    syscall
    testq  %rax, %rax            /* a bound handler returns a tick count;  */
    jns    1f                    /* an unbound one returns -EXO_ENOSYS     */
    movq   $(SSE_PROBE_MARKER + SSE_PROBE_R_BAD_TICKS), %rdi
    jmp    9f
1:

    /* ── 4. Nothing in the kernel may have touched them ──────────────── */
    SSE_CHECK_ALL_XMM

    movq   $(SSE_PROBE_MARKER + SSE_PROBE_R_OK), %rdi
9:
    movq   $SYS_LIBOS_RETURN, %rax
    syscall
sse_ring3_probe_end:
    ud2                          /* unreachable: SYS_LIBOS_RETURN does not
                                  * come back, and this label's only job is
                                  * to bound the copy above. */
