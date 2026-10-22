/*
 * rtc_probe.s — ring-3 code for test_rtc_k.c (SCRUM-249).
 *
 * Three real `syscall`s for EXO_SYS_RTC_READ, each against the LibOS's own
 * CR3:
 *   1. a buffer in the image's data blob (valid, mapped, writable) -> 0
 *   2. NULL                                                       -> -EFAULT
 *   3. 0x200000, the kernel image (identity-mapped user-accessible in a
 *      RING3_PROBE build, so only the handler's window check stops it)
 *                                                                 -> -EFAULT
 * The result is a bitmask handed back through libos_return(): bit 0 set if
 * call 1 returned 0, bit 1 if call 2 returned -EXO_EFAULT, bit 2 if call 3
 * did. All three good is 7. A fault instead of an error never gets this far.
 *
 * Position-independent, as sound_stream_probe.s: no internal jump/call, no
 * RIP-relative reference (the SCRUM-50 entry convention) -- hence setcc
 * rather than branches.
 */

#include "exo_syscall.h"
#include "libos_launch.h"

.code64

.set SYS_LIBOS_RETURN,   LIBOS_RETURN_SYSCALL_NUM
.set SYS_RTC_READ,       EXO_SYS_RTC_READ
.set KERNEL_PTR,         0x200000
.set NEG_EFAULT,         -EXO_EFAULT

.global rtc_probe
.global rtc_probe_end
rtc_probe:
    movq $SYS_RTC_READ, %rax
    movq $LIBOS_LAUNCH_DATA_VADDR, %rdi
    syscall
    movq %rax, %r12

    movq $SYS_RTC_READ, %rax
    xorl %edi, %edi
    syscall
    movq %rax, %r13

    movq $SYS_RTC_READ, %rax
    movq $KERNEL_PTR, %rdi
    syscall
    movq %rax, %r14

    xorl %edi, %edi

    cmpq $0, %r12
    sete %al
    movzbl %al, %eax
    orq %rax, %rdi

    cmpq $NEG_EFAULT, %r13
    sete %al
    movzbl %al, %eax
    shlq $1, %rax
    orq %rax, %rdi

    cmpq $NEG_EFAULT, %r14
    sete %al
    movzbl %al, %eax
    shlq $2, %rax
    orq %rax, %rdi

    movq $SYS_LIBOS_RETURN, %rax
    syscall
rtc_probe_end:
