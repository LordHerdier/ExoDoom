#ifndef PAGE_ALLOC_H
#define PAGE_ALLOC_H

#include <stdint.h>

#include "multiboot2.h"

/*
 * Physical page ownership (SCRUM-152, secure-binding epic SCRUM-151).
 *
 * Every page the allocator manages carries an owner tag alongside its
 * allocated/free bit.  The tag is the concrete mechanism behind the isolation
 * guarantee in docs/architecture.md §2 / docs/syscall_spec.md §3.3: a resource
 * syscall (exo_page_free, and later exo_page_map/-unmap in SCRUM-153) enforces
 * the tag so a LibOS can only touch pages it owns.
 *
 *   PAGE_OWNER_FREE    the page is not allocated
 *   PAGE_OWNER_KERNEL  reserved kernel memory (bitmap, owner table, kernel
 *                      image, WAD module) or a page taken by kernel-internal
 *                      alloc_page(); never handed to or freeable by a LibOS
 *   >= PAGE_OWNER_LIBOS a specific LibOS context id
 *
 * The type is 16-bit to size the metadata for the multi-LibOS future
 * (SCRUM-147); v1 uses the single id PAGE_OWNER_LIBOS.  Its top bit is the
 * revocation mark (SCRUM-156, below), so the id range is the low 15 bits.
 */
typedef uint16_t page_owner_t;

#define PAGE_OWNER_FREE    ((page_owner_t)0)
#define PAGE_OWNER_KERNEL  ((page_owner_t)1)
#define PAGE_OWNER_LIBOS   ((page_owner_t)2)

/*
 * Revocation mark (SCRUM-156).  The top bit of a page's tag records that the
 * kernel has *asked* for the page back; the remaining 15 bits still name the
 * owner, because a page under revocation is still that context's page until
 * the kernel actually takes it.  Marking is phase 1 of the repossession
 * protocol in docs/syscall_spec.md §3.6 — an ask, not a seizure: a marked page
 * can still be read, written and (SCRUM-153) mapped by its owner.
 *
 * Keeping the mark inside the owner tag rather than in a parallel array is
 * what makes compliance free: free_page_owned() resets the tag to
 * PAGE_OWNER_FREE, which clears owner and mark in the same store, so a LibOS
 * that returns a marked page leaves nothing behind to reconcile.
 *
 * page_owner() masks the bit off, so every existing comparison against
 * PAGE_OWNER_* keeps working; ask page_revoke_pending() for the mark itself.
 * 15 bits still leave 32,766 LibOS context ids for SCRUM-147.
 */
#define PAGE_OWNER_REVOKED ((page_owner_t)0x8000)
#define PAGE_OWNER_ID_MASK ((page_owner_t)0x7FFF)

/* Status codes from free_page_owned().  ABI-agnostic on purpose: the PMM does
 * not know about EXO_E* codes, so the syscall layer maps these to errnos. */
#define PAGE_FREE_OK       0    /* freed; owner reset to PAGE_OWNER_FREE       */
#define PAGE_FREE_EINVAL   (-1) /* unaligned/out-of-range addr, or double free */
#define PAGE_FREE_EPERM    (-2) /* valid allocated page owned by someone else  */

/* Status codes from the revocation entry points below.  Same shape as
 * PAGE_FREE_* (0 success, negative failure) but a distinct set, because
 * "the context does not hold this page" is not an error in a revocation — it
 * is how the kernel learns the LibOS already complied. */
#define PAGE_REVOKE_OK     0    /* marked / cleared / reclaimed                */
#define PAGE_REVOKE_EINVAL (-1) /* unaligned or out-of-range address           */
#define PAGE_REVOKE_ENOENT (-2) /* `owner` does not hold this page (free, or
                                 * allocated to somebody else)                 */

// Initializes the bitmap allocator using the first eligible usable memory region.
//
// NOTE: currently only the first large usable region is managed.

void page_alloc_init(const struct mb2_info* mb);

