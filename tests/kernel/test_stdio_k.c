/*
 * test_stdio_k.c — Kernel-side CUnit tests for src/stdio.c (SCRUM-20).
 *
 * printf itself writes to COM1 serial, which can't be asserted from inside
 * the kernel. Instead we drive the shared formatting core (kvprintf) with a
 * buffer sink, so the exact formatted bytes are captured in memory and can be
 * compared with CU_ASSERT_STRING_EQUAL. printf is a thin wrapper over this
 * core, so covering kvprintf covers the format engine.
 */

#include "kunit.h"
#include "stdio.h"

/* Buffer sink: appends each emitted char, bounded by the buffer size. */
struct sink {
    char  *p;
    size_t n;    /* bytes written so far */
    size_t cap;  /* usable capacity, leaving room for the NUL */
};

static void buf_emit(int c, void *ctx)
{
    struct sink *s = ctx;
    if (s->n < s->cap)
        s->p[s->n] = (char)c;
    s->n++;
}

/* Format into out[] via kvprintf; NUL-terminate; return kvprintf's count. */
static int fmt_to_buf(char *out, size_t cap, const char *fmt, ...)
{
    struct sink s = { out, 0, cap - 1 };
    va_list ap;
    va_start(ap, fmt);
    int n = kvprintf(buf_emit, &s, fmt, ap);
    va_end(ap);
    out[s.n < s.cap ? s.n : s.cap] = '\0';
    return n;
}

static void test_plain_string(void)
{
    char buf[64];
    int n = fmt_to_buf(buf, sizeof buf, "hello world");
    CU_ASSERT_STRING_EQUAL(buf, "hello world");
    CU_ASSERT_EQUAL(n, 11);
}

static void test_d_signed(void)
{
    char buf[64];
    fmt_to_buf(buf, sizeof buf, "%d", 42);
    CU_ASSERT_STRING_EQUAL(buf, "42");
    fmt_to_buf(buf, sizeof buf, "%d", -42);
    CU_ASSERT_STRING_EQUAL(buf, "-42");
    fmt_to_buf(buf, sizeof buf, "%d", 0);
    CU_ASSERT_STRING_EQUAL(buf, "0");
}

static void test_d_int_min(void)
{
    char buf[64];
    /* INT_MIN must not overflow when negated. */
    fmt_to_buf(buf, sizeof buf, "%d", (int)(-2147483647 - 1));
    CU_ASSERT_STRING_EQUAL(buf, "-2147483648");
}

static void test_u_unsigned(void)
{
    char buf[64];
    fmt_to_buf(buf, sizeof buf, "%u", 4294967295U);
    CU_ASSERT_STRING_EQUAL(buf, "4294967295");
    fmt_to_buf(buf, sizeof buf, "%u", 0U);
    CU_ASSERT_STRING_EQUAL(buf, "0");
}

static void test_x_hex(void)
{
    char buf[64];
    fmt_to_buf(buf, sizeof buf, "%x", 0xdeadbeefU);
    CU_ASSERT_STRING_EQUAL(buf, "deadbeef");
    fmt_to_buf(buf, sizeof buf, "%x", 0U);
    CU_ASSERT_STRING_EQUAL(buf, "0");
    fmt_to_buf(buf, sizeof buf, "%x", 0x1aU);
    CU_ASSERT_STRING_EQUAL(buf, "1a");
}

static void test_c_char(void)
{
    char buf[64];
    fmt_to_buf(buf, sizeof buf, "%c", 'Z');
    CU_ASSERT_STRING_EQUAL(buf, "Z");
}

static void test_s_string(void)
{
    char buf[64];
    fmt_to_buf(buf, sizeof buf, "%s", "doom");
    CU_ASSERT_STRING_EQUAL(buf, "doom");
}

static void test_s_null(void)
{
    char buf[64];
    fmt_to_buf(buf, sizeof buf, "%s", (char *)0);
    CU_ASSERT_STRING_EQUAL(buf, "(null)");
}

