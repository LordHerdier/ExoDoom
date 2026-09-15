#pragma once

/*
 * syscall_yield.h — exo_yield handler (SCRUM-109).
 *
 * Binds exo_yield (#19) to the dispatcher in src/syscall.c: the number and
 * the LibOS-side stub have existed since SCRUM-24/-108's EXO_SYS_YIELD
 * borrow, but nothing answered it as a real handler until this ticket.
 * context_switch_request() (src/context.c/h, SCRUM-108) is the switch
 * primitive; context_next_ready() (same files, added by this ticket) is the
 * round-robin policy this handler uses to pick a target, since the real
 * exo_yield() ABI (docs/syscall_spec.md §3.2 #19) takes no argument naming
 * one, unlike the test-local SYS_SWITCH borrow tests/kernel/
 * test_context_switch_k.c still uses to drive the raw primitive directly.
 *
 * Call from kernel_main after syscall_pit_init() (same placement rule every
 * other syscall_*_init() follows: the handler table lives in syscall_init(),
 * called once at the top) and ahead of the TESTING branch.
 */
void syscall_yield_init(void);
