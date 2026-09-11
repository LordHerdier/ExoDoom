#ifndef VMM_H
#define VMM_H

#include <stdint.h>

#include "multiboot2.h"
#include "page_alloc.h"   /* page_owner_t — address-space registry keys on it */

/*
 * vmm — the kernel's own 4-level page tables (SCRUM-15).
 *
 * boot.s builds a throwaway identity map in .bss (4 GB of 2 MB pages) purely
 * to get long mode running.  That map is static, blanket, and unowned: it
 * cannot describe per-region permissions, it maps 4 GB of address space that
 * mostly does not exist, and its tables are not pages the PMM knows about, so
 * nothing can ever be mapped or unmapped at runtime.
 *
 * vmm_init() replaces it with tables built from PMM pages (PAGE_OWNER_KERNEL,
 * so no LibOS can ever free one — docs/memory.md §6) that map only what the
 * kernel actually has: low memory, the kernel image and bump pool, usable RAM
 * (which covers the WAD module and the multiboot info), and the framebuffer
 * aperture.  Everything is identity-mapped, so virtual == physical still holds
 * and nothing downstream has to think about translation yet.
 *
 * What this buys, beyond being able to say the kernel owns its own map:
 * vmm_map_page()/vmm_unmap_page() are the primitive SCRUM-16 (framebuffer/WAD
 * mapping), SCRUM-48 (per-LibOS address spaces) and exo_page_map/-unmap
 * (SCRUM-153) are built on.
 *
 * There is no page-fault handler yet (SCRUM-17), so a fault is still fatal.
 */

#define VMM_PAGE_SIZE       0x1000ULL
#define VMM_LARGE_PAGE_SIZE 0x200000ULL

/* Page-table entry flags.  These are the architectural bits, used both for
 * leaves and (the first three) for the links between levels.
 *
 * Note the absence of NX: bit 63 is reserved unless EFER.NXE is set, and
 * setting it without enabling NXE first faults on the walk.  Enabling NXE and
 * marking the non-text mappings NX belongs with the per-section permission
 * work in SCRUM-16, not here. */
#define VMM_PRESENT (1ULL << 0)
#define VMM_WRITE   (1ULL << 1)
#define VMM_USER    (1ULL << 2)
#define VMM_PWT     (1ULL << 3)
#define VMM_PCD     (1ULL << 4)
#define VMM_HUGE    (1ULL << 7)   /* PS: 2 MB leaf at the PD level */

/* Status codes.  ABI-agnostic like the PMM's (page_alloc.h): the syscall layer
 * maps them to EXO_E* codes when SCRUM-153 exposes mapping to a LibOS. */
#define VMM_OK      0
#define VMM_ENOMEM  (-1)   /* out of pages for a new table                  */
#define VMM_EINVAL  (-2)   /* unaligned or non-canonical address            */
#define VMM_EEXIST  (-3)   /* already mapped to a different physical page   */
#define VMM_ENOENT  (-4)   /* not mapped                                    */

/*
 * Build the kernel address space and load CR3 with it.  `mb` supplies the
 * multiboot info (mapped explicitly, since firmware may place it outside a
 * usable-RAM region) and `fb` the framebuffer geometry, or NULL if GRUB gave
 * us no framebuffer tag.
 *
 * Returns VMM_OK, or a negative status if a table allocation failed or a
 * required region could not be mapped — in which case CR3 is left alone and
 * the boot map stays active, so the caller can still report the failure.
 */
int vmm_init(const struct mb2_info *mb, const struct mb2_tag_framebuffer *fb);

/* Physical address of the kernel PML4, or 0 before vmm_init(). */
uint64_t vmm_kernel_pml4(void);

/* Whether the kernel map is the one in CR3.  False before vmm_init() and
 * after a failed one — in which case the boot map is still live and this
 * module's mappings describe nothing. */
int vmm_is_active(void);

/* How many 4 KiB pages the map's tables occupy — the PMM cost of the address
 * space, reported at boot and asserted against in the tests. */
uint32_t vmm_table_pages(void);

/* Map one 4 KiB page.  Both addresses must be page-aligned.  Mapping an
 * address that already resolves to `paddr` succeeds (and updates the flags);
 * mapping one that resolves elsewhere returns VMM_EEXIST rather than silently
 * repointing it.  A 2 MB leaf covering `vaddr` is split into a page table
 * first, preserving the mappings around it. */
int vmm_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags);

/* Map `size` bytes, using 2 MB leaves wherever the addresses and the
 * remaining length allow and 4 KiB pages elsewhere.  `size` is rounded up to
 * a page; the addresses must be page-aligned.
 *
 * Not atomic: on failure part of the range may already be mapped, and the
 * caller is responsible for unmapping what it asked for.  Fine for boot-time
 * construction (CR3 is not loaded yet, and a failure there is fatal anyway);
 * a runtime caller that cares should map page by page. */
int vmm_map_range(uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags);

/* Unmap one 4 KiB page and flush its TLB entry.  Splits a covering 2 MB leaf
 * so the neighbouring pages stay mapped.  Returns VMM_ENOENT if it was not
 * mapped.  Does not free the physical page — that is the caller's (and, for a
 * LibOS, exo_page_free's) business. */
int vmm_unmap_page(uint64_t vaddr);

/* Resolve `vaddr` through the live tables.  On VMM_OK, *paddr_out (if
 * non-NULL) gets the physical address including the offset within the page,
 * and *flags_out (if non-NULL) the leaf entry's flag bits.  Returns
 * VMM_ENOENT if any level along the walk is not present. */
int vmm_translate(uint64_t vaddr, uint64_t *paddr_out, uint64_t *flags_out);

