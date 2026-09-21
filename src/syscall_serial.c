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

/* #8 — write `len` bytes of `buf` to COM1.
 *   >= 0           bytes written (always exactly `len` on success — the
 *                  serial driver is a busy-wait UART, not a buffered device
 *                  that can partially accept a write)
 *   -EXO_EFAULT    [buf, buf+len) is not entirely inside the LibOS window
 *   -EXO_EINVAL    len exceeds SERIAL_WRITE_MAX_LEN
 * Backs the printf/fprintf shim (docs/syscall_spec.md §3.2 #8). */
static int64_t sys_serial_write(uint64_t buf, uint64_t len, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (len > SERIAL_WRITE_MAX_LEN)
        return -EXO_EINVAL;

    /* Bounds alone isn't enough: the LibOS window is reserved-but-unmapped
     * until the caller exo_page_maps it, so an in-window-but-unmapped buf
     * still has to be rejected here, or the read below takes a fatal
     * supervisor-mode page fault (SCRUM-186). */
    if (!exo_range_in_user_window(buf, len) ||
        !exo_user_range_mapped(buf, len, 0))
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