// Allocate one 4K page and stamp `owner` as its owner.  Returns the physical
// address or NULL when the pool is exhausted.
void* alloc_page_owned(page_owner_t owner);

// Free a page previously handed out to `owner`.  Returns PAGE_FREE_OK on
// success (owner reset to FREE, bit cleared), PAGE_FREE_EINVAL for a bad
// address or a double free, PAGE_FREE_EPERM when the page is allocated but
// owned by a different context (including PAGE_OWNER_KERNEL).
int free_page_owned(void* addr, page_owner_t owner);

// Owner of the page containing `addr`, or PAGE_OWNER_FREE for an
// unaligned/out-of-range/free page.  For enforcement and tests.
page_owner_t page_owner(void* addr);

// Kernel-facing allocation: a page owned by PAGE_OWNER_KERNEL, so no LibOS can
// ever free it.  Use this for kernel-internal pages (e.g. future page tables).
void* alloc_page(void);

void free_page(void* addr);

// Like free_page, but reports the outcome so callers can distinguish success
// from a bad request.  Returns 0 on success and non-zero on an
// unaligned/out-of-range address or a double free.  Frees a KERNEL-owned page
// (the kernel-internal contract of free_page); ownership-checked freeing on
// behalf of a LibOS goes through free_page_owned().  free_page() is a void
// wrapper around this.
int free_page_checked(void* addr);

/* ---- Revocation / repossession (SCRUM-156) ------------------------------
 *
 * The primitives the revocation protocol (src/revoke.c) is built from.  They
 * live here because only this file can see the owner table; the policy that
 * decides *when* to revoke lives in src/revoke.c, and docs/syscall_spec.md
 * §3.6 is the protocol they implement.
 */

// Phase 1 — mark the page containing `addr` as pending revocation.  The page
// keeps its owner and stays fully usable; only the mark changes.  Idempotent.
// Returns PAGE_REVOKE_OK, PAGE_REVOKE_EINVAL for a bad address, or
// PAGE_REVOKE_ENOENT when `owner` does not hold the page.
int page_revoke_mark(void* addr, page_owner_t owner);

// Withdraw a mark set by page_revoke_mark().  Same return values; clearing an
// unmarked page owned by `owner` is PAGE_REVOKE_OK, not an error.
int page_revoke_clear(void* addr, page_owner_t owner);

// Whether the page containing `addr` is marked.  0 for an unmarked, free,
// unaligned or out-of-range page — a "no" that never has to be distinguished
// from "no such page", because neither is pending.
int page_revoke_pending(void* addr);

// Phase 2 — take the page back from `owner` whether or not it consents, and
// whether or not it was marked first.  Returns PAGE_REVOKE_OK when the page
// was reclaimed, PAGE_REVOKE_ENOENT when `owner` no longer holds it (it
// complied, or the page has since been handed to another context — either way
// there is nothing to take and the reclaim must not touch it), or
// PAGE_REVOKE_EINVAL for a bad address.
int page_reclaim(void* addr, page_owner_t owner);

// Reclaim every page `owner` holds; returns how many were taken.  This is the
// sweep exo_exit performs (SCRUM-155) and the whole of the v1 revocation
// policy.  PAGE_OWNER_FREE and PAGE_OWNER_KERNEL are refused outright (0
// pages): sweeping the kernel would free the bitmap, the owner table, the
// kernel image and the WAD module out from under a running system.
uint32_t page_reclaim_all(page_owner_t owner);

// How many pages `owner` currently holds.  For reclamation accounting and
// tests; O(total_pages).
uint32_t page_count_owned(page_owner_t owner);

// One past the last address the allocator manages (managed_base +
// total_pages * PAGE_SIZE), or 0 if page_alloc_init() hasn't run yet. Lets
// callers (tests included) derive a genuinely out-of-range address instead of
// hardcoding one that assumes a particular QEMU memory size.
uintptr_t page_alloc_pool_end(void);

#endif
