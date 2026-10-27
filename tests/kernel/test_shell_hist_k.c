/*
 * test_shell_hist_k.c — the shell's command history ring (SCRUM-273).
 *
 * src/shell/shell_hist.c is ring-3 shell code; docker/scripts/build.sh
 * compiles a second copy into the TESTING kernel (same arrangement as
 * test_shell_err_k.c).
 */

#include "kunit.h"
#include "shell/shell_hist.h"

#include <stddef.h>

static int streq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void push_num(shell_hist_t *h, unsigned n)
{
    char buf[8] = { 'c', 0, 0, 0, 0, 0, 0, 0 };
    unsigned i = 1;
    char tmp[6];
    unsigned t = 0;

    do {
        tmp[t++] = (char)('0' + n % 10);
        n /= 10;
    } while (n > 0);
    while (t > 0) {
        buf[i++] = tmp[--t];
    }
    shell_hist_push(h, buf);
}

static void test_empty_history(void)
{
    shell_hist_t h;
    shell_hist_init(&h);
    CU_ASSERT_TRUE(shell_hist_prev(&h, "abc") == NULL);
    CU_ASSERT_TRUE(shell_hist_next(&h) == NULL);
}

static void test_recall_order_and_draft(void)
{
    shell_hist_t h;
    shell_hist_init(&h);
    shell_hist_push(&h, "one");
    shell_hist_push(&h, "two");
    shell_hist_push(&h, "three");

    CU_ASSERT_TRUE(streq(shell_hist_prev(&h, "typ"), "three"));
    CU_ASSERT_TRUE(streq(shell_hist_prev(&h, "ignored"), "two"));
    CU_ASSERT_TRUE(streq(shell_hist_prev(&h, "ignored"), "one"));
    /* Stays on the oldest. */
    CU_ASSERT_TRUE(streq(shell_hist_prev(&h, "ignored"), "one"));
    CU_ASSERT_TRUE(streq(shell_hist_next(&h), "two"));
    CU_ASSERT_TRUE(streq(shell_hist_next(&h), "three"));
    /* Past the newest: the line being typed comes back, browse ends. */
    CU_ASSERT_TRUE(streq(shell_hist_next(&h), "typ"));
    CU_ASSERT_TRUE(shell_hist_next(&h) == NULL);
}

static void test_empty_draft_restored(void)
{
    shell_hist_t h;
    shell_hist_init(&h);
    shell_hist_push(&h, "x");
    CU_ASSERT_TRUE(streq(shell_hist_prev(&h, ""), "x"));
    CU_ASSERT_TRUE(streq(shell_hist_next(&h), ""));
}

static void test_empty_and_duplicate_not_stored(void)
{
    shell_hist_t h;
    shell_hist_init(&h);
    shell_hist_push(&h, "");
    CU_ASSERT_EQUAL(h.count, 0);
    shell_hist_push(&h, "ls");
    shell_hist_push(&h, "ls");
    CU_ASSERT_EQUAL(h.count, 1);
    shell_hist_push(&h, "cat a");
    shell_hist_push(&h, "ls");        /* not adjacent to the last "ls" */
    CU_ASSERT_EQUAL(h.count, 3);
    CU_ASSERT_TRUE(streq(shell_hist_prev(&h, ""), "ls"));
    CU_ASSERT_TRUE(streq(shell_hist_prev(&h, ""), "cat a"));
    CU_ASSERT_TRUE(streq(shell_hist_prev(&h, ""), "ls"));
}

static void test_wraps_after_max(void)
{
    shell_hist_t h;
    char want[8];
    shell_hist_init(&h);

    for (unsigned i = 1; i <= 16; i++) {
        push_num(&h, i);
    }
    CU_ASSERT_EQUAL(h.count, 16);
    /* Full but not yet wrapped: c1 is still the oldest. */
    for (unsigned i = 0; i < 16; i++) {
        (void)shell_hist_prev(&h, "");
    }
    CU_ASSERT_TRUE(streq(shell_hist_prev(&h, ""), "c1"));

    /* 40 pushes in all: only c25..c40 remain, newest first. */
    for (unsigned i = 17; i <= 40; i++) {
        push_num(&h, i);
    }
    CU_ASSERT_EQUAL(h.count, 16);
    for (unsigned i = 0; i < 16; i++) {
        const char *got = shell_hist_prev(&h, "");
        unsigned n = 40 - i;
        want[0] = 'c';
        want[1] = (char)('0' + n / 10);
        want[2] = (char)('0' + n % 10);
        want[3] = '\0';
        CU_ASSERT_TRUE(streq(got, want));
    }
    CU_ASSERT_TRUE(streq(shell_hist_prev(&h, ""), "c25"));
}

static void test_push_ends_browse(void)
{
    shell_hist_t h;
    shell_hist_init(&h);
    shell_hist_push(&h, "a");
    shell_hist_push(&h, "b");
    (void)shell_hist_prev(&h, "");
    (void)shell_hist_prev(&h, "");
    shell_hist_push(&h, "c");
    CU_ASSERT_TRUE(shell_hist_next(&h) == NULL);
    CU_ASSERT_TRUE(streq(shell_hist_prev(&h, "d"), "c"));
}

static void test_long_line_is_cut(void)
{
    shell_hist_t h;
    char big[SHELL_HIST_LEN + 20];
    shell_hist_init(&h);

    for (size_t i = 0; i < sizeof(big) - 1; i++) {
        big[i] = 'x';
    }
    big[sizeof(big) - 1] = '\0';
    shell_hist_push(&h, big);
    shell_hist_push(&h, big);         /* the cut copy still collapses */
    CU_ASSERT_EQUAL(h.count, 1);

    const char *got = shell_hist_prev(&h, "");
    size_t len = 0;
    while (got[len] != '\0') {
        len++;
    }
    CU_ASSERT_EQUAL(len, SHELL_HIST_LEN - 1);
}

void suite_shell_hist_tests(CU_pSuite s)
{
    CU_add_test(s, "empty history recalls nothing", test_empty_history);
    CU_add_test(s, "Up/Down order and draft restore", test_recall_order_and_draft);
    CU_add_test(s, "empty draft is restored", test_empty_draft_restored);
    CU_add_test(s, "empty and repeated lines not stored",
                test_empty_and_duplicate_not_stored);
    CU_add_test(s, "ring wraps after 16 entries", test_wraps_after_max);
    CU_add_test(s, "push ends a browse", test_push_ends_browse);
    CU_add_test(s, "overlong line is cut", test_long_line_is_cut);
}
