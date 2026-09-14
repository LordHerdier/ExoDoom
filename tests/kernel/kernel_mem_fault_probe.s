/*
 * kernel_mem_fault_probe.s — the code blob libos_build_image() copies into a
 * real LibOS address space to prove ring-3 kernel-memory access faults
 * (SCRUM-55).
 *
 * Must be position-independent, same rule as libos_launch_probe.s: no
 * internal jump/call and no RIP-relative data reference, since
 * libos_build_image() memcpy's these bytes to LIBOS_LAUNCH_CODE_VADDR, a
 * different address than this file links at. A fixed *immediate* is fine
 * either way -- including one resolved by the linker, like `_load_start`
 * below: its value is an absolute kernel address that means the same thing
 * regardless of where these bytes end up running from.
 *
 * Preprocessed with the C preprocessor before assembly (SCRUM-50 convention,
 * docker/scripts/build.sh, `-x assembler-with-cpp` + `-DEXO_KERNEL`), so
 * SYS_LIBOS_RETURN below is the real LIBOS_RETURN_SYSCALL_NUM, not a
 * hand-copied literal that could silently drift from exo_syscall.h.
 *
 * Targets `_load_start` (src/linker.ld's kernel image base, exported by
 * src/vmm.c and every ring-3 launch suite already) rather than an arbitrary
 * kernel address: it is guaranteed present (the whole kernel image is
 * identity-mapped at 4 KiB granularity, src/vmm.c's vmm_init() step 2) and,
 * since SCRUM-55, supervisor-only everywhere except the two legacy ring-3
 * probes' own narrow code ranges (src/vmm.c's expose_ring3_legacy_probes())
 * -- this file is not one of them, so both a read and a write here are a
 * *protection* fault (PF_ERR_PRESENT set), not a not-present one. That
 * distinction is the whole point: it proves the kernel disallows the access
 * rather than coincidentally having nothing mapped there.
 */

#include "exo_syscall.h"
#include "libos_launch.h"

.code64

.set SYS_LIBOS_RETURN, LIBOS_RETURN_SYSCALL_NUM
.set RESULT_MARKER, 0xFEEDFACE

.extern _load_start

.global kernel_mem_fault_probe
.global kernel_mem_fault_probe_read_resume
.global kernel_mem_fault_probe_write_resume
.global kernel_mem_fault_probe_end
kernel_mem_fault_probe:
    movabsq $_load_start, %rdi
    movq (%rdi), %rax             /* faults: protection, read, CPL 3 */
kernel_mem_fault_probe_read_resume:
    movabsq $_load_start, %rdi
    movq $0x5A5A5A5A, (%rdi)      /* faults: protection, write, CPL 3 */
kernel_mem_fault_probe_write_resume:
    movq $RESULT_MARKER, %rdi    /* proves this point was actually reached */
    movq $SYS_LIBOS_RETURN, %rax
    syscall
kernel_mem_fault_probe_end:
    ud2                           /* unreachable: SYS_LIBOS_RETURN does not
                                   * come back, and this label's only job is
                                   * to bound the copy above. */
