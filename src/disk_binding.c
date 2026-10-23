#include "disk_binding.h"

#include <stddef.h>
#include <stdint.h>

/*
 * disk_binding.c — the disk ownership table (SCRUM-188, extended by
 * SCRUM-237 from one owner to one sector extent per owner).
 *
 * State is whether a drive was found at boot and a small table of
 * {owner, base, count, revoke mark}. Extents never overlap, which is the
 * whole isolation story at this layer: src/syscall_disk.c asks
 * disk_binding_check_range() before every transfer, so a LibOS can only
 * reach sectors it holds. A whole-device claim is just the extent
 * [0, DISK_BIND_LBA_LIMIT), so it conflicts with everyone else by the same
 * overlap rule rather than by a special case.
 *
 * No locking. Syscalls run with interrupts masked (IA32_FMASK clears IF, see
 * src/syscall.c) and the entry path is single-threaded by construction, so
 * the read-modify-write in disk_binding_acquire_extent() cannot be
 * interleaved. That assumption becomes false with preemptive multi-LibOS
 * scheduling and is called out in docs/syscall_spec.md §3.3/§3.5, the same
 * caveat fb_binding.c carries.
 */

typedef struct {
    page_owner_t owner;   /* PAGE_OWNER_FREE = unused slot                  */
    uint32_t     base;
    uint32_t     count;   /* real length, never 0                           */
    int          revoke_marked; /* the kernel has asked `owner` to let go   */
} disk_extent_t;

static int           disk_present;
static disk_extent_t extents[DISK_BIND_MAX_EXTENTS];

/* Volumes assigned by the launcher (SCRUM-237); same shape as `extents` but
 * with no mark -- an assignment is permission, not a binding. */
static disk_extent_t assigned[DISK_BIND_MAX_EXTENTS];

static void clear_all(void)
{
    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++) {
        extents[i].owner         = PAGE_OWNER_FREE;
        extents[i].base          = 0;
        extents[i].count         = 0;
        extents[i].revoke_marked = 0;
    }
}

static disk_extent_t *find(page_owner_t who)
{
    if (who == PAGE_OWNER_FREE)
        return NULL;
    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++)
        if (extents[i].owner == who)
            return &extents[i];
    return NULL;
}

static disk_extent_t *find_assigned(page_owner_t who)
{
    if (who == PAGE_OWNER_FREE)
        return NULL;
    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++)
        if (assigned[i].owner == who)
            return &assigned[i];
    return NULL;
}

/* Do [a, a+an) and [b, b+bn) share a sector? 64-bit so a whole-device end
 * cannot wrap. */
static int overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an;
}

int disk_binding_init(int present)
{
    disk_present = present ? 1 : 0;
    clear_all();
    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++) {
        assigned[i].owner = PAGE_OWNER_FREE;
        assigned[i].base  = 0;
        assigned[i].count = 0;
    }

    return disk_present ? DISK_BIND_OK : DISK_BIND_ENODEV;
}

int disk_binding_present(void)
{
    return disk_present;
}

int disk_binding_acquire(page_owner_t who)
{
    return disk_binding_acquire_extent(who, 0, 0);
}

int disk_binding_acquire_extent(page_owner_t who, uint32_t base,
                                uint32_t count)
{
    if (!disk_present)
        return DISK_BIND_ENODEV;

    /* PAGE_OWNER_FREE is the "unheld" sentinel, so it can never be an owner —
     * acquiring on its behalf would bind the disk to nobody and leave it
     * looking free to the next caller. */
    if (who == PAGE_OWNER_FREE)
        return DISK_BIND_EBUSY;

    if (count == 0) {
        if (base != 0)
            return DISK_BIND_ERANGE;
        count = DISK_BIND_LBA_LIMIT;
    } else if (base >= DISK_BIND_LBA_LIMIT ||
               count > DISK_BIND_LBA_LIMIT - base) {
        return DISK_BIND_ERANGE;
    }

    /* A context with an assigned volume may bind that volume and nothing
     * else; everyone else is kept out of every assigned volume. */
    disk_extent_t *own = find_assigned(who);
    if (own && (own->base != base || own->count != count))
        return DISK_BIND_EPERM;
    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++)
        if (assigned[i].owner != PAGE_OWNER_FREE && assigned[i].owner != who &&
            overlap(base, count, assigned[i].base, assigned[i].count))
            return DISK_BIND_EBUSY;

    disk_extent_t *mine = find(who);
    if (mine) {
        /* Re-acquire of the same extent is success; a different one is not
         * silently swapped in, because the old one may hold a mounted
         * volume the caller has not let go of. */
        return (mine->base == base && mine->count == count)
                   ? DISK_BIND_OK : DISK_BIND_EBUSY;
    }

    disk_extent_t *free_slot = NULL;
    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++) {
        disk_extent_t *e = &extents[i];
        if (e->owner == PAGE_OWNER_FREE) {
            if (!free_slot)
                free_slot = e;
            continue;
        }
        if (overlap(base, count, e->base, e->count))
            return DISK_BIND_EBUSY;
    }

    if (!free_slot)
        return DISK_BIND_ENOSLOT;

    free_slot->owner         = who;
    free_slot->base          = base;
    free_slot->count         = count;
    free_slot->revoke_marked = 0;

    return DISK_BIND_OK;
}

