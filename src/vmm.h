#ifndef VMM_H
#define VMM_H

#include <stdint.h>

#include "multiboot2.h"

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
 * a page; the addresses must be page-aligned. */
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

#endif
