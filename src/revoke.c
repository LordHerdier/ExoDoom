#include "revoke.h"

#include "disk_binding.h"
#include "fb_binding.h"
#include "fb_shadow.h"
#include "page_alloc.h"
#include "serial.h"
#include "vmm.h"

#include <stddef.h>
#include <stdint.h>

/*
 * revoke.c — the revocation protocol (SCRUM-156).
 *
 * src/revoke.h states the protocol; this file sequences it.  Every operation
 * is a two-way switch: pick the mechanism for the resource kind, then map its
 * status onto the REVOKE_* set.  Nothing here touches an ownership table
 * directly — page_alloc.c and fb_binding.c own their own bookkeeping, and
 * keeping the reach one-way is what lets a third resource (a disk extent, a
 * sound channel) be added without editing either of them.
 *
 * SCRUM-159 adds one exception to "nothing here touches page tables": once a
 * force reclaims a resource in the ownership table, this file also tears
 * down `who`'s own mapping(s) of it, via vmm_unmap_phys_range_in() (src/
 * vmm.h). That has to happen here rather than in page_alloc.c/fb_binding.c
 * because only this layer (and syscall_mem.c) has reason to know about
 * address spaces at all — see vmm_unmap_phys_range_in()'s own comment for why
 * a reverse scan of `who`'s address space, rather than a separate mapping
 * record, is enough: post-SCRUM-48 a context's own LibOS window is the only
 * place its mappings can live.
 *
 * No locking, for the same reason as fb_binding.c: syscalls run with IF
 * cleared by IA32_FMASK and the entry path is single-threaded.  Preemptive
 * multi-LibOS scheduling (SCRUM-147) invalidates that and will need a lock
 * around the mark-then-reclaim sequence, not merely around each half.
 */

static revoke_record_t record;

/* The page-shaped view of a resource.  page_alloc.c speaks in pointers and
 * this layer in uint64_t physical addresses, so the cast lives in one place. */
static void *page_of(revoke_res_t what)
{
    return (void *)(uintptr_t)what.paddr;
}

/* `who`'s own bound PML4, or NULL if it has none — vmm_unmap_phys_range_in()
 * treats NULL as "nothing to unmap", matching an unbound context (never
 * given its own address space, or already torn down) having no mappings to
 * tear down either. */
static uint64_t *revoke_pml4_for(page_owner_t who)
{
    return (uint64_t *)(uintptr_t)vmm_address_space_for(who);
}

int revoke_request(page_owner_t who, revoke_res_t what)
{
    int rc;

    switch (what.kind) {
    case REVOKE_PAGE:
        switch (page_revoke_mark(page_of(what), who)) {
        case PAGE_REVOKE_OK:     rc = REVOKE_OK;     break;
        case PAGE_REVOKE_ENOENT: rc = REVOKE_ENOENT; break;
        default:                 rc = REVOKE_EINVAL; break;
        }
        break;

    case REVOKE_FRAMEBUFFER:
        rc = (fb_binding_revoke_mark(who) == FB_REVOKE_OK) ? REVOKE_OK
                                                           : REVOKE_ENOENT;
        break;

    case REVOKE_DISK:
        rc = (disk_binding_revoke_mark(who) == DISK_REVOKE_OK) ? REVOKE_OK
                                                               : REVOKE_ENOENT;
        break;

    default:
        rc = REVOKE_EINVAL;
        break;
    }

    /* Counted only when a mark actually landed: a request against a resource
     * the context does not hold never reached the owner, so counting it would
     * make the record claim asks that were never made. */
    if (rc == REVOKE_OK)
        record.requested++;

    return rc;
}

int revoke_withdraw(page_owner_t who, revoke_res_t what)
{
    int rc;

    switch (what.kind) {
    case REVOKE_PAGE:
        switch (page_revoke_clear(page_of(what), who)) {
        case PAGE_REVOKE_OK:     rc = REVOKE_OK;     break;
        case PAGE_REVOKE_ENOENT: rc = REVOKE_ENOENT; break;
        default:                 rc = REVOKE_EINVAL; break;
        }
        break;

    case REVOKE_FRAMEBUFFER:
        rc = (fb_binding_revoke_clear(who) == FB_REVOKE_OK) ? REVOKE_OK
                                                            : REVOKE_ENOENT;
        break;

    case REVOKE_DISK:
        rc = (disk_binding_revoke_clear(who) == DISK_REVOKE_OK) ? REVOKE_OK
                                                                : REVOKE_ENOENT;
        break;

    default:
        rc = REVOKE_EINVAL;
        break;
    }

    if (rc == REVOKE_OK)
        record.withdrawn++;

    return rc;
}

int revoke_pending(revoke_res_t what)
{
    switch (what.kind) {
    case REVOKE_PAGE:        return page_revoke_pending(page_of(what));
    case REVOKE_FRAMEBUFFER: return fb_binding_revoke_pending();
    case REVOKE_DISK:        return disk_binding_revoke_pending();
    default:                 return 0;
    }
}

