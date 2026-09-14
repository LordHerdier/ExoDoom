#include "syscall_kbd.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "ps2.h"

#include <stddef.h>
#include <stdint.h>

/*
 * syscall_kbd.c — the exo_kbd_poll handler (#6, docs/syscall_spec.md §3.2).
 *
 * Same shape as sys_serial_write (src/syscall_serial.c): no ownership to
 * check -- the keyboard, like COM1, is not a resource anyone acquires or
 * loses -- so the only policy question is whether `event_out` names memory
 * the caller could have written into itself. The ring itself
 * (src/kbd_ring.c) is strictly single-consumer; with one LibOS in v1 that is
 * exactly what this handler is.
 */

/* #6 — dequeue one keyboard event into *event_out.
 *   1              an event was dequeued and *event_out filled
 *   0              the ring was empty; *event_out untouched
 *   -EXO_EFAULT    event_out is not entirely inside the LibOS window
 * Backs DG_GetKey and the ring-3 automap viewer's <-/-> navigation. */
static int64_t sys_kbd_poll(uint64_t event_out, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!exo_range_in_user_window(event_out, sizeof(exo_kbd_event_t)))
        return -EXO_EFAULT;

    kbd_event_t ev;
    if (!exo_kbd_poll(&ev))
        return 0;

    exo_kbd_event_t *out = (exo_kbd_event_t *)(uintptr_t)event_out;
    out->pressed   = ev.pressed;
    out->key       = ev.key;
    out->modifiers = ev.modifiers;
    out->reserved  = 0;

    return 1;
}

void syscall_kbd_init(void)
{
    exo_syscall_register(EXO_SYS_KBD_POLL, sys_kbd_poll);
}
