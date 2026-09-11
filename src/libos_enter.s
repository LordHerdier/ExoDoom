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

/* RFLAGS for the launch: bit 1 is reserved and must be set; IF stays clear
 * (SCRUM-170 tracks proving a hardware interrupt taken at CPL 3 switches to
 * TSS.RSP0 correctly -- until that lands, nothing here relies on it). */
.set LAUNCH_RFLAGS, 0x002

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