int revoke_force(page_owner_t who, revoke_res_t what)
{
    switch (what.kind) {
    case REVOKE_PAGE:
        switch (page_reclaim(page_of(what), who)) {
        case PAGE_REVOKE_OK: {
            /* SCRUM-159: the page is back in the ownership table, but `who`
             * may still have a live PTE pointing at it in its own address
             * space -- clear that too, so the next owner the PMM hands this
             * frame to is never reachable through `who`'s stale mapping. */
            uint64_t page = what.paddr & ~(uint64_t)(VMM_PAGE_SIZE - 1);
            vmm_unmap_phys_range_in(revoke_pml4_for(who), page,
                                    page + VMM_PAGE_SIZE);
            record.forced++;
            record.pages_reclaimed++;
            return REVOKE_OK;
        }
        case PAGE_REVOKE_ENOENT:
            record.returned++;
            return REVOKE_RETURNED;
        default:
            return REVOKE_EINVAL;
        }

    case REVOKE_FRAMEBUFFER:
        if (fb_binding_reclaim(who) == FB_REVOKE_OK) {
            /* Legacy direct-FB-mapping path (src/fb_binding.c) -- the common
             * per-LibOS case is a shadow framebuffer, ordinary owned pages
             * already covered by the REVOKE_PAGE case above and by
             * revoke_all()'s window sweep below. */
            const fb_geometry_t *geom = fb_binding_geometry();
            if (geom != NULL) {
                uint64_t lo = geom->phys_addr & ~(uint64_t)(VMM_PAGE_SIZE - 1);
                uint64_t hi = (geom->phys_addr +
                              (uint64_t)geom->pitch * geom->height +
                              VMM_PAGE_SIZE - 1) &
                              ~(uint64_t)(VMM_PAGE_SIZE - 1);
                vmm_unmap_phys_range_in(revoke_pml4_for(who), lo, hi);
            }
            record.forced++;
            record.fb_reclaimed++;
            return REVOKE_OK;
        }
        record.returned++;
        return REVOKE_RETURNED;

    case REVOKE_DISK:
        /* No page-table teardown needed here, unlike REVOKE_FRAMEBUFFER
         * above: the disk binding gates a syscall path (src/syscall_disk.c),
         * not a mapping into the caller's address space. */
        if (disk_binding_reclaim(who) == DISK_REVOKE_OK) {
            record.forced++;
            record.disk_reclaimed++;
            return REVOKE_OK;
        }
        record.returned++;
        return REVOKE_RETURNED;

    default:
        return REVOKE_EINVAL;
    }
}

uint32_t revoke_all(page_owner_t who)
{
    /* page_reclaim_all() refuses the reserved ids itself; checking here too
     * keeps the framebuffer half from being taken on their behalf, which no
     * per-page guard would have caught. */
    if (who == PAGE_OWNER_FREE || who == PAGE_OWNER_KERNEL)
        return 0;

    uint32_t pages = page_reclaim_all(who);
    uint32_t total = pages;

    record.pages_reclaimed += pages;
    record.forced          += pages;

    /* Drops this context's virtual-framebuffer directory entry, if it has
     * one (SCRUM-112) -- the underlying pages are already part of the
     * page_reclaim_all() sweep above, since they were allocated owned by
     * `who` like any other page. This only prevents a stale slot from
     * outliving the context or being handed to whoever reuses its id. */
    fb_shadow_release(who);

    if (fb_binding_reclaim(who) == FB_REVOKE_OK) {
        record.fb_reclaimed++;
        record.forced++;
        total++;
    }

    if (disk_binding_reclaim(who) == DISK_REVOKE_OK) {
        record.disk_reclaimed++;
        record.forced++;
        total++;
    }

    /* SCRUM-159: page_reclaim_all() doesn't report which addresses it took
     * back, so rather than tracking them individually, wipe every mapping
     * left in `who`'s own window in one pass -- covers the pages just
     * reclaimed above, any shadow framebuffer (ordinary owned pages, same
     * sweep) and the legacy direct-FB mapping alike, without needing to
     * enumerate them. A context with no address space bound (never given one,
     * or already torn down) has nothing to sweep here. */
    vmm_unmap_phys_range_in(revoke_pml4_for(who), 0, UINT64_MAX);

    /* One line per sweep, not per resource: a context that exits holding a
     * thousand pages should not push the rest of the boot log off the
     * screen. */
    serial_print("revoke: reclaimed ");
    serial_print_u32(total);
    serial_print(" resource(s) from context ");
    serial_print_u32((uint32_t)who);
    serial_print("\n");

    return total;
}

const revoke_record_t *revoke_record(void)
{
    return &record;
}

void revoke_record_reset(void)
{
    record.requested       = 0;
    record.withdrawn       = 0;
    record.forced          = 0;
    record.returned        = 0;
    record.pages_reclaimed = 0;
    record.fb_reclaimed    = 0;
    record.disk_reclaimed  = 0;
}
