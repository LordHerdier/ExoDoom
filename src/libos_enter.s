/*
 * libos_enter.s — the actual ring-3 transition (SCRUM-47).
 *
 * Generalizes tests/kernel/ring3_probe.s's ring3_run/ring3_escape pair --
 * same saved-RSP unwind, same "iretq down, syscall back up" shape -- into
 * real, non-TESTING-gated infrastructure. See src/libos_launch.h for why
 * this exists as a separate thing from the probe rather than reusing it.
 */

#include "libos_launch.h"

.code64

/* Selectors and RFLAGS values now shared with src/context_switch.s
 * (SCRUM-108) via LIBOS_LAUNCH_USER_SS/_CS/_RFLAGS/_RFLAGS_IRQ in
 * src/libos_launch.h -- see that header's comment for why one definition
 * replaces what used to be a private `.set` here. SCRUM-170 proved a
 * hardware interrupt taken at CPL 3 switches to TSS.RSP0 correctly (see
 * libos_enter_irq below and tests/kernel/test_irq_entry_k.c), but every
 * fault/launch test in tests/kernel/ still depends on LIBOS_LAUNCH_RFLAGS's
 * exact value for libos_enter() -- do not change it; add a new entry point
 * instead, as libos_enter_irq below does.
 *
 * LIBOS_LAUNCH_RFLAGS_IRQ keeps PIT/keyboard IRQs landing (on TSS.RSP0, then
 * back to CPL 3 via their own iretq) for as long as the launched context
 * runs. Needed by any LibOS whose ring-3 code relies on exo_get_ticks()
 * actually advancing or exo_kbd_poll() actually seeing input while it runs
 * -- with IF clear (LIBOS_LAUNCH_RFLAGS above) neither IRQ0 nor IRQ1 can
 * ever be recognised once `iretq` drops to ring 3, so exo_get_ticks() would
 * return a frozen value forever and exo_kbd_poll() would never see a
 * keypress.
 *
 * This is the actual exercise of what SCRUM-170 asked to be verified:
 * idt_set_gate()'s IST=0 gates mean a hardware interrupt taken at CPL 3
 * loads TSS.RSP0 exactly like the CPL-3 page-fault SCRUM-46 already proved
 * for an *exception* -- the ISR runs on the kernel stack, EOIs, and
 * `iretq`s straight back to CPL 3, the same mechanism, just a different
 * vector. Proved live by tests/kernel/test_irq_entry_k.c, analogous to
 * tests/kernel/test_tss_k.c's page-fault probe: a LibOS launched through
 * this entry point busy-waits on real exo_get_ticks() advancement, and
 * src/pit.c's irq0_handler records (TESTING builds only) the stack pointer
 * it observed on entry, which the test checks lands inside TSS.RSP0's
 * range. Manual, interactive evidence exists too -- a LibOS launched via
 * this entry point ran a multi-second interactive loop under continuous
 * PIT/keyboard IRQ traffic with no fault -- but the KUnit probe is the
 * deterministic, CI-checked proof. */
.set USER_SS, LIBOS_LAUNCH_USER_SS
.set USER_CS, LIBOS_LAUNCH_USER_CS
.set LAUNCH_RFLAGS, LIBOS_LAUNCH_RFLAGS
.set LAUNCH_RFLAGS_IRQ, LIBOS_LAUNCH_RFLAGS_IRQ

.section .bss
.align 8
libos_saved_rsp:
    .quad 0

.section .text

/*
 * void libos_iretq_enter(uint64_t entry_rip, uint64_t user_rsp,
 *                         uint64_t rflags);
 * Arguments arrive in RDI, RSI, RDX per the SysV ABI.
 *
 * The one place that builds a ring-3 iretq frame (SS/RSP/RFLAGS/CS/RIP,
 * LIBOS_LAUNCH_USER_SS/_CS) and executes it. libos_enter/libos_enter_irq
 * below jump here after pushing their own callee-saved GPRs and loading
 * RDI/RSI/RDX from their arguments/RFLAGS constant; src/context_switch.s's
 * context_switch_tail jumps here too, after restoring the incoming
 * context's callee-saved GPRs and loading RDI/RSI/RDX (and, uniquely to
 * that caller, RAX -- see its own comment) from context_regs_t. One
 * definition of the frame shape instead of three that could drift apart.
 * Never returns to its caller in the ordinary sense; whoever jumps here has
 * already arranged how control comes back (libos_return(), or a future
 * context switch away).
 */
.global libos_iretq_enter
libos_iretq_enter:
    /* An iretq frame is the only way into a lower privilege level: the CPU
     * pops RIP, CS, RFLAGS, RSP and SS, and the CPL comes from the CS RPL. */
    pushq $USER_SS
    pushq %rsi                  /* user RSP */
    pushq %rdx                  /* RFLAGS */
    pushq $USER_CS
    pushq %rdi                  /* user RIP */
    iretq

/*
 * uint64_t libos_enter(uint64_t entry_vaddr, uint64_t stack_top_vaddr);
 *
 * `iretq` to CPL 3 at `entry_vaddr` (RDI) with RSP = `stack_top_vaddr`
 * (RSI). Does not return to its caller in the ordinary sense -- control
 * comes back, if at all, through libos_return(), whose first argument
 * becomes this call's apparent return value. Callee-saved registers are
 * preserved across that round trip the same way ring3_run's are.
 */
.global libos_enter
libos_enter:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    movq %rsp, libos_saved_rsp(%rip)

    movq $LAUNCH_RFLAGS, %rdx    /* entry_vaddr (%rdi), stack_top_vaddr
                                  * (%rsi) already sit where
                                  * libos_iretq_enter expects them. */
    jmp libos_iretq_enter

/*
 * uint64_t libos_enter_irq(uint64_t entry_vaddr, uint64_t stack_top_vaddr);
 *
 * Identical to libos_enter() above except for RFLAGS -- IF is set
 * (LAUNCH_RFLAGS_IRQ), so PIT/keyboard IRQs keep landing (on TSS.RSP0, then
 * back to CPL 3 via their own iretq) for as long as this launched context
 * runs. See LAUNCH_RFLAGS_IRQ's comment above for why libos_enter() itself
 * is deliberately left alone rather than changed in place.
 */
.global libos_enter_irq
libos_enter_irq:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    movq %rsp, libos_saved_rsp(%rip)

    movq $LAUNCH_RFLAGS_IRQ, %rdx
    jmp libos_iretq_enter

/*
 * int64_t libos_return(uint64_t result, ...);
 *
 * Reached from ring 3 via the ordinary syscall path once a caller registers
 * it as a handler (exo_syscall_register), so it arrives with the launched
 * code's result in RDI (a1) already marshalled by syscall_entry.s. Unwinds
 * to libos_enter's caller instead of returning to its own.
 */
.global libos_return
libos_return:
    movq %rdi, %rax              /* becomes libos_enter's return value */
    movq libos_saved_rsp(%rip), %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret
