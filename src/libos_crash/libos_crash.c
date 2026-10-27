/*
 * libos_crash.c — a program that crashes on purpose (SCRUM-283).
 *
 * `run crash` prints a line and then reads address 0. The kernel ends it (a
 * ring-3 page fault, src/fault.c), switches to the next ready context and the
 * shell reports "program crash stopped: illegal memory access". It exists so
 * that containment can be seen on a real boot, not only in the test suite;
 * docs/demo.md uses it for the "crash a program, the machine survives" beat.
 *
 * Same shape as libos_hello.c, no libc shim.
 */

#include "exo_syscall.h"
#include "libos_fb.h"
#include "fb.h"
#include "fb_console.h"

#include <stdint.h>

__attribute__((section(".text.entry")))
void libos_crash_main(void);

/* .data, not .rodata: build_ring3_link_target's objcopy step fails on an
 * empty .data (see libos_hello.c). */
static char crash_text[] = "About to read address 0 ...\n";

__attribute__((section(".text.entry")))
void libos_crash_main(void)
{
    libos_fb_t lfb;
    framebuffer_t fb;
    fb_console_t con;

    if (libos_fb_map(&lfb) == 0 &&
        fb_init_bgrx8888(&fb, (uintptr_t)lfb.vaddr, lfb.pitch, lfb.width,
                         lfb.height, lfb.bpp) &&
        fbcon_init(&con, &fb)) {
        fbcon_write(&con, crash_text);
    }

    /* volatile so the load is not optimised away; address 0 is the unmapped
     * NULL guard page. */
    volatile uint64_t *null_page = (volatile uint64_t *)0;
    uint64_t v = *null_page;

    /* Not reached. If it somehow is, leave through the front door. */
    exo_syscall1(EXO_SYS_EXIT, v);
    for (;;) { exo_yield(); }
}
