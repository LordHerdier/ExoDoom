/*
 * test_syscall_mem_k.c — exo_page_alloc / exo_page_free syscalls (SCRUM-34).
 *
 * Drives the real dispatch path, exo_syscall_dispatch(EXO_SYS_PAGE_*, ...) —
 * the same route a ring-3 `syscall` takes once the entry stub has marshalled
 * its arguments (that stub is covered separately by test_syscall_k.c).  The
 * handlers are bound in kernel_main by syscall_mem_init() before run_tests(),
 * and page_alloc_init() has already made the PMM live, so these call into the
 * real allocator.
 *
 * These exercise the same-owner happy path and the address-validation failures,
 * which return -EXO_EINVAL: an unaligned/out-of-range address or a double free
 * of an already-free page.  Cross-owner freeing (-EXO_EPERM, SCRUM-152) is
 * covered by test_ownership_k.c.
 */

#include "kunit.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "page_alloc.h"

#define PAGE_SIZE 4096

/* Convenience wrappers over the dispatcher for the two syscalls under test. */
static int64_t do_page_alloc(void)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
}

static int64_t do_page_free(uint64_t paddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_FREE, paddr, 0, 0, 0, 0, 0);
}

/* The boot path must have bound both numbers; without this the rest of the
 * suite would only be re-proving the dispatcher's -EXO_ENOSYS fallback. */
static void test_handlers_are_bound(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_PAGE_ALLOC));
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_PAGE_FREE));
}

static void test_alloc_returns_aligned_page(void)
{
    int64_t p = do_page_alloc();

    CU_ASSERT(p > 0);                          /* real phys addr, not an error */
    CU_ASSERT_EQUAL((uint64_t)p % PAGE_SIZE, 0);

    do_page_free((uint64_t)p);                 /* keep the pool clean */
}

static void test_alloc_free_round_trip(void)
{
    int64_t p = do_page_alloc();
    CU_ASSERT(p > 0);

    CU_ASSERT_EQUAL(do_page_free((uint64_t)p), 0);
}

static void test_double_free_rejected(void)
{
    int64_t p = do_page_alloc();
    CU_ASSERT(p > 0);

    CU_ASSERT_EQUAL(do_page_free((uint64_t)p), 0);
    CU_ASSERT_EQUAL(do_page_free((uint64_t)p), -EXO_EINVAL);
}

static void test_bogus_free_rejected(void)
{
    int64_t p = do_page_alloc();
    CU_ASSERT(p > 0);

    /* Unaligned: one byte into a genuinely allocated page. */
    CU_ASSERT_EQUAL(do_page_free((uint64_t)p + 1), -EXO_EINVAL);

    /* Out of range: page-aligned but past the pool's managed end, derived
     * from the allocator itself so this stays correct regardless of QEMU's
     * configured memory size. */
    uintptr_t out_of_range = page_alloc_pool_end();
    CU_ASSERT(out_of_range != 0);
    CU_ASSERT_EQUAL(do_page_free((uint64_t)out_of_range), -EXO_EINVAL);

    do_page_free((uint64_t)p);                 /* p itself is still allocated */
}

static void test_free_returns_page_to_pool(void)
{
    int64_t a = do_page_alloc();
    CU_ASSERT(a > 0);
    CU_ASSERT_EQUAL(do_page_free((uint64_t)a), 0);

    /* A subsequent allocation must still succeed — the free put the page back
     * rather than leaking it. */
    int64_t b = do_page_alloc();
    CU_ASSERT(b > 0);

    do_page_free((uint64_t)b);
}

void suite_syscall_mem_tests(CU_pSuite s)
{
    CU_add_test(s, "handlers are bound",        test_handlers_are_bound);
    CU_add_test(s, "alloc returns aligned page", test_alloc_returns_aligned_page);
    CU_add_test(s, "alloc/free round trip",     test_alloc_free_round_trip);
    CU_add_test(s, "double free rejected",      test_double_free_rejected);
    CU_add_test(s, "bogus free rejected",       test_bogus_free_rejected);
    CU_add_test(s, "free returns page to pool", test_free_returns_page_to_pool);
}
