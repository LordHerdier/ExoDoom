#include "syscall_statusbar.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "status_bar.h"

#include <stdint.h>

/* #45 -- set or read the status bar toggle. Takes effect on the compositor's
 * next tick; nothing is drawn from here. */
static int64_t sys_statusbar(uint64_t mode, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    switch (mode) {
    case EXO_STATUSBAR_OFF:
        status_bar_set_enabled(0);
        break;
    case EXO_STATUSBAR_ON:
        status_bar_set_enabled(1);
        break;
    case EXO_STATUSBAR_QUERY:
        break;
    default:
        return -EXO_EINVAL;
    }
    return status_bar_enabled() ? 1 : 0;
}

void syscall_statusbar_init(void)
{
    exo_syscall_register(EXO_SYS_STATUSBAR, sys_statusbar);
}
