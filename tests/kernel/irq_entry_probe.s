/*
 * irq_entry_probe.s — ring-3 code that busy-waits on exo_get_ticks() until
 * real time passes (SCRUM-170).
 *
 * Launched via libos_enter_irq() (src/libos_enter.s) rather than the
 * ordinary libos_enter(), so RFLAGS.IF is set on the way in. That is the
 * whole point of the probe: kernel_main never `sti`s ahead of run_tests()
 * (see tests/kernel/test_syscall_pit_k.c's file comment), so the only way
 * IRQ0 can be recognised anywhere in this test binary is during the exact
 * window this probe runs in at CPL 3. If exo_get_ticks() (#5) ever reports a
 * later value than it started with, IRQ0 fired -- and since the CPU is at
 * CPL 3 the whole time this loop runs, it fired while at CPL 3.
 *
 * Preprocessed with the C preprocessor before assembly (see
 * docker/scripts/build.sh), so SYS_GET_TICKS/SYS_LIBOS_RETURN below are the
 * real EXO_SYS_GET_TICKS/LIBOS_RETURN_SYSCALL_NUM macros. Same PIC rules as
 * every other probe in this tree: no internal jump/call target crosses a
 * relocation boundary, no RIP-relative data reference (this probe has no
 * data segment at all).
 *
 * %rbx is callee-saved and the ABI's syscall convention preserves every
 * register but RAX across the `syscall` round trip (src/syscall_entry.s),
 * so it safely carries the starting tick count across the loop.
 */

#include "exo_syscall.h"
#include "libos_launch.h"

.code64

.set SYS_GET_TICKS,    EXO_SYS_GET_TICKS
.set SYS_LIBOS_RETURN, LIBOS_RETURN_SYSCALL_NUM
.set RESULT_MARKER,    0x1120DEAD

/* How many milliseconds of real, IRQ0-driven tick advancement to wait for
 * before escaping -- comfortably longer than a handful of missed/coalesced
 * ticks could explain away, short enough not to stall the suite. */
.set WAIT_MS, 5

.global libos_irq_entry_probe
.global libos_irq_entry_probe_end
libos_irq_entry_probe:
    movq    $SYS_GET_TICKS, %rax
    syscall
    movq    %rax, %rbx           /* rbx = starting tick count */
1:
    movq    $SYS_GET_TICKS, %rax
    syscall
    subq    %rbx, %rax
    cmpq    $WAIT_MS, %rax
    jl      1b

    movq    $RESULT_MARKER, %rdi
    movq    $SYS_LIBOS_RETURN, %rax
    syscall
libos_irq_entry_probe_end:
    ud2                           /* unreachable: SYS_LIBOS_RETURN does not
                                   * come back, and this label's only job is
                                   * to bound the copy above. */
