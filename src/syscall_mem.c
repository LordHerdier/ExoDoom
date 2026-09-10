#include "syscall_mem.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "page_alloc.h"
#include "fb_binding.h"
#include "vmm.h"

#include <stddef.h>
#include <stdint.h>

/*
 * syscall_mem.c — the memory syscalls: exo_page_alloc / exo_page_free
 * (SCRUM-34) and exo_page_map / exo_page_unmap (SCRUM-35, enforcement
 * SCRUM-153).
 *
 * These are the "resource" syscalls: they translate the uniform 6-argument
 * dispatcher ABI (src/syscall.h) into calls on the physical page allocator
 * (src/page_alloc.c) and the page table walker (src/vmm.c), and back into the
 * -EXO_E* return convention.  Both of those modules stay ABI-agnostic; the
 * mapping from their plain int status to an errno-style code lives here.
 *
 * This file is also where the secure-binding rule of docs/syscall_spec.md §3.3
 * is enforced for memory: page_alloc.c knows who owns a page and fb_binding.c
 * knows who holds the framebuffer, but neither knows what a caller is allowed
 * to *do* with that fact.  may_map_phys() / may_unmap_phys() below are that
 * policy, and they are the reason exo_page_map is a secure binding rather than
 * an unprotected physical mmap.
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

/* ---- Mapping permission (SCRUM-153) ------------------------------------- */

/*
 * May `who` install a mapping of physical page `paddr`?
 *
 * The framebuffer is asked first and its answer is final when it has one: FB
 * pages sit outside the RAM the PMM manages, so page_owner() reports
 * PAGE_OWNER_FREE for every one of them and the generic check below would
 * wave through exactly the memory the binding exists to protect.  §3.5 spells
 * out why the verdict is three-valued rather than a bool.
 */
static int may_map_phys(uint64_t paddr, page_owner_t who)
{
    switch (fb_binding_check_map(paddr, who)) {
    case FB_MAP_ALLOW:  return 1;
    case FB_MAP_DENY:   return 0;
    default:            break;      /* FB_MAP_NOT_FB — ordinary RAM */
    }

    return page_owner((void *)(uintptr_t)paddr) == who;
}

/*
 * May `who` remove a mapping of physical page `paddr` — either by unmapping it
 * or by mapping something else over it?
 *
 * Deliberately looser than may_map_phys(): what it refuses is a page that
 * currently belongs to somebody else, not every page the caller may not map.
 * Removing a mapping changes an address space, not a page, so it cannot harm
 * whoever owns the page — and two cases make the looser rule necessary rather
 * than merely defensible:
 *
 *   - A page owned by **nobody**.  §3.2 #3 lets a LibOS free a page that is
 *     still mapped, and it must then be able to clean up the mapping it left
 *     behind.
 *   - **Framebuffer memory the caller no longer holds.**  A LibOS that
 *     acquires the framebuffer, maps it, and then loses the binding — to
 *     revocation (§3.6) or to its own release — would otherwise be stuck with
 *     an address it can neither unmap nor reuse, forever.
 *
 * Both are the same argument, and both are bounded by the same v1 caveat as
 * everything else here: with one address space (SCRUM-48) a LibOS that drops a
 * mapping it did not create is clobbering a peer's window rather than its own.
 * What it cannot do is take a page away from the kernel or from another
 * context, which is the guarantee §3.3 actually makes.
 */
static int may_unmap_phys(uint64_t paddr, page_owner_t who)
{
    if (fb_binding_contains(paddr))
        return 1;

    page_owner_t owner = page_owner((void *)(uintptr_t)paddr);
    return owner == who || owner == PAGE_OWNER_FREE;
}

/* Is `vaddr` inside the window a LibOS is allowed to map into?  See
 * EXO_USER_VA_BASE in src/exo_syscall.h for why the window exists. */
static int in_user_window(uint64_t vaddr)
{
    return vaddr >= EXO_USER_VA_BASE && vaddr < EXO_USER_VA_END;
}

/* vmm.c's status codes in the ABI's terms (src/vmm.h).  Two of them are
 * "bad argument" from the caller's point of view rather than kernel failures:
 * VMM_ENOENT ("nothing mapped there") is what §3.2 #3 promises -EINVAL for,
 * and VMM_EEXIST is the walker refusing to silently repoint a live mapping —
 * see sys_page_map for why that is the caller's problem to fix. */
