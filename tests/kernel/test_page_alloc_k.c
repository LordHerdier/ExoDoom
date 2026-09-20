/*
 * test_page_alloc_k.c — physical memory manager, multi-region support
 * (SCRUM-158).
 *
 * page_alloc_init() (src/page_alloc.c) used to register only the first
 * MULTIBOOT_MMAP_AVAILABLE region above 1 MB and return; it now loops over
 * every such region, each with its own bitmap + owner table, and every
 * allocation/free/ownership/revocation entry point resolves an address to
 * its owning region before touching that region's arrays. The ownership
 * mechanics themselves (owner tags, EPERM enforcement, reclaim sweeps) are
 * covered by test_ownership_k.c; this suite is about the region-resolution
 * refactor underneath them not regressing, and the boundary behavior that
 * changed shape because of it.
 *
 * CI's QEMU `-m 256M` boot reports exactly one usable region above 1 MB, so
 * these tests can't force `page_alloc_region_count() > 1` -- there's no
 * synthetic-mmap test harness here (SCRUM-158's plan explicitly scoped that
 * out: faking multiboot mmap data would be more invasive than the ticket,
 * and isn't needed to prove the single-region path -- the one path CI can
 * actually exercise -- still works after the refactor). The boundary tests
 * below are written to be meaningful regardless of region count: they check
 * behavior at the edge of the *last* managed region, which is exactly the
 * code path page_alloc.c's region_index_of() had to get right for both one
 * region and many.
 */

#include "kunit.h"
#include "page_alloc.h"

#include <stdint.h>

#define TEST_LIBOS  ((page_owner_t)(PAGE_OWNER_LIBOS + 10))
#define OTHER_LIBOS ((page_owner_t)(PAGE_OWNER_LIBOS + 11))

/* page_alloc_init() runs ahead of the TESTING branch in kernel_main, so by
 * the time any test runs, at least one region must already be registered. */
static void test_region_count_positive(void)
{
    CU_ASSERT_TRUE(page_alloc_region_count() >= 1);
}

/* Basic alloc/free round trip against the real boot-time pool -- a
 * regression guard for the switch from one flat bitmap/owner array to a
 * per-region array of them. */
static void test_alloc_free_roundtrip(void)
{
    void* p = alloc_page_owned(TEST_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(p);
    CU_ASSERT_EQUAL(page_owner(p), TEST_LIBOS);

    CU_ASSERT_EQUAL(free_page_owned(p, TEST_LIBOS), PAGE_FREE_OK);
    CU_ASSERT_EQUAL(page_owner(p), PAGE_OWNER_FREE);
}

/* Two distinct owners allocating concurrently must resolve to independent
 * tags -- proves region_index_of() + the per-region owners[] it indexes
 * into aren't aliasing two different pages onto the same slot. */
static void test_two_owners_stay_independent(void)
{
    void* a = alloc_page_owned(TEST_LIBOS);
    void* b = alloc_page_owned(OTHER_LIBOS);

    CU_ASSERT_PTR_NOT_NULL(a);
    CU_ASSERT_PTR_NOT_NULL(b);
    CU_ASSERT_NOT_EQUAL(a, b);

    CU_ASSERT_EQUAL(page_owner(a), TEST_LIBOS);
    CU_ASSERT_EQUAL(page_owner(b), OTHER_LIBOS);

    CU_ASSERT_EQUAL(free_page_owned(a, TEST_LIBOS), PAGE_FREE_OK);
    CU_ASSERT_EQUAL(page_owner(b), OTHER_LIBOS);   /* freeing `a` left `b` alone */

    CU_ASSERT_EQUAL(free_page_owned(b, OTHER_LIBOS), PAGE_FREE_OK);
}

/* page_alloc_pool_end() is one past the end of the highest-based registered
 * region -- genuinely out of range regardless of how many regions exist.
 * Before SCRUM-158 "out of range" meant "past the one region's total_pages";
 * it now means "past every registered region", and this is the address
 * region_index_of() must correctly refuse. */
static void test_pool_end_is_out_of_range(void)
{
    uintptr_t end = page_alloc_pool_end();
    CU_ASSERT_TRUE(end != 0);

    CU_ASSERT_EQUAL(page_owner((void*)end), PAGE_OWNER_FREE);
    CU_ASSERT_FALSE(page_revoke_pending((void*)end));

    /* Not allocated to anyone (it isn't a managed page at all), so freeing it
     * is EINVAL, not EPERM -- same contract as any other out-of-range
     * address. */
    CU_ASSERT_EQUAL(free_page_owned((void*)end, TEST_LIBOS), PAGE_FREE_EINVAL);
}

/* A freshly allocated page's address must fall strictly before pool_end --
 * a basic sanity bound on the region the allocator actually drew from. */
static void test_allocated_page_before_pool_end(void)
{
    void* p = alloc_page_owned(TEST_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(p);

    CU_ASSERT_TRUE((uintptr_t)p < page_alloc_pool_end());

    CU_ASSERT_EQUAL(free_page_owned(p, TEST_LIBOS), PAGE_FREE_OK);
}

/* alloc_pages_contig_owned() (SCRUM-158 note: a run can never span two
 * regions) -- every page in a run must resolve, via page_owner(), to the
 * same owner and be freeable individually. This is the per-region inner
 * scan the refactor kept unchanged, wrapped in a new outer loop over
 * regions[]; a run corrupting a neighboring region's bitmap/owners would
 * show up here as a mis-tagged page. */
static void test_contig_run_pages_resolve_consistently(void)
{
    const uint32_t count = 4;
    void* base = alloc_pages_contig_owned(TEST_LIBOS, count);
    CU_ASSERT_PTR_NOT_NULL(base);

    uintptr_t p = (uintptr_t)base;
    for (uint32_t i = 0; i < count; i++) {
        CU_ASSERT_EQUAL(page_owner((void*)(p + (uintptr_t)i * 4096)), TEST_LIBOS);
    }

    for (uint32_t i = 0; i < count; i++) {
        CU_ASSERT_EQUAL(
            free_page_owned((void*)(p + (uintptr_t)i * 4096), TEST_LIBOS),
            PAGE_FREE_OK);
    }
}

void suite_page_alloc_tests(CU_pSuite s)
{
    CU_add_test(s, "region count is positive after init",
            test_region_count_positive);
    CU_add_test(s, "alloc/free round trip",           test_alloc_free_roundtrip);
    CU_add_test(s, "two owners stay independent",     test_two_owners_stay_independent);
    CU_add_test(s, "pool end is out of range",         test_pool_end_is_out_of_range);
    CU_add_test(s, "allocated page precedes pool end", test_allocated_page_before_pool_end);
    CU_add_test(s, "contig run resolves consistently",
            test_contig_run_pages_resolve_consistently);
}
