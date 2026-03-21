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

/* ---- memcpy ---- */

static void test_memcpy(void) {
    unsigned char src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned char dst[8] = {0};

    /* full copy */
    memcpy(dst, src, 8);
    for (int i = 0; i < 8; i++)
        ASSERT_EQ(dst[i], src[i]);

    /* partial copy */
    memset(dst, 0, 8);
    memcpy(dst, src, 4);
    for (int i = 0; i < 4; i++)
        ASSERT_EQ(dst[i], src[i]);
    for (int i = 4; i < 8; i++)
        ASSERT_EQ(dst[i], (unsigned char)0);

    /* n=0 is a no-op */
    memset(dst, 0xBB, 8);
    memcpy(dst, src, 0);
    ASSERT_EQ(dst[0], (unsigned char)0xBB);

    /* return value is dest */
    ASSERT_EQ(memcpy(dst, src, 1), (void *)dst);
}

/* ---- memmove ---- */

static void test_memmove(void) {
    unsigned char buf[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    unsigned char dst[10];

    /* non-overlapping: behaves like memcpy */
    memcpy(dst, buf, 10);
    memmove(dst, buf, 10);
    for (int i = 0; i < 10; i++)
        ASSERT_EQ(dst[i], buf[i]);

    /* overlap: dest > src (must copy backwards) */
    unsigned char fwd[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    memmove(fwd + 2, fwd, 6);  /* shift right by 2 */
    ASSERT_EQ(fwd[2], (unsigned char)1);
    ASSERT_EQ(fwd[3], (unsigned char)2);
    ASSERT_EQ(fwd[4], (unsigned char)3);
    ASSERT_EQ(fwd[5], (unsigned char)4);
    ASSERT_EQ(fwd[6], (unsigned char)5);
    ASSERT_EQ(fwd[7], (unsigned char)6);

    /* overlap: dest < src (can copy forwards) */
    unsigned char bwd[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    memmove(bwd, bwd + 2, 6);  /* shift left by 2 */
    ASSERT_EQ(bwd[0], (unsigned char)3);
    ASSERT_EQ(bwd[1], (unsigned char)4);
    ASSERT_EQ(bwd[2], (unsigned char)5);
    ASSERT_EQ(bwd[3], (unsigned char)6);
    ASSERT_EQ(bwd[4], (unsigned char)7);
    ASSERT_EQ(bwd[5], (unsigned char)8);

    /* n=0 is a no-op */
    unsigned char nop[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    memmove(nop + 1, nop, 0);
    ASSERT_EQ(nop[1], (unsigned char)0xBB);

    /* return value is dest */
    ASSERT_EQ(memmove(dst, buf, 1), (void *)dst);
}

/* ---- memcmp ---- */

static void test_memcmp(void) {
    /* equal regions */
    ASSERT_EQ(memcmp("abc", "abc", 3), 0);

    /* n=0 always equal */
    ASSERT_EQ(memcmp("abc", "xyz", 0), 0);

    /* first differing byte determines sign */
    ASSERT_EQ(memcmp("abc", "abd", 3) < 0, 1); /* c < d */
    ASSERT_EQ(memcmp("abd", "abc", 3) > 0, 1); /* d > c */

    /* only compares n bytes — trailing difference ignored */
    ASSERT_EQ(memcmp("abcX", "abcY", 3), 0);

    /* unsigned byte comparison: 0x80 > 0x01 */
    unsigned char lo[] = {0x01};
    unsigned char hi[] = {0x80};
    ASSERT_EQ(memcmp(hi, lo, 1) > 0, 1);
    ASSERT_EQ(memcmp(lo, hi, 1) < 0, 1);
}

/* ---- main ---- */

int main(void) {
    test_strlen();
    test_strcmp();
    test_memset();
    test_memcpy();
    test_memmove();
    test_memcmp();

    printf("%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
