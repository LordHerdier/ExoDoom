/*
 * libos_hello.c — the smallest complete ExoDoom program (SCRUM-238).
 *
 * docs/libos-authoring.md walks through this file; it is built and packaged
 * as hello.exo by docker/scripts/build.sh so that walkthrough is compiled and
 * run, not just written. It maps its framebuffer, prints two lines, then
 * prints the key code of every key pressed until Q or Esc.
 *
 * No libc shim is linked in: the framebuffer console is its only output.
 */

#include "exo_syscall.h"
#include "libos_fb.h"
#include "fb.h"
#include "fb_console.h"
#include "ps2.h"   /* ps2_key_t values (KEY_Q, KEY_ESC) only */

#include <stdint.h>

__attribute__((section(".text.entry")))
void libos_hello_main(void);

/* .data, not .rodata: build_ring3_link_target's objcopy step fails on an
 * empty .data, and this program has no other writable global to fill it. */
static char hello_text[] =
    "Hello from a LibOS.\nPress keys; Q or Esc quits.\n";

static void write_u32(fb_console_t *con, uint32_t val)
{
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + val % 10);
        val /= 10;
    } while (val > 0);
    fbcon_write(con, &buf[i]);
}

/* exo_exit (#20) does not return here: the kernel reclaims everything this
 * context holds and switches to the next ready one. The loop is for the
 * compiler, which cannot know that. */
static void hello_exit(void)
{
    exo_syscall1(EXO_SYS_EXIT, 0);
    for (;;) { exo_yield(); }
}

__attribute__((section(".text.entry")))
void libos_hello_main(void)
{
    libos_fb_t lfb;
    framebuffer_t fb;
    fb_console_t con;

    if (libos_fb_map(&lfb) != 0 ||
        !fb_init_bgrx8888(&fb, (uintptr_t)lfb.vaddr, lfb.pitch, lfb.width,
                          lfb.height, lfb.bpp) ||
        !fbcon_init(&con, &fb)) {
        hello_exit();
    }

    fbcon_write(&con, hello_text);

    for (;;) {
        exo_kbd_event_t ev;
        if (exo_kbd_poll(&ev) <= 0) {
            /* Every syscall runs with interrupts off, so polling back to
             * back starves the 1 kHz timer. Spinning on `pause` between
             * polls costs no syscall (libos_clock.c measured the
             * difference). */
            for (volatile uint32_t spin = 0; spin < 20000; spin++) {
                __asm__ volatile("pause");
            }
            continue;
        }
        if (!ev.pressed) {
            continue;
        }
        if (ev.key == KEY_Q || ev.key == KEY_ESC) {
            hello_exit();
        }
        fbcon_write(&con, "key ");
        write_u32(&con, ev.key);
        fbcon_write(&con, "\n");
    }
}
