/*
 * test_smoke.c — Smoke tests for the KUnit harness itself.
 *
 * These tests exercise every assert macro to confirm the framework is wired
 * up correctly end-to-end.  If any smoke test fails, the harness is broken,
 * not the code under test.
 */

#include "kunit.h"

static void test_assert_true_false(void)
{
    CU_ASSERT_TRUE(1);
    CU_ASSERT_TRUE(42);
    CU_ASSERT_FALSE(0);
}

static void test_assert_equal(void)
{
    CU_ASSERT_EQUAL(1 + 1, 2);
    CU_ASSERT_EQUAL(0, 0);
    CU_ASSERT_NOT_EQUAL(1, 2);
}

static void test_assert_string(void)
{
    CU_ASSERT_STRING_EQUAL("hello", "hello");
    CU_ASSERT_STRING_EQUAL("", "");
    CU_ASSERT_STRING_NOT_EQUAL("foo", "bar");
}

static void test_assert_ptr(void)
{
    static int sentinel = 1;
    void *null_ptr = NULL;
    void *real_ptr = &sentinel;

    CU_ASSERT_PTR_NULL(null_ptr);
    CU_ASSERT_PTR_NOT_NULL(real_ptr);
}

void suite_smoke_tests(CU_pSuite s)
{
    CU_add_test(s, "assert_true_false", test_assert_true_false);
    CU_add_test(s, "assert_equal",      test_assert_equal);
    CU_add_test(s, "assert_string",     test_assert_string);
    CU_add_test(s, "assert_ptr",        test_assert_ptr);
}
