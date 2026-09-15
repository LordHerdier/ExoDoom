#include "syscall_yield.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "context.h"

#include <stdint.h>

/* #19 — cooperatively yield to the next runnable LibOS context
 * (docs/syscall_spec.md §3.2 #19). Always returns 0 ("returns when
 * rescheduled") -- the ABI documents no error path, so both "nothing else
 * is READY" (context_next_ready() returns PAGE_OWNER_FREE) and "the current
 * context has no row in the table to switch away from" (the boot-time
 * default LibOS -- context_switch_request() returns CONTEXT_ENOENT, see its
 * own comment) collapse to the same observable behavior: the syscall
 * returns and the caller carries on, exactly as if nothing else were
 * runnable. */
static int64_t sys_yield(uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    page_owner_t next = context_next_ready(context_current());
    if (next != PAGE_OWNER_FREE) {
        context_switch_request(next);
    }
    return 0;
}

void syscall_yield_init(void)
{
    exo_syscall_register(EXO_SYS_YIELD, sys_yield);
}
