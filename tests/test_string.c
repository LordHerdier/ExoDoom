#include <stdio.h>
#include <stdlib.h>
#include "../src/string.h"

static int tests_run = 0;
static int tests_failed = 0;

#define ASSERT_EQ(expr, expected) do {                                     \
    tests_run++;                                                           \
    __typeof__(expected) _got = (expr);                                    \
    __typeof__(expected) _exp = (expected);                                \
    if (_got != _exp) {                                                    \
        fprintf(stderr, "FAIL %s:%d: %s — expected %lld, got %lld\n",    \
            __FILE__, __LINE__, #expr,                                     \
            (long long)_exp, (long long)_got);                             \
        tests_failed++;                                                    \
    }                                                                      \
} while (0)

/* ---- strlen ---- */

static void test_strlen(void) {
    ASSERT_EQ(strlen(""),          (size_t)0);
    ASSERT_EQ(strlen("a"),         (size_t)1);
    ASSERT_EQ(strlen("hello"),     (size_t)5);
    ASSERT_EQ(strlen("hello\0x"),  (size_t)5); /* stops at first NUL */
}

/* ---- strcmp ---- */

static void test_strcmp(void) {
    ASSERT_EQ(strcmp("",      ""),      0);
    ASSERT_EQ(strcmp("abc",   "abc"),   0);
    ASSERT_EQ(strcmp("",      "abc") < 0, 1); /* empty < non-empty */
    ASSERT_EQ(strcmp("abc",   "") > 0,   1); /* non-empty > empty */
    ASSERT_EQ(strcmp("abc",   "abd") < 0, 1); /* c < d */
    ASSERT_EQ(strcmp("abd",   "abc") > 0, 1); /* d > c */
    ASSERT_EQ(strcmp("abc",   "ab")  > 0, 1); /* longer > shorter */
    ASSERT_EQ(strcmp("ab",    "abc") < 0, 1); /* shorter < longer */
}

/* ---- main ---- */

int main(void) {
    test_strlen();
    test_strcmp();

    printf("%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
