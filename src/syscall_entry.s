/*
 * syscall_entry.s — x86_64 `syscall` entry stub (SCRUM-32).
 *
 * IA32_LSTAR points here (see syscall_init in src/syscall.c).  On entry the
 * CPU has already:
 *
 *   - loaded CS = STAR[47:32] = 0x08 and SS = 0x10, putting us at CPL 0;
 *   - saved the caller's RIP in RCX and RFLAGS in R11, destroying whatever
 *     was in those two registers;
 *   - cleared the RFLAGS bits in IA32_FMASK, so interrupts are off.
 *
 * What it has *not* done is switch stacks.  Unlike an interrupt, `syscall`
 * leaves RSP pointing at the caller's stack, so the first thing here must be
 * to get off it — a ring-3 RSP is attacker-controlled and may be garbage.
 *
 * Register contract (docs/syscall_spec.md §3.1): everything except RAX, RCX
 * and R11 is preserved, argument registers included.  That is stronger than
 * the SysV callee-saved set, and deliberately so: the LibOS stubs in
 * src/exo_syscall.h list only rcx/r11/memory as clobbered, so the compiler
 * may keep a value live in RDI/RSI/RDX/R10/R8/R9 across a `syscall` and reuse
 * it afterwards.  Saving only RBX/RBP/R12-R15 here would corrupt arguments in
 * any loop that repeats a syscall — see the repeat test in
 * tests/kernel/test_syscall_k.c, which exists to catch exactly that.
 */

.code64

/* ── Kernel stack for syscall entry ───────────────────────────────────────
 *
 * Deliberately *not* boot.s's stack_top.  The ring-3 test probe re-enters
 * the kernel while the kernel is already nested on the boot stack waiting to
 * resume, so reusing it would overwrite the frames the test returns through.
 * A real LibOS syscall does not have that problem today, but will as soon as
 * the kernel can be mid-anything when a syscall arrives.
 *
 * 16 KiB, matching boot.s.  Aligned to 16 so the ABI's alignment requirement
 * at the `call` below can be reasoned about by counting pushes.
 */
.section .bss

/* Where the interrupted user RSP is parked for the duration of the call.
 *
 * A single global, not a per-CPU slot, which makes this path non-reentrant:
 * it is safe only because FMASK clears IF (so no interrupt can arrive and
 * issue its own syscall) and there is one CPU.  The upgrade path when
 * SCRUM-107 introduces multiple LibOS contexts is `swapgs` plus a per-CPU
 * block reached through IA32_KERNEL_GS_BASE; that is the only reason the
 * kernel would need a GS base at all.
 *
 * Placed *below* the stack buffer deliberately.  Putting it immediately after
 * syscall_stack_end would leave it abutting the end the stack actually grows
 * down from — safe, since `push` pre-decrements, but only by one byte of
 * reasoning, and it would stop being safe the moment anything adjusted the
 * stack top. */
.align 8
saved_user_rsp:
    .skip 8

.align 16
syscall_stack_bottom:
    .skip 16384
syscall_stack_end:

.section .data
.align 8
syscall_stack_top:
    .quad syscall_stack_end

.section .text

.global syscall_entry
.extern exo_syscall_dispatch

syscall_entry:
    /* Swap stacks.  Both of these are %rip-relative memory operands, so
     * neither needs a scratch register — which matters, because at this
     * point every register except RCX and R11 still belongs to the caller
     * and RCX/R11 hold the return RIP and RFLAGS we need for `sysretq`. */
    movq %rsp, saved_user_rsp(%rip)
    movq syscall_stack_top(%rip), %rsp

    /* Save the full caller state.  RCX and R11 first: the ABI does not
     * promise to preserve them, but `sysretq` reads the return RIP out of
     * RCX and the return RFLAGS out of R11, so they have to come back. */
    push %rcx
    push %r11

    /* Callee-saved set — the C dispatcher preserves these, but a handler
     * written in assembly later would not, and the cost is four pushes. */
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15

    /* The argument registers.  These are the ones a callee-saved-only stub
     * would drop on the floor. */
    push %rdi
    push %rsi
    push %rdx
    push %r10
    push %r8
    push %r9

    /*
     * ── Marshal into the SysV order exo_syscall_dispatch expects ─────────
     *
     *   syscall ABI:   RAX=num  RDI=a1 RSI=a2 RDX=a3 R10=a4 R8=a5 R9=a6
     *   SysV C ABI:    RDI=num  RSI=a1 RDX=a2 RCX=a3 R8=a4  R9=a5  [rsp]=a6
     *
     * Every argument shifts one position right, so the moves must run
     * right-to-left or each would clobber the source of the next.
     *
     * 14 pushes = 112 bytes, and RSP started 16-aligned, so it still is.
     * The stacked 6th argument has to sit at [RSP] with RSP 16-aligned at
     * the `call`, hence the padding quadword: sub 8 then push 8 keeps the
     * alignment and puts a6 where the callee looks for it.
     */
    subq $8, %rsp
    push %r9            /* a6 -> stack slot */

    movq %r8,  %r9      /* a5 -> 6th register argument */
    movq %r10, %r8      /* a4 -> 5th */
    movq %rdx, %rcx     /* a3 -> 4th */
    movq %rsi, %rdx     /* a2 -> 3rd */
    movq %rdi, %rsi     /* a1 -> 2nd */
    movq %rax, %rdi     /* num -> 1st */

    call exo_syscall_dispatch
    /* RAX now holds the value the caller sees; it is the one register we are
     * allowed to change, so it is left alone from here on. */

    addq $16, %rsp      /* drop the stacked argument and its padding */

    /* Restore in exact reverse. */
    pop %r9
    pop %r8
    pop %r10
    pop %rdx
    pop %rsi
    pop %rdi

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx

    pop %r11            /* caller's RFLAGS -> restored by sysretq */
    pop %rcx            /* caller's RIP    -> restored by sysretq */

    movq saved_user_rsp(%rip), %rsp

    /*
     * sysretq (not sysret): the 64-bit form, which returns to a 64-bit code
     * segment.  CS = STAR[63:48] + 16 = 0x28 and SS = STAR[63:48] + 8 =
     * 0x20, both forced to CPL 3 — the CPU does not consult RCX/R11 for
     * anything but RIP and RFLAGS.  RCX must be canonical or this faults;
     * validating it is SCRUM-54's job, not this stub's.
     */
    sysretq
