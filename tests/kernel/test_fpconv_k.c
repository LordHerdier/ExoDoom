/*
 * test_fpconv_k.c — SCRUM-65's tests for src/fpconv.c.
 *
 * This is the half of Doom's floating point that can be tested, and the
 * reason src/fpconv.c exists as a separate module: the kernel builds
 * -mno-sse, so no test TU can name `double` at all, but every part of a
 * float conversion that can actually be WRONG -- exponent handling,
 * rounding, carry propagation, the subnormal and infinity edges -- is
 * integer work on a bit pattern, and that is what this drives.
 *
 * The expected strings and bit patterns below were produced by glibc's
 * snprintf and strtod on the host and pasted in as literals.  That is on
 * purpose: a test that computed its own expectations from the same
 * algorithm would only prove the code agrees with itself.  These are an
 * independent implementation's answers.
 */

#include "kunit.h"

#include "fpconv.h"
#include "string.h"

#include <stdint.h>

/*
 * Bit patterns, written as hex rather than as double literals -- which this
 * TU could not compile even if it wanted to.  Each is glibc's encoding of the
 * value named in the comment.
 */
#define BITS_ZERO      0x0000000000000000ULL /*  0.0                */
#define BITS_NEG_ZERO  0x8000000000000000ULL /* -0.0                */
#define BITS_ONE       0x3FF0000000000000ULL /*  1.0                */
#define BITS_NEG_ONE   0xBFF0000000000000ULL /* -1.0                */
#define BITS_HALF      0x3FE0000000000000ULL /*  0.5                */
#define BITS_ONE_HALF  0x3FF8000000000000ULL /*  1.5                */
#define BITS_TWO_75    0x4006000000000000ULL /*  2.75               */
#define BITS_NEG_2_75  0xC006000000000000ULL /* -2.75               */
#define BITS_PI        0x400921FB54442D18ULL /*  3.141592653589793  */
#define BITS_TENTH     0x3FB999999999999AULL /*  0.1                */
#define BITS_9_9999    0x4023FFF2E48E8A71ULL /*  9.9999             */
#define BITS_THIRTYFIVE 0x4041800000000000ULL /* 35.0               */
#define BITS_INF       0x7FF0000000000000ULL /*  inf                */
#define BITS_NEG_INF   0xFFF0000000000000ULL /* -inf                */
#define BITS_NAN       0x7FF8000000000000ULL /*  nan                */

static void expect_fmt(uint64_t bits, unsigned precision, const char *want)
{
    char buf[64];

    exo_fmt_f64(buf, sizeof buf, bits, precision);
    CU_ASSERT_STRING_EQUAL(buf, want);
}

static void test_fmt_basic_values(void)
{
    /* Every expected string here is what glibc's snprintf("%.*f") produced. */
    expect_fmt(BITS_ZERO, 6, "0.000000");
    expect_fmt(BITS_ONE, 6, "1.000000");
    expect_fmt(BITS_NEG_ONE, 6, "-1.000000");
    expect_fmt(BITS_HALF, 6, "0.500000");
    expect_fmt(BITS_ONE_HALF, 2, "1.50");
    expect_fmt(BITS_THIRTYFIVE, 1, "35.0");
    expect_fmt(BITS_PI, 6, "3.141593");
    expect_fmt(BITS_PI, 0, "3");
    expect_fmt(BITS_TENTH, 6, "0.100000");

    /* -0.0 keeps its sign, the way a hosted printf does.  This is the case a
     * "if (value == 0) print zero" shortcut gets wrong. */
    expect_fmt(BITS_NEG_ZERO, 6, "-0.000000");
}

static void test_fmt_rounding_is_half_to_even(void)
{
    /*
     * Exact ties round to even, matching a hosted printf under the default
     * FE_TONEAREST.  0.5 at precision 0 is "0", not "1".
     *
     * An earlier version of exo_fmt_f64 rounded half-away-from-zero, which
     * disagreed with glibc on every one of these -- it was found by this
     * comparison and not by inspection.
     */
    expect_fmt(BITS_HALF, 0, "0");      /* 0.5  -> 0 (0 is even) */
    expect_fmt(BITS_ONE_HALF, 0, "2");  /* 1.5  -> 2 (2 is even) */
    expect_fmt(BITS_TWO_75, 1, "2.8");  /* 2.75 -> 2.8           */
    expect_fmt(BITS_NEG_2_75, 1, "-2.8");
}

static void test_fmt_carry_reaches_the_integer_part(void)
{
    /*
     * 9.9999 at 3 decimal places is "10.000": the carry out of the last digit
     * has to propagate back through every 9 AND into the integer part.  A
     * round-as-you-go loop cannot do this -- it would have printed the 9s
     * already -- which is why the digits are buffered before rounding.
     */
    expect_fmt(BITS_9_9999, 3, "10.000");
    expect_fmt(BITS_9_9999, 4, "9.9999");
    expect_fmt(BITS_9_9999, 0, "10");
}

static void test_fmt_specials(void)
{
    /* C99 spells these lowercase for %f. */
    expect_fmt(BITS_INF, 6, "inf");
    expect_fmt(BITS_NEG_INF, 6, "-inf");
    expect_fmt(BITS_NAN, 6, "nan");
}

static void test_fmt_precision_zero_drops_the_point(void)
{
    /* "%.0f" produces no decimal point at all -- not a trailing one. */
    char buf[64];

    exo_fmt_f64(buf, sizeof buf, BITS_ONE, 0);
    CU_ASSERT_STRING_EQUAL(buf, "1");
    CU_ASSERT_PTR_NULL(strchr(buf, '.'));
}

