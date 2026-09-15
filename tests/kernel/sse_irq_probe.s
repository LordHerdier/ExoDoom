/*
 * sse_irq_probe.s — XMM state carried across a hardware interrupt taken at
 * CPL 3 (SCRUM-177).
 *
 * sse_ring3_probe.s covers the syscall path, which the LibOS enters
 * deliberately and at a point of its own choosing.  This covers the harder
 * half: src/isr.s's irq0_stub can interrupt ring-3 code between *any* two
 * instructions, including the middle of a float computation, and it saves GP
 * registers only -- no fxsave, no XMM spill.  If that were wrong, Doom would
 * not fail cleanly; it would produce subtly wrong geometry at whatever rate
 * IRQ0 happens to land inside r_draw/r_plane arithmetic, which is close to
 * the worst debugging experience this kernel could hand someone.
 *
 * Mechanically this is tests/kernel/irq_entry_probe.s (SCRUM-170) with XMM
 * seeding wrapped around it, and it borrows that probe's whole argument for
 * why an interrupt is guaranteed to land: kernel_main never `sti`s ahead of
 * run_tests(), so the only window in which IRQ0 can be recognised anywhere in
 * this test binary is while a context launched through libos_enter_irq()
 * (RFLAGS.IF set) is running -- and exo_get_ticks() advancing at all is proof
 * that it was.  The busy-wait therefore does double duty: it is the clock
 * this probe waits on, and it is the evidence that interrupts really did fire
 * at CPL 3 while the XMM registers below held their patterns.
 *
 * Preprocessed with the C preprocessor before assembly, same as every other
 * probe here; the PIC rules are the same too (relative branches inside the
 * blob, no data region).
 */

#include "exo_syscall.h"
#include "libos_launch.h"
#include "sse_probe_result.h"

.code64

.set SYS_GET_TICKS,    EXO_SYS_GET_TICKS
.set SYS_LIBOS_RETURN, LIBOS_RETURN_SYSCALL_NUM

/* Milliseconds of real, IRQ0-driven tick advancement to wait for -- long
 * enough that a handful of coalesced ticks cannot explain it away, short
 * enough not to stall the suite.  Same value, for the same reason, as
 * irq_entry_probe.s's WAIT_MS. */
.set WAIT_MS, 5

.global sse_irq_probe
.global sse_irq_probe_end
sse_irq_probe:
    /* Seed before the wait: every interrupt taken during the loop below
     * lands with these patterns live in the register file. */
    SSE_SEED_ALL_XMM

    movq   $SYS_GET_TICKS, %rax
    syscall
    testq  %rax, %rax
    jns    1f
    movq   $(SSE_PROBE_MARKER + SSE_PROBE_R_BAD_TICKS), %rdi
    jmp    9f
1:
    /* %r14 carries the starting tick count across the loop: callee-saved,
     * and the syscall ABI preserves every register but RAX anyway
     * (src/syscall_entry.s), so nothing here can lose it. */
    movq   %rax, %r14
2:
    movq   $SYS_GET_TICKS, %rax
    syscall
    subq   %r14, %rax
    cmpq   $WAIT_MS, %rax
    jl     2b

    /* Interrupts have demonstrably fired at CPL 3 by now.  Every XMM
     * register must still read back what this probe put there. */
    SSE_CHECK_ALL_XMM

    movq   $(SSE_PROBE_MARKER + SSE_PROBE_R_OK), %rdi
9:
    movq   $SYS_LIBOS_RETURN, %rax
    syscall
sse_irq_probe_end:
    ud2                          /* unreachable: SYS_LIBOS_RETURN does not
                                  * come back, and this label's only job is
                                  * to bound the copy above. */
