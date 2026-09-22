/*
 * context_switch.s — the asm half of the context switch (SCRUM-108, extended
 * by SCRUM-178 to also carry rdi/rsi/rdx/r10/r8/r9 -- see context.h's own
 * comment on context_regs_t for why).
 *
 * context_switch_tail is reached by a `jmp` from src/syscall_entry.s,
 * spliced in right after `call exo_syscall_dispatch` returns, in place of
 * that file's normal pop+sysretq epilogue -- taken only when
 * src/context.c's context_switch_request() armed context_switch_pending
 * during the call just made. See context.h's top comment for the overall
 * design.
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
 *   RDI/RSI/RDX/R10/R8/R9 -- also NOT live-safe, for the same reason as
 *                        RCX/R11: SysV caller-saved, so ordinary C between
 *                        here and the syscall entry is free to clobber them.
 *                        Also read back off the same pushed stack slots
 *                        (SAVED_RDI_OFF etc. below) rather than trusted live.
 */

#include "libos_launch.h"   /* LIBOS_LAUNCH_USER_SS/_CS -- the same constants
                             * src/libos_enter.s's libos_iretq_enter uses to
                             * build its iretq frame; this file builds its
                             * own copy of that same frame shape below rather
                             * than jumping to libos_iretq_enter, so it needs
                             * them directly too -- see the incoming-side
                             * comment for why. */

.set USER_SS, LIBOS_LAUNCH_USER_SS
.set USER_CS, LIBOS_LAUNCH_USER_CS

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
.set CTX_REGS_OFF_RAX,    72
.set CTX_REGS_OFF_RDI,    80
.set CTX_REGS_OFF_RSI,    88
.set CTX_REGS_OFF_RDX,    96
.set CTX_REGS_OFF_R10,    104
.set CTX_REGS_OFF_R8,     112
.set CTX_REGS_OFF_R9,     120

/* Offsets from %rsp, valid only right where context_switch_tail is entered
 * (via `jmp`, right after `call exo_syscall_dispatch` returns, before that
 * file's `addq $16, %rsp`). Derived from src/syscall_entry.s's push order:
 * 14 pushes (rcx, r11, rbx, rbp, r12, r13, r14, r15, rdi, rsi, rdx, r10, r8,
 * r9) followed by `subq $8, %rsp; push %r9` for the stacked 6th argument --
 * 128 bytes total between the first push (rcx, deepest/highest address) and
 * the current %rsp. rcx is the 1st push (128 - 8 = 120 bytes above %rsp);
 * r11 is the 2nd (128 - 16 = 112); rdi/rsi/rdx/r10/r8/r9 are the 9th-14th
 * (128 - 72 = 56, then 48, 40, 32, 24, 16). */
.set SAVED_RCX_OFF, 120
.set SAVED_R11_OFF, 112
.set SAVED_RDI_OFF, 56
.set SAVED_RSI_OFF, 48
.set SAVED_RDX_OFF, 40
.set SAVED_R10_OFF, 32
.set SAVED_R8_OFF,  24
.set SAVED_R9_OFF,  16

.section .text

.global context_switch_tail
.extern context_switch_out_regs
.extern context_switch_in_regs
.extern context_switch_in_pml4
.extern context_switch_in_id
.extern context_set_current
.extern saved_user_rsp