static void test_fmt_never_overruns(void)
{
    /*
     * Truncation rather than overrun, and still NUL-terminated.  This
     * function formats diagnostics on failure paths, so a buffer overrun
     * here would corrupt memory precisely when something has already gone
     * wrong.
     */
    char small[5];
    char guard[16];
    size_t n;

    memset(guard, 0x7E, sizeof guard);

    n = exo_fmt_f64(small, sizeof small, BITS_PI, 6);
    CU_ASSERT_TRUE(n < sizeof small);
    CU_ASSERT_EQUAL(small[sizeof small - 1], '\0');
    /* Whatever fits must be a prefix of the full answer. */
    CU_ASSERT_TRUE(small[0] == '3');

    /* A zero capacity must write nothing at all. */
    CU_ASSERT_EQUAL(exo_fmt_f64(guard, 0, BITS_PI, 6), 0u);
    CU_ASSERT_EQUAL((unsigned char)guard[0], 0x7Eu);
}

/* ---- parsing ---------------------------------------------------------- */

static void expect_parse(const char *text, uint64_t want)
{
    CU_ASSERT_EQUAL(exo_parse_f64(text, 0), want);
}

static void test_parse_exact_values(void)
{
    /*
     * Bit-for-bit against glibc's strtod.  Everything here is inside the
     * range src/fpconv.h documents as exact (|decimal exponent| <= 15), which
     * covers every value a Doom config file holds.
     */
    expect_parse("0", BITS_ZERO);
    expect_parse("1", BITS_ONE);
    expect_parse("1.0", BITS_ONE);
    expect_parse("-1", BITS_NEG_ONE);
    expect_parse("0.5", BITS_HALF);
    expect_parse("1.5", BITS_ONE_HALF);
    expect_parse("2.75", BITS_TWO_75);
    expect_parse("-2.75", BITS_NEG_2_75);
    expect_parse("35", BITS_THIRTYFIVE);
    expect_parse("35.0", BITS_THIRTYFIVE);
    expect_parse("0.1", BITS_TENTH);
    expect_parse("3.141592653589793", BITS_PI);

    /* Leading whitespace and an explicit '+' are both accepted. */
    expect_parse("  1.5", BITS_ONE_HALF);
    expect_parse("+1.5", BITS_ONE_HALF);

    /* Exponent forms. */
    expect_parse("1.5e0", BITS_ONE_HALF);
    expect_parse("15e-1", BITS_ONE_HALF);
    expect_parse("0.015E2", BITS_ONE_HALF);
}

static void test_parse_rejects_non_numbers(void)
{
    const char *end;

    /* atof returns 0 for input it cannot convert, and *endp must be the
     * ORIGINAL pointer -- including any whitespace and sign already walked
     * past, or a caller looping over a string would advance and never
     * terminate. */
    CU_ASSERT_EQUAL(exo_parse_f64("abc", &end), BITS_ZERO);
    CU_ASSERT_STRING_EQUAL(end, "abc");

    CU_ASSERT_EQUAL(exo_parse_f64("", &end), BITS_ZERO);
    CU_ASSERT_STRING_EQUAL(end, "");

    /* A sign with no digits is not a number. */
    CU_ASSERT_EQUAL(exo_parse_f64("  -x", &end), BITS_ZERO);
    CU_ASSERT_STRING_EQUAL(end, "  -x");
}

static void test_parse_stops_where_it_should(void)
{
    const char *end;

    exo_parse_f64("12.5xyz", &end);
    CU_ASSERT_STRING_EQUAL(end, "xyz");

    /* "1e" has no exponent digits, so the 'e' is not part of the number --
     * a parser that consumed it would hand the caller back the wrong tail. */
    exo_parse_f64("1e", &end);
    CU_ASSERT_STRING_EQUAL(end, "e");
}

static void test_parse_round_trips_through_format(void)
{
    /*
     * The two halves of this module against each other.  Not a substitute
     * for the reference comparisons above -- a pair of matching bugs would
     * survive it -- but it is what catches a sign or exponent convention
     * that one side applies and the other does not.
     */
    static const char *const values[] = {
        "0.000000", "1.000000", "-1.000000", "0.500000",
        "2.750000", "35.000000", "-2.750000"
    };
    unsigned i;

    for (i = 0; i < sizeof values / sizeof values[0]; i++) {
        char     buf[64];
        uint64_t bits = exo_parse_f64(values[i], 0);

        exo_fmt_f64(buf, sizeof buf, bits, 6);
        CU_ASSERT_STRING_EQUAL(buf, values[i]);
    }
}

void suite_fpconv_tests(CU_pSuite s)
{
    CU_add_test(s, "format basic values", test_fmt_basic_values);
    CU_add_test(s, "rounding is half-to-even",
                test_fmt_rounding_is_half_to_even);
    CU_add_test(s, "carry reaches the integer part",
                test_fmt_carry_reaches_the_integer_part);
    CU_add_test(s, "inf/nan", test_fmt_specials);
    CU_add_test(s, "precision 0 drops the point",
                test_fmt_precision_zero_drops_the_point);
    CU_add_test(s, "format never overruns", test_fmt_never_overruns);
    CU_add_test(s, "parse exact values", test_parse_exact_values);
    CU_add_test(s, "parse rejects non-numbers", test_parse_rejects_non_numbers);
    CU_add_test(s, "parse stops where it should",
                test_parse_stops_where_it_should);
    CU_add_test(s, "parse/format round trip",
                test_parse_round_trips_through_format);
}
