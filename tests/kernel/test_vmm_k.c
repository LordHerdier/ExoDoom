/*
 * test_vmm_k.c — kernel page tables (SCRUM-15).
 *
 * These run *on* the map under test: vmm_init() has already loaded CR3 by the
 * time run_tests() is called, so every assertion here is about the live
 * address space, not a simulation of one.  That cuts both ways — a test that
 * unmaps the wrong page takes the machine down rather than failing — so the
 * mapping tests work in a scratch virtual window far above anything the
 * identity map uses, and put back what they borrow.
 */

#include "kunit.h"
#include "vmm.h"
#include "page_alloc.h"
#include "memory.h"

#include <stdint.h>

/*
 * A virtual window nothing else touches: 64 GiB up, above the 4 GiB the
 * identity map can reach even in principle (QEMU gives us 256 MiB of RAM and a
 * framebuffer just under 4 GiB).  Canonical, and guaranteed unmapped.
 */
#define SCRATCH_VA   0x0000001000000000ULL
/* Second window, 2 MiB-aligned, for the large-page and split tests. */
#define SCRATCH_VA_2M 0x0000001040000000ULL

extern uint8_t _load_start[];

/* The map exists, CR3 points at it, and its tables belong to the kernel. */
static void test_kernel_map_is_live(void)
{
    uint64_t pml4 = vmm_kernel_pml4();

    CU_ASSERT_NOT_EQUAL(pml4, 0);
    CU_ASSERT_EQUAL(pml4 % VMM_PAGE_SIZE, 0);

    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    /* CR3's low 12 bits are PCD/PWT, not part of the address. */
    CU_ASSERT_EQUAL(cr3 & ~0xFFFULL, pml4);

    /* Table pages come from alloc_page(), so a LibOS cannot free one
     * (SCRUM-152).  This is the map's half of the secure-binding guarantee. */
    CU_ASSERT_EQUAL(page_owner((void *)(uintptr_t)pml4), PAGE_OWNER_KERNEL);
    CU_ASSERT_TRUE(vmm_table_pages() > 0);
}

/* The kernel image, its stack and the bump pool are identity-mapped. */
static void test_kernel_image_identity_mapped(void)
{
    uint64_t paddr = 0, flags = 0;
    uint64_t text = (uint64_t)(uintptr_t)&test_kernel_image_identity_mapped;

    CU_ASSERT_EQUAL(vmm_translate(text, &paddr, &flags), VMM_OK);
    CU_ASSERT_EQUAL(paddr, text);
    CU_ASSERT_TRUE((flags & VMM_PRESENT) != 0);

    uint64_t local = (uint64_t)(uintptr_t)&paddr;         /* on the stack */
    CU_ASSERT_EQUAL(vmm_translate(local, &paddr, NULL), VMM_OK);
    CU_ASSERT_EQUAL(paddr, local);

    uint64_t load = (uint64_t)(uintptr_t)_load_start;
    CU_ASSERT_EQUAL(vmm_translate(load, &paddr, NULL), VMM_OK);
    CU_ASSERT_EQUAL(paddr, load);

    /* One page below the bump pointer: the PMM bitmap and owner table live
     * here, and the map has to cover them or the allocator's own metadata
     * would fault. */
    uint64_t heap = (uint64_t)memory_base_address() - VMM_PAGE_SIZE;
    CU_ASSERT_EQUAL(vmm_translate(heap, &paddr, NULL), VMM_OK);
    CU_ASSERT_EQUAL(paddr, heap);
}

/*
 * Test builds map the identity range user-accessible, matching the
 * --defsym RING3_PROBE=1 gate boot.s gets from build.sh: the ring-3 probe in
 * test_syscall_k.c runs against *these* tables, and the U/S bit is only
 * honoured when set at every level.  The C-side mirror of build.sh's step 1b.
 * (A shipped kernel sets no USER bit; this file only ever builds with
 * -DTESTING, so that direction is build.sh's to assert.)
 */
static void test_testing_build_maps_user_accessible(void)
{
    uint64_t flags = 0;
    uint64_t text = (uint64_t)(uintptr_t)&test_testing_build_maps_user_accessible;

    CU_ASSERT_EQUAL(vmm_translate(text, NULL, &flags), VMM_OK);
    CU_ASSERT_TRUE((flags & VMM_USER) != 0);
}

