#include "syscall_kbd_bind.h"
#include "syscall.h"
#include "syscall_launch.h"
#include "exo_syscall.h"
#include "kbd_source.h"

#include <stdint.h>

/* #44 -- bind a keyboard source to the context the caller holds (SCRUM-297).
 * Checked in the order the answers are useful in, as sys_pane_set does: whether
 * the caller may touch `ctx` at all, then the source, which depends on what is
 * plugged in right now. */
static int64_t sys_kbd_bind(uint64_t ctx, uint64_t source, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (ctx > (page_owner_t)~0u) {
        return -EXO_ENOENT;
    }
    int64_t rc = launch_hold_check((page_owner_t)ctx,
                                   syscall_current_context());
    if (rc != 0) {
        return rc;
    }

    if (source == EXO_KBD_SRC_NONE) {
        kbd_source_release_ctx((page_owner_t)ctx);
        return 0;
    }
    if (source >= KBD_SRC_MAX) {
        return -EXO_EINVAL;
    }
    return kbd_source_claim((int)source, (page_owner_t)ctx);
}

void syscall_kbd_bind_init(void)
{
    exo_syscall_register(EXO_SYS_KBD_BIND, sys_kbd_bind);
}
