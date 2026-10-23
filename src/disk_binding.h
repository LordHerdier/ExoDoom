#pragma once

#include <stdint.h>
#include "page_alloc.h"   /* page_owner_t — the resource ownership tag type */

/*
 * disk_binding.h — disk secure binding (SCRUM-188, epic SCRUM-151).
 *
 * exo_disk_read/exo_disk_write (#24/#25, SCRUM-103) shipped with no
 * ownership concept: the raw disk was the one exokernel resource any LibOS
 * could touch regardless of who else was using it. This module makes it an
 * owned resource in the same sense as the framebuffer (src/fb_binding.c),
 * whose pre-SCRUM-112 shape — a single exclusive owner, established by an
 * explicit acquire call — is the direct template: a disk head can't be
 * virtualized into one private copy per caller the way a framebuffer can
 * (src/fb_shadow.c), so there is no per-context multiplexing to reach for
 * here.
 *
 *   - exo_disk_acquire (#26) **establishes** the binding — one owner at a
 *     time, -EXO_EBUSY for anyone else.
 *   - exo_disk_read/exo_disk_write **enforce** it — a caller that does not
 *     hold the binding gets -EXO_EBUSY, whether nobody holds it yet or
 *     someone else does.
 *   - revoke_request()/revoke_force() (src/revoke.c) can reclaim it the same
 *     way they do the framebuffer.
 *
 * SCRUM-237 generalises the single owner slot to a table of sector extents
 * (LBA ranges), one per owner, which must not overlap. That is what lets two
 * LibOSes each own a filesystem volume on the one drive at the same time: the
 * kernel never learns what is written in an extent, only who may touch which
 * sectors. A bare acquire (no range) is the degenerate case -- the whole
 * device as one extent -- so it conflicts with every other holder, exactly as
 * the single-owner binding did.
 *
 * Unlike fb_binding.c there is no geometry to publish — a disk isn't a
 * rectangle — so the only boot-time fact this module needs is whether a
 * drive answered at all (src/ata.c's ata_init() result, already threaded
 * through syscall_disk_init()).
 */

/* Status codes from disk_binding_acquire(). */
#define DISK_BIND_OK       0    /* caller now holds the disk                */
#define DISK_BIND_EBUSY  (-1)   /* a different context holds it             */
#define DISK_BIND_ENODEV (-2)   /* this machine has no usable drive         */
#define DISK_BIND_ERANGE (-3)   /* extent malformed, or access outside the
                                 * caller's extent                          */
#define DISK_BIND_ENOSLOT (-4)  /* extent table full                        */

#define DISK_BIND_EPERM  (-5)   /* the caller has an assigned extent and this
                                 * is not it                                */

/* Most owners that can hold an extent at once. */
#define DISK_BIND_MAX_EXTENTS 8

/* 28-bit LBA addressing (src/ata.h): sector count of the largest addressable
 * disk. A whole-device claim is the extent [0, DISK_BIND_LBA_LIMIT). */
#define DISK_BIND_LBA_LIMIT 0x10000000u

/* Status codes from the revocation entry points, mirroring fb_binding.h.
 * ENOENT is not a failure of the protocol: it is how the kernel learns the
 * context already let the disk go. */
#define DISK_REVOKE_OK      0   /* marked / cleared / reclaimed             */
#define DISK_REVOKE_ENOENT (-1) /* `who` does not hold the disk             */

/*
 * Record whether this machine has a usable drive, and drop any existing
 * binding. Called once from syscall_disk_init() with ata_init()'s own
 * result, ahead of the TESTING branch so acquire works identically on a
 * normal boot and under the test runner. Returns DISK_BIND_OK if `present`
 * is nonzero, DISK_BIND_ENODEV otherwise.
 */
int disk_binding_init(int present);

/* Whether disk_binding_init() was told a drive exists. The single source of
 * truth for disk presence — src/syscall_disk.c's -EXO_ENODEV check reads
 * this rather than keeping its own copy of ata_init()'s result, so the two
 * can never disagree (unlike a caller re-deriving presence independently). */
int disk_binding_present(void);

/*
 * Bind the disk to `who`.
 *
 *   DISK_BIND_OK      `who` now owns it — including the case where it
 *                      already did, so a LibOS that acquires twice is not
 *                      punished for it. Only "another LibOS holds it" is
 *                      -EBUSY.
 *   DISK_BIND_EBUSY    somebody else owns it
 *   DISK_BIND_ENODEV   no drive exists
 */
int disk_binding_acquire(page_owner_t who);

/*
 * Bind the sectors [base, base + count) to `who` (SCRUM-237). `count == 0`
 * means the whole device and requires `base == 0`; anything else must lie
 * inside [0, DISK_BIND_LBA_LIMIT) without wrapping (DISK_BIND_ERANGE).
 *
 *   DISK_BIND_OK      `who` now holds exactly this extent -- including when
 *                      it already held that same extent.
 *   DISK_BIND_EBUSY    the extent overlaps another owner's, or `who` already
 *                      holds a *different* extent (release first)
 *   DISK_BIND_ENOSLOT  the table is full
 *   DISK_BIND_ENODEV   no drive exists
 *   DISK_BIND_ERANGE   malformed extent
 */