static void test_p_pointer(void)
{
    char buf[64];
    fmt_to_buf(buf, sizeof buf, "%p", (void *)0x1000);
    CU_ASSERT_STRING_EQUAL(buf, "0x1000");
    fmt_to_buf(buf, sizeof buf, "%p", (void *)0);
    CU_ASSERT_STRING_EQUAL(buf, "0x0");
}

static void test_percent_literal(void)
{
    char buf[64];
    int n = fmt_to_buf(buf, sizeof buf, "100%%");
    CU_ASSERT_STRING_EQUAL(buf, "100%");
    CU_ASSERT_EQUAL(n, 4);
}

static void test_mixed(void)
{
    char buf[64];
    fmt_to_buf(buf, sizeof buf, "%s=%d 0x%x %c", "n", -7, 0xffU, '!');
    CU_ASSERT_STRING_EQUAL(buf, "n=-7 0xff !");
}

static void test_width_right(void)
{
    char buf[64];
    fmt_to_buf(buf, sizeof buf, "%5d", 42);
    CU_ASSERT_STRING_EQUAL(buf, "   42");
}

static void test_width_left(void)
{
    char buf[64];
    fmt_to_buf(buf, sizeof buf, "%-5d", 42);
    CU_ASSERT_STRING_EQUAL(buf, "42   ");
}

static void test_width_zero_pad(void)
{
    char buf[64];
    fmt_to_buf(buf, sizeof buf, "%08x", 0x1aU);
    CU_ASSERT_STRING_EQUAL(buf, "0000001a");
}

static void test_zero_pad_negative(void)
{
    char buf[64];
    /* sign must come before the zero fill */
    fmt_to_buf(buf, sizeof buf, "%05d", -42);
    CU_ASSERT_STRING_EQUAL(buf, "-0042");
}

static void test_width_string(void)
{
    char buf[64];
    fmt_to_buf(buf, sizeof buf, "%10s", "hi");
    CU_ASSERT_STRING_EQUAL(buf, "        hi");
    fmt_to_buf(buf, sizeof buf, "%-10s", "hi");
    CU_ASSERT_STRING_EQUAL(buf, "hi        ");
}

static void test_width_no_truncate(void)
{
    char buf[64];
    /* width narrower than the value never truncates */
    fmt_to_buf(buf, sizeof buf, "%2d", 12345);
    CU_ASSERT_STRING_EQUAL(buf, "12345");
}

static void test_return_count(void)
{
    char buf[64];
    int n = fmt_to_buf(buf, sizeof buf, "%5d-%s", 42, "ok");
    /* "   42-ok" == 8 chars */
    CU_ASSERT_EQUAL(n, 8);
    CU_ASSERT_STRING_EQUAL(buf, "   42-ok");
}

void suite_stdio_tests(CU_pSuite s)
{
    CU_add_test(s, "plain_string",       test_plain_string);
    CU_add_test(s, "d_signed",           test_d_signed);
    CU_add_test(s, "d_int_min",          test_d_int_min);
    CU_add_test(s, "u_unsigned",         test_u_unsigned);
    CU_add_test(s, "x_hex",              test_x_hex);
    CU_add_test(s, "c_char",             test_c_char);
    CU_add_test(s, "s_string",           test_s_string);
    CU_add_test(s, "s_null",             test_s_null);
    CU_add_test(s, "p_pointer",          test_p_pointer);
    CU_add_test(s, "percent_literal",    test_percent_literal);
    CU_add_test(s, "mixed",              test_mixed);
    CU_add_test(s, "width_right",        test_width_right);
    CU_add_test(s, "width_left",         test_width_left);
    CU_add_test(s, "width_zero_pad",     test_width_zero_pad);
    CU_add_test(s, "zero_pad_negative",  test_zero_pad_negative);
    CU_add_test(s, "width_string",       test_width_string);
    CU_add_test(s, "width_no_truncate",  test_width_no_truncate);
    CU_add_test(s, "return_count",       test_return_count);
}
