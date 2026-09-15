/*
 * context_switch_probe_a.s — SCRUM-108 context switch test probe, side A.
 *
 * Launched by test_context_switch_k.c via the ordinary kernel-mode
 * libos_enter() (same as every other ring-3 launch test). Seeds known
 * values into the SysV callee-saved registers, writes a marker to its own
 * data page, then calls the test-registered SYS_SWITCH syscall (borrowing
 * EXO_SYS_YIELD -- unbound in production today, exactly the way
 * libos_return borrows EXO_SYS_EXIT elsewhere in this tree) naming side B's
 * context id as its argument. Control returns here -- at the instruction
 * right after `syscall`, the same way an ordinary syscall returns, not via
 * iretq -- once B has run and switched back. Writes the post-resume
 * register values and a second marker, then exits via the ordinary
 * libos_return()/EXO_SYS_EXIT borrow to unwind back out to the KUnit test's
 * own libos_enter() call.
 *
 * Data layout at LIBOS_LAUNCH_DATA_VADDR (filled in / read back by the test
 * via the physical page, the same technique test_libos_launch_k.c's
 * test_build_image_places_code_and_data uses):
 *   +0x00  peer_id   (in: side B's context id, set by the test before launch)
 *   +0x08  mark1     (out: MARK1, written before the switch)
 *   +0x10  rbx_after (out: should read back as SEED_RBX)
 *   +0x18  rbp_after (out: should read back as SEED_RBP)
 *   +0x20  r12_after (out: should read back as SEED_R12)
 *   +0x28  mark2     (out: MARK2, written after resuming)
 *
 * Must be position-independent per libos_launch.h's convention: every data
 * reference uses the fixed immediate LIBOS_LAUNCH_DATA_VADDR, never an
 * internal RIP-relative offset from wherever this file happens to link.
 */

#include "exo_syscall.h"
#include "libos_launch.h"

.code64

.set SYS_SWITCH, EXO_SYS_YIELD
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

.global context_switch_probe_a
.global context_switch_probe_a_end
context_switch_probe_a:
    movabsq $SEED_RBX, %rbx
    movabsq $SEED_RBP, %rbp
    movabsq $SEED_R12, %r12
    movabsq $SEED_R13, %r13
    movabsq $SEED_R14, %r14
    movabsq $SEED_R15, %r15

    movabsq $LIBOS_LAUNCH_DATA_VADDR, %rax
    movabsq $MARK1, %rdx
    movq %rdx, 0x08(%rax)

    movq 0x00(%rax), %rdi      /* peer_id -> SYS_SWITCH's argument */
    movq $SYS_SWITCH, %rax
    syscall
    /* Resumes here once B has run and switched back -- an ordinary syscall
     * return, the same as any other syscall this code makes; nothing about
     * getting back to this exact point is special-cased. */

    movabsq $LIBOS_LAUNCH_DATA_VADDR, %rax
    movq %rbx, 0x10(%rax)
    movq %rbp, 0x18(%rax)
    movq %r12, 0x20(%rax)
    movabsq $MARK2, %rdx
    movq %rdx, 0x28(%rax)

    movq $RESULT_MARKER, %rdi
    movq $SYS_LIBOS_RETURN, %rax
    syscall
context_switch_probe_a_end:
    ud2                          /* unreachable: SYS_LIBOS_RETURN does not
                                  * come back, and this label's only job is
                                  * to bound the copy above. */
