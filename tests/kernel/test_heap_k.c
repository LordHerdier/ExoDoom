/*
 * test_heap_k.c -- kernel heap allocator (SCRUM-25).
 *
 * Drives src/heap.c directly (heap_alloc/heap_free/heap_realloc), plus a
 * couple of sanity checks that kmalloc/kfree/krealloc (src/memory.c) route
 * to it once the PMM is live -- which it always is by the time tests run
 * (see kernel_main's boot order in CLAUDE.md).
 */

#include "kunit.h"
#include "heap.h"
#include "memory.h"

#include <stdint.h>

static void test_alloc_nonnull_and_aligned(void) {
    void *p = heap_alloc(37);
    CU_ASSERT_PTR_NOT_NULL(p);
    CU_ASSERT_EQUAL((uintptr_t)p % 16, 0);
    heap_free(p);
}

static void test_sequential_allocs_dont_overlap(void) {
    void *a = heap_alloc(64);
    void *b = heap_alloc(128);
    CU_ASSERT_PTR_NOT_NULL(a);
    CU_ASSERT_PTR_NOT_NULL(b);

    uintptr_t ua = (uintptr_t)a, ub = (uintptr_t)b;
    int disjoint = (ua + 64 <= ub) || (ub + 128 <= ua);
    CU_ASSERT_TRUE(disjoint);

    heap_free(a);
    heap_free(b);
}

static void test_free_and_realloc_preserve_data(void) {
    unsigned char *p = (unsigned char *)heap_alloc(32);
    CU_ASSERT_PTR_NOT_NULL(p);
    for (int i = 0; i < 32; i++) p[i] = (unsigned char)i;

    unsigned char *q = (unsigned char *)heap_realloc(p, 256);
    CU_ASSERT_PTR_NOT_NULL(q);
    int ok = 1;
    for (int i = 0; i < 32; i++) {
        if (q[i] != (unsigned char)i) ok = 0;
    }
    CU_ASSERT_TRUE(ok);

    heap_free(q);
}

static void test_free_then_realloc_reuses_block(void) {
    void *a = heap_alloc(64);
    CU_ASSERT_PTR_NOT_NULL(a);
    heap_free(a);

    void *b = heap_alloc(64);
    CU_ASSERT_PTR_NOT_NULL(b);
    CU_ASSERT_EQUAL(a, b);   /* first-fit should reuse the freed block */

    heap_free(b);
}

static void test_large_alloc_forces_growth(void) {
    /* Bigger than one 4K page -- forces heap_grow_one_page() to run more
     * than once and, if contiguous, extend a segment across pages. */
    void *p = heap_alloc(4096 * 3);
    CU_ASSERT_PTR_NOT_NULL(p);

    unsigned char *bytes = (unsigned char *)p;
    bytes[0] = 0xAB;
    bytes[4096 * 3 - 1] = 0xCD;
    CU_ASSERT_EQUAL(bytes[0], 0xAB);
    CU_ASSERT_EQUAL(bytes[4096 * 3 - 1], 0xCD);

    heap_free(p);
}

static void test_realloc_null_and_zero(void) {
    void *p = heap_realloc(NULL, 48);
    CU_ASSERT_PTR_NOT_NULL(p);

    void *q = heap_realloc(p, 0);
    CU_ASSERT_PTR_NULL(q);
}

static void test_kmalloc_kfree_roundtrip(void) {
    void *p = kmalloc(100);
    CU_ASSERT_PTR_NOT_NULL(p);
    kfree(p);

    void *q = krealloc(NULL, 50);
    CU_ASSERT_PTR_NOT_NULL(q);
    kfree(q);
}

void suite_heap_tests(CU_pSuite s) {
    CU_add_test(s, "alloc nonnull and aligned",       test_alloc_nonnull_and_aligned);
    CU_add_test(s, "sequential allocs don't overlap", test_sequential_allocs_dont_overlap);
    CU_add_test(s, "free and realloc preserve data",  test_free_and_realloc_preserve_data);
    CU_add_test(s, "free then realloc reuses block",  test_free_then_realloc_reuses_block);
    CU_add_test(s, "large alloc forces growth",       test_large_alloc_forces_growth);
    CU_add_test(s, "realloc NULL and zero",           test_realloc_null_and_zero);
    CU_add_test(s, "kmalloc/kfree roundtrip",         test_kmalloc_kfree_roundtrip);
}
