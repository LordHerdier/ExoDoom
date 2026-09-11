/*
 * tss_fault_probe.s — a deliberate fault taken from CPL 3 (SCRUM-46).
 *
 * Launched the same way tests/kernel/ring3_probe.s launches ring3_probe:
 * ring3_run() (defined there, generic over the entry point) drops to CPL 3
 * at tss_fault_probe and gives it a fresh stack; getting home again is the
 * SYS_ESCAPE syscall, handled by ring3_escape.
 *
 * The point of this probe is narrower than ring3_probe's: it doesn't care
 * about register preservation, only about surviving a #PF taken while
 * already at CPL 3. Before tss_init() loaded a TSS with a valid RSP0, this
 * exact sequence triple-faulted QEMU (see fault.c's comment) -- the CPU
 * has nowhere to build the exception frame. The TESTING fault hook (see
 * test_tss_k.c) resumes execution at the label below instead of halting,
 * so a passing run here is a passing run of the CPU's own privilege-change
 * stack switch, not a re-implementation of it.
 */

.code64

.set SYS_ESCAPE, 20          /* EXO_SYS_EXIT, borrowed -- see ring3_probe.s */

/* Inside the LibOS window (EXO_USER_VA_BASE = 64 TiB) and guaranteed
 * unmapped, same reasoning as test_fault_k.c's FAULT_VA -- just a different
 * offset so the two probes' addresses never collide if a test ever compares
 * them. */
.set FAULT_VA, 0x400000000000 + 0x9000

.global tss_fault_probe
.global tss_fault_probe_resume
tss_fault_probe:
    movq $FAULT_VA, %rdi
    movq $0x5A5A5A5A, (%rdi)    /* faults: not-present, write, CPL 3 */
tss_fault_probe_resume:
    xorq %rdi, %rdi             /* result = 0: reached the resume label */
    movq $SYS_ESCAPE, %rax
    syscall

    /* Unreachable: SYS_ESCAPE does not come back. */
    ud2
