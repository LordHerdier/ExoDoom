#include "disk_binding.h"
#include "blkdev.h"

#include <stddef.h>
#include <stdint.h>

/*
 * disk_binding.c — the disk ownership table (SCRUM-188, extended by
 * SCRUM-237 from one owner to one sector extent per owner, and by SCRUM-244
 * to one extent per owner *per device*).
 *
 * State is a small table of
 * {owner, device, base, count, revoke mark}. Extents on the same device
 * never overlap, which is the
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
    uint32_t     dev;     /* block device index (src/blkdev.h); 0 for every
                           * assignment -- volume slots are a device-0 layout */
    uint32_t     base;
    uint32_t     count;   /* real length, never 0                           */
    int          revoke_marked; /* the kernel has asked `owner` to let go   */
    int          read_only;     /* SCRUM-75: reads only; may be shared with
                                 * other read-only holders                  */
} disk_extent_t;

static disk_extent_t extents[DISK_BIND_MAX_EXTENTS];

/* Volumes assigned by the launcher (SCRUM-237); same shape as `extents` but
 * with no mark -- an assignment is permission, not a binding. Always device
 * 0: the slot region (src/exo_volume.h) is a system-disk layout. */
static disk_extent_t assigned[DISK_BIND_MAX_EXTENTS];

static void clear_slot(disk_extent_t *e)
{
    e->owner         = PAGE_OWNER_FREE;
    e->dev           = 0;
    e->base          = 0;
    e->count         = 0;
    e->revoke_marked = 0;
    e->read_only     = 0;
}

static disk_extent_t *find(page_owner_t who, uint32_t dev)
{
    if (who == PAGE_OWNER_FREE)
        return NULL;
    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++)
        if (extents[i].owner == who && extents[i].dev == dev)
            return &extents[i];
    return NULL;
}

/* Any extent `who` holds, on any device -- what the revocation entry points
 * act on, since a context being reclaimed gives up every disk it holds. */
static disk_extent_t *find_any(page_owner_t who)
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
 * cannot wrap. Callers compare devices first: the same LBA on two devices is
 * two different sectors. */
static int overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an;
}

int disk_binding_init(void)
{
    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++) {
        clear_slot(&extents[i]);
        clear_slot(&assigned[i]);
    }

    return blkdev_count() ? DISK_BIND_OK : DISK_BIND_ENODEV;
}

/* The registry, not a copy of it: a device exists here exactly when
 * blkdev_get() has it, so the two can never disagree. */
int disk_binding_present(uint32_t dev)
{
    return blkdev_get(dev) != NULL;
}

int disk_binding_acquire(page_owner_t who, uint32_t dev)
{
    return disk_binding_acquire_extent(who, dev, 0, 0);
}

int disk_binding_acquire_extent(page_owner_t who, uint32_t dev, uint32_t base,
                                uint32_t count)
{
    if (!disk_binding_present(dev))
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
     * else -- no other device either, or a packaged program could reach a
     * second disk just by asking; everyone else is kept out of every
     * assigned volume. */
    disk_extent_t *own = find_assigned(who);
    if (own && (own->dev != dev || own->base != base || own->count != count))
        return DISK_BIND_EPERM;
    /* A read-only assignment grants no exclusivity, so it keeps nobody out. */
    int ro = own != NULL && own->read_only;
    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++)
        if (assigned[i].owner != PAGE_OWNER_FREE && assigned[i].owner != who &&
            !assigned[i].read_only && assigned[i].dev == dev &&
            overlap(base, count, assigned[i].base, assigned[i].count))
            return DISK_BIND_EBUSY;

    disk_extent_t *mine = find(who, dev);
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
        if (e->dev == dev && overlap(base, count, e->base, e->count) &&
            !(ro && e->read_only))
            return DISK_BIND_EBUSY;
    }

    if (!free_slot)
        return DISK_BIND_ENOSLOT;

    /* Revocation is per context: if the kernel has asked `who` to give its
     * disk binding back, an extent it takes now is part of what it was asked
     * for. Starting it unmarked would let a context that has been asked to
     * comply pick up a second device, release the first, and look as if it
     * had complied while still holding a disk. */
    int marked = disk_binding_revoke_pending(who);

    free_slot->owner         = who;
    free_slot->dev           = dev;
    free_slot->base          = base;
    free_slot->count         = count;
    free_slot->revoke_marked = marked;
    free_slot->read_only     = ro;

    return DISK_BIND_OK;
}

int disk_binding_assign(page_owner_t who, uint32_t base, uint32_t count)
{
    if (!disk_binding_present(0))
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
        } else if (!assigned[i].read_only &&
                   overlap(base, count, assigned[i].base, assigned[i].count)) {
            return DISK_BIND_EBUSY;
        }
    }
    /* Another context already *holding* sectors in the volume (a whole-device
     * claim, say) would make the assignee's own acquire fail later, after the
     * launch had reported success. Refuse here instead. */
    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++)
        if (extents[i].owner != PAGE_OWNER_FREE && extents[i].owner != who &&
            extents[i].dev == 0 &&
            overlap(base, count, extents[i].base, extents[i].count))
            return DISK_BIND_EBUSY;
    if (!free_slot)
        return DISK_BIND_ENOSLOT;

    free_slot->owner = who;
    free_slot->dev   = 0;
    free_slot->base  = base;
    free_slot->count = count;
    return DISK_BIND_OK;
}

