#include "syscall_pit.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "pit.h"

#include <stdint.h>

/* #5 — milliseconds since boot, monotonic. Never fails; backs both
 * DG_GetTicksMs and DG_SleepMs (docs/syscall_spec.md §3.2 #5). No ownership
 * to check, the same as exo_serial_write — the tick count isn't a resource
 * anyone acquires, just a value every LibOS may read. */
static int64_t sys_get_ticks(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    return (int64_t)kernel_get_ticks_ms();
}

void syscall_pit_init(void)
{
    exo_syscall_register(EXO_SYS_GET_TICKS, sys_get_ticks);
}