int disk_binding_acquire_extent(page_owner_t who, uint32_t base,
                                uint32_t count);

/*
 * Assign `who` the extent [base, base + count) as its volume (SCRUM-237).
 * Set by the launcher from the package manifest before the program runs.
 * An assignment does not bind anything by itself; it constrains acquires:
 *
 *   - `who` may acquire only exactly this extent (anything else, including a
 *     whole-device claim, is DISK_BIND_EPERM), and
 *   - no *other* context may acquire anything overlapping it
 *     (DISK_BIND_EBUSY), whether or not `who` currently holds it.
 *
 * That is what stops a program asking for someone else's sectors, and is why
 * a volume stays private while its owner is between a release and the next
 * acquire. Returns DISK_BIND_OK, DISK_BIND_ERANGE (count == 0 or off the end
 * of the disk), DISK_BIND_EBUSY (overlaps another assignment, or `who`
 * already has one), DISK_BIND_ENOSLOT, or DISK_BIND_ENODEV.
 */
int disk_binding_assign(page_owner_t who, uint32_t base, uint32_t count);

/* The extent assigned to `who`: DISK_BIND_OK with base and count filled, or
 * DISK_BIND_EBUSY if `who` has none. */
int disk_binding_assignment_of(page_owner_t who, uint32_t *base,
                               uint32_t *count);

/* Drop `who`'s assignment, if any (and nothing else -- a held extent is
 * released through disk_binding_release()). Called when the context is torn
 * down (revoke_all()); a voluntary exo_disk_release must NOT call it, or the
 * program could re-acquire an arbitrary range afterwards. */
void disk_binding_forget(page_owner_t who);

/* The extent `who` holds, written to *base / *count (count is the real
 * length, never 0). DISK_BIND_OK, or DISK_BIND_EBUSY if `who` holds none. */
int disk_binding_extent_of(page_owner_t who, uint32_t *base, uint32_t *count);

/*
 * Whether `who` may touch sectors [lba, lba + count): DISK_BIND_OK if the
 * range lies entirely inside its extent, DISK_BIND_EBUSY if `who` holds no
 * extent at all, DISK_BIND_ERANGE if it holds one and the range leaves it.
 * This is the enforcement point exo_disk_read/exo_disk_write call.
 */
int disk_binding_check_range(page_owner_t who, uint64_t lba, uint64_t count);

/*
 * Release the binding if `who` holds it; a no-op otherwise, so reclamation
 * can call it unconditionally for a context that may never have acquired.
 * This is the voluntary return in the revocation protocol
 * (docs/syscall_spec.md §3.6) and clears any pending mark along with the
 * binding. Kernel-driven reclamation goes through revoke_all() in
 * src/revoke.h.
 *
 * Reachable from ring 3 through exo_disk_release (#27, SCRUM-189), which is
 * what makes phase 2 possible for the disk at all: this is a kernel
 * function, so before that syscall existed a LibOS could acquire the disk
 * and had no way to give it back short of exiting.
 */
void disk_binding_release(page_owner_t who);

/* Some current holder, or PAGE_OWNER_FREE when nothing is held. With only
 * whole-device claims this is *the* owner, as before SCRUM-237; with several
 * extents it is the first held entry and callers that care who holds what
 * should use disk_binding_extent_of(). */
page_owner_t disk_binding_owner(void);

/* ---- Revocation / repossession ------------------------------------------
 *
 * The disk half of the protocol in docs/syscall_spec.md §3.6, mirroring
 * fb_binding_revoke_mark / fb_binding_reclaim. src/revoke.c sequences them;
 * this module only knows how to mark the binding and how to take it back.
 */

/* Phase 1 — record that the kernel wants the disk back from `who`. The
 * binding is untouched: a marked owner still owns the disk and disk_read/
 * write for it still succeed, because the mark is an ask, not a seizure.
 * Idempotent. DISK_REVOKE_ENOENT if `who` does not hold it. */
int disk_binding_revoke_mark(page_owner_t who);

/* Withdraw a mark set by disk_binding_revoke_mark(). DISK_REVOKE_ENOENT if
 * `who` does not hold the disk; clearing an unmarked binding it does hold is
 * DISK_REVOKE_OK. */
int disk_binding_revoke_clear(page_owner_t who);

/* Whether the current binding is marked for revocation. 0 when the disk is
 * unheld or unmarked. */
int disk_binding_revoke_pending(void);

/* Phase 3 — take the disk back from `who`, marked or not. (Phase 2 is `who`
 * complying, via disk_binding_release() -- exo_disk_release, #27, from ring
 * 3.) Returns DISK_REVOKE_OK if it was
 * taken, DISK_REVOKE_ENOENT if `who` did not hold it (it complied, or
 * another context has since acquired — in which case the reclaim must leave
 * that context's binding alone). disk_binding_release() is the voluntary
 * form of the same operation and discards the status. */
int disk_binding_reclaim(page_owner_t who);
