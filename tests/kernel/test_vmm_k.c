/*
 * test_vmm_k.c — the page table walker (SCRUM-35).
 *
 * Tests the mechanism directly, below the syscall: installing and removing
 * 4 KiB mappings in the live address space, resolving virtual addresses
 * through the real page tables, and splitting one of the boot identity map's
 * 2 MiB pages without disturbing its other 511 4 KiB neighbours.  The
 * ownership rule layered on top of this is test_page_map_k.c's job.
 *
 * These run against the address space the kernel is executing in — there is no
 * spare one to experiment in — so every test that changes a mapping restores
 * it, and every scratch virtual address is taken from the LibOS window above
 * 4 GiB, where the kernel keeps nothing.
 */

#include "kunit.h"
#include "vmm.h"
#include "page_alloc.h"
#include "exo_syscall.h"

/* Scratch virtual addresses, spaced far enough apart that no two tests share
 * a page table and an escaped mapping cannot be mistaken for a live one. */
#define SCRATCH_A (EXO_USER_VA_BASE + 0x0000000ULL)
#define SCRATCH_B (EXO_USER_VA_BASE + 0x4000000ULL)
#define SCRATCH_C (EXO_USER_VA_BASE + 0x8000000ULL)

/* A value no page of freshly allocated memory is likely to hold by accident. */
#define PATTERN_A 0x5CA1AB1E5CA1AB1EULL
#define PATTERN_B 0xD00DFEEDDEADBEEFULL

static volatile uint64_t *as_ptr(uint64_t vaddr)
{
    return (volatile uint64_t *)(uintptr_t)vaddr;
}

/* CR3 must name a page table that is actually reachable; every other test here
 * depends on the walk starting somewhere real. */
static void test_current_pml4_is_sane(void)
{
    uint64_t root = vmm_current_pml4();

    CU_ASSERT_NOT_EQUAL(root, 0);
    CU_ASSERT_EQUAL(root % VMM_PAGE_SIZE, 0);
}

/* Below 4 GiB the boot map is an identity map, so translation is the identity
 * function — including for an address in the middle of a page, whose offset
 * has to survive the 2 MiB leaf's 21-bit offset arithmetic. */
static void test_translate_is_identity_below_4g(void)
{
    uint64_t here = (uint64_t)(uintptr_t)&test_translate_is_identity_below_4g;
    uint64_t phys = 0;

    CU_ASSERT_EQUAL(vmm_translate(here, &phys), VMM_OK);
    CU_ASSERT_EQUAL(phys, here);

    CU_ASSERT_EQUAL(vmm_translate(here + 0x123, &phys), VMM_OK);
    CU_ASSERT_EQUAL(phys, here + 0x123);
}

/* Nothing is mapped in the LibOS window until something maps it. */
static void test_translate_unmapped_is_enoent(void)
{
    uint64_t phys = 0;

    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_A, &phys), VMM_ENOENT);
}

/*
 * The round trip the whole story exists for: a physical page becomes reachable
 * at a virtual address of the caller's choosing, and a write through that
 * address lands in that physical page — checked through the identity map,
 * which is an independent view of the same memory.
 */
static void test_map_makes_page_reachable(void)
{
    void *page = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(page);
    if (page == NULL)
        return;

    uint64_t paddr = (uint64_t)(uintptr_t)page;

    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_A, paddr, VMM_MAP_WRITE), VMM_OK);

    uint64_t resolved = 0;
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_A, &resolved), VMM_OK);
    CU_ASSERT_EQUAL(resolved, paddr);

    *as_ptr(SCRATCH_A) = PATTERN_A;
    CU_ASSERT_EQUAL(*as_ptr(paddr), PATTERN_A);

    /* And the mapping goes away again, leaving the page itself untouched. */
    CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_A), VMM_OK);
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_A, &resolved), VMM_ENOENT);
    CU_ASSERT_EQUAL(*as_ptr(paddr), PATTERN_A);

    free_page(page);
}

/* Remapping the same virtual address is how a LibOS moves a window over
 * physical memory; the second mapping must win outright. */
static void test_remap_replaces_previous(void)
{
    void *first  = alloc_page();
    void *second = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(first);
    CU_ASSERT_PTR_NOT_NULL(second);
    if (first == NULL || second == NULL)
        return;

    *(volatile uint64_t *)first  = PATTERN_A;
    *(volatile uint64_t *)second = PATTERN_B;

    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_B, (uint64_t)(uintptr_t)first,
                                 VMM_MAP_WRITE), VMM_OK);
    CU_ASSERT_EQUAL(*as_ptr(SCRATCH_B), PATTERN_A);

    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_B, (uint64_t)(uintptr_t)second,
                                 VMM_MAP_WRITE), VMM_OK);
    CU_ASSERT_EQUAL(*as_ptr(SCRATCH_B), PATTERN_B);

    CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_B), VMM_OK);
    free_page(first);
    free_page(second);
}