int disk_binding_assign(page_owner_t who, uint32_t base, uint32_t count)
{
    if (!disk_present)
        return DISK_BIND_ENODEV;
    if (who == PAGE_OWNER_FREE || find_assigned(who))
        return DISK_BIND_EBUSY;
    if (count == 0 || base >= DISK_BIND_LBA_LIMIT ||
        count > DISK_BIND_LBA_LIMIT - base)
        return DISK_BIND_ERANGE;

    disk_extent_t *free_slot = NULL;
    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++) {
        if (assigned[i].owner == PAGE_OWNER_FREE) {
            if (!free_slot)
                free_slot = &assigned[i];
        } else if (overlap(base, count, assigned[i].base, assigned[i].count)) {
            return DISK_BIND_EBUSY;
        }
    }
    if (!free_slot)
        return DISK_BIND_ENOSLOT;

    free_slot->owner = who;
    free_slot->base  = base;
    free_slot->count = count;
    return DISK_BIND_OK;
}

int disk_binding_assignment_of(page_owner_t who, uint32_t *base,
                               uint32_t *count)
{
    disk_extent_t *a = find_assigned(who);
    if (!a)
        return DISK_BIND_EBUSY;

    *base  = a->base;
    *count = a->count;
    return DISK_BIND_OK;
}

void disk_binding_forget(page_owner_t who)
{
    disk_extent_t *a = find_assigned(who);
    if (a) {
        a->owner = PAGE_OWNER_FREE;
        a->base  = 0;
        a->count = 0;
    }
}

int disk_binding_extent_of(page_owner_t who, uint32_t *base, uint32_t *count)
{
    disk_extent_t *e = find(who);
    if (!e)
        return DISK_BIND_EBUSY;

    *base  = e->base;
    *count = e->count;
    return DISK_BIND_OK;
}

int disk_binding_check_range(page_owner_t who, uint64_t lba, uint64_t count)
{
    disk_extent_t *e = find(who);
    if (!e)
        return DISK_BIND_EBUSY;

    if (lba < e->base || lba > (uint64_t)e->base + e->count ||
        count > (uint64_t)e->base + e->count - lba)
        return DISK_BIND_ERANGE;

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
    if (!disk_present)
        return PAGE_OWNER_FREE;

    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++)
        if (extents[i].owner != PAGE_OWNER_FREE)
            return extents[i].owner;
    return PAGE_OWNER_FREE;
}

/* ---- Revocation / repossession ------------------------------------------- */

/* Does `who` hold an extent? PAGE_OWNER_FREE never does — it is the "unheld"
 * sentinel, and without this check reclaiming on its behalf would match an
 * unused slot and report a phantom success. */
static disk_extent_t *held(page_owner_t who)
{
    return disk_present ? find(who) : NULL;
}

int disk_binding_revoke_mark(page_owner_t who)
{
    disk_extent_t *e = held(who);
    if (!e)
        return DISK_REVOKE_ENOENT;

    e->revoke_marked = 1;
    return DISK_REVOKE_OK;
}

int disk_binding_revoke_clear(page_owner_t who)
{
    disk_extent_t *e = held(who);
    if (!e)
        return DISK_REVOKE_ENOENT;

    e->revoke_marked = 0;
    return DISK_REVOKE_OK;
}

int disk_binding_revoke_pending(void)
{
    if (!disk_present)
        return 0;

    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++)
        if (extents[i].owner != PAGE_OWNER_FREE && extents[i].revoke_marked)
            return 1;
    return 0;
}

int disk_binding_reclaim(page_owner_t who)
{
    disk_extent_t *e = held(who);
    if (!e)
        return DISK_REVOKE_ENOENT;

    e->owner         = PAGE_OWNER_FREE;
    e->base          = 0;
    e->count         = 0;
    e->revoke_marked = 0;
    return DISK_REVOKE_OK;
}
