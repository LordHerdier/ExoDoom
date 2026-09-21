/*
 * test_memory_isolation_k.c — memory isolation stress test (SCRUM-59).
 *
 * Blocked by SCRUM-37 (the LibOS-side allocator built on exo_page_alloc/
 * exo_page_free) until it landed, precisely so this suite can drive the real
 * syscall path rather than src/page_alloc.c directly the way
 * test_page_alloc_k.c does. It drives exo_syscall_dispatch(EXO_SYS_PAGE_*,
 * ...) from kernel context, the same convention test_syscall_mem_k.c and
 * test_libos_page_alloc_k.c use (see either file's top comment for why: the
 * inline `syscall`-instruction stubs return via sysretq, which unconditionally
 * drops to CPL 3).
 *
 * This is deliberately *not* built on libos_page_alloc() (SCRUM-37): that
 * wrapper also does an EXO_SYS_PAGE_MAP after every EXO_SYS_PAGE_ALLOC and
 * caps out at LIBOS_PAGE_ALLOC_MAX_PAGES (4096) -- a virtual-address-window
 * limit, not the physical pool. Exhausting *that* is what
 * test_libos_page_alloc_k.c's test_slot_table_exhaustion already covers.
 * SCRUM-59 wants the real PMM driven to -EXO_ENOMEM, which only needs
 * EXO_SYS_PAGE_ALLOC/_FREE -- no mapping, so no VA-window ceiling applies.
 *
 * docs/memory.md's page accounting table puts QEMU `-m 256M` (what
 * make docker-test/docker-ci boot) at ~62,064 available pages after the
 * kernel image, bump pool and WAD module are reserved; MEM_ISO_MAX_PAGES
 * below is sized to the full 65,536-page pool so the exhaustion array never
 * depends on that reservation math staying exactly the same.
 */

#include "kunit.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "page_alloc.h"
#include "heap.h"
#include "memory.h"

#include <stdint.h>

/* Generous upper bound on total managed pages: docs/memory.md's "Total
 * pages: 65,536 (256 MiB)" for QEMU `-m 256M`. Static, not stack-local --
 * 65536 * sizeof(uint64_t) = 512 KiB would blow the 16 KiB kernel stack
 * (same reasoning as test_libos_page_alloc_k.c's exhaustion_vaddrs and
 * src/stdlib.c's qsort note). */
#define MEM_ISO_MAX_PAGES 65536u
static uint64_t mem_iso_paddrs[MEM_ISO_MAX_PAGES];

static int64_t do_page_alloc(void)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
}

static int64_t do_page_free(uint64_t paddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_FREE, paddr, 0, 0, 0, 0, 0);
}

static int heap_stats_equal(const heap_stats_t *a, const heap_stats_t *b)
{
    return a->total_bytes == b->total_bytes &&
           a->free_bytes  == b->free_bytes  &&
           a->used_bytes  == b->used_bytes  &&
           a->free_blocks == b->free_blocks &&
           a->used_blocks == b->used_blocks &&
           a->segments    == b->segments    &&
           a->pages       == b->pages;
}

static void test_allocate_all_free_and_free_all(void)
{
    /* page_count_owned(PAGE_OWNER_FREE) is not usable here: its contract
     * (src/page_alloc.c) is "pages currently allocated to `owner`", and it
     * short-circuits to 0 for PAGE_OWNER_FREE by definition -- a free page's
     * bitmap bit is clear, so it can never match the "allocated" scan
     * page_count_owned() runs. Exhaustion is instead proven the functional
     * way: loop until the real dispatcher returns -EXO_ENOMEM, and treat
     * however many succeeded before that as "every free page there was". */
    uint32_t libos_before  = page_count_owned(PAGE_OWNER_LIBOS);
    uint32_t kernel_before = page_count_owned(PAGE_OWNER_KERNEL);
    heap_stats_t heap_before;
    heap_get_stats(&heap_before);

    /* ---- Exhaust: allocate every free page through the syscall path. ---- */
    uint32_t n = 0;
    for (;;) {
        if (n >= MEM_ISO_MAX_PAGES) {
            /* Array too small for this pool -- fail loudly rather than
             * overrun it. */
            CU_ASSERT_TRUE(n < MEM_ISO_MAX_PAGES);
            break;
        }

        int64_t p = do_page_alloc();
        if (p <= 0) {
            CU_ASSERT_EQUAL(p, -EXO_ENOMEM);
            break;
        }

        mem_iso_paddrs[n++] = (uint64_t)p;
    }

    /* Sanity: a full 256M pool has tens of thousands of pages -- if this
     * came back tiny, something upstream (WAD/kernel reservation, an
     * earlier suite's leak) shrank the pool, and the rest of this test
     * would be exercising the wrong thing. */
    CU_ASSERT_TRUE(n > 1000u);
    CU_ASSERT_EQUAL(page_count_owned(PAGE_OWNER_LIBOS), libos_before + n);
    CU_ASSERT_EQUAL(page_count_owned(PAGE_OWNER_KERNEL), kernel_before);

    /* ---- Kernel heap survives peak PMM pressure. ----
     * A small allocation that fits in the heap's existing free space (no
     * heap_grow_one_page() -> alloc_page() call, which would fail with the
     * PMM fully saturated) proves kmalloc/kfree don't secretly depend on the
     * LibOS pool having slack. */
    heap_stats_t heap_mid_before;
    heap_get_stats(&heap_mid_before);

    void *canary = kmalloc(64);
    CU_ASSERT_PTR_NOT_NULL(canary);
    if (canary != NULL) {
        volatile uint8_t *w = (volatile uint8_t *)canary;
        *w = 0x5A;
        CU_ASSERT_EQUAL(*w, 0x5A);
    }
    kfree(canary);

    heap_stats_t heap_mid_after;
    heap_get_stats(&heap_mid_after);
    CU_ASSERT_TRUE(heap_stats_equal(&heap_mid_before, &heap_mid_after));

    /* ---- Free every page back through the syscall path. ---- */
    for (uint32_t i = 0; i < n; i++) {
        CU_ASSERT_EQUAL(do_page_free(mem_iso_paddrs[i]), 0);
    }

    /* ---- Everything is back exactly where it started. ---- */
    CU_ASSERT_EQUAL(page_count_owned(PAGE_OWNER_LIBOS), libos_before);
    CU_ASSERT_EQUAL(page_count_owned(PAGE_OWNER_KERNEL), kernel_before);

    heap_stats_t heap_after;
    heap_get_stats(&heap_after);
    CU_ASSERT_TRUE(heap_stats_equal(&heap_before, &heap_after));

    /* Functional, not just count-correct: the pool is still usable. */
    int64_t p = do_page_alloc();
    CU_ASSERT(p > 0);
    if (p > 0)
        CU_ASSERT_EQUAL(do_page_free((uint64_t)p), 0);
}

void suite_memory_isolation_tests(CU_pSuite s)
{
    CU_add_test(s, "allocate all free pages, free them, heap and PMM intact",
               test_allocate_all_free_and_free_all);
}
