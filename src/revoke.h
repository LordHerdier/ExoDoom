#pragma once
#include <stdint.h>

#include "page_alloc.h"   /* page_owner_t — the resource ownership tag type */

/*
 * revoke.h — resource revocation / repossession (SCRUM-156, epic SCRUM-151).
 *
 * Secure binding (SCRUM-152, -154) answers "may this LibOS touch this
 * resource?".  Revocation answers the other half of the exokernel bargain:
 * having granted a resource, the kernel can take it back.  Aegis calls this
 * *repossession*, and it is what makes an exokernel's grants safe to hand out
 * generously — the kernel never has to refuse a request out of fear that it
 * can never get the resource back.
 *
 * This file is the protocol; the mechanisms live one layer down and know
 * nothing about each other:
 *
 *   pages        page_revoke_mark / page_reclaim / page_reclaim_all
 *                (src/page_alloc.h — the mark is a bit in the owner tag)
 *   framebuffer  fb_binding_revoke_mark / fb_binding_reclaim
 *                (src/fb_binding.h — the mark is a flag beside the binding)
 *
 * The layering matches page_alloc.c / syscall_mem.c: everything here is
 * ABI-agnostic and speaks REVOKE_* status codes.  Nothing in this header is a
 * syscall; when SCRUM-155 binds exo_exit (#20) its handler calls revoke_all().
 *
 * The protocol, in full (docs/syscall_spec.md §3.6):
 *
 *   1. request  the kernel marks the resource — "I want this back".  The
 *               owner keeps it and keeps using it; the mark is an ask, not a
 *               seizure.  Under multi-LibOS scheduling (SCRUM-147) this is
 *               where the upcall to the LibOS's repossession handler goes.
 *   2. comply   the LibOS returns the resource the ordinary way
 *               (exo_page_free, or releasing the framebuffer).  The mark
 *               disappears with the binding; there is nothing to reconcile.
 *   3. force    the kernel takes what was not returned.  revoke_force() for
 *               one resource, revoke_all() for everything a context holds.
 *
 * v1 runs one LibOS and has no upcall path, so the *policy* is trivial: step 1
 * never fires on the boot path and step 3 happens only when a context exits.
 * The mechanism, its data structures and its accounting are complete, which is
 * what SCRUM-147 needs in place before it can schedule two LibOSes against one
 * framebuffer.
 */

/* Which resource a revocation names.  Deliberately not a bitmask: a request
 * names one resource, and "all of them" is revoke_all(), whose sweep is a
 * different operation from revoking a resource the caller can name. */
typedef enum {
    REVOKE_PAGE        = 0,   /* one 4 KiB physical page, named by `paddr` */
    REVOKE_FRAMEBUFFER = 1,   /* the framebuffer binding                   */
} revoke_kind_t;

typedef struct {
    revoke_kind_t kind;
    uint64_t      paddr;      /* REVOKE_PAGE only; ignored otherwise */
} revoke_res_t;

/*
 * Status codes.  REVOKE_RETURNED is not a failure: it is the protocol
 * succeeding by the other route — the kernel asked, the LibOS gave the
 * resource back, and the force step found nothing left to take.  Callers that
 * only care whether the context still holds the resource afterwards can treat
 * OK and RETURNED alike; the accounting in revoke_record() distinguishes them.
 */
#define REVOKE_OK         0   /* the kernel took the resource               */
#define REVOKE_RETURNED   1   /* `who` no longer holds it — nothing to take */
#define REVOKE_EINVAL   (-1)  /* malformed resource (bad paddr, bad kind)   */
#define REVOKE_ENOENT   (-2)  /* `who` does not hold the named resource     */

/* Resource constructors, so call sites read as revoke_force(ctx,
 * revoke_res_page(p)) rather than assembling a struct literal each time. */
static inline revoke_res_t revoke_res_page(uint64_t paddr)
{
    revoke_res_t r;

    r.kind  = REVOKE_PAGE;
    r.paddr = paddr;

    return r;
}

static inline revoke_res_t revoke_res_fb(void)
{
    revoke_res_t r;

    r.kind  = REVOKE_FRAMEBUFFER;
    r.paddr = 0;

    return r;
}

/*
 * Phase 1 — ask `who` for `what` back.  Marks the resource in the ownership
 * table and leaves it entirely usable by its owner: a marked page can still be
 * read, written, freed and (SCRUM-153) mapped, and a marked framebuffer still
 * passes fb_binding_check_map().  Idempotent.
 *
 * REVOKE_OK, REVOKE_ENOENT when `who` does not hold the resource, or
 * REVOKE_EINVAL for a malformed one.
 */
int revoke_request(page_owner_t who, revoke_res_t what);

/* Withdraw a request.  Same return values as revoke_request().  Exists because
 * a scheduler that asked for a page and then satisfied the demand elsewhere
 * must be able to take the ask back — otherwise the mark outlives the reason
 * for it and the next sweep reads as forced. */
int revoke_withdraw(page_owner_t who, revoke_res_t what);

/* Whether `what` is currently marked.  0 for an unmarked, unheld or malformed
 * resource — none of which is pending. */
int revoke_pending(revoke_res_t what);

/*
 * Phase 3 — take `what` from `who`, with or without a prior request: exo_exit
 * reclamation is an unconditional force, and the two-phase sequence is the
 * polite path to the same call.
 *
 * REVOKE_OK       the resource was reclaimed
 * REVOKE_RETURNED `who` no longer holds it (it complied, or the resource has
 *                 since been granted to another context — either way this call
 *                 leaves that context's grant alone)
 * REVOKE_EINVAL   malformed resource
 */
int revoke_force(page_owner_t who, revoke_res_t what);

/*
 * Reclaim everything `who` holds — every page tagged with its id, plus the
 * framebuffer if it holds it — and return how many resources were taken
 * (pages + framebuffer).  This is the whole of the v1 revocation policy and
 * the hook SCRUM-155's exo_exit calls; nothing on the boot path calls it yet,
 * so a LibOS that exits today keeps its pages and the screen.
 *
 * PAGE_OWNER_FREE and PAGE_OWNER_KERNEL are refused (0): neither names a
 * revocable context, and sweeping the kernel would free the page bitmap, the
 * owner table, the kernel image and the WAD module out from under the system.
 */
uint32_t revoke_all(page_owner_t who);

/*
 * The repossession record — Aegis's "repossession vector", reduced to what a
 * single-LibOS kernel can act on.  It is the audit trail the design note
 * promises: how often the kernel asked, how often asking was enough, and how
 * often it had to take the resource itself.  Machine-wide in v1 because there
 * is one LibOS; SCRUM-147 makes it a field of the context structure.
 */
typedef struct {
    uint32_t requested;        /* revoke_request calls that marked something  */
    uint32_t withdrawn;        /* requests taken back before the force step   */
    uint32_t forced;           /* resources the kernel had to take            */
    uint32_t returned;         /* forces that found the resource already back */
    uint32_t pages_reclaimed;  /* pages taken, revoke_all sweeps included     */
    uint32_t fb_reclaimed;     /* framebuffer bindings taken                  */
} revoke_record_t;

/* The live record.  The pointer is to module-static storage and stays valid
 * for the life of the kernel. */
const revoke_record_t *revoke_record(void);

/* Zero the record.  For tests, and for the point where SCRUM-147 hands a
 * context id to a new LibOS. */
void revoke_record_reset(void);
