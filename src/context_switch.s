/*
 * context_switch.s — the asm half of the context switch (SCRUM-108).
 *
 * context_switch_tail is reached by a `jmp` from src/syscall_entry.s,
 * spliced in right after `call exo_syscall_dispatch` returns, in place of
 * that file's normal pop+sysretq epilogue -- taken only when
 * src/context.c's context_switch_request() armed context_switch_pending
 * during the call just made. See context.h's top comment for the overall
 * design and why context_regs_t only needs the SysV callee-saved set plus
 * rsp/rip/rflags.
 *
 * At the point this is reached, the outgoing context's saved state is
 * available, but not all of it in live registers:
 *
 *   RBX/RBP/R12-R15   -- the outgoing context's callee-saved GPRs, still
 *                        live and correct. SysV guarantees
 *                        exo_syscall_dispatch() and everything it calls
 *                        (context_switch_request(), any C code before it)
 *                        restore these to their pre-call values -- no
 *                        stack-walking needed.
 *   saved_user_rsp    -- the outgoing context's RSP, parked by
 *                        syscall_entry.s's own prologue before the call, in
 *                        a memory global rather than a register -- also
 *                        untouched.
 *   RCX / R11         -- NOT live-safe. `syscall` loaded the outgoing
 *                        context's ring-3 RIP/RFLAGS into these on entry,
 *                        but they are caller-saved in the SysV C ABI, so
 *                        exo_syscall_dispatch()/context_switch_request()
 *                        (both ordinary C) are free to clobber them --
 *                        and do, e.g. via any call they make internally.
 *                        syscall_entry.s's own prologue already pushed the
 *                        *original* values onto the kernel stack before the
 *                        call for exactly this reason (its normal
 *                        pop+sysretq epilogue reads them back the same way);
 *                        this tail reads them from the same two stack slots
 *                        via %rsp, still valid since nothing has adjusted
 *                        %rsp since `call exo_syscall_dispatch` returned.
 *                        SAVED_RCX_OFF/SAVED_R11_OFF below are that stack
 *                        layout's offsets -- see the comment there for the
 *                        exact derivation, and keep both in sync with
 *                        syscall_entry.s's push order if it ever changes.
 */

#include "libos_launch.h"   /* LIBOS_LAUNCH_USER_SS/_CS */

.code64

/* Mirrors context_regs_t's layout in src/context.h -- that header
 * _Static_assert()s these offsets, so a layout change there is a compile
 * error, not a silent mismatch here. */
.set CTX_REGS_OFF_RSP,    0
.set CTX_REGS_OFF_RIP,    8
.set CTX_REGS_OFF_RFLAGS, 16
.set CTX_REGS_OFF_RBX,    24
.set CTX_REGS_OFF_RBP,    32
.set CTX_REGS_OFF_R12,    40
.set CTX_REGS_OFF_R13,    48
.set CTX_REGS_OFF_R14,    56
.set CTX_REGS_OFF_R15,    64

/* Offsets from %rsp, valid only right where context_switch_tail is entered
 * (via `jmp`, right after `call exo_syscall_dispatch` returns, before that
 * file's `addq $16, %rsp`). Derived from src/syscall_entry.s's push order:
 * 14 pushes (rcx, r11, rbx, rbp, r12, r13, r14, r15, rdi, rsi, rdx, r10, r8,
 * r9) followed by `subq $8, %rsp; push %r9` for the stacked 6th argument --
 * 128 bytes total between the first push (rcx, deepest/highest address) and
 * the current %rsp. rcx is the 1st push (128 - 8 = 120 bytes above %rsp);
 * r11 is the 2nd (128 - 16 = 112). */
.set SAVED_RCX_OFF, 120
.set SAVED_R11_OFF, 112

.section .text

.global context_switch_tail
.extern context_switch_out_regs
.extern context_switch_in_regs
.extern context_switch_in_pml4
.extern saved_user_rsp

context_switch_tail:
    /* Capture the outgoing context: RAX is free (dispatch's return value is
     * being discarded on this path -- the outgoing context resumes later
     * through its own saved RIP, not through this call's return). */
    movq context_switch_out_regs(%rip), %rax
    movq saved_user_rsp(%rip), %rdx
    movq %rdx, CTX_REGS_OFF_RSP(%rax)
    movq SAVED_RCX_OFF(%rsp), %rdx
    movq %rdx, CTX_REGS_OFF_RIP(%rax)
    movq SAVED_R11_OFF(%rsp), %rdx
    movq %rdx, CTX_REGS_OFF_RFLAGS(%rax)
    movq %rbx, CTX_REGS_OFF_RBX(%rax)
    movq %rbp, CTX_REGS_OFF_RBP(%rax)
    movq %r12, CTX_REGS_OFF_R12(%rax)
    movq %r13, CTX_REGS_OFF_R13(%rax)
    movq %r14, CTX_REGS_OFF_R14(%rax)
    movq %r15, CTX_REGS_OFF_R15(%rax)

    /* Swap address spaces. Safe to do here, before loading the incoming
     * context's own registers below: kernel memory (this code, these
     * globals, saved_user_rsp) lives under the shared PML4[0] link every
     * address space carries (see docs/architecture.md's vmm.c section), so
     * it stays mapped and reachable by the same virtual addresses across the
     * switch. */
    movq context_switch_in_pml4(%rip), %rax
    movq %rax, %cr3

    /* Restore the incoming context's callee-saved GPRs, then build an iretq
     * frame from its saved RSP/RFLAGS/RIP -- the same shape libos_enter.s
     * builds for a fresh launch (LIBOS_LAUNCH_USER_SS/_CS), whether this
     * context was primed (context_prime()) or is resuming from a previous
     * switch-out captured above. */
    movq context_switch_in_regs(%rip), %rax
    movq CTX_REGS_OFF_RBX(%rax), %rbx
    movq CTX_REGS_OFF_RBP(%rax), %rbp
    movq CTX_REGS_OFF_R12(%rax), %r12
    movq CTX_REGS_OFF_R13(%rax), %r13
    movq CTX_REGS_OFF_R14(%rax), %r14
    movq CTX_REGS_OFF_R15(%rax), %r15

    pushq $LIBOS_LAUNCH_USER_SS
    pushq CTX_REGS_OFF_RSP(%rax)
    pushq CTX_REGS_OFF_RFLAGS(%rax)
    pushq $LIBOS_LAUNCH_USER_CS
    pushq CTX_REGS_OFF_RIP(%rax)
    iretq
