#pragma once

/*
 * syscall_mem.h — memory syscall handlers (SCRUM-34).
 *
 * Registers the two physical-page syscalls (exo_page_alloc #0, exo_page_free #1)
 * with the dispatcher in src/syscall.c.  Must run after page_alloc_init() (the
 * PMM has to exist) and after syscall_init() (the handler table lives there);
 * called from kernel_main ahead of the TESTING branch so the handlers are bound
 * for both a normal boot and the in-kernel test run.
 *
 * No ownership tracking yet — a page is handed out and taken back with no
 * per-caller tag.  The owner table and -EXO_EPERM enforcement are SCRUM-152,
 * which layers on top of these handlers.
 */
void syscall_mem_init(void);
