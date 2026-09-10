#pragma once
#include <stdint.h>

/*
 * vmm.h — 4-level page table walker (SCRUM-35).
 *
 * boot.s builds a static 4 GiB identity map out of 2 MiB pages and enables
 * paging (SCRUM-15); nothing after that ever touched a page table again.  This
 * module is the first code that does: it installs and removes 4 KiB mappings
 * in the live address space (the one CR3 names), allocating the intermediate
 * PDPT/PD/PT pages it needs from the physical page allocator.
 *
 * It is the mechanism under exo_page_map / exo_page_unmap (src/syscall_mem.c)
 * and it is deliberately *only* the mechanism: it enforces no ownership and
 * knows nothing about LibOS contexts or -EXO_E* codes.  The protection rule
 * (docs/syscall_spec.md §3.3 — the caller must own the physical page, or hold
 * the framebuffer binding for it) lives in the syscall handler, in the same
 * layering as page_alloc.c / syscall_mem.c and fb_binding.c / syscall_fb.c.
 *
 * Reaching a page table: every table is a physical page below 4 GiB and the
 * identity map is still in force, so a table's physical address doubles as a
 * pointer to it.  That stops being true the day the kernel moves to a
 * higher-half map (SCRUM-48), at which point table_ptr() in vmm.c is the one
 * place that has to learn about an offset.
 */

#define VMM_PAGE_SIZE 4096u

/*
 * Mapping attributes.  A mapping is always present — that is what mapping
 * means — so PRESENT is not expressible here; unmap is how a mapping stops
 * being present.  These are the module's own bits, not x86 PTE bits: the
 * translation to hardware flags is vmm.c's business.
 *
 * VMM_MAP_EXEC is accepted and ignored, because EFER.NXE is not enabled: with
 * NX off every mapping is executable and asking for a non-executable one is a
 * request the hardware is not currently configured to honour.  It is part of
 * the interface now so that callers can express intent (and so the ABI flag
 * EXO_PAGE_EXEC has somewhere to land) without a rename when NXE is turned on.
 */
#define VMM_MAP_WRITE  (1u << 0)   /* writable; otherwise read-only          */
#define VMM_MAP_USER   (1u << 1)   /* reachable from CPL 3                   */
#define VMM_MAP_EXEC   (1u << 2)   /* accepted, ignored until EFER.NXE       */

/* Status codes.  ABI-agnostic on purpose (see page_alloc.h for the same
 * pattern): the syscall layer maps these onto -EXO_E* codes. */
#define VMM_OK       0
#define VMM_EINVAL (-1)   /* unaligned, non-canonical, or out-of-range addr  */
#define VMM_ENOMEM (-2)   /* no physical page left for an intermediate table */
#define VMM_ENOENT (-3)   /* nothing mapped at that virtual address          */

/*
 * Map the 4 KiB physical page `paddr` at `vaddr` with `attrs` (a mask of
 * VMM_MAP_*), replacing whatever was mapped there.  Both addresses must be
 * 4 KiB-aligned and `vaddr` must be canonical, or VMM_EINVAL.
 *
 * Missing PDPT/PD/PT levels are allocated from the PMM as PAGE_OWNER_KERNEL
 * pages — so no LibOS can free the page tables that describe its own address
 * space — and zeroed before use.  VMM_ENOMEM if the pool is exhausted;
 * partially built levels are left in place, which is harmless: they are empty
 * tables that the next map through the same region reuses.
 *
 * If `vaddr` falls inside a 2 MiB page (as all of the boot identity map does),
 * that page is first split into a 512-entry PT that reproduces it exactly,
 * so the other 511 4 KiB mappings it covered survive the operation.
 */
int vmm_map_page(uint64_t vaddr, uint64_t paddr, uint32_t attrs);

/*
 * Remove the mapping at `vaddr`.  The physical page is untouched — freeing it
 * is exo_page_free's job.  VMM_ENOENT if nothing is mapped there, VMM_EINVAL
 * for an unaligned or non-canonical address.
 *
 * A 2 MiB page is split first, exactly as in vmm_map_page, so unmapping one
 * 4 KiB page out of the identity map does not blow a 2 MiB hole in it.
 * Intermediate tables that become empty are not freed; a LibOS that maps and
 * unmaps across a wide address range can therefore retain a bounded number of
 * empty tables, which is a deliberate trade (walking a table to discover it is
 * empty on every unmap costs more than the pages are worth at v1 scale).
 */
int vmm_unmap_page(uint64_t vaddr);

/*
 * Resolve `vaddr` to a physical address through the live page tables, honouring
 * 2 MiB pages.  Any `vaddr` is accepted, aligned or not; the offset within the
 * page is carried into *paddr_out.  VMM_ENOENT if the walk hits a
 * non-present entry.  For the syscall layer (exo_page_unmap has to know which
 * physical page it is about to unmap before it can check who owns it) and for
 * tests.
 */
int vmm_translate(uint64_t vaddr, uint64_t *paddr_out);

/* Physical address of the PML4 currently in CR3.  Exposed for tests and
 * diagnostics; the mapping calls read it themselves. */
uint64_t vmm_current_pml4(void);
