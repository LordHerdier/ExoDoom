/*
 * libos_launch_probe.s — the code blob libos_build_image() copies into a
 * real LibOS address space and libos_enter() jumps into (SCRUM-47).
 *
 * Must be position-independent: libos_build_image() memcpy's these bytes to
 * LIBOS_LAUNCH_CODE_VADDR, a different address than this file links at, so
 * no internal jump/call and no RIP-relative data reference is allowed. A
 * fixed *immediate* address is fine either way -- its value does not depend
 * on where this blob itself ends up.
 *
 * The probe faults on an address inside its own LibOS window that nothing
 * mapped (LIBOS_LAUNCH_CODE_VADDR + 0x10000, well clear of the code and
 * stack pages at +0x1000/+0x2000) rather than on kernel memory. It would be
 * more direct to touch kernel memory instead, but TESTING builds
 * deliberately map the *entire* kernel identity range VMM_USER (src/vmm.c's
 * KERNEL_LEAF/KERNEL_MAP_USER) so tests/kernel/ring3_probe.s and
 * tss_fault_probe.s can execute against kernel .text/.bss at CPL 3 -- so a
 * kernel-memory read would not actually fault under the harness this test
 * itself runs in. vmm.c's own comment names the fix: SCRUM-55/56 tighten
 * KERNEL_MAP_USER back to supervisor-only once nothing still needs the
 * blanket exception, and assert the wall for real. This probe proves the
 * launch mechanism -- real address space, real CPL 3 execution, a fault
 * caught and the machine kept running -- which is what makes that later
 * tightening safe to test against.
 *
 * `probe_end` bounds the copy the same way fault_probe.s's resume labels
 * bound a single instruction: nothing else lives in this file, so nothing
 * else can be reordered in between by the assembler.
 */

.code64

.set SYS_LIBOS_RETURN, 20     /* EXO_SYS_EXIT, borrowed -- see ring3_probe.s */
.set RESULT_MARKER, 0x600DC0DE

/* EXO_USER_VA_BASE (src/exo_syscall.h) + an offset well clear of the code
 * and stack pages libos_build_image() maps at +0x1000/+0x2000. A fixed
 * immediate, not computed from this blob's own address -- must match
 * LIBOS_LAUNCH_PROBE_FAULT_VADDR in test_libos_launch_k.c. */
.set FAULT_VA, 0x400000000000 + 0x20000

.global libos_launch_probe
.global libos_launch_probe_resume
.global libos_launch_probe_end
libos_launch_probe:
    movabsq $FAULT_VA, %rdi
    movq (%rdi), %rdi            /* faults: not-present, CPL 3 */
libos_launch_probe_resume:
    movq $RESULT_MARKER, %rdi   /* proves this point was actually reached */
    movq $SYS_LIBOS_RETURN, %rax
    syscall
libos_launch_probe_end:
    ud2                          /* unreachable: SYS_LIBOS_RETURN does not
                                  * come back, and this label's only job is
                                  * to bound the copy above. */
