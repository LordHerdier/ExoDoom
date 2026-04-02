/*
 * test_string_k.c — Kernel-side CUnit tests for src/string.c.
 *
 * Covers the most important behaviours of each function.  Exhaustive
 * edge-case coverage is deferred to the Sprint 1 string test ticket.
 */

#include "kunit.h"
#include "string.h"

static void test_strlen_basic(void)
{
    CU_ASSERT(strlen("") == 0);
    CU_ASSERT(strlen("a") == 1);
    CU_ASSERT(strlen("hello") == 5);
}

static void test_strlen_embedded_nul(void)
{
    /* strlen stops at the first NUL byte */
    CU_ASSERT(strlen("ab\0cd") == 2);
}

static void test_strcmp_equal(void)
{
    CU_ASSERT_EQUAL(strcmp("abc", "abc"), 0);
    CU_ASSERT_EQUAL(strcmp("", ""), 0);
}

static void test_strcmp_order(void)
{
    CU_ASSERT(strcmp("abc", "abd") < 0);
    CU_ASSERT(strcmp("abd", "abc") > 0);
    CU_ASSERT(strcmp("a", "aa") < 0);
}

static void test_memset_fills(void)
{
    char buf[8];
    memset(buf, 0xAA, 4);
    CU_ASSERT_EQUAL((unsigned char)buf[0], 0xAAU);
    CU_ASSERT_EQUAL((unsigned char)buf[3], 0xAAU);
}

static void test_memset_zero(void)
{
    char buf[4] = {1, 2, 3, 4};
    memset(buf, 0, 4);
    CU_ASSERT_EQUAL((unsigned char)buf[0], 0U);
    CU_ASSERT_EQUAL((unsigned char)buf[3], 0U);
}

static void test_memcpy_basic(void)
{
    const char src[5] = {1, 2, 3, 4, 5};
    char dst[5]       = {0, 0, 0, 0, 0};
    memcpy(dst, src, 5);
    CU_ASSERT_EQUAL(memcmp(dst, src, 5), 0);
}

static void test_memmove_overlap_forward(void)
{
    char buf[8] = {1, 2, 3, 4, 0, 0, 0, 0};
    memmove(buf + 2, buf, 4);   /* dest > src: must copy backward */
    CU_ASSERT_EQUAL((unsigned char)buf[2], 1U);
    CU_ASSERT_EQUAL((unsigned char)buf[5], 4U);
}

static void test_memcmp_equal(void)
{
    CU_ASSERT_EQUAL(memcmp("abc", "abc", 3), 0);
}

static void test_memcmp_diff(void)
{
    CU_ASSERT(memcmp("abc", "abd", 3) < 0);
    CU_ASSERT(memcmp("abd", "abc", 3) > 0);
}

static void test_strchr_found(void)
{
    const char *s = "hello";
    CU_ASSERT_PTR_NOT_NULL(strchr(s, 'e'));
    CU_ASSERT_PTR_NOT_NULL(strchr(s, 'h'));
    CU_ASSERT_PTR_NOT_NULL(strchr(s, 'o'));
}

static void test_strchr_not_found(void)
{
    CU_ASSERT_PTR_NULL(strchr("hello", 'z'));
}

static void test_strchr_nul_terminator(void)
{
    const char *s = "hello";
    /* strchr(s, '\0') must return pointer to terminator */
    CU_ASSERT_PTR_NOT_NULL(strchr(s, '\0'));
}

void suite_string_tests(CU_pSuite s)
{
    CU_add_test(s, "strlen_basic",           test_strlen_basic);
    CU_add_test(s, "strlen_embedded_nul",    test_strlen_embedded_nul);
    CU_add_test(s, "strcmp_equal",           test_strcmp_equal);
    CU_add_test(s, "strcmp_order",           test_strcmp_order);
    CU_add_test(s, "memset_fills",           test_memset_fills);
    CU_add_test(s, "memset_zero",            test_memset_zero);
    CU_add_test(s, "memcpy_basic",           test_memcpy_basic);
    CU_add_test(s, "memmove_overlap_forward",test_memmove_overlap_forward);
    CU_add_test(s, "memcmp_equal",           test_memcmp_equal);
    CU_add_test(s, "memcmp_diff",            test_memcmp_diff);
    CU_add_test(s, "strchr_found",           test_strchr_found);
    CU_add_test(s, "strchr_not_found",       test_strchr_not_found);
    CU_add_test(s, "strchr_nul_terminator",  test_strchr_nul_terminator);
}