/* Page 0 is left unmapped so a NULL dereference faults instead of quietly
 * reading the interrupt vector table. */
static void test_null_page_unmapped(void)
{
    CU_ASSERT_EQUAL(vmm_translate(0, NULL, NULL), VMM_ENOENT);
    /* ...but the rest of low memory is mapped. */
    uint64_t paddr = 0;
    CU_ASSERT_EQUAL(vmm_translate(0x1000, &paddr, NULL), VMM_OK);
    CU_ASSERT_EQUAL(paddr, 0x1000);
}

/* Nothing is mapped in the scratch window before the tests below use it. */
static void test_unmapped_address_translates_to_enoent(void)
{
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_VA, NULL, NULL), VMM_ENOENT);
    CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_VA), VMM_ENOENT);
}

/* Map a real page at a scratch address, prove the mapping carries data, then
 * unmap it.  The round trip through two virtual addresses is what proves the
 * MMU is actually consulting the new tables. */
static void test_map_write_unmap_roundtrip(void)
{
    void *page = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(page);
    if (page == NULL) return;
    uint64_t phys = (uint64_t)(uintptr_t)page;

    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_VA, phys, VMM_PRESENT | VMM_WRITE),
                    VMM_OK);

    uint64_t resolved = 0;
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_VA, &resolved, NULL), VMM_OK);
    CU_ASSERT_EQUAL(resolved, phys);

    /* Offsets within the page survive translation. */
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_VA + 0x2A0, &resolved, NULL), VMM_OK);
    CU_ASSERT_EQUAL(resolved, phys + 0x2A0);

    /* Write through the new mapping, read back through the identity one. */
    volatile uint64_t *via_scratch = (volatile uint64_t *)(uintptr_t)SCRATCH_VA;
    volatile uint64_t *via_identity = (volatile uint64_t *)(uintptr_t)phys;

    *via_scratch = 0xC0FFEE0DDF00DULL;
    CU_ASSERT_EQUAL(*via_identity, 0xC0FFEE0DDF00DULL);

    CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_VA), VMM_OK);
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_VA, NULL, NULL), VMM_ENOENT);

    /* Unmapping does not free the page — the PMM still has it. */
    CU_ASSERT_EQUAL(page_owner(page), PAGE_OWNER_KERNEL);
    free_page(page);
}

/* Remapping the same physical page is idempotent; repointing a live mapping
 * somewhere else is refused rather than silently honoured. */
static void test_remap_rules(void)
{
    void *a = alloc_page();
    void *b = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(a);
    CU_ASSERT_PTR_NOT_NULL(b);
    if (a == NULL || b == NULL) return;

    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_VA, (uint64_t)(uintptr_t)a,
                                 VMM_PRESENT | VMM_WRITE), VMM_OK);
    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_VA, (uint64_t)(uintptr_t)a,
                                 VMM_PRESENT | VMM_WRITE), VMM_OK);
    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_VA, (uint64_t)(uintptr_t)b,
                                 VMM_PRESENT | VMM_WRITE), VMM_EEXIST);

    /* After an unmap the address is free to point elsewhere. */
    CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_VA), VMM_OK);
    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_VA, (uint64_t)(uintptr_t)b,
                                 VMM_PRESENT | VMM_WRITE), VMM_OK);
    CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_VA), VMM_OK);

    free_page(a);
    free_page(b);
}

/* Unaligned and non-canonical addresses are rejected, not truncated. */
static void test_alignment_and_canonical_checks(void)
{
    void *page = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(page);
    if (page == NULL) return;
    uint64_t phys = (uint64_t)(uintptr_t)page;

    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_VA + 1, phys, VMM_PRESENT), VMM_EINVAL);
    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_VA, phys + 8, VMM_PRESENT), VMM_EINVAL);
    /* Bits 63:48 do not repeat bit 47 — the CPU would fault on this address
     * before the walk even started. */
    CU_ASSERT_EQUAL(vmm_map_page(0x0001000000000000ULL, phys, VMM_PRESENT),
                    VMM_EINVAL);
    CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_VA + 1), VMM_EINVAL);

    free_page(page);
}