int disk_binding_assign_ro(page_owner_t who, uint32_t base, uint32_t count)
{
    if (!disk_binding_present(0))
        return DISK_BIND_ENODEV;
    if (who == PAGE_OWNER_FREE || find_assigned(who))
        return DISK_BIND_EBUSY;
    if (count == 0 || base >= DISK_BIND_LBA_LIMIT ||
        count > DISK_BIND_LBA_LIMIT - base)
        return DISK_BIND_ERANGE;

    /* Readers may share, but never with a private read-write volume: refuse
     * here rather than let the launched program's acquire fail later. */
    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++)
        if (assigned[i].owner != PAGE_OWNER_FREE && !assigned[i].read_only &&
            overlap(base, count, assigned[i].base, assigned[i].count))
            return DISK_BIND_EBUSY;

    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++) {
        if (assigned[i].owner == PAGE_OWNER_FREE) {
            assigned[i].owner     = who;
            assigned[i].dev       = 0;
            assigned[i].base      = base;
            assigned[i].count     = count;
            assigned[i].read_only = 1;
            return DISK_BIND_OK;
        }
    }
    return DISK_BIND_ENOSLOT;
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
    if (a)
        clear_slot(a);
}

int disk_binding_extent_of(page_owner_t who, uint32_t dev, uint32_t *base,
                           uint32_t *count)
{
    disk_extent_t *e = find(who, dev);
    if (!e)
        return DISK_BIND_EBUSY;

    *base  = e->base;
    *count = e->count;
    return DISK_BIND_OK;
}

int disk_binding_check_range(page_owner_t who, uint32_t dev, uint64_t lba,
                             uint64_t count)
{
    disk_extent_t *e = find(who, dev);
    if (!e)
        return DISK_BIND_EBUSY;

    if (lba < e->base || lba > (uint64_t)e->base + e->count ||
        count > (uint64_t)e->base + e->count - lba)
        return DISK_BIND_ERANGE;

    return DISK_BIND_OK;
}

int disk_binding_check_write(page_owner_t who, uint32_t dev, uint64_t lba,
                             uint64_t count)
{
    int rc = disk_binding_check_range(who, dev, lba, count);
    if (rc != DISK_BIND_OK)
        return rc;

    return find(who, dev)->read_only ? DISK_BIND_EROFS : DISK_BIND_OK;
}

void disk_binding_release(page_owner_t who, uint32_t dev)
{
    /* The voluntary return, and the compliance half of the revocation
     * protocol: dropping the binding drops the mark with it. One device
     * only -- an installer holding the source and the system disk lets go
     * of each when it is done with it. */
    disk_extent_t *e = find(who, dev);
    if (e)
        clear_slot(e);
}

page_owner_t disk_binding_owner(uint32_t dev)
{
    if (!disk_binding_present(dev))
        return PAGE_OWNER_FREE;

    page_owner_t sole = PAGE_OWNER_FREE;
    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++) {
        if (extents[i].owner == PAGE_OWNER_FREE || extents[i].dev != dev)
            continue;
        if (sole != PAGE_OWNER_FREE)
            return PAGE_OWNER_FREE; /* several holders: no single owner */
        sole = extents[i].owner;
    }
    return sole;
}

/* ---- Revocation / repossession -------------------------------------------
 *
 * Per context, not per device (SCRUM-244): the kernel asks a context to give
 * back the disk, and reclaims it, as one resource, so every extent the
 * context holds on every device is marked, cleared or taken together. That
 * keeps src/revoke.c's REVOKE_DISK a single resource kind. */

/* PAGE_OWNER_FREE never holds anything — it is the "unheld" sentinel, and
 * without find_any()'s check reclaiming on its behalf would match an unused
 * slot and report a phantom success. */
int disk_binding_revoke_mark(page_owner_t who)
{
    if (!find_any(who))
        return DISK_REVOKE_ENOENT;

    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++)
        if (extents[i].owner == who)
            extents[i].revoke_marked = 1;
    return DISK_REVOKE_OK;
}

int disk_binding_revoke_clear(page_owner_t who)
{
    if (!find_any(who))
        return DISK_REVOKE_ENOENT;

    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++)
        if (extents[i].owner == who)
            extents[i].revoke_marked = 0;
    return DISK_REVOKE_OK;
}

int disk_binding_revoke_pending(page_owner_t who)
{
    if (who == PAGE_OWNER_FREE)
        return 0;
    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++)
        if (extents[i].owner == who && extents[i].revoke_marked)
            return 1;
    return 0;
}

int disk_binding_reclaim(page_owner_t who)
{
    if (!find_any(who))
        return DISK_REVOKE_ENOENT;

    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++)
        if (extents[i].owner == who)
            clear_slot(&extents[i]);
    return DISK_REVOKE_OK;
}
