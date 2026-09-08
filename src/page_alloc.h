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
 * (SCRUM-147); v1 uses the single id PAGE_OWNER_LIBOS.
 */
typedef uint16_t page_owner_t;

#define PAGE_OWNER_FREE    ((page_owner_t)0)
#define PAGE_OWNER_KERNEL  ((page_owner_t)1)
#define PAGE_OWNER_LIBOS   ((page_owner_t)2)

/* Status codes from free_page_owned().  ABI-agnostic on purpose: the PMM does
 * not know about EXO_E* codes, so the syscall layer maps these to errnos. */
#define PAGE_FREE_OK       0    /* freed; owner reset to PAGE_OWNER_FREE       */
#define PAGE_FREE_EINVAL   (-1) /* unaligned/out-of-range addr, or double free */
#define PAGE_FREE_EPERM    (-2) /* valid allocated page owned by someone else  */

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

// One past the last address the allocator manages (managed_base +
// total_pages * PAGE_SIZE), or 0 if page_alloc_init() hasn't run yet. Lets
// callers (tests included) derive a genuinely out-of-range address instead of
// hardcoding one that assumes a particular QEMU memory size.
uintptr_t page_alloc_pool_end(void);

#endif
