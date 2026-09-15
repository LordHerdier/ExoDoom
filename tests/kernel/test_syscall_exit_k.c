#include "kunit.h"

#include "exo_syscall.h"
#include "fb_binding.h"
#include "page_alloc.h"
#include "syscall.h"
#include "syscall_exit.h"

static void test_exit_handler_is_bound(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_EXIT));
}

static void test_exit_reclaims_pages_and_framebuffer(void)
{
    page_owner_t owner = syscall_current_context();

    void *p1 = alloc_page_owned(owner);
    void *p2 = alloc_page_owned(owner);

    CU_ASSERT_PTR_NOT_NULL(p1);
    CU_ASSERT_PTR_NOT_NULL(p2);

    CU_ASSERT_EQUAL(fb_binding_acquire(owner), FB_BIND_OK);
    CU_ASSERT_EQUAL(fb_binding_owner(), owner);

    CU_ASSERT_EQUAL(
        exo_syscall_dispatch(EXO_SYS_EXIT, 7, 0, 0, 0, 0, 0),
        0);

    CU_ASSERT_EQUAL(page_owner(p1), PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(page_owner(p2), PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);
}

static void test_exit_does_not_reclaim_kernel_page(void)
{
    void *kp = alloc_page();

    CU_ASSERT_PTR_NOT_NULL(kp);
    CU_ASSERT_EQUAL(page_owner(kp), PAGE_OWNER_KERNEL);

    CU_ASSERT_EQUAL(
        exo_syscall_dispatch(EXO_SYS_EXIT, 0, 0, 0, 0, 0, 0),
        0);

    CU_ASSERT_EQUAL(page_owner(kp), PAGE_OWNER_KERNEL);

    CU_ASSERT_EQUAL(free_page_checked(kp), 0);
}

void suite_syscall_exit_tests(CU_pSuite s)
{
    CU_add_test(s, "handler is bound",
                test_exit_handler_is_bound);
    CU_add_test(s, "exit reclaims pages and framebuffer",
                test_exit_reclaims_pages_and_framebuffer);
    CU_add_test(s, "exit preserves kernel pages",
                test_exit_does_not_reclaim_kernel_page);
}
