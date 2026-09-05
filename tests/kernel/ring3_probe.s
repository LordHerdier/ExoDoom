/*
 * ring3_probe.s — ring-3 test harness for the syscall entry path (SCRUM-32).
 *
 * TESTING builds only.  This is the smallest thing that can prove the
 * acceptance criterion "executing `syscall` from ring 3 enters the dispatcher
 * and every argument register survives the round trip": a way down to CPL 3,
 * a probe that runs there, and a way back.
 *
 * It is not a LibOS and does not pretend to be one — no separate address
 * space, no loader, no TSS.  SCRUM-47 replaces it with the real ring-3 launch
 * once per-LibOS page directories (SCRUM-48) exist.  Two things make the
 * shortcut safe here and nowhere else:
 *
 *   - run_tests() executes before idt_init()/sti in kernel_main, so the probe
 *     cannot be interrupted.  That is why no TSS is needed: `syscall` never
 *     consults TSS.RSP0, and only an interrupt taken *in* ring 3 would.
 *   - docker/scripts/build.sh assembles boot.s with --defsym RING3_PROBE=1
 *     for TESTING builds, which sets the U/S bit through the identity map.
 *     Ring-3 code cannot execute at all without it.
 *
 * Getting home is the awkward part: ring-3 code cannot `ret` into ring 0, and
 * `sysretq` only ever goes the other way.  So ring3_run saves the kernel
 * stack pointer before dropping to CPL 3 and ring3_escape restores it — a
 * hand-rolled setjmp/longjmp pair.  ring3_escape runs as a syscall handler,
 * which means it is already on the syscall stack at CPL 0 with interrupts
 * off; abandoning the entry stub's frame there is fine, because that stack is
 * reset from syscall_stack_top on every entry.
 */

.code64

/* Selectors from the GDT in src/boot.s, with RPL 3.  See the comment there
 * for why sysret pins them to these values. */
.set USER_SS, 0x20 | 3
.set USER_CS, 0x28 | 3

/* RFLAGS for the probe: bit 1 is reserved and must be set, IF stays clear so
 * nothing can interrupt ring-3 code before the IDT exists. */
.set USER_RFLAGS, 0x002

/*
 * Scratch syscall numbers, borrowed from the ABI's unimplemented tail.
 * test_syscall_k.c binds them before the probe runs and unbinds them after,
 * so nothing outside the test ever sees a handler on these.  SCRUM-109 and
 * SCRUM-155 implement them for real; when they do, this file needs its own
 * numbers rather than borrowed ones.
 */
.set SYS_ECHO,   19          /* EXO_SYS_YIELD */
.set SYS_ESCAPE, 20          /* EXO_SYS_EXIT  */

/* Sentinels.  Distinct in every byte so a partial restore, a swapped pair or
 * a sign-extended 32-bit truncation all show up as a specific failed bit. */
.set S_RDI, 0xA1A1A1A1A1A1A1A1
.set S_RSI, 0xB2B2B2B2B2B2B2B2
.set S_RDX, 0xC3C3C3C3C3C3C3C3
.set S_R10, 0xD4D4D4D4D4D4D4D4
.set S_R8,  0xE5E5E5E5E5E5E5E5
.set S_R9,  0xF6F6F6F6F6F6F6F6
.set S_RBX, 0x1122334455667788
.set S_RBP, 0x8877665544332211
.set S_R12, 0x0F0F0F0F0F0F0F0F
.set S_R13, 0xF0F0F0F0F0F0F0F0
.set S_R14, 0x0123456789ABCDEF
.set S_R15, 0xFEDCBA9876543210

/* What the echo handler returns, checked by the probe. */
.set ECHO_RET, 0x5EC0DE

.section .bss
.align 8
ring3_saved_rsp:
    .quad 0

.section .text

/*
 * uint64_t ring3_run(void (*entry)(void), void *user_stack_top);
 *
 * Drops to CPL 3 at `entry` with RSP = user_stack_top.  Does not return
 * normally — control comes back through ring3_escape, whose argument becomes
 * this function's return value.
 */
