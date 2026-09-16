/*
 * doom_panic.c — I_Error / I_Quit back end (SCRUM-83).
 *
 * See src/doom_panic.h for what this is and why it is not in the vendored
 * tree.  This file is about the two things that are easy to get wrong:
 * getting the bytes out, and not coming back.
 */

#include "doom_panic.h"

#include "stdio.h" /* vprintf, printf -- one formatting engine, one sink */

#ifdef EXO_KERNEL
#include "serial.h"
#else
#include "exo_syscall.h"
/*
 * The kernel's own cap on one exo_serial_write (src/syscall_serial.h).  That
 * header is kernel-only and src/stdio.c already declines to include it from
 * the ring-3 side for exactly this reason, so the limit is restated rather
 * than a ring-3 TU reaching into a ring-0 header.  If the two ever disagree
 * the symptom is bounded and loud -- exo_serial_write answers -EXO_EINVAL
 * and the tail of a panic message goes missing -- not silent corruption.
 */
#define SERIAL_WRITE_MAX_LEN 4096u
#endif

/*
 * Set on entry to doom_panic_begin and never cleared outside tests.
 *
 * Deliberately not `static` in the sense of hidden: doom_panic_in_progress()
 * exists so a test can watch the guard flip without having to survive the
 * halt that normally follows it.
 */
static int panic_active;

int doom_panic_in_progress(void)
{
    return panic_active;
}

void doom_panic_reset(void)
{
    panic_active = 0;
}

void doom_panic_write(const char *buf, size_t len)
{
    if (buf == NULL) {
        return;
    }

#ifdef EXO_KERNEL
    /*
     * In a kernel build there is no syscall and therefore no cap: serial.c
     * is a direct call.  The loop below is still written the same way so
     * both builds agree on what a zero-length write does (nothing).
     */
    {
        size_t i;
        for (i = 0; i < len; i++) {
            serial_putc(buf[i]);
        }
    }
#else
    /*
     * exo_serial_write refuses anything over SERIAL_WRITE_MAX_LEN with
     * -EXO_EINVAL (src/syscall_serial.c).  A panic message is normally one
     * line and nowhere near the cap, but "normally" is doing a lot of work
     * in a function whose entire job is to run when things have gone wrong
     * -- an I_Error carrying a long path or a corrupted string is exactly
     * the case you want to read, and it is exactly the case a single
     * unchunked write would drop in full.
     */
    while (len > 0) {
        size_t chunk = len > SERIAL_WRITE_MAX_LEN ? SERIAL_WRITE_MAX_LEN : len;

        if (exo_serial_write(buf, chunk) < 0) {
            /* Nothing useful to report it to -- this IS the reporting
             * channel.  Stop rather than spin retrying a call that has
             * already said no. */
            return;
        }

        buf += chunk;
        len -= chunk;
    }
#endif
}

/*
 * Stop, permanently.
 *
 * Ring 3 asks the kernel to tear the context down (exo_exit, #20), which is
 * the orderly answer and the one that releases the context's pages and its
 * framebuffer binding via revoke_all().  The spin after it is not dead code:
 * exo_exit returns if the syscall is unbound, and a LibOS that keeps
 * executing after announcing a fatal error is worse than one that stops.
 *
 * Ring 0 cannot exo_exit -- there is no context to exit -- so it flushes
 * COM1 first (buffered bytes are lost otherwise, which on a panic path means
 * losing the message that explains the panic) and then halts with interrupts
 * masked, so not even a timer tick resumes it.
 */
static void doom_stop(int code)
{
#ifdef EXO_KERNEL
    (void)code;
    serial_flush();
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
#else
    exo_exit(code);
    for (;;) {
        __asm__ volatile("pause" ::: "memory");
    }
#endif
}

void doom_panic_halt(void)
{
    doom_stop(-1);
}

void doom_panic_begin(const char *prefix, const char *fmt, va_list ap)
{
    /*
     * Re-entry.  An I_Error raised from inside an atexit handler that was
     * itself run by an I_Error would otherwise recurse through the formatter
     * with a stack that is one 4 KiB page on the ring-3 side
     * (LIBOS_LAUNCH_STACK_VADDR, src/libos_launch.h) -- so the second
     * failure would present as a stack overflow, burying the first one,
     * which is the message that actually mattered.
     *
     * Written with doom_panic_write() and a fixed string rather than
     * printf(): whatever went wrong may well be in the formatter or in the
     * arguments handed to it, and this is the one message that has to get
     * out regardless.
     */
    if (panic_active) {
        static const char recursive[] =
            "\nI_Error: recursive panic, halting.\n";
        doom_panic_write(recursive, sizeof(recursive) - 1);
        doom_stop(-1);
    }

    panic_active = 1;

    if (prefix != NULL) {
        printf("%s", prefix);
    }

    if (fmt != NULL) {
        /* vprintf, not a private formatter: kvprintf is already the shared
         * engine behind printf (src/stdio.c) and already routes to COM1 on
         * the kernel side and through one buffered exo_serial_write per line
         * on the ring-3 side.  A second copy here would be a second set of
         * conversion bugs. */
        vprintf(fmt, ap);
    }

    printf("\n");

    /*
     * Returns on purpose -- I_Error still has Doom's atexit list to walk,
     * and it can only do that after the message is out.  doom_panic_halt()
     * is the other half.
     */
}

void doom_halt(const char *msg)
{
    if (msg != NULL) {
        printf("%s\n", msg);
    }

    /* 0, not -1: I_Quit is Doom shutting down because it was asked to, not
     * because something broke. */
    doom_stop(0);
}
