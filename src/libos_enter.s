/*
 * libos_enter.s — the actual ring-3 transition (SCRUM-47).
 *
 * Generalizes tests/kernel/ring3_probe.s's ring3_run/ring3_escape pair --
 * same saved-RSP unwind, same "iretq down, syscall back up" shape -- into
 * real, non-TESTING-gated infrastructure. See src/libos_launch.h for why
 * this exists as a separate thing from the probe rather than reusing it.
 */

.code64

/* Selectors from the GDT in src/boot.s. sysretq's arithmetic pins these
 * values for the syscall return path; iretq does not require it, but using
 * anything else here would be needless inconsistency. */
.set USER_SS, 0x20 | 3
.set USER_CS, 0x28 | 3

/* RFLAGS for libos_enter(): bit 1 is reserved and must be set; IF stays
 * clear. SCRUM-170 proved a hardware interrupt taken at CPL 3 switches to
 * TSS.RSP0 correctly (see libos_enter_irq below and
 * tests/kernel/test_irq_entry_k.c), but every fault/launch test in
 * tests/kernel/ still depends on this exact value -- do not change it; add
 * a new entry point instead, as libos_enter_irq below does. */
.set LAUNCH_RFLAGS, 0x002

/* RFLAGS for libos_enter_irq(): same, but with IF set, so PIT/keyboard IRQs
 * keep landing (on TSS.RSP0, then back to CPL 3 via their own iretq) for as
 * long as the launched context runs. Needed by any LibOS whose ring-3 code
 * relies on exo_get_ticks() actually advancing or exo_kbd_poll() actually
 * seeing input while it runs -- with IF clear (LAUNCH_RFLAGS above) neither
 * IRQ0 nor IRQ1 can ever be recognised once `iretq` drops to ring 3, so
 * exo_get_ticks() would return a frozen value forever and exo_kbd_poll()
 * would never see a keypress.
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
.set LAUNCH_RFLAGS_IRQ, 0x202

.section .bss
.align 8
libos_saved_rsp:
    .quad 0

.section .text

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

    /* An iretq frame is the only way into a lower privilege level: the CPU
     * pops RIP, CS, RFLAGS, RSP and SS, and the CPL comes from the CS RPL. */
    pushq $USER_SS
    pushq %rsi                  /* user RSP = stack_top_vaddr */
    pushq $LAUNCH_RFLAGS
    pushq $USER_CS
    pushq %rdi                  /* user RIP = entry_vaddr */
    iretq

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

    pushq $USER_SS
    pushq %rsi                  /* user RSP = stack_top_vaddr */
    pushq $LAUNCH_RFLAGS_IRQ
    pushq $USER_CS
    pushq %rdi                  /* user RIP = entry_vaddr */
    iretq

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
