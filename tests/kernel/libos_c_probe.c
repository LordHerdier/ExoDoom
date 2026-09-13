/*
 * libos_c_probe.c — a compiled (not hand-assembled) ring-3 probe (SCRUM-173).
 *
 * Every previous ring-3 probe in this tree (ring3_probe.s, libos_launch_probe.s,
 * libos_main_probe.s) is hand-written assembly following a strict manual
 * convention: no internal RIP-relative reference to anything outside the code
 * blob, because libos_build_image() (src/libos_launch.c, SCRUM-49) copies raw
 * bytes verbatim to LIBOS_LAUNCH_CODE_VADDR/_DATA_VADDR with no relocation
 * processing, and those probes were originally compiled/linked as part of the
 * kernel image at kernel addresses (2M) before being copied elsewhere.
 *
 * This file sidesteps that convention instead of following it: docker/scripts/
 * build.sh links it *separately*, with its own linker script
 * (libos_c_probe.ld.in) that places .text at the real LIBOS_LAUNCH_CODE_VADDR
 * and .data/.bss at the real LIBOS_LAUNCH_DATA_VADDR -- the exact addresses
 * libos_build_image() will map it to. Because the compiler and linker already
 * see the address this code will actually run at, ordinary absolute and
 * RIP-relative addressing (a normal global array reference, here) resolves
 * correctly with no hand-written workaround. That is the whole point of a
 * separate LibOS link target: proving a *compiled* libc shim (SCRUM-51) can
 * run at ring 3 does not require rewriting it in assembly.
 *
 * build.sh extracts the resulting .text and .data bytes with objcopy and
 * embeds them as ordinary blobs in the kernel image (see its own comments);
 * test_libos_c_probe_k.c drives libos_build_image()/libos_enter() against
 * those blobs exactly like test_libos_main_k.c does against the hand-written
 * probe, and asserts the same thing: a real, bound syscall (exo_serial_write,
 * #8) called from the launched code returns its real result through
 * libos_return(), not a fixed marker.
 *
 * #undef EXO_KERNEL for the same reason test_exo_syscall_k.c does: this file
 * wants the LibOS view of exo_syscall.h (the inline `syscall`-instruction
 * stubs), not the kernel view docker/scripts/build.sh's -DEXO_KERNEL
 * otherwise selects for every TU it compiles.
 */
#undef EXO_KERNEL

#include "exo_syscall.h"
#include "libos_launch.h"

/* .data, not .rodata: proves the *data* region libos_build_image() places at
 * LIBOS_LAUNCH_DATA_VADDR loads correctly too, not just .text. */
char libos_c_probe_msg[] = "hello from compiled ring 3\n";

void libos_c_probe_main(void)
{
    int64_t result = exo_serial_write(libos_c_probe_msg,
                                      sizeof(libos_c_probe_msg) - 1);

    /* LIBOS_RETURN_SYSCALL_NUM has no named wrapper (it is a test-only
     * borrow of EXO_SYS_EXIT, see libos_launch.h) -- exo_syscall1 is the
     * same raw stub every named wrapper in exo_syscall.h is built from. */
    exo_syscall1(LIBOS_RETURN_SYSCALL_NUM, (uint64_t)result);

    /* Unreachable: LIBOS_RETURN_SYSCALL_NUM's handler unwinds the kernel
     * straight back into whatever called libos_enter() and never returns
     * control here. Guards against a `ret` into whatever garbage is above
     * this frame if that assumption is ever wrong. */
    for (;;) { }
}
