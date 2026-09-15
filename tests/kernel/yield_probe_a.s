/*
 * yield_probe_a.s — SCRUM-109 real exo_yield syscall test probe, side A.
 *
 * Launched by test_syscall_yield_k.c via the ordinary kernel-mode
 * libos_enter(), same as context_switch_probe_a.s (SCRUM-108) that this
 * probe is adapted from. Unlike that probe, this one calls the *real* bound
 * exo_yield syscall with **no argument** -- the real ABI
 * (docs/syscall_spec.md §3.2 #19) takes none; which context resumes is
 * decided by the kernel's own round-robin policy (context_next_ready(),
 * src/context.c), not named by the caller. Seeds known values into the
 * SysV callee-saved registers, writes a marker to its own data page, then
 * calls exo_yield(). Control returns here -- at the instruction right after
 * `syscall`, the same way an ordinary syscall returns -- once B has run and
 * yielded back. Writes the post-resume register values and a second
 * marker, then exits via the ordinary libos_return()/EXO_SYS_EXIT borrow to
 * unwind back out to the KUnit test's own libos_enter() call.
 *
 * Data layout at LIBOS_LAUNCH_DATA_VADDR (filled in / read back by the test
 * via the physical page, same technique test_libos_launch_k.c's
 * test_build_image_places_code_and_data uses):
 *   +0x00  mark1     (out: MARK1, written before yielding)
 *   +0x08  rbx_after (out: should read back as SEED_RBX)
 *   +0x10  rbp_after (out: should read back as SEED_RBP)
 *   +0x18  r12_after (out: should read back as SEED_R12)
 *   +0x20  r13_after (out: should read back as SEED_R13)
 *   +0x28  r14_after (out: should read back as SEED_R14)
 *   +0x30  r15_after (out: should read back as SEED_R15)
 *   +0x38  mark2     (out: MARK2, written after resuming)
 *
 * Must be position-independent per libos_launch.h's convention: every data
 * reference uses the fixed immediate LIBOS_LAUNCH_DATA_VADDR, never an
 * internal RIP-relative offset from wherever this file happens to link.
 */

#include "exo_syscall.h"
#include "libos_launch.h"

.code64

.set SYS_LIBOS_RETURN, LIBOS_RETURN_SYSCALL_NUM

.set SEED_RBX, 0x1111111111111111
.set SEED_RBP, 0x2222222222222222
.set SEED_R12, 0x3333333333333333
.set SEED_R13, 0x4444444444444444
.set SEED_R14, 0x5555555555555555
.set SEED_R15, 0x6666666666666666

.set MARK1, 0xA000000000000001
.set MARK2, 0xA000000000000002
.set RESULT_MARKER, 0x600DC0DE

.global yield_probe_a
.global yield_probe_a_end
yield_probe_a:
    movabsq $SEED_RBX, %rbx
    movabsq $SEED_RBP, %rbp
    movabsq $SEED_R12, %r12
    movabsq $SEED_R13, %r13
    movabsq $SEED_R14, %r14
    movabsq $SEED_R15, %r15

    movabsq $LIBOS_LAUNCH_DATA_VADDR, %rax
    movabsq $MARK1, %rdx
    movq %rdx, 0x00(%rax)

    movq $EXO_SYS_YIELD, %rax
    syscall
    /* Resumes here once B has run and yielded back -- an ordinary syscall
     * return, the same as any other syscall this code makes; nothing about
     * getting back to this exact point is special-cased. */

    movabsq $LIBOS_LAUNCH_DATA_VADDR, %rax
    movq %rbx, 0x08(%rax)
    movq %rbp, 0x10(%rax)
    movq %r12, 0x18(%rax)
    movq %r13, 0x20(%rax)
    movq %r14, 0x28(%rax)
    movq %r15, 0x30(%rax)
    movabsq $MARK2, %rdx
    movq %rdx, 0x38(%rax)

    movq $RESULT_MARKER, %rdi
    movq $SYS_LIBOS_RETURN, %rax
    syscall
yield_probe_a_end:
    ud2                          /* unreachable: SYS_LIBOS_RETURN does not
                                  * come back, and this label's only job is
                                  * to bound the copy above. */
