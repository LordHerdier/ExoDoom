#include "vmm.h"
#include "page_alloc.h"

#include <stddef.h>
#include <stdint.h>

/*
 * vmm.c — the 4-level page table walker behind exo_page_map (SCRUM-35).
 *
 * Everything here operates on the address space CR3 names.  There is exactly
 * one of those today; per-LibOS address spaces (SCRUM-48) turn the implicit
 * "current PML4" below into a parameter, which is why every walk starts from
 * pml4_root() rather than from a cached pointer.
 */

/* ---- Hardware page table entry bits (Intel SDM Vol. 3A §4.5) ------------ */

#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITE      (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_PS         (1ULL << 7)   /* PD entry maps a 2 MiB page          */

/* Bits 51:12 of an entry are the physical address of the next table (or, with
 * PS set, of the 2 MiB page).  Masking with this also strips the PAT bit (12)
 * of a 2 MiB leaf, which is what makes the flag arithmetic in split_2m()
 * correct: PAT lives at a different bit in a 4 KiB PTE, so it must not be
 * carried across unexamined. */
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL

/* Extent of the boot identity map (boot.s: 4 PDs × 1 GiB).  A page table has
 * to be *reachable* to be edited, and until the kernel maps physical memory
 * somewhere of its own choosing, reachable means "identity-mapped". */
#define IDENTITY_LIMIT 0x0000000100000000ULL

/* Non-canonical addresses fault on use and cannot be described by a page
 * table at all, so they are rejected up front rather than walked. */
#define CANONICAL_LOW_END   0x0000800000000000ULL
#define CANONICAL_HIGH_START 0xFFFF800000000000ULL

/* ---- CPU access ---------------------------------------------------------- */

static inline uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

/* Reload CR3 with its current value: flushes every non-global TLB entry.  Used
 * after a 2 MiB split, where the stale entry to shoot down is a large-page
 * translation covering a whole 2 MiB region rather than the single page an
 * invlpg names. */
static inline void flush_tlb_all(void)
{
    __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3"
                      ::: "rax", "memory");
}

static inline void invlpg(uint64_t vaddr)
{
    __asm__ volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
}

/* ---- Address arithmetic -------------------------------------------------- */

static inline uint64_t pml4_index(uint64_t v) { return (v >> 39) & 0x1FF; }
static inline uint64_t pdpt_index(uint64_t v) { return (v >> 30) & 0x1FF; }
static inline uint64_t pd_index  (uint64_t v) { return (v >> 21) & 0x1FF; }
static inline uint64_t pt_index  (uint64_t v) { return (v >> 12) & 0x1FF; }

static int is_canonical(uint64_t v)
{
    return v < CANONICAL_LOW_END || v >= CANONICAL_HIGH_START;
}

static int page_aligned(uint64_t addr)
{
    return (addr & (VMM_PAGE_SIZE - 1)) == 0;
}

/*
 * A table's physical address doubles as a pointer to it, because the boot
 * identity map is still in force.  NULL for anything above it: such a table
 * cannot be edited without first mapping it, and the PMM never hands out pages
 * that high on the machines this kernel targets, so this is a guard against a
 * corrupt entry rather than a case to handle.
 */
static uint64_t *table_ptr(uint64_t phys)
{
    if (phys >= IDENTITY_LIMIT)
        return NULL;
    return (uint64_t *)(uintptr_t)phys;
}

static uint64_t *pml4_root(void)
{
    return table_ptr(read_cr3() & PTE_ADDR_MASK);
}

/* The hardware flags a mapping request asks for.  PRESENT is unconditional —
 * see the note on VMM_MAP_EXEC in vmm.h for why EXEC contributes nothing. */
static uint64_t leaf_flags(uint32_t attrs)
{
    uint64_t f = PTE_PRESENT;
    if (attrs & VMM_MAP_WRITE) f |= PTE_WRITE;
    if (attrs & VMM_MAP_USER)  f |= PTE_USER;
    return f;
}

/*
 * Flags for an *intermediate* entry.  The CPU ANDs write and user permission
 * across all four levels, so a link has to be at least as permissive as the
 * leaves beneath it; the leaf is what actually decides access.  This is why
 * promote_link() below widens an existing link rather than leaving it alone:
 * the boot identity map's PML4[0] is present|write only, and a user mapping
 * installed anywhere under it would be unreachable from ring 3 otherwise.
 *
 * Widening a link grants nothing by itself — every 2 MiB leaf in the identity
 * map keeps its own supervisor-only flags on a non-test build, and those still
 * deny ring 3.
 */
static uint64_t link_flags(uint32_t attrs)
{
    uint64_t f = PTE_PRESENT | PTE_WRITE;
    if (attrs & VMM_MAP_USER) f |= PTE_USER;
    return f;
}

static void promote_link(uint64_t *entry, uint32_t attrs)
{
    *entry |= (link_flags(attrs) & (PTE_WRITE | PTE_USER));
}

/* ---- Table management ---------------------------------------------------- */

/* Allocate a zeroed page table.  PAGE_OWNER_KERNEL, via alloc_page(), so that
 * a LibOS cannot hand the page describing its own address space back to the
 * PMM with exo_page_free. */
static uint64_t *alloc_table(uint64_t *phys_out)
{
    void *page = alloc_page();
    if (page == NULL)
        return NULL;

    uint64_t *table = (uint64_t *)page;
    for (int i = 0; i < 512; i++)
        table[i] = 0;

    *phys_out = (uint64_t)(uintptr_t)page;
    return table;
}

