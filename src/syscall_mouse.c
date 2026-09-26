#include "syscall_mouse.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "ps2_mouse.h"

#include <stddef.h>
#include <stdint.h>

/*
 * syscall_mouse.c — the exo_mouse_poll handler (SCRUM-52, #7,
 * docs/syscall_spec.md §3.2).
 *
 * Same shape as sys_kbd_poll (src/syscall_kbd.c): no ownership to check --
 * the mouse, like the keyboard, is not a resource anyone acquires or loses --
 * so the only policy question is whether `state_out` names memory the caller
 * could have written into itself. Unlike sys_kbd_poll there is no "empty"
 * case: ps2_mouse_poll() always has a state to report (zero deltas and
 * whatever buttons currently read), so this handler always writes *state_out
 * and returns 0 once the bounds check passes.
 */

/* #7 — write the accumulated mouse state into *state_out, then reset the
 * dx/dy accumulators (buttons persists as a level).
 *   0              *state_out filled
 *   -EXO_EFAULT    state_out is not entirely inside the LibOS window
 * Backs any ring-3 LibOS that needs mouse input (Doom's ev_mouse posting is
 * SCRUM-80, not this ticket). */
static int64_t sys_mouse_poll(uint64_t state_out, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    /* Bounds alone isn't enough: the LibOS window is reserved-but-unmapped
     * until the caller exo_page_maps it, so an in-window-but-unmapped
     * state_out still has to be rejected here, or the write below takes a
     * fatal supervisor-mode page fault (same SCRUM-186 hazard sys_kbd_poll
     * guards against). */
    if (!exo_range_in_user_window(state_out, sizeof(exo_mouse_state_t)) ||
        !exo_user_range_mapped(state_out, sizeof(exo_mouse_state_t), 1))
        return -EXO_EFAULT;

    ps2_mouse_state_t st;
    ps2_mouse_poll(&st);

    exo_mouse_state_t *out = (exo_mouse_state_t *)(uintptr_t)state_out;
    out->dx       = st.dx;
    out->dy       = st.dy;
    out->buttons  = st.buttons;
    out->reserved = 0;

    return 0;
}

void syscall_mouse_init(void)
{
    exo_syscall_register(EXO_SYS_MOUSE_POLL, sys_mouse_poll);
}
