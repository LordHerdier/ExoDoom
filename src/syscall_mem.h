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
 * Ownership-enforced (SCRUM-152): exo_page_alloc stamps the calling context as
 * the owner of the returned page and exo_page_free refuses to free a page the
 * caller does not own (-EXO_EPERM).  The owner tags live in the PMM
 * (src/page_alloc.c); the current context comes from syscall_current_context().
 */
void syscall_mem_init(void);
