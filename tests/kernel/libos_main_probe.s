/*
 * libos_main_probe.s — the libos_main() entry-framework demonstration
 * (SCRUM-50).
 *
 * Where libos_launch_probe.s (SCRUM-47/-49) only proves the CR3 switch,
 * CPL-3 execution and fault handling, this probe proves the thing SCRUM-50
 * actually adds: code running at LIBOS_LAUNCH_CODE_VADDR issuing a real,
 * bound syscall -- exo_serial_write (#8) -- against data placed in the
 * *data* region libos_build_image() maps separately (SCRUM-49), then
 * escaping the same way every launch probe in this tree does.
 *
 * Preprocessed with the C preprocessor before assembly
 * (docker/scripts/build.sh, `-x assembler-with-cpp` + `-DEXO_KERNEL`), so
 * DATA_VADDR/SYS_SERIAL_WRITE/SYS_LIBOS_RETURN below are the real
 * LIBOS_LAUNCH_DATA_VADDR / EXO_SYS_SERIAL_WRITE / LIBOS_RETURN_SYSCALL_NUM
 * macros, not hand-copied literals -- exactly the drift libos_launch_probe.s
 * used to be exposed to before SCRUM-50 (see that file's history).
 *
 * Same PIC rules as libos_launch_probe.s: no internal jump/call, no
 * RIP-relative reference to anything that moves. libos_main_probe_data is
 * read at *build* time by libos_build_image() (the kernel is still on its
 * own CR3 then, so this file's normal, kernel-image-linked address for it
 * works fine as the `data` source argument) but is never referenced by
 * *address* from inside the probe itself -- the probe only ever names the
 * fixed immediate DATA_VADDR, the address the bytes are copied *to*, per
 * libos_launch.h's convention for referencing the data region.
 */

#include "exo_syscall.h"
#include "libos_launch.h"

.code64

.set SYS_SERIAL_WRITE, EXO_SYS_SERIAL_WRITE
.set SYS_LIBOS_RETURN, LIBOS_RETURN_SYSCALL_NUM
.set DATA_VADDR, LIBOS_LAUNCH_DATA_VADDR

.global libos_main_probe
.global libos_main_probe_end
.global libos_main_probe_data
.global libos_main_probe_data_end
libos_main_probe:
    movabsq $DATA_VADDR, %rdi
    movq    $(libos_main_probe_data_end - libos_main_probe_data), %rsi
    movq    $SYS_SERIAL_WRITE, %rax
    syscall
    /* %rax is the real serial-write result (bytes written, or a negative
     * -EXO_E* if something above is wrong) -- hand it back as-is so the
     * test can assert the actual syscall outcome, not a fixed marker. */
    movq    %rax, %rdi
    movq    $SYS_LIBOS_RETURN, %rax
    syscall
libos_main_probe_end:
    ud2                          /* unreachable: SYS_LIBOS_RETURN does not
                                  * come back, and this label's only job is
                                  * to bound the copy above. */

.section .rodata
libos_main_probe_data:
    .ascii "hello from ring 3\n"
libos_main_probe_data_end:

.section .text