.global ring3_run
ring3_run:
    /* Saved so ring3_escape can put the kernel back exactly as it was. */
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    movq %rsp, ring3_saved_rsp(%rip)

    /* An iretq frame is the only way into a lower privilege level: the CPU
     * pops RIP, CS, RFLAGS, RSP and SS, and the CPL comes from the CS RPL. */
    pushq $USER_SS
    pushq %rsi                  /* user RSP */
    pushq $USER_RFLAGS
    pushq $USER_CS
    pushq %rdi                  /* user RIP */
    iretq

/*
 * int64_t ring3_escape(uint64_t result, ...);
 *
 * Registered as the SYS_ESCAPE handler, so it is reached from ring 3 via the
 * normal syscall path and arrives with the probe's result in RDI (a1).
 * Never returns to its caller; unwinds to ring3_run's caller instead.
 */
.global ring3_escape
ring3_escape:
    movq %rdi, %rax             /* becomes ring3_run's return value */
    movq ring3_saved_rsp(%rip), %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret

/*
 * CHECK — compare one register against its sentinel, recording a bit in R11
 * on mismatch.  RCX and R11 are the only registers free to use as scratch
 * after a syscall, since those are the two the instruction destroys.
 */
.macro CHECK reg, sentinel, bit
    movabsq $\sentinel, %rcx
    cmpq %rcx, \reg
    je 1f
    orq $\bit, %r11
1:
.endm

/*
 * void ring3_probe(void);  — runs at CPL 3.
 *
 * Loads every register the ABI promises to preserve with a sentinel, issues
 * the echo syscall *twice*, and only then checks them.
 *
 * The second call is the point of the test rather than padding.  It is the
 * failure mode src/exo_syscall.h warns about: the stubs pass arguments as
 * plain inputs, so a compiler may leave a value sitting in an argument
 * register and reuse it on the next call.  An entry stub that saved only the
 * callee-saved set would pass the first check and fail here.
 *
 * Failures accumulate as a bitmask in R11 and travel out through
 * ring3_escape, so a failing CI run names the register rather than just
 * hanging.
 */
.global ring3_probe
ring3_probe:
    movabsq $S_RDI, %rdi
    movabsq $S_RSI, %rsi
    movabsq $S_RDX, %rdx
    movabsq $S_R10, %r10
    movabsq $S_R8,  %r8
    movabsq $S_R9,  %r9
    movabsq $S_RBX, %rbx
    movabsq $S_RBP, %rbp
    movabsq $S_R12, %r12
    movabsq $S_R13, %r13
    movabsq $S_R14, %r14
    movabsq $S_R15, %r15

    movq $SYS_ECHO, %rax
    syscall

    /* Not reloaded — the registers must still hold the sentinels. */
    movq $SYS_ECHO, %rax
    syscall

    xorq %r11, %r11             /* failure mask */

    /* Confirm this really is CPL 3 and not a ring-0 test wearing a costume.
     * Everything else here would pass identically at CPL 0, so without this
     * the suite could go green while proving nothing about the privilege
     * transition. */
    movq %cs, %rcx
    cmpq $USER_CS, %rcx
    je 3f
    orq $(1 << 13), %r11
3:

    /* The return value proves the dispatcher actually ran the handler,
     * rather than the path quietly returning -EXO_ENOSYS. */
    cmpq $ECHO_RET, %rax
    je 2f
    orq $(1 << 12), %r11
2:

    CHECK %rdi, S_RDI, (1 << 0)
    CHECK %rsi, S_RSI, (1 << 1)
    CHECK %rdx, S_RDX, (1 << 2)
    CHECK %r10, S_R10, (1 << 3)
    CHECK %r8,  S_R8,  (1 << 4)
    CHECK %r9,  S_R9,  (1 << 5)
    CHECK %rbx, S_RBX, (1 << 6)
    CHECK %rbp, S_RBP, (1 << 7)
    CHECK %r12, S_R12, (1 << 8)
    CHECK %r13, S_R13, (1 << 9)
    CHECK %r14, S_R14, (1 << 10)
    CHECK %r15, S_R15, (1 << 11)

    movq %r11, %rdi             /* result -> ring3_escape's first argument */
    movq $SYS_ESCAPE, %rax
    syscall

    /* Unreachable: SYS_ESCAPE does not come back. */
    ud2
