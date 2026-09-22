#pragma once

#include "page_alloc.h"   /* page_owner_t — the resource ownership tag type */

/*
 * disk_binding.h — disk secure binding (SCRUM-188, epic SCRUM-151).
 *
 * exo_disk_read/exo_disk_write (#27/#28, SCRUM-103) shipped with no
 * ownership concept: the raw disk was the one exokernel resource any LibOS
 * could touch regardless of who else was using it. This module makes it an
 * owned resource in the same sense as the framebuffer (src/fb_binding.c),
 * whose pre-SCRUM-112 shape — a single exclusive owner, established by an
 * explicit acquire call — is the direct template: a disk head can't be
 * virtualized into one private copy per caller the way a framebuffer can
 * (src/fb_shadow.c), so there is no per-context multiplexing to reach for
 * here.
 *
 *   - exo_disk_acquire (#29) **establishes** the binding — one owner at a
 *     time, -EXO_EBUSY for anyone else.
 *   - exo_disk_read/exo_disk_write **enforce** it — a caller that does not
 *     hold the binding gets -EXO_EBUSY, whether nobody holds it yet or
 *     someone else does.
 *   - revoke_request()/revoke_force() (src/revoke.c) can reclaim it the same
 *     way they do the framebuffer.
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
 * Release the binding if `who` holds it; a no-op otherwise, so reclamation
 * can call it unconditionally for a context that may never have acquired.
 * This is the voluntary return in the revocation protocol
 * (docs/syscall_spec.md §3.6) and clears any pending mark along with the
 * binding. Kernel-driven reclamation goes through revoke_all() in
 * src/revoke.h.
 */
void disk_binding_release(page_owner_t who);

/* Current owner, or PAGE_OWNER_FREE when the disk is unheld. */
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
 * complying, via disk_binding_release().) Returns DISK_REVOKE_OK if it was
 * taken, DISK_REVOKE_ENOENT if `who` did not hold it (it complied, or
 * another context has since acquired — in which case the reclaim must leave
 * that context's binding alone). disk_binding_release() is the voluntary
 * form of the same operation and discards the status. */
int disk_binding_reclaim(page_owner_t who);
