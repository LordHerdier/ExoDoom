/*
 * sound_port_probe.s — ring-3 code for test_syscall_sound_k.c (SCRUM-100).
 *
 * Tries to program the PC speaker directly, every way a LibOS might: set
 * PIT channel 2's mode (0x43), load its divisor (0x42), and read-modify-
 * write the speaker gate (0x61). Each of those four instructions must take
 * #GP at CPL 3 (IOPL 0, no I/O permission bitmap -- src/tss.c); the test's
 * fault hook counts them and steps RIP past each one. Every one is the
 * 2-byte imm8-port form (E4/E6 ib) precisely so the hook can skip them with
 * a fixed `rip += 2`. Consecutive port instructions are always separated by
 * one that cannot fault: gp_fault_handler() refuses to call the hook for a
 * fault at the very RIP it last resumed to (its re-fault-loop guard), so two
 * faulting instructions back to back would halt the machine.
 *
 * Then it asks the right way -- exo_sound_tone(440, 1000) through the real
 * `syscall` instruction -- and hands that syscall's result back through
 * libos_return(), so the test sees the genuine return value.
 *
 * Position-independent (copied to LIBOS_LAUNCH_CODE_VADDR by
 * libos_build_image()): no internal jump/call, no RIP-relative reference.
 */

#include "exo_syscall.h"
#include "libos_launch.h"

.code64

.set SYS_LIBOS_RETURN, LIBOS_RETURN_SYSCALL_NUM
.set SYS_SOUND_TONE,   EXO_SYS_SOUND_TONE

.global sound_port_probe
.global sound_port_probe_end
sound_port_probe:
    movb $0xB6, %al
    outb %al, $0x43               /* #GP: channel 2 mode */
    movb $0x10, %al
    outb %al, $0x42               /* #GP: channel 2 divisor */
    nop
    inb  $0x61, %al               /* #GP: read speaker gate */
    orb  $0x03, %al
    outb %al, $0x61               /* #GP: open speaker gate */

    movq $SYS_SOUND_TONE, %rax
    movq $440, %rdi
    movq $1000, %rsi
    syscall

    movq %rax, %rdi               /* exo_sound_tone's real return value */
    movq $SYS_LIBOS_RETURN, %rax
    syscall
sound_port_probe_end:
