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

/* ---- memset ---- */

static void test_memset(void) {
    unsigned char buf[8];

    /* fill entire buffer */
    memset(buf, 0xAB, sizeof(buf));
    for (int i = 0; i < 8; i++)
        ASSERT_EQ(buf[i], (unsigned char)0xAB);

    /* zero fill */
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < 8; i++)
        ASSERT_EQ(buf[i], (unsigned char)0);

    /* only c is treated as unsigned char (low byte of int) */
    memset(buf, 0x1FF, 1);
    ASSERT_EQ(buf[0], (unsigned char)0xFF);

    /* n=0 is a no-op; surrounding bytes untouched */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x11;
    memset(buf + 1, 0xCC, 0);
    ASSERT_EQ(buf[0], (unsigned char)0x11);

    /* partial fill leaves rest intact */
    memset(buf, 0, sizeof(buf));
    memset(buf, 0x55, 4);
    for (int i = 0; i < 4; i++)
        ASSERT_EQ(buf[i], (unsigned char)0x55);
    for (int i = 4; i < 8; i++)
        ASSERT_EQ(buf[i], (unsigned char)0x00);

    /* return value is the original pointer */
    ASSERT_EQ(memset(buf, 0, 1), (void *)buf);
}

/* ---- main ---- */

int main(void) {
    test_strlen();
    test_strcmp();
    test_memset();

    printf("%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
