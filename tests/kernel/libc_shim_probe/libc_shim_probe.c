/*
 * libc_shim_probe.c — a compiled libc shim running at ring 3 (SCRUM-51).
 *
 * SCRUM-173 proved that *some* compiled C, linked at LIBOS_LAUNCH_CODE_VADDR/
 * _DATA_VADDR by its own dedicated build.sh step, can call a real bound
 * syscall from ring 3 and return its result through libos_return() (see
 * tests/kernel/libos_c_probe/). This file reuses that exact same mechanism
 * -- the same two-region linker script shape, the same objcopy-and-embed
 * pipeline -- to prove the specific thing SCRUM-51's acceptance criterion
 * asks for: malloc, printf and the timer all work from ring 3 through the
 * real `syscall` instruction, not just a single hand-picked syscall.
 *
 * This links in the *actual* libc shim sources (src/stdlib.c, src/stdio.c,
 * src/string.c, src/ctype.c) plus the LibOS allocators they now sit on
 * (src/libos_heap.c, src/libos_page_alloc.c) -- compiled WITHOUT -DEXO_KERNEL,
 * the same #ifdef switch src/exo_syscall.h already uses, so:
 *
 *   - malloc/free (src/stdlib.c) resolve to libos_heap_alloc/_free
 *     (src/libos_heap.c, SCRUM-38), which get pages from libos_page_alloc()
 *     (src/libos_page_alloc.c, SCRUM-37), which under !EXO_KERNEL calls the
 *     real exo_page_alloc()/exo_page_map() inline `syscall` stubs instead of
 *     the in-process exo_syscall_dispatch() call it uses when linked into
 *     the kernel for its own unit tests.
 *   - printf (src/stdio.c) buffers a full line and flushes it through the
 *     real exo_serial_write() `syscall` stub in one call, rather than one
 *     exo_serial_write per character.
 *   - exo_get_ticks() (already bound under SCRUM-172) is called directly --
 *     nothing to port there, just something to prove from ring 3 too.
 *   - src/libos_fb.c (SCRUM-36) composes exo_fb_acquire()/exo_page_map()
 *     into one call and is exercised the same way: a real framebuffer
 *     mapping obtained and written to from ring 3.
 *
 * test_libc_shim_probe_k.c drives this the same way test_libos_c_probe_k.c
 * drives libos_c_probe_main(): libos_build_image() + libos_test_launch(),
 * then asserts on the bitmask this function hands back through
 * libos_return() rather than trusting "it didn't fault" alone.
 */

#include "exo_syscall.h"
#include "libos_launch.h"
#include "libos_fb.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libc_shim_probe_result.h"

/* .data, not .rodata -- same reasoning as libos_c_probe_msg: proves the
 * compiled shim's own .data region loads correctly at
 * LIBOS_LAUNCH_DATA_VADDR, not just its .text. */
static char libc_shim_probe_msg[] = "hello from the libc shim\n";

void libc_shim_probe_main(void)
{
    uint32_t result = 0;

    /* malloc -> libos_heap_alloc -> libos_page_alloc -> real
     * exo_page_alloc()/exo_page_map() syscalls. Written through and read
     * back via string.c's strcat/strcmp (also linked into this probe) so a
     * corrupt or aliased allocation shows up as a content mismatch, not just
     * a NULL check. */
    char *buf = malloc(64);
    if (buf != NULL) {
        memset(buf, 0, 64);
        strcat(buf, "malloc works");
        if (strcmp(buf, "malloc works") == 0)
            result |= LIBC_SHIM_OK_MALLOC;
        free(buf);
    }

    /* printf -> the real exo_serial_write() syscall, one flush for the
     * whole line. Comparing the returned count against the known message
     * length (minus stdio.c's trailing-NUL-free format) proves the syscall
     * actually ran and reported a real length, not a fixed placeholder. */
    int n = printf("%s", libc_shim_probe_msg);
    if (n == (int)(sizeof(libc_shim_probe_msg) - 1))
        result |= LIBC_SHIM_OK_PRINTF;

    /* exo_get_ticks() -- SCRUM-172 already proved the handler from kernel
     * context; this proves the same real `syscall` reaches it from ring 3.
     * test_libc_shim_probe_k.c enables interrupts and waits briefly before
     * launching this code specifically so ticks are guaranteed nonzero by
     * the time this runs -- see that file's comment. */
    int64_t ticks = exo_get_ticks();
    if (ticks > 0)
        result |= LIBC_SHIM_OK_TICKS;

    /* libos_fb_map() -> exo_fb_acquire() + a loop of exo_page_map() real
     * syscalls, mapping the framebuffer into this address space. Writing a
     * known pattern to the first and last pixel of the mapped range and
     * reading it straight back proves the mapping is genuinely writable,
     * not just present -- test_libc_shim_probe_k.c independently confirms
     * the bytes landed in the real framebuffer by reading the same offsets
     * back through the kernel's own identity-mapped view once this probe
     * returns. */
    libos_fb_t fb;
    if (libos_fb_map(&fb) == 0 && fb.vaddr != NULL && fb.height > 0) {
        volatile uint32_t *first = (volatile uint32_t *)fb.vaddr;
        volatile uint32_t *last =
            (volatile uint32_t *)((uint8_t *)fb.vaddr +
                                  (uint64_t)fb.pitch * (fb.height - 1));

        *first = 0xDEADBEEFu;
        *last  = 0xCAFEF00Du;

        if (*first == 0xDEADBEEFu && *last == 0xCAFEF00Du)
            result |= LIBC_SHIM_OK_FB;
    }

    /* LIBOS_RETURN_SYSCALL_NUM has no named wrapper -- see libos_c_probe.c's
     * identical comment. */
    exo_syscall1(LIBOS_RETURN_SYSCALL_NUM, (uint64_t)result);

    /* Unreachable -- see libos_c_probe.c's identical comment. */
    for (;;) { }
}
