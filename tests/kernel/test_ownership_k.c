/*
 * test_ownership_k.c — physical page ownership table (SCRUM-152).
 *
 * The secure-binding primitive the SCRUM-151 epic is built on: every managed
 * page carries an owner tag (FREE / KERNEL / a LibOS id), exo_page_alloc stamps
 * the caller as owner, and exo_page_free refuses to free a page the caller does
 * not own.  These tests drive the PMM API (src/page_alloc.c) directly for the
 * tag mechanics, and the real dispatch path (exo_syscall_dispatch) for the
 * ring-3-visible -EXO_EPERM enforcement.
 *
 * The wider cross-context / framebuffer isolation suite is SCRUM-157.
 */

#include "kunit.h"
#include "page_alloc.h"
#include "syscall.h"
#include "exo_syscall.h"

#include <stdint.h>

/* A second, distinct LibOS id — the "another context" in the ownership checks.
 * v1 only ever runs PAGE_OWNER_LIBOS, so this id owns no real pages; it is used
 * to stamp a page under test and to stand in as a foreign caller. */
#define OTHER_LIBOS ((page_owner_t)(PAGE_OWNER_LIBOS + 1))

static int64_t do_page_free(uint64_t paddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_FREE, paddr, 0, 0, 0, 0, 0);
}

/* alloc_page_owned stamps the requested owner; the kernel-facing alloc_page
 * stamps KERNEL. */
static void test_alloc_stamps_owner(void)
{
    void* lp = alloc_page_owned(PAGE_OWNER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(lp);
    CU_ASSERT_EQUAL(page_owner(lp), PAGE_OWNER_LIBOS);

    void* kp = alloc_page();               /* kernel-internal allocation */
    CU_ASSERT_PTR_NOT_NULL(kp);
    CU_ASSERT_EQUAL(page_owner(kp), PAGE_OWNER_KERNEL);

    CU_ASSERT_EQUAL(free_page_owned(lp, PAGE_OWNER_LIBOS), PAGE_FREE_OK);
    CU_ASSERT_EQUAL(free_page_checked(kp), 0);   /* frees as KERNEL */
}

/* The owner can free its own page; the tag reverts to FREE afterwards. */
static void test_owner_frees_own_page(void)
{
    void* p = alloc_page_owned(PAGE_OWNER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(p);

    CU_ASSERT_EQUAL(free_page_owned(p, PAGE_OWNER_LIBOS), PAGE_FREE_OK);
    CU_ASSERT_EQUAL(page_owner(p), PAGE_OWNER_FREE);
}

/* A LibOS cannot free a page owned by another context. */
static void test_foreign_free_rejected(void)
{
    void* p = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(p);

    /* PAGE_OWNER_LIBOS (the v1 caller) does not own it. */
    CU_ASSERT_EQUAL(free_page_owned(p, PAGE_OWNER_LIBOS), PAGE_FREE_EPERM);
    /* The rejected free left it allocated and owned by OTHER_LIBOS. */
    CU_ASSERT_EQUAL(page_owner(p), OTHER_LIBOS);

    CU_ASSERT_EQUAL(free_page_owned(p, OTHER_LIBOS), PAGE_FREE_OK);
}

/* A LibOS cannot free a KERNEL-owned page — the map/free hole from issue #25. */
static void test_kernel_page_not_freeable_by_libos(void)
{
    void* kp = alloc_page();               /* KERNEL-owned */
    CU_ASSERT_PTR_NOT_NULL(kp);

    CU_ASSERT_EQUAL(free_page_owned(kp, PAGE_OWNER_LIBOS), PAGE_FREE_EPERM);

    /* Same rejection over the real syscall path: the current context is a
     * LibOS, so freeing a kernel page returns -EXO_EPERM. */
    CU_ASSERT_EQUAL(do_page_free((uint64_t)(uintptr_t)kp), -EXO_EPERM);

    CU_ASSERT_EQUAL(free_page_checked(kp), 0);   /* kernel reclaims it */
}

/* A KERNEL-owned page is never handed back out to a LibOS while it is held. */
static void test_kernel_page_not_reallocated(void)
{
    void* kp = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(kp);

    void* lp = alloc_page_owned(PAGE_OWNER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(lp);
    CU_ASSERT_NOT_EQUAL(lp, kp);       /* did not re-hand the kernel page */

    CU_ASSERT_EQUAL(free_page_owned(lp, PAGE_OWNER_LIBOS), PAGE_FREE_OK);
    CU_ASSERT_EQUAL(free_page_checked(kp), 0);
}

void suite_ownership_tests(CU_pSuite s)
{
    CU_add_test(s, "alloc stamps owner",             test_alloc_stamps_owner);
    CU_add_test(s, "owner frees own page",           test_owner_frees_own_page);
    CU_add_test(s, "foreign free rejected (EPERM)",  test_foreign_free_rejected);
    CU_add_test(s, "kernel page not freeable by libos",
                test_kernel_page_not_freeable_by_libos);
    CU_add_test(s, "kernel page not reallocated",    test_kernel_page_not_reallocated);
}
