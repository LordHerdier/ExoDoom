/*
 * test_ctype_k.c — Kernel-side CUnit tests for src/ctype.c.
 */

#include "kunit.h"
#include "ctype.h"

static void test_isalpha(void)
{
    /* ASCII letters */
    CU_ASSERT_TRUE(isalpha('a'));
    CU_ASSERT_TRUE(isalpha('z'));
    CU_ASSERT_TRUE(isalpha('A'));
    CU_ASSERT_TRUE(isalpha('Z'));
    /* Non-letters */
    CU_ASSERT_FALSE(isalpha('0'));
    CU_ASSERT_FALSE(isalpha(' '));
    CU_ASSERT_FALSE(isalpha('!'));
    CU_ASSERT_FALSE(isalpha('\0'));
}

static void test_isdigit(void)
{
    CU_ASSERT_TRUE(isdigit('0'));
    CU_ASSERT_TRUE(isdigit('5'));
    CU_ASSERT_TRUE(isdigit('9'));
    CU_ASSERT_FALSE(isdigit('a'));
    CU_ASSERT_FALSE(isdigit('/'));  /* one before '0' */
    CU_ASSERT_FALSE(isdigit(':'));  /* one after  '9' */
}

static void test_isspace(void)
{
    CU_ASSERT_TRUE(isspace(' '));
    CU_ASSERT_TRUE(isspace('\t'));
    CU_ASSERT_TRUE(isspace('\n'));
    CU_ASSERT_TRUE(isspace('\r'));
    CU_ASSERT_FALSE(isspace('a'));
    CU_ASSERT_FALSE(isspace('0'));
}

static void test_toupper(void)
{
    CU_ASSERT_EQUAL(toupper('a'), 'A');
    CU_ASSERT_EQUAL(toupper('z'), 'Z');
    /* Already upper — no change */
    CU_ASSERT_EQUAL(toupper('A'), 'A');
    /* Non-alpha — no change */
    CU_ASSERT_EQUAL(toupper('0'), '0');
    CU_ASSERT_EQUAL(toupper(' '), ' ');
}

static void test_tolower(void)
{
    CU_ASSERT_EQUAL(tolower('A'), 'a');
    CU_ASSERT_EQUAL(tolower('Z'), 'z');
    /* Already lower — no change */
    CU_ASSERT_EQUAL(tolower('a'), 'a');
    /* Non-alpha — no change */
    CU_ASSERT_EQUAL(tolower('0'), '0');
    CU_ASSERT_EQUAL(tolower(' '), ' ');
}

void suite_ctype_tests(CU_pSuite s)
{
    CU_add_test(s, "isalpha", test_isalpha);
    CU_add_test(s, "isdigit", test_isdigit);
    CU_add_test(s, "isspace", test_isspace);
    CU_add_test(s, "toupper", test_toupper);
    CU_add_test(s, "tolower", test_tolower);
}