/*
 * A 2 MiB leaf, then a 4 KiB map inside it.  The split has to preserve the 511
 * pages it does not touch, which is the property that lets exo_page_map
 * (SCRUM-153) hand out single pages inside bulk-mapped regions.
 *
 * The alias maps kernel physical memory (2 MiB at _load_start, which is
 * 2 MiB-aligned by the linker script) at a scratch virtual address; nothing
 * writes through it, and it is torn down page by page at the end.
 */
static void test_large_page_split_preserves_neighbours(void)
{
    uint64_t phys_2m = (uint64_t)(uintptr_t)_load_start;
    CU_ASSERT_EQUAL(phys_2m % VMM_LARGE_PAGE_SIZE, 0);
    if (phys_2m % VMM_LARGE_PAGE_SIZE != 0) return;

    CU_ASSERT_EQUAL(vmm_map_range(SCRATCH_VA_2M, phys_2m, VMM_LARGE_PAGE_SIZE,
                                  VMM_PRESENT | VMM_WRITE), VMM_OK);

    /* One PDE covers the whole block: the map cost a table page for the PDPT
     * and PD, but no page table. */
    uint64_t resolved = 0;
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_VA_2M + 0x1F0000, &resolved, NULL),
                    VMM_OK);
    CU_ASSERT_EQUAL(resolved, phys_2m + 0x1F0000);

    /* Now repoint a single page in the middle of it. */
    void *page = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(page);
    if (page == NULL) return;
    uint64_t odd_va = SCRATCH_VA_2M + 0x8000;

    CU_ASSERT_EQUAL(vmm_unmap_page(odd_va), VMM_OK);   /* splits the leaf */
    CU_ASSERT_EQUAL(vmm_map_page(odd_va, (uint64_t)(uintptr_t)page,
                                 VMM_PRESENT | VMM_WRITE), VMM_OK);

    CU_ASSERT_EQUAL(vmm_translate(odd_va, &resolved, NULL), VMM_OK);
    CU_ASSERT_EQUAL(resolved, (uint64_t)(uintptr_t)page);

    /* Its neighbours still point where the 2 MiB leaf put them. */
    CU_ASSERT_EQUAL(vmm_translate(odd_va - VMM_PAGE_SIZE, &resolved, NULL),
                    VMM_OK);
    CU_ASSERT_EQUAL(resolved, phys_2m + 0x8000 - VMM_PAGE_SIZE);
    CU_ASSERT_EQUAL(vmm_translate(odd_va + VMM_PAGE_SIZE, &resolved, NULL),
                    VMM_OK);
    CU_ASSERT_EQUAL(resolved, phys_2m + 0x8000 + VMM_PAGE_SIZE);
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_VA_2M + 0x1F0000, &resolved, NULL),
                    VMM_OK);
    CU_ASSERT_EQUAL(resolved, phys_2m + 0x1F0000);

    /* Tear the alias down again so no test leaves a second view of kernel
     * memory behind. */
    for (uint64_t off = 0; off < VMM_LARGE_PAGE_SIZE; off += VMM_PAGE_SIZE) {
        CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_VA_2M + off), VMM_OK);
    }
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_VA_2M, NULL, NULL), VMM_ENOENT);

    free_page(page);
}

void suite_vmm_tests(CU_pSuite s)
{
    CU_add_test(s, "kernel map is live in CR3", test_kernel_map_is_live);
    CU_add_test(s, "kernel image is identity mapped",
                test_kernel_image_identity_mapped);
    CU_add_test(s, "test build maps user-accessible",
                test_testing_build_maps_user_accessible);
    CU_add_test(s, "page 0 is unmapped", test_null_page_unmapped);
    CU_add_test(s, "unmapped address reports ENOENT",
                test_unmapped_address_translates_to_enoent);
    CU_add_test(s, "map/write/unmap round trip", test_map_write_unmap_roundtrip);
    CU_add_test(s, "remap rules", test_remap_rules);
    CU_add_test(s, "alignment and canonical checks",
                test_alignment_and_canonical_checks);
    CU_add_test(s, "2 MiB split preserves neighbours",
                test_large_page_split_preserves_neighbours);
}