/*
 * ── Address spaces beyond the kernel's own (SCRUM-48) ───────────────────
 *
 * vmm_map_page/-unmap/-translate above always edit or read kernel_pml4 — the
 * one address space vmm_init() builds. The *_in variants below take an
 * explicit root instead, so the same walker can build and edit a LibOS's
 * private address space without a second copy of next_level()/
 * split_large_page().  vmm_map_page(...) is exactly vmm_map_page_in
 * (vmm_kernel_pml4-backed root, ...); nothing about the plain names changes.
 */

int vmm_map_page_in(uint64_t *pml4, uint64_t vaddr, uint64_t paddr,
                    uint64_t flags);
int vmm_map_range_in(uint64_t *pml4, uint64_t vaddr, uint64_t paddr,
                     uint64_t size, uint64_t flags);
int vmm_unmap_page_in(uint64_t *pml4, uint64_t vaddr);
int vmm_translate_in(uint64_t *pml4, uint64_t vaddr, uint64_t *paddr_out,
                     uint64_t *flags_out);

/*
 * Allocate a new PML4 and give it the kernel's own PML4[0] link verbatim —
 * not a copy of the tables it points to, the same physical PDPT kernel_pml4
 * uses.  That is what makes the two address spaces agree about kernel memory
 * for free: no synchronization is needed when the kernel's own map changes,
 * because there is only one PDPT to change.  The LibOS mapping window's PML4
 * range (VMM_LIBOS_PML4_START..END in vmm.c, derived from EXO_USER_VA_BASE/
 * END in src/exo_syscall.h) is left zero for the caller to map into with
 * vmm_map_page_in().
 *
 * Fails with VMM_EINVAL if vmm_init() has not run (nothing to share yet), or
 * VMM_ENOMEM if the PMM is out of pages.
 */
int vmm_create_address_space(uint64_t *pml4_phys_out);

/*
 * Tear down `owner`'s bound address space in one call: free every private
 * table (the LibOS window's PML4 range, and the PML4 itself), then remove
 * `owner` from the registry. Index 0 — the shared kernel subtree — is left
 * alone unconditionally; freeing it would take down every address space at
 * once, this one included.
 *
 * Deliberately keyed on `owner` rather than a raw `pml4_phys`: a caller that
 * could pass the physical address directly could also replay it after this
 * call already freed it, or after a later vmm_create_address_space() reused
 * the same physical page for an unrelated address space. Going through the
 * registry means a repeat call finds `owner` already unbound and safely
 * does nothing (VMM_ENOENT) instead of freeing memory that is no longer —
 * or is no longer *only* — this address space's.
 *
 * Does not free the physical pages the LibOS window pointed at — unmapping
 * never does (see vmm_unmap_page); that is exo_page_free's/revoke's job, not
 * this one's.
 *
 * Returns VMM_OK, or VMM_ENOENT if `owner` has nothing bound (including a
 * second call after the first already tore it down).
 */
int vmm_destroy_address_space(page_owner_t owner);

/*
 * Load `pml4_phys` into CR3.  The mechanical half of switching to a LibOS's
 * address space; SCRUM-47 is what decides *when* to call it (immediately
 * before the iret into ring 3, and — once there is more than one LibOS —
 * on every context switch). Safe to call from ring 0 against any address
 * space this module built, which is what this ticket's own tests do to prove
 * the kernel survives running on a foreign CR3 before anything depends on
 * it.
 *
 * Returns VMM_EINVAL for a 0 argument rather than loading it — 0 is
 * vmm_address_space_for()'s "no binding" sentinel, and CR3=0 points at the
 * page vmm_init() deliberately leaves unmapped as a NULL guard, which would
 * triple-fault the machine on the very next memory access.
 */
int vmm_switch_address_space(uint64_t pml4_phys);

/*
 * ── Per-context address-space registry ───────────────────────────────────
 *
 * Which PML4 a context (page_owner_t, the same id page_alloc.c/fb_binding.c/
 * revoke.c already use) runs on.  A flat table because v1 has exactly one
 * LibOS; SCRUM-147 is expected to grow VMM_MAX_ADDRESS_SPACES (or replace the
 * lookup with a field on its context struct) once there is more than one
 * entry worth optimizing for — nothing above this layer should assume the
 * lookup is O(1) or unbounded.
 */
#define VMM_MAX_ADDRESS_SPACES 4

/*
 * Bind `owner`'s address space to `pml4_phys` (from
 * vmm_create_address_space, or vmm_kernel_pml4() while a LibOS still shares
 * the kernel's own map).  Rebinding an already-bound owner overwrites the
 * old entry — nothing today calls this to rebind a live context, but nothing
 * stops it either, since there is no teardown-then-rebind ordering to get
 * wrong yet.
 *
 * PAGE_OWNER_FREE and PAGE_OWNER_KERNEL are refused with VMM_EINVAL, same
 * refusal src/revoke.c gives those two ids: neither names a schedulable
 * context. VMM_ENOMEM if the table is full and `owner` is not already in it.
 */
int vmm_bind_address_space(page_owner_t owner, uint64_t pml4_phys);

/* Physical PML4 bound to `owner`, or 0 if none. 0 is safe as a sentinel: page
 * 0 is vmm_init()'s deliberate NULL guard, so it can never be a real PML4's
 * address. */
uint64_t vmm_address_space_for(page_owner_t owner);

/* Clear `owner`'s binding. Does not free the PML4 — call
 * vmm_destroy_address_space() first if that is what is wanted. For a
 * context's exit path once one exists. */
void vmm_unbind_address_space(page_owner_t owner);

#endif
