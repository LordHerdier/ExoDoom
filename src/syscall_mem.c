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
 * -EXO_ENOMEM when the pool is exhausted.  The page is stamped with the calling
 * context as owner (secure binding, SCRUM-152) so only that context can later
 * free or (SCRUM-153) map it. */
static int64_t sys_page_alloc(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    void* page = alloc_page_owned(syscall_current_context());
    if (page == NULL)
        return -EXO_ENOMEM;

    return (int64_t)(uintptr_t)page;
}

/* #1 — return a page from exo_page_alloc.  Ownership-enforced (SCRUM-152):
 *   0             freed (the caller owned paddr)
 *   -EXO_EPERM    paddr is an allocated page owned by the kernel or another
 *                 LibOS — the caller may not free it
 *   -EXO_EINVAL   unaligned / out-of-range address, or a double free of a page
 *                 that is already FREE
 * The PMM returns ABI-agnostic PAGE_FREE_* codes; the errno mapping is here. */
static int64_t sys_page_free(uint64_t paddr, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    switch (free_page_owned((void*)(uintptr_t)paddr, syscall_current_context())) {
    case PAGE_FREE_OK:    return 0;
    case PAGE_FREE_EPERM: return -EXO_EPERM;
    default:              return -EXO_EINVAL;
    }
}

void syscall_mem_init(void)
{
    exo_syscall_register(EXO_SYS_PAGE_ALLOC, sys_page_alloc);
    exo_syscall_register(EXO_SYS_PAGE_FREE,  sys_page_free);
}
