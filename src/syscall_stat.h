#pragma once

/*
 * syscall_stat.h — introspection syscall handlers (SCRUM-113).
 *
 * Binds exo_memstat (#22) and exo_pslist (#23) to the dispatcher in
 * src/syscall.c. Both are read-only snapshots of state src/page_alloc.c and
 * src/context.c already track for other reasons (ownership enforcement,
 * scheduling) — this file only translates it into the ABI structs in
 * src/exo_syscall.h. Back the shell's `memstat`/`pslist` commands
 * (src/shell/shell_main.c).
 *
 * Call from kernel_main after syscall_init() (the handler table lives there)
 * and ahead of the TESTING branch, so both handlers are bound for a normal
 * boot and the in-kernel test run alike.
 */
void syscall_stat_init(void);