context_switch_tail:
    /* Capture the outgoing context. RAX at this point holds
     * exo_syscall_dispatch()'s return value for the very call that armed
     * this switch (the outgoing context's own exo_yield()) -- it must
     * survive to the context's next resume, since that is the value
     * exo_yield() is supposed to return. Stash it in %r11 (dead here: its
     * live value was already superseded by the pushed copy
     * context_switch_tail reads via SAVED_R11_OFF below) before using %rax
     * itself as a scratch pointer. */
    movq %rax, %r11
    movq context_switch_out_regs(%rip), %rax
    movq %r11, CTX_REGS_OFF_RAX(%rax)
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

    /* SCRUM-178: the six argument registers docs/syscall_spec.md's ABI
     * promises survive an ordinary syscall untouched -- read straight off
     * the same pushed stack slots syscall_entry.s's own non-switching
     * epilogue restores them from (SAVED_*_OFF above), via %rcx as scratch
     * (dead here: already consumed into CTX_REGS_OFF_RIP above, and not
     * needed again). Without this, compiled C that keeps a value live in
     * one of these across a `syscall` -- exactly what exo_syscall.h's
     * clobber list entitles it to do -- sees it corrupted the moment that
     * particular call happens to trigger a real switch instead of an
     * ordinary return. */
    movq SAVED_RDI_OFF(%rsp), %rcx
    movq %rcx, CTX_REGS_OFF_RDI(%rax)
    movq SAVED_RSI_OFF(%rsp), %rcx
    movq %rcx, CTX_REGS_OFF_RSI(%rax)
    movq SAVED_RDX_OFF(%rsp), %rcx
    movq %rcx, CTX_REGS_OFF_RDX(%rax)
    movq SAVED_R10_OFF(%rsp), %rcx
    movq %rcx, CTX_REGS_OFF_R10(%rax)
    movq SAVED_R8_OFF(%rsp), %rcx
    movq %rcx, CTX_REGS_OFF_R8(%rax)
    movq SAVED_R9_OFF(%rsp), %rcx
    movq %rcx, CTX_REGS_OFF_R9(%rax)

    /* Swap address spaces. Safe to do here, before loading the incoming
     * context's own registers below: kernel memory (this code, these
     * globals, saved_user_rsp) lives under the shared PML4[0] link every
     * address space carries (see docs/architecture.md's vmm.c section), so
     * it stays mapped and reachable by the same virtual addresses across the
     * switch. */
    movq context_switch_in_pml4(%rip), %rax
    movq %rax, %cr3

    /* SCRUM-179: commit context_current() itself here, not eagerly inside
     * context_switch_request() -- this is the one point where the
     * outgoing context's CR3 is already gone but nothing depending on the
     * incoming context's registers has run yet, so it is the correct place
     * to declare "who is running now" for context_current()/
     * syscall_current_context() purposes. Safe to `call` an ordinary C
     * function here: %rsp is still the 16-aligned value it was on entry to
     * context_switch_tail (nothing has pushed anything yet), and every
     * caller-saved register (rax/rcx/rdx/rsi/rdi/r8-r11) is dead scratch at
     * this exact point -- the outgoing context's state is already written
     * to memory above, and the incoming context's callee-saved GPRs
     * (rbx/rbp/r12-r15) haven't been touched yet, so the call's own
     * caller-saved clobbers cost nothing live. */
    movq context_switch_in_id(%rip), %rdi
    call context_set_current

    /* Restore the incoming context's callee-saved GPRs. */
    movq context_switch_in_regs(%rip), %rax
    movq CTX_REGS_OFF_RBX(%rax), %rbx
    movq CTX_REGS_OFF_RBP(%rax), %rbp
    movq CTX_REGS_OFF_R12(%rax), %r12
    movq CTX_REGS_OFF_R13(%rax), %r13
    movq CTX_REGS_OFF_R14(%rax), %r14
    movq CTX_REGS_OFF_R15(%rax), %r15

    /* Build the iretq frame directly (the same SS/RSP/RFLAGS/CS/RIP shape
     * src/libos_enter.s's libos_iretq_enter builds) by pushing straight from
     * the incoming context_regs_t via memory operands, rather than routing
     * through libos_iretq_enter as before SCRUM-178: that routine takes
     * entry/stack/rflags in RDI/RSI/RDX, which would collide with restoring
     * *this* context's own real RDI/RSI/RDX below -- one shared definition
     * of the frame shape was worth it when RAX was the only extra register
     * in play, but pushing from memory here keeps every argument register
     * free for its actual restore instead of needing to stage it around a
     * borrowed calling convention. */
    pushq $USER_SS
    pushq CTX_REGS_OFF_RSP(%rax)
    pushq CTX_REGS_OFF_RFLAGS(%rax)
    pushq $USER_CS
    pushq CTX_REGS_OFF_RIP(%rax)

    /* SCRUM-178: restore the incoming context's own argument registers --
     * see the outgoing-side capture above and context.h's context_regs_t
     * comment for why this matters. Safe to do now: the values just pushed
     * above already came off %rax via memory operands, so RDI/RSI/RDX/R10/
     * R8/R9 have been free scratch (unused) since the callee-saved restores
     * finished. */
    movq CTX_REGS_OFF_RDI(%rax), %rdi
    movq CTX_REGS_OFF_RSI(%rax), %rsi
    movq CTX_REGS_OFF_RDX(%rax), %rdx
    movq CTX_REGS_OFF_R10(%rax), %r10
    movq CTX_REGS_OFF_R8(%rax),  %r8
    movq CTX_REGS_OFF_R9(%rax),  %r9

    /* RAX itself is loaded last, from the incoming context's own saved
     * value -- see the outgoing-side comment above. Must happen after every
     * CTX_REGS_OFF_*(%rax) read above, since this clobbers the pointer. */
    movq CTX_REGS_OFF_RAX(%rax), %rax
    iretq