/*
 * Splitting a 2 MiB page: mapping something else over one 4 KiB slice of the
 * identity map must leave the rest of that 2 MiB region exactly as it was.
 *
 * The victim is a page this test owns, and its 2 MiB neighbourhood contains
 * live kernel memory — which is the point.  If the split reproduced the large
 * page incorrectly, the neighbour check below would read garbage, and a kernel
 * whose own identity map has holes in it would not survive to report it.
 */
static void test_split_preserves_neighbours(void)
{
    void *victim    = alloc_page();
    void *elsewhere = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(victim);
    CU_ASSERT_PTR_NOT_NULL(elsewhere);
    if (victim == NULL || elsewhere == NULL)
        return;

    uint64_t vaddr = (uint64_t)(uintptr_t)victim;

    /* A witness page in the same 2 MiB region as the victim but not the same
     * 4 KiB page: the first page of the region, or the second if the victim
     * happens to be the first.  The split has to reproduce its mapping. */
    uint64_t region = vaddr & ~0x1FFFFFULL;
    uint64_t witness = (region == vaddr) ? region + VMM_PAGE_SIZE : region;

    *(volatile uint64_t *)victim    = PATTERN_A;
    *(volatile uint64_t *)elsewhere = PATTERN_B;

    /* Map `elsewhere` over the victim's own virtual address.  This is only
     * possible if the 2 MiB page covering both is split first. */
    CU_ASSERT_EQUAL(vmm_map_page(vaddr, (uint64_t)(uintptr_t)elsewhere,
                                 VMM_MAP_WRITE), VMM_OK);
    CU_ASSERT_EQUAL(*as_ptr(vaddr), PATTERN_B);

    /* The witness came out of the same split and must still resolve to
     * itself; so must every other page of the region, of which it stands in
     * for 510. */
    uint64_t resolved = 0;
    CU_ASSERT_EQUAL(vmm_translate(witness, &resolved), VMM_OK);
    CU_ASSERT_EQUAL(resolved, witness);

    /* Put the identity mapping back before anything else runs. */
    CU_ASSERT_EQUAL(vmm_map_page(vaddr, vaddr, VMM_MAP_WRITE), VMM_OK);
    CU_ASSERT_EQUAL(*as_ptr(vaddr), PATTERN_A);
    CU_ASSERT_EQUAL(*(volatile uint64_t *)elsewhere, PATTERN_B);

    free_page(victim);
    free_page(elsewhere);
}

/* Both addresses are page numbers, not byte addresses; a caller that means
 * something else has made a mistake the kernel can catch. */
static void test_map_rejects_unaligned(void)
{
    void *page = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(page);
    if (page == NULL)
        return;

    uint64_t paddr = (uint64_t)(uintptr_t)page;

    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_C + 1, paddr, VMM_MAP_WRITE),
                    VMM_EINVAL);
    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_C, paddr + 1, VMM_MAP_WRITE),
                    VMM_EINVAL);
    CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_C + 8), VMM_EINVAL);

    /* Rejected calls must not have built any of the walk on the way down. */
    uint64_t phys = 0;
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_C, &phys), VMM_ENOENT);

    free_page(page);
}

/* A non-canonical address has no page table entry to describe it — the first
 * address above the lower canonical half is the boundary case. */
static void test_map_rejects_noncanonical(void)
{
    void *page = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(page);
    if (page == NULL)
        return;

    CU_ASSERT_EQUAL(vmm_map_page(EXO_USER_VA_END, (uint64_t)(uintptr_t)page,
                                 VMM_MAP_WRITE), VMM_EINVAL);
    CU_ASSERT_EQUAL(vmm_unmap_page(EXO_USER_VA_END), VMM_EINVAL);

    free_page(page);
}

/* Unmapping what was never mapped is a distinct answer from unmapping
 * something successfully — the syscall layer turns it into -EXO_EINVAL. */
static void test_unmap_unmapped_is_enoent(void)
{
    CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_C), VMM_ENOENT);
}

void suite_vmm_tests(CU_pSuite s)
{
    CU_add_test(s, "current_pml4_is_sane",         test_current_pml4_is_sane);
    CU_add_test(s, "translate_identity_below_4g",  test_translate_is_identity_below_4g);
    CU_add_test(s, "translate_unmapped_enoent",    test_translate_unmapped_is_enoent);
    CU_add_test(s, "map_makes_page_reachable",     test_map_makes_page_reachable);
    CU_add_test(s, "remap_replaces_previous",      test_remap_replaces_previous);
    CU_add_test(s, "split_preserves_neighbours",   test_split_preserves_neighbours);
    CU_add_test(s, "map_rejects_unaligned",        test_map_rejects_unaligned);
    CU_add_test(s, "map_rejects_noncanonical",     test_map_rejects_noncanonical);
    CU_add_test(s, "unmap_unmapped_enoent",        test_unmap_unmapped_is_enoent);
}
