#include "syscall_mem.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "page_alloc.h"

#include <stddef.h>
#include <stdint.h>

/*
 * syscall_mem.c — the exo_page_alloc / exo_page_free handlers (SCRUM-34).
 *
 * These are the first two "resource" syscalls: they translate the uniform
 * 6-argument dispatcher ABI (src/syscall.h) into calls on the physical page
 * allocator (src/page_alloc.c) and back into the -EXO_E* return convention.
 * page_alloc.c stays ABI-agnostic; the mapping from its plain int status to an
 * errno-style code lives here.
 */

/* #0 — allocate one 4K physical page.  Returns its physical address, which is
 * always positive (the managed pool starts at >= 1 MiB, so page 0 is never
 * handed out and no success value collides with the negative error range), or
 * -EXO_ENOMEM when the pool is exhausted. */
static int64_t sys_page_alloc(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    void* page = alloc_page();
    if (page == NULL)
        return -EXO_ENOMEM;

    return (int64_t)(uintptr_t)page;
}

/* #1 — return a page from exo_page_alloc.  0 on success, or -EXO_EINVAL for an
 * unaligned / out-of-range address or a double free.  With no ownership model
 * yet (SCRUM-152), every bad free is EINVAL rather than EPERM — matching the
 * exo_page_free wrapper contract in src/exo_syscall.h. */
static int64_t sys_page_free(uint64_t paddr, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (free_page_checked((void*)(uintptr_t)paddr) != 0)
        return -EXO_EINVAL;

    return 0;
}

void syscall_mem_init(void)
{
    exo_syscall_register(EXO_SYS_PAGE_ALLOC, sys_page_alloc);
    exo_syscall_register(EXO_SYS_PAGE_FREE,  sys_page_free);
}
