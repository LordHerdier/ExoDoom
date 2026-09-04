#include "sys_time.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "pit.h"

/*
 * sys_time.c — exo_get_ticks (SCRUM-33), syscall #5.
 *
 * docs/syscall_spec.md §3.2 #5: "Return uint32_t milliseconds since boot.
 * Zero arguments." Backs DG_GetTicksMs and DG_SleepMs.
 */

/*
 * Handlers take the full six arguments regardless of arity — the dispatcher
 * cannot know an individual syscall's signature, so it always passes six.
 * exo_get_ticks uses none of them, and the ABI does not reserve the right to
 * reject a caller that left junk in the argument registers, so they are
 * discarded rather than validated.
 */
static int64_t sys_get_ticks(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    /*
     * Widened from uint32_t, so the result is always non-negative and can
     * never be mistaken for a -EXO_E* error code.  A signed 32-bit intermediate
     * would start returning negatives after ~24.9 days of uptime and the
     * caller would read them as errors; the counter itself still wraps at
     * ~49.7 days, which is documented as acceptable and is longer than any
     * plausible session.
     *
     * This is why the spec says exo_get_ticks never fails: there is no input
     * to reject and no state to be missing.  Called before pit_init(), it
     * reports 0 rather than an error.
     */
    return (int64_t)(uint64_t)kernel_get_ticks_ms();
}

void sys_time_init(void)
{
    exo_syscall_register(EXO_SYS_GET_TICKS, sys_get_ticks);
}