/*
 * Replace a 2 MiB leaf with a page table that reproduces it exactly.
 *
 * The 512 new entries cover the same physical range with the same flags, so
 * nothing about the mapping changes — only its granularity does.  That is what
 * lets a single 4 KiB mapping be installed anywhere in the boot identity map
 * without disturbing the 511 neighbours sharing its 2 MiB page.
 */
static int split_2m(uint64_t *pde)
{
    uint64_t phys;
    uint64_t *pt = alloc_table(&phys);
    if (pt == NULL)
        return VMM_ENOMEM;

    uint64_t base  = *pde & PTE_ADDR_MASK;
    uint64_t flags = *pde & ~(PTE_ADDR_MASK | PTE_PS);

    for (uint64_t i = 0; i < 512; i++)
        pt[i] = (base + i * VMM_PAGE_SIZE) | flags;

    *pde = phys | PTE_PRESENT | PTE_WRITE | (*pde & PTE_USER);

    /* The stale translation is a 2 MiB entry, not a 4 KiB one. */
    flush_tlb_all();
    return VMM_OK;
}

/*
 * Walk to the PT that governs `vaddr`.
 *
 * With `create` set, missing levels are allocated and existing links are
 * widened to `attrs`; without it, a missing level is VMM_ENOENT and nothing is
 * modified.  Either way a 2 MiB leaf found at the PD level is split, because
 * both operations need 4 KiB granularity to say anything at all about a single
 * page — a split allocates, which is why unmap can also answer VMM_ENOMEM.
 */
static int walk_to_pt(uint64_t vaddr, int create, uint32_t attrs,
                      uint64_t **pt_out)
{
    uint64_t *table = pml4_root();
    if (table == NULL)
        return VMM_EINVAL;

    const uint64_t index[3] = {
        pml4_index(vaddr), pdpt_index(vaddr), pd_index(vaddr)
    };

    for (int level = 0; level < 3; level++) {
        uint64_t *entry = &table[index[level]];

        /* A 2 MiB page lives at the PD level (level 2 of this loop). */
        if (level == 2 && (*entry & PTE_PRESENT) && (*entry & PTE_PS)) {
            int rc = split_2m(entry);
            if (rc != VMM_OK)
                return rc;
        }

        if (!(*entry & PTE_PRESENT)) {
            if (!create)
                return VMM_ENOENT;

            uint64_t phys;
            if (alloc_table(&phys) == NULL)
                return VMM_ENOMEM;

            *entry = phys | link_flags(attrs);
        } else if (create) {
            promote_link(entry, attrs);
        }

        table = table_ptr(*entry & PTE_ADDR_MASK);
        if (table == NULL)
            return VMM_EINVAL;
    }

    *pt_out = table;
    return VMM_OK;
}

/* ---- Public interface ---------------------------------------------------- */

uint64_t vmm_current_pml4(void)
{
    return read_cr3() & PTE_ADDR_MASK;
}

int vmm_map_page(uint64_t vaddr, uint64_t paddr, uint32_t attrs)
{
    if (!page_aligned(vaddr) || !is_canonical(vaddr))
        return VMM_EINVAL;

    /* Catches an unaligned physical address and one too wide for an entry in
     * the same test: everything outside bits 51:12 must be zero. */
    if ((paddr & ~PTE_ADDR_MASK) != 0)
        return VMM_EINVAL;

    uint64_t *pt;
    int rc = walk_to_pt(vaddr, /*create=*/1, attrs, &pt);
    if (rc != VMM_OK)
        return rc;

    pt[pt_index(vaddr)] = paddr | leaf_flags(attrs);
    invlpg(vaddr);
    return VMM_OK;
}

int vmm_unmap_page(uint64_t vaddr)
{
    if (!page_aligned(vaddr) || !is_canonical(vaddr))
        return VMM_EINVAL;

    uint64_t *pt;
    int rc = walk_to_pt(vaddr, /*create=*/0, 0, &pt);
    if (rc != VMM_OK)
        return rc;

    uint64_t *pte = &pt[pt_index(vaddr)];
    if (!(*pte & PTE_PRESENT))
        return VMM_ENOENT;

    *pte = 0;
    invlpg(vaddr);
    return VMM_OK;
}

int vmm_translate(uint64_t vaddr, uint64_t *paddr_out)
{
    if (paddr_out == NULL || !is_canonical(vaddr))
        return VMM_EINVAL;

    uint64_t *table = pml4_root();
    if (table == NULL)
        return VMM_EINVAL;

    const uint64_t index[3] = {
        pml4_index(vaddr), pdpt_index(vaddr), pd_index(vaddr)
    };

    for (int level = 0; level < 3; level++) {
        uint64_t entry = table[index[level]];

        if (!(entry & PTE_PRESENT))
            return VMM_ENOENT;

        /* A 2 MiB leaf ends the walk early; the offset is the low 21 bits. */
        if (level == 2 && (entry & PTE_PS)) {
            *paddr_out = (entry & PTE_ADDR_MASK) | (vaddr & 0x1FFFFF);
            return VMM_OK;
        }

        table = table_ptr(entry & PTE_ADDR_MASK);
        if (table == NULL)
            return VMM_EINVAL;
    }

    uint64_t pte = table[pt_index(vaddr)];
    if (!(pte & PTE_PRESENT))
        return VMM_ENOENT;

    *paddr_out = (pte & PTE_ADDR_MASK) | (vaddr & (VMM_PAGE_SIZE - 1));
    return VMM_OK;
}
