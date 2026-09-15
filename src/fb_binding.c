#include "fb_binding.h"

#include <stddef.h>
#include <stdint.h>

/*
 * fb_binding.c — the framebuffer ownership table (SCRUM-154).
 *
 * State is the geometry the kernel published at boot, the page-aligned
 * physical extent derived from it once, the current owner, and (SCRUM-156)
 * whether the kernel has asked that owner to give the framebuffer back.  A
 * single owner slot is all v1 needs; the multi-LibOS future (SCRUM-147)
 * changes who can be in the slot, not the shape of the table.
 *
 * No locking.  Syscalls run with interrupts masked (IA32_FMASK clears IF, see
 * src/syscall.c) and the entry path is single-threaded by construction, so the
 * read-modify-write in fb_binding_acquire() cannot be interleaved.  That
 * assumption becomes false with preemptive multi-LibOS scheduling and is
 * called out in docs/syscall_spec.md §3.3.
 */

#define PAGE_SIZE 4096u

static fb_geometry_t geometry;
static uint64_t      fb_page_base;   /* first FB page, page-aligned  */
static uint64_t      fb_page_end;    /* one past the last FB page    */
static int           fb_present;
static page_owner_t  fb_owner = PAGE_OWNER_FREE;
/* SCRUM-156: the kernel has asked fb_owner for the framebuffer back.  A
 * separate flag rather than a bit in fb_owner (the page table's trick) because
 * there is exactly one binding to describe, and a flag reads better than a
 * masked id everywhere the owner is compared. */
static int           fb_revoke_marked;

int fb_binding_init(const fb_geometry_t *geom)
{
    /* Any re-publish drops the binding: the resource being described is not
     * the one the previous owner acquired. */
    fb_present       = 0;
    fb_owner         = PAGE_OWNER_FREE;
    fb_revoke_marked = 0;
    fb_page_base     = 0;
    fb_page_end      = 0;

    if (geom == NULL)
        return FB_BIND_ENODEV;

    /* A framebuffer at physical 0, or with a zero dimension, is a bootloader
     * that did not give us one.  Reject it here so every later query can
     * assume a sane extent. */
    if (geom->phys_addr == 0 || geom->width == 0 || geom->height == 0 ||
        geom->pitch == 0 || geom->bpp == 0)
        return FB_BIND_ENODEV;

    /* pitch and height are both 32-bit, so the product cannot overflow 64
     * bits; base + size and the page round-up can, and a wrapped extent would
     * make fb_binding_contains() answer for the whole address space. */
    uint64_t size = (uint64_t)geom->pitch * (uint64_t)geom->height;

    if (geom->phys_addr > UINT64_MAX - size)
        return FB_BIND_ENODEV;

    uint64_t end = geom->phys_addr + size;

    if (end > UINT64_MAX - (PAGE_SIZE - 1))
        return FB_BIND_ENODEV;

    geometry     = *geom;
    fb_page_base = geom->phys_addr & ~(uint64_t)(PAGE_SIZE - 1);
    fb_page_end  = (end + (PAGE_SIZE - 1)) & ~(uint64_t)(PAGE_SIZE - 1);
    fb_present   = 1;

    return FB_BIND_OK;
}

const fb_geometry_t *fb_binding_geometry(void)
{
    return fb_present ? &geometry : NULL;
}

int fb_binding_acquire(page_owner_t who)
{
    if (!fb_present)
        return FB_BIND_ENODEV;

    /* PAGE_OWNER_FREE is the "unheld" sentinel, so it can never be an owner —
     * acquiring on its behalf would bind the framebuffer to nobody and leave
     * it looking free to the next caller. */
    if (who == PAGE_OWNER_FREE)
        return FB_BIND_EBUSY;

    if (fb_owner != PAGE_OWNER_FREE && fb_owner != who)
        return FB_BIND_EBUSY;

    fb_owner = who;

    return FB_BIND_OK;
}

void fb_binding_release(page_owner_t who)
{
    /* The voluntary return, and the compliance half of the revocation
     * protocol: dropping the binding drops the mark with it. */
    (void)fb_binding_reclaim(who);
}

page_owner_t fb_binding_owner(void)
{
    return fb_present ? fb_owner : PAGE_OWNER_FREE;
}

/* ---- Revocation / repossession (SCRUM-156) ------------------------------- */

/* Does `who` hold the framebuffer?  PAGE_OWNER_FREE never does — it is the
 * "unheld" sentinel, and without this check reclaiming on its behalf would
 * match an already-free binding and report a phantom success. */
static int holds_binding(page_owner_t who)
{
    return who != PAGE_OWNER_FREE && fb_binding_owner() == who;
}

int fb_binding_revoke_mark(page_owner_t who)
{
    if (!holds_binding(who))
        return FB_REVOKE_ENOENT;

    fb_revoke_marked = 1;
    return FB_REVOKE_OK;
}

int fb_binding_revoke_clear(page_owner_t who)
{
    if (!holds_binding(who))
        return FB_REVOKE_ENOENT;

    fb_revoke_marked = 0;
    return FB_REVOKE_OK;
}

int fb_binding_revoke_pending(void)
{
    /* An unheld framebuffer is never pending: reclaim and release both clear
     * the flag, but the guard keeps the answer true even if a future path
     * forgets to. */
    return fb_binding_owner() != PAGE_OWNER_FREE && fb_revoke_marked;
}

int fb_binding_reclaim(page_owner_t who)
{
    if (!holds_binding(who))
        return FB_REVOKE_ENOENT;

    fb_owner         = PAGE_OWNER_FREE;
    fb_revoke_marked = 0;
    return FB_REVOKE_OK;
}

int fb_binding_contains(uint64_t paddr)
{
    if (!fb_present)
        return 0;

    return paddr >= fb_page_base && paddr < fb_page_end;
}

int fb_binding_check_map(uint64_t paddr, page_owner_t who)
{
    if (!fb_binding_contains(paddr))
        return FB_MAP_NOT_FB;

    if (who != PAGE_OWNER_FREE && fb_owner == who)
        return FB_MAP_ALLOW;

    /* Framebuffer memory that the caller has not acquired — including the case
     * where nobody has.  An unheld framebuffer is not public property: the
     * LibOS has to bind it first. */
    return FB_MAP_DENY;
}
