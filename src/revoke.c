#include "revoke.h"

#include "fb_binding.h"
#include "page_alloc.h"
#include "serial.h"

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
    default:                 return 0;
    }
}

int revoke_force(page_owner_t who, revoke_res_t what)
{
    switch (what.kind) {
    case REVOKE_PAGE:
        switch (page_reclaim(page_of(what), who)) {
        case PAGE_REVOKE_OK:
            record.forced++;
            record.pages_reclaimed++;
            return REVOKE_OK;
        case PAGE_REVOKE_ENOENT:
            record.returned++;
            return REVOKE_RETURNED;
        default:
            return REVOKE_EINVAL;
        }

    case REVOKE_FRAMEBUFFER:
        if (fb_binding_reclaim(who) == FB_REVOKE_OK) {
            record.forced++;
            record.fb_reclaimed++;
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

    if (fb_binding_reclaim(who) == FB_REVOKE_OK) {
        record.fb_reclaimed++;
        record.forced++;
        total++;
    }

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
}
