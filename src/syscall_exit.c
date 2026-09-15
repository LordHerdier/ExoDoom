#include "syscall_exit.h"

#include <stdint.h>

#include "exo_syscall.h"
#include "fb_binding.h"
#include "page_alloc.h"
#include "syscall.h"

/*
 * SCRUM-155 — resource reclamation on LibOS exit.
 *
 * The ownership tag is the authority for deciding which resources belong to
 * the terminating context.  Never reclaim PAGE_OWNER_FREE or
 * PAGE_OWNER_KERNEL.
 *
 * Page-map revocation and file-descriptor teardown are added by their owning
 * subsystems once those facilities exist.  Neither subsystem is implemented
 * on the current main branch.
 */
static int64_t sys_exit(uint64_t code, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)code;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;

    page_owner_t owner = syscall_current_context();

    /*
     * Defense in depth.  A terminating LibOS must never be allowed to trigger
     * reclamation of the FREE sentinel or kernel-owned pages.
     */
    if (owner == PAGE_OWNER_FREE || owner == PAGE_OWNER_KERNEL)
        return -EXO_EPERM;

    /*
     * Release non-page bindings before returning physical pages to the free
     * pool.  Future mapping teardown must also happen before page reclamation
     * so no surviving context retains a mapping to a recycled page.
     */
    fb_binding_release(owner);
    (void)reclaim_pages_owned(owner);

    /*
     * v1 has no scheduler/context destruction path yet, so there is currently
     * nowhere safe for the kernel to transfer control after teardown.
     */
    return 0;
}

void syscall_exit_init(void)
{
    exo_syscall_register(EXO_SYS_EXIT, sys_exit);
}