static int64_t vmm_status_to_errno(int rc)
{
    switch (rc) {
    case VMM_OK:     return 0;
    case VMM_ENOMEM: return -EXO_ENOMEM;
    default:         return -EXO_EINVAL;
    }
}

/* #2 — map physical page `paddr` at `vaddr` in the caller's address space.
 *   0             mapped
 *   -EXO_EINVAL   vaddr or paddr misaligned, flags has an unknown bit, or
 *                 vaddr already resolves to a *different* page (see below)
 *   -EXO_EPERM    vaddr outside the LibOS window, or the caller does not own
 *                 the page it is mapping
 *   -EXO_ENOMEM   no physical page left for an intermediate page table
 *
 * Permission is decided before anything is written, so a rejected call leaves
 * the address space exactly as it found it.
 *
 * Note what the walker does *not* allow: repointing a live mapping at a
 * different physical page in one call is VMM_EEXIST, not a silent replace
 * (src/vmm.h).  A LibOS moving a window over physical memory therefore has to
 * exo_page_unmap first — which is the ownership-checked operation, so "map
 * over it" cannot be used to drop a mapping the caller would not have been
 * allowed to unmap.  Re-mapping the same page is permitted and updates the
 * flags. */
static int64_t sys_page_map(uint64_t vaddr, uint64_t paddr, uint64_t flags,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;

    const uint64_t known = EXO_PAGE_READ | EXO_PAGE_WRITE |
                           EXO_PAGE_USER | EXO_PAGE_EXEC;
    if ((flags & ~known) != 0)
        return -EXO_EINVAL;

    if ((vaddr % VMM_PAGE_SIZE) != 0 || (paddr % VMM_PAGE_SIZE) != 0)
        return -EXO_EINVAL;

    if (!in_user_window(vaddr))
        return -EXO_EPERM;

    if (!may_map_phys(paddr, syscall_current_context()))
        return -EXO_EPERM;

    /* EXO_PAGE_READ is accepted and dropped: a present page is readable on
     * x86, so there is no bit to set and no way to honour its absence.  So is
     * EXO_PAGE_EXEC, until EFER.NXE is enabled — see src/vmm.h on why setting
     * bit 63 before that faults on the walk.  VMM_PRESENT is added by the
     * walker itself. */
    uint64_t attrs = 0;
    if (flags & EXO_PAGE_WRITE) attrs |= VMM_WRITE;
    if (flags & EXO_PAGE_USER)  attrs |= VMM_USER;

    return vmm_status_to_errno(vmm_map_page(vaddr, paddr, attrs));
}

/* #3 — remove the mapping at `vaddr`.  The physical page is left allocated;
 * exo_page_free is what returns it.
 *   0             unmapped
 *   -EXO_EINVAL   vaddr misaligned, or nothing is mapped there
 *   -EXO_EPERM    vaddr outside the LibOS window, or the mapping is of a page
 *                 belonging to the kernel or to another context
 *   -EXO_ENOMEM   the address was covered by a 2 MiB page and splitting it
 *                 needed a page table the PMM could not provide */
static int64_t sys_page_unmap(uint64_t vaddr, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if ((vaddr % VMM_PAGE_SIZE) != 0)
        return -EXO_EINVAL;

    if (!in_user_window(vaddr))
        return -EXO_EPERM;

    uint64_t paddr;
    if (vmm_translate(vaddr, &paddr, NULL) != VMM_OK)
        return -EXO_EINVAL;

    if (!may_unmap_phys(paddr & ~(uint64_t)(VMM_PAGE_SIZE - 1),
                        syscall_current_context()))
        return -EXO_EPERM;

    return vmm_status_to_errno(vmm_unmap_page(vaddr));
}

void syscall_mem_init(void)
{
    exo_syscall_register(EXO_SYS_PAGE_ALLOC, sys_page_alloc);
    exo_syscall_register(EXO_SYS_PAGE_FREE,  sys_page_free);
    exo_syscall_register(EXO_SYS_PAGE_MAP,   sys_page_map);
    exo_syscall_register(EXO_SYS_PAGE_UNMAP, sys_page_unmap);
}
