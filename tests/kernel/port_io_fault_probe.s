/*
 * port_io_fault_probe.s — the code blob libos_build_image() copies into a
 * real LibOS address space to prove ring-3 port I/O traps #GP (SCRUM-56).
 *
 * Must be position-independent, same rule as libos_launch_probe.s: no
 * internal jump/call and no RIP-relative data reference, since
 * libos_build_image() memcpy's these bytes to LIBOS_LAUNCH_CODE_VADDR, a
 * different address than this file links at. A fixed *immediate* is fine.
 *
 * Preprocessed with the C preprocessor before assembly (SCRUM-50 convention,
 * docker/scripts/build.sh, `-x assembler-with-cpp` + `-DEXO_KERNEL`), so
 * SYS_LIBOS_RETURN below is the real LIBOS_RETURN_SYSCALL_NUM, not a
 * hand-copied literal that could silently drift from exo_syscall.h.
 *
 * `outb %al, $0x80` writes to the POST diagnostic / I/O-delay port -- picked
 * because it is never read back and has no observable side effect on real
 * hardware or under QEMU, unlike a PIC/PIT/keyboard controller port. The
 * fault happens before the write ever reaches the port: tss_init() (SCRUM-46,
 * src/tss.c) points TSS.iomap_base one byte past the TSS segment limit, so
 * any port access below IOPL reads as "no permission bitmap present" and
 * traps #GP at CPL 3 regardless of which port is named or what value is
 * loaded into %al.
 */

#include "exo_syscall.h"
#include "libos_launch.h"

.code64

.set SYS_LIBOS_RETURN, LIBOS_RETURN_SYSCALL_NUM
.set RESULT_MARKER, 0xBADD0170

.global port_io_fault_probe
.global port_io_fault_probe_resume
.global port_io_fault_probe_end
port_io_fault_probe:
    movb $0, %al
    outb %al, $0x80               /* faults: #GP, no I/O bitmap at CPL 3 */
port_io_fault_probe_resume:
    movq $RESULT_MARKER, %rdi    /* proves this point was actually reached */
    movq $SYS_LIBOS_RETURN, %rax
    syscall
port_io_fault_probe_end:
    ud2                           /* unreachable: SYS_LIBOS_RETURN does not
                                   * come back, and this label's only job is
                                   * to bound the copy above. */
