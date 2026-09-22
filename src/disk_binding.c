#include "disk_binding.h"

#include <stddef.h>
#include <stdint.h>

/*
 * disk_binding.c — the disk ownership table (SCRUM-188).
 *
 * State is just whether a drive was found at boot, the current owner, and
 * whether the kernel has asked that owner to give the disk back. A single
 * owner slot is all v1 needs, same as fb_binding.c pre-SCRUM-112.
 *
 * No locking. Syscalls run with interrupts masked (IA32_FMASK clears IF, see
 * src/syscall.c) and the entry path is single-threaded by construction, so
 * the read-modify-write in disk_binding_acquire() cannot be interleaved.
 * That assumption becomes false with preemptive multi-LibOS scheduling and
 * is called out in docs/syscall_spec.md §3.3/§3.5, the same caveat
 * fb_binding.c carries.
 */

static int          disk_present;
static page_owner_t disk_owner = PAGE_OWNER_FREE;
/* The kernel has asked disk_owner for the disk back. A separate flag rather
 * than a bit in disk_owner (the page table's trick) because there is exactly
 * one binding to describe. */
static int          disk_revoke_marked;

int disk_binding_init(int present)
{
    disk_present       = present ? 1 : 0;
    disk_owner         = PAGE_OWNER_FREE;
    disk_revoke_marked = 0;

    return disk_present ? DISK_BIND_OK : DISK_BIND_ENODEV;
}

int disk_binding_present(void)
{
    return disk_present;
}

int disk_binding_acquire(page_owner_t who)
{
    if (!disk_present)
        return DISK_BIND_ENODEV;

    /* PAGE_OWNER_FREE is the "unheld" sentinel, so it can never be an owner —
     * acquiring on its behalf would bind the disk to nobody and leave it
     * looking free to the next caller. */
    if (who == PAGE_OWNER_FREE)
        return DISK_BIND_EBUSY;

    if (disk_owner != PAGE_OWNER_FREE && disk_owner != who)
        return DISK_BIND_EBUSY;

    disk_owner = who;

    return DISK_BIND_OK;
}

void disk_binding_release(page_owner_t who)
{
    /* The voluntary return, and the compliance half of the revocation
     * protocol: dropping the binding drops the mark with it. */
    (void)disk_binding_reclaim(who);
}

page_owner_t disk_binding_owner(void)
{
    return disk_present ? disk_owner : PAGE_OWNER_FREE;
}

/* ---- Revocation / repossession ------------------------------------------- */

/* Does `who` hold the disk? PAGE_OWNER_FREE never does — it is the "unheld"
 * sentinel, and without this check reclaiming on its behalf would match an
 * already-free binding and report a phantom success. */
static int holds_binding(page_owner_t who)
{
    return who != PAGE_OWNER_FREE && disk_binding_owner() == who;
}

int disk_binding_revoke_mark(page_owner_t who)
{
    if (!holds_binding(who))
        return DISK_REVOKE_ENOENT;

    disk_revoke_marked = 1;
    return DISK_REVOKE_OK;
}

int disk_binding_revoke_clear(page_owner_t who)
{
    if (!holds_binding(who))
        return DISK_REVOKE_ENOENT;

    disk_revoke_marked = 0;
    return DISK_REVOKE_OK;
}

int disk_binding_revoke_pending(void)
{
    return disk_binding_owner() != PAGE_OWNER_FREE && disk_revoke_marked;
}

int disk_binding_reclaim(page_owner_t who)
{
    if (!holds_binding(who))
        return DISK_REVOKE_ENOENT;

    disk_owner         = PAGE_OWNER_FREE;
    disk_revoke_marked = 0;
    return DISK_REVOKE_OK;
}
