/*
 * test_memory_k.c — Kernel-side CUnit tests for src/memory.c and src/mmap.c.
 *
 * Memory tests exercise the bump allocator (kmalloc/kfree).
 * Mmap tests verify that multiboot memory-map parsing produces a
 * plausible layout for a QEMU run with -m 256M.
 */

#include <stdint.h>
#include "kunit.h"
#include "memory.h"
#include "mmap.h"
#include "multiboot.h"

extern char _bss_end;

/* ---- Allocation tests -------------------------------------------------- */

static void test_alloc_non_null(void)
{
    void *p = kmalloc(1);
    CU_ASSERT_PTR_NOT_NULL(p);
}

static void test_alloc_page_aligned(void)
{
    void *p = kmalloc(1);
    CU_ASSERT_PTR_NOT_NULL(p);
    CU_ASSERT(((uintptr_t)p & 0xFFF) == 0);
}

static void test_alloc_monotonic(void)
{
    /* Each successive allocation must return a higher address. */
    void *p1 = kmalloc(1);
    void *p2 = kmalloc(1);
    void *p3 = kmalloc(1);
    CU_ASSERT((uintptr_t)p2 > (uintptr_t)p1);
    CU_ASSERT((uintptr_t)p3 > (uintptr_t)p2);
}

static void test_alloc_no_overlap(void)
{
    /* Two 4 KB allocations must not share any bytes. */
    void *p1 = kmalloc(4096);
    void *p2 = kmalloc(4096);
    CU_ASSERT((uintptr_t)p2 >= (uintptr_t)p1 + 4096);
}

static void test_alloc_above_kernel(void)
{
    /* Placement allocator must stay above _bss_end (the kernel region). */
    uint32_t base = memory_base_address();
    CU_ASSERT(base >= (uint32_t)(uintptr_t)&_bss_end);
}

static void test_free_roundtrip(void)
{
    /* Allocate N pages then free them — no crash or hang. */
    void *ptrs[4];
    int i;
    for (i = 0; i < 4; i++) {
        ptrs[i] = kmalloc(4096);
        CU_ASSERT_PTR_NOT_NULL(ptrs[i]);
    }
    for (i = 0; i < 4; i++)
        kfree(ptrs[i]);
    CU_ASSERT_TRUE(1);
}

static void test_double_free_safe(void)
{
    /* Freeing the same pointer twice must log an error and not crash. */
    void *p = kmalloc(1);
    CU_ASSERT_PTR_NOT_NULL(p);
    kfree(p);
    kfree(p); /* second call: logs "double-free" to serial, then returns */
    CU_ASSERT_TRUE(1);
}

/* ---- Mmap tests -------------------------------------------------------- */

static void test_mmap_has_regions(void)
{
    uint32_t count = 0;
    mmap_get_regions(&count);
    /* Any real PC BIOS produces at least two distinct regions. */
    CU_ASSERT(count >= 2);
}

static void test_mmap_first_region_usable(void)
{
    /* Conventional memory at address 0 is always usable on x86. */
    uint32_t count = 0;
    const mmap_region_t *r = mmap_get_regions(&count);
    CU_ASSERT(count > 0);
    CU_ASSERT_EQUAL(r[0].base, (uint64_t)0);
    CU_ASSERT_EQUAL(r[0].type, (uint32_t)MULTIBOOT_MMAP_AVAILABLE);
}

static void test_mmap_has_extended_memory(void)
{
    /* Must have at least one usable region above 1 MB (extended memory). */
    uint32_t count = 0;
    const mmap_region_t *r = mmap_get_regions(&count);
    unsigned int found = 0;
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (r[i].type == MULTIBOOT_MMAP_AVAILABLE && r[i].base >= 0x100000ULL) {
            found = 1;
            break;
        }
    }
    CU_ASSERT_TRUE(found);
}

static void test_mmap_total_usable_256m(void)
{
    /* With -m 256M the total usable memory must exceed 200 MB. */
    uint32_t count = 0;
    const mmap_region_t *r = mmap_get_regions(&count);
    uint64_t total = 0;
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (r[i].type == MULTIBOOT_MMAP_AVAILABLE)
            total += r[i].length;
    }
    CU_ASSERT(total > (uint64_t)200 * 1024 * 1024);
}

static void test_mmap_has_reserved_region(void)
{
    /* Standard PC always has reserved regions (ROM, EBDA, etc.). */
    uint32_t count = 0;
    const mmap_region_t *r = mmap_get_regions(&count);
    unsigned int found = 0;
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (r[i].type == MULTIBOOT_MMAP_RESERVED) {
            found = 1;
            break;
        }
    }
    CU_ASSERT_TRUE(found);
}

/* ---- Suite registration ----------------------------------------------- */

void suite_memory_tests(CU_pSuite s)
{
    CU_add_test(s, "alloc_non_null",           test_alloc_non_null);
    CU_add_test(s, "alloc_page_aligned",       test_alloc_page_aligned);
    CU_add_test(s, "alloc_monotonic",          test_alloc_monotonic);
    CU_add_test(s, "alloc_no_overlap",         test_alloc_no_overlap);
    CU_add_test(s, "alloc_above_kernel",       test_alloc_above_kernel);
    CU_add_test(s, "free_roundtrip",           test_free_roundtrip);
    CU_add_test(s, "double_free_safe",         test_double_free_safe);
    CU_add_test(s, "mmap_has_regions",         test_mmap_has_regions);
    CU_add_test(s, "mmap_first_region_usable", test_mmap_first_region_usable);
    CU_add_test(s, "mmap_has_extended_memory", test_mmap_has_extended_memory);
    CU_add_test(s, "mmap_total_usable_256m",   test_mmap_total_usable_256m);
    CU_add_test(s, "mmap_has_reserved_region", test_mmap_has_reserved_region);
}
