/*
 * fault_contain_probe.s — ring-3 blobs for the fault-containment suite
 * (SCRUM-283): a victim that takes a chosen fault, and a survivor that proves
 * the machine kept running by returning to the kernel.
 *
 * Same placement rules as kernel_mem_fault_probe.s: copied whole to
 * LIBOS_LAUNCH_CODE_VADDR, so no absolute data references; the jumps below
 * are relative within the blob and survive the copy. Preprocessed with cpp
 * (-DEXO_KERNEL), so the constants are the real ones.
 *
 * Victim data page (LIBOS_LAUNCH_DATA_VADDR): +0x00 selects the fault, set by
 * the test before launch:
 *   0  NULL read           (#PF, not-present)
 *   1  write to kernel     (#PF, protection)
 *   2  cli                 (#GP)
 *   3  hlt                 (#GP)
 *   4  in %dx, %al         (#GP, I/O bitmap denies)
 * No selector value is expected to fall through; the trailing ud2 only
 * bounds the copy and would turn a missed fault into a visible #UD.
 */

#include "exo_syscall.h"
#include "libos_launch.h"

.code64

.set SYS_LIBOS_RETURN, LIBOS_RETURN_SYSCALL_NUM
.set SURVIVOR_MARKER, 0x600DC0DE

.extern _load_start

.global fault_contain_victim
.global fault_contain_victim_end
fault_contain_victim:
    movabsq $LIBOS_LAUNCH_DATA_VADDR, %rax
    movq 0x00(%rax), %rax
    cmpq $0, %rax
    je  1f
    cmpq $1, %rax
    je  2f
    cmpq $2, %rax
    je  3f
    cmpq $3, %rax
    je  4f
    cmpq $4, %rax
    je  5f
    ud2
1:  xorl %eax, %eax
    movq (%rax), %rax              /* NULL read */
    ud2
2:  movabsq $_load_start, %rdi
    movq $0x5A5A5A5A, (%rdi)       /* write to supervisor-only kernel page */
    ud2
3:  cli
    ud2
4:  hlt
    ud2
5:  movw $0x80, %dx
    inb %dx, %al
    ud2
fault_contain_victim_end:
    ud2

.global fault_contain_survivor
.global fault_contain_survivor_end
fault_contain_survivor:
    movq $SURVIVOR_MARKER, %rdi
    movq $SYS_LIBOS_RETURN, %rax
    syscall
fault_contain_survivor_end:
    ud2
