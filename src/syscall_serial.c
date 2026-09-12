#include "syscall_serial.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "serial.h"

#include <stddef.h>
#include <stdint.h>

/*
 * syscall_serial.c — the exo_serial_write handler (SCRUM-50).
 *
 * #8 is COM1's only LibOS-facing entry point: the kernel's own diagnostics
 * call serial_print()/serial_putc() directly, but a LibOS has no business
 * touching the UART itself (there is exactly one, and two contexts racing on
 * it would interleave garbage), so this is the printf/fprintf shim's whole
 * backend. There is no ownership to check here the way page_alloc.c/
 * fb_binding.c have one — COM1 is not a resource anyone acquires or loses,
 * just a channel every LibOS may write to — so the only policy question is
 * whether `buf`/`len` actually names memory the caller could have written
 * into itself.
 */

/* Is [buf, buf+len) entirely inside the LibOS mapping window? Mirrors
 * syscall_mem.c's in_user_window(), duplicated rather than shared because
 * the two files check different things (a single vaddr there, a range here)
 * and neither has a second caller yet to justify a shared header for one
 * predicate. `len == 0` is trivially in-window regardless of `buf` — an
 * empty write can't read past anything. */
static int range_in_user_window(uint64_t buf, uint64_t len)
{
    if (len == 0)
        return 1;

    if (buf < EXO_USER_VA_BASE || buf >= EXO_USER_VA_END)
        return 0;

    /* buf is already >= EXO_USER_VA_BASE and < EXO_USER_VA_END, both well
     * clear of the top of a 64-bit range, so buf + len cannot wrap here even
     * though len is caller-controlled and otherwise unbounded. */
    uint64_t end = buf + len;
    return end >= buf && end <= EXO_USER_VA_END;
}

/* #8 — write `len` bytes of `buf` to COM1.
 *   >= 0           bytes written (always exactly `len` on success — the
 *                  serial driver is a busy-wait UART, not a buffered device
 *                  that can partially accept a write)
 *   -EXO_EFAULT    [buf, buf+len) is not entirely inside the LibOS window
 * Backs the printf/fprintf shim (docs/syscall_spec.md §3.2 #8). */
static int64_t sys_serial_write(uint64_t buf, uint64_t len, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!range_in_user_window(buf, len))
        return -EXO_EFAULT;

    const char *p = (const char *)(uintptr_t)buf;
    for (uint64_t i = 0; i < len; i++)
        serial_putc(p[i]);

    return (int64_t)len;
}

void syscall_serial_init(void)
{
    exo_syscall_register(EXO_SYS_SERIAL_WRITE, sys_serial_write);
}
