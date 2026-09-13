/*
 * test_libos_page_alloc_k.c — LibOS-side page allocator (SCRUM-37).
 *
 * libos_page_alloc()/libos_page_free() drive the real dispatcher,
 * exo_syscall_dispatch(EXO_SYS_PAGE_*, ...) — see libos_page_alloc.c's top
 * comment for why that's the dispatcher call directly and not the inline
 * `syscall`-instruction stubs. exo_page_alloc/exo_page_free (SCRUM-34) and
 * exo_page_map/exo_page_unmap (SCRUM-35) are all live by the time run_tests()
 * gets here. These tests check the syscalls actually happened, not just that
 * libos_page_alloc() returned a non-NULL pointer: page_owner() and
 * vmm_translate() are read straight from the kernel side of the same state
 * the syscalls just changed.
 */

#include "kunit.h"
#include "libos_page_alloc.h"
#include "page_alloc.h"
#include "vmm.h"

#include <stdint.h>

#define PAGE_SIZE 0x1000ULL

static uint64_t paddr_of(void *vaddr)
{
    uint64_t paddr = 0;
    int rc = vmm_translate((uint64_t)(uintptr_t)vaddr, &paddr, NULL);
    CU_ASSERT_EQUAL(rc, VMM_OK);
    return paddr;
}

static void test_alloc_returns_mapped_writable_page(void)
{
    void *p = libos_page_alloc();
    CU_ASSERT_PTR_NOT_NULL(p);
    CU_ASSERT_EQUAL((uint64_t)(uintptr_t)p % PAGE_SIZE, 0);

    volatile uint32_t *w = (volatile uint32_t *)p;
    *w = 0xdeadbeef;
    CU_ASSERT_EQUAL(*w, 0xdeadbeefu);

    uint64_t paddr = paddr_of(p);
    CU_ASSERT_NOT_EQUAL(paddr, 0);
    CU_ASSERT_EQUAL(page_owner((void *)(uintptr_t)paddr), PAGE_OWNER_LIBOS);

    libos_page_free(p);
}

static void test_free_unmaps_and_returns_page(void)
{
    void *p = libos_page_alloc();
    CU_ASSERT_PTR_NOT_NULL(p);
    uint64_t paddr = paddr_of(p);

    CU_ASSERT_EQUAL(libos_page_free(p), 0);

    uint64_t out = 0;
    CU_ASSERT_EQUAL(vmm_translate((uint64_t)(uintptr_t)p, &out, NULL),
                    VMM_ENOENT);
    CU_ASSERT_EQUAL(page_owner((void *)(uintptr_t)paddr), PAGE_OWNER_FREE);
}

static void test_double_free_rejected(void)
{
    void *p = libos_page_alloc();
    CU_ASSERT_PTR_NOT_NULL(p);

    CU_ASSERT_EQUAL(libos_page_free(p), 0);
    CU_ASSERT_EQUAL(libos_page_free(p), -1);
}

static void test_bogus_vaddr_rejected(void)
{
    void *p = libos_page_alloc();
    CU_ASSERT_PTR_NOT_NULL(p);

    /* Unaligned: one byte into a genuinely allocated page. */
    CU_ASSERT_EQUAL(libos_page_free((char *)p + 1), -1);

    /* Never handed out at all. */
    CU_ASSERT_EQUAL(libos_page_free((void *)(uintptr_t)0x1000), -1);

    libos_page_free(p);
}

static void test_freed_slot_is_reused(void)
{
    void *a = libos_page_alloc();
    CU_ASSERT_PTR_NOT_NULL(a);
    CU_ASSERT_EQUAL(libos_page_free(a), 0);

    void *b = libos_page_alloc();
    CU_ASSERT_PTR_NOT_NULL(b);
    CU_ASSERT_EQUAL(a, b);   /* LIFO free-list reuse, not a new high-water slot */

    libos_page_free(b);
}

/* Static, not stack-local: LIBOS_PAGE_ALLOC_MAX_PAGES * sizeof(void*) would
 * blow the 16 KiB kernel stack (docs/memory.md, src/stdlib.c's qsort note
 * has the same constraint for the same reason). */
static void *exhaustion_vaddrs[LIBOS_PAGE_ALLOC_MAX_PAGES];

static void test_slot_table_exhaustion(void)
{
    libos_page_alloc_init();

    uint32_t n;
    for (n = 0; n < LIBOS_PAGE_ALLOC_MAX_PAGES; n++) {
        exhaustion_vaddrs[n] = libos_page_alloc();
        if (exhaustion_vaddrs[n] == NULL)
            break;
    }
    CU_ASSERT_EQUAL(n, LIBOS_PAGE_ALLOC_MAX_PAGES);
    CU_ASSERT_PTR_NULL(libos_page_alloc());   /* the slot table is full */

    for (uint32_t i = 0; i < n; i++)
        CU_ASSERT_EQUAL(libos_page_free(exhaustion_vaddrs[i]), 0);

    /* Restored to a clean allocator for any suite that runs after this one. */
    libos_page_alloc_init();
}

void suite_libos_page_alloc_tests(CU_pSuite s)
{
    CU_add_test(s, "alloc returns mapped writable page",
               test_alloc_returns_mapped_writable_page);
    CU_add_test(s, "free unmaps and returns page",
               test_free_unmaps_and_returns_page);
    CU_add_test(s, "double free rejected", test_double_free_rejected);
    CU_add_test(s, "bogus vaddr rejected", test_bogus_vaddr_rejected);
    CU_add_test(s, "freed slot is reused", test_freed_slot_is_reused);
    CU_add_test(s, "slot table exhaustion", test_slot_table_exhaustion);
}
