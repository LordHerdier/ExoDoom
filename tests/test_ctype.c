/* TODO: migrate these tests to CUnit once the testing pipeline is set up */
#include "../src/ctype.h"
#include <stdio.h>
#include <stdlib.h>

static int tests_run = 0;
static int tests_failed = 0;

#define ASSERT_EQ(expr, expected)                                              \
  do {                                                                         \
    tests_run++;                                                               \
    __typeof__(expected) _got = (expr);                                        \
    __typeof__(expected) _exp = (expected);                                    \
    if (_got != _exp) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s — expected %lld, got %lld\n", __FILE__,  \
              __LINE__, #expr, (long long)_exp, (long long)_got);              \
      tests_failed++;                                                          \
    }                                                                          \
  } while (0)

/* ---- isalpha ---- */

static void test_isalpha(void) {
  /* all lowercase letters return nonzero */
  for (int c = 'a'; c <= 'z'; c++)
    ASSERT_EQ(isalpha(c) != 0, 1);

  /* all uppercase letters return nonzero */
  for (int c = 'A'; c <= 'Z'; c++)
    ASSERT_EQ(isalpha(c) != 0, 1);

  /* digits return zero */
  for (int c = '0'; c <= '9'; c++)
    ASSERT_EQ(isalpha(c), 0);

  /* boundaries just outside the alpha ranges */
  ASSERT_EQ(isalpha('@'), 0); /* 'A' - 1 */
  ASSERT_EQ(isalpha('['), 0); /* 'Z' + 1 */
  ASSERT_EQ(isalpha('`'), 0); /* 'a' - 1 */
  ASSERT_EQ(isalpha('{'), 0); /* 'z' + 1 */

  /* common non-alpha characters */
  ASSERT_EQ(isalpha(' '), 0);
  ASSERT_EQ(isalpha('\n'), 0);
  ASSERT_EQ(isalpha('\t'), 0);
  ASSERT_EQ(isalpha('\0'), 0);
  ASSERT_EQ(isalpha('!'), 0);
  ASSERT_EQ(isalpha('_'), 0);

  /* EOF (-1) returns zero */
  ASSERT_EQ(isalpha(-1), 0);

  /* high bytes (valid unsigned char values above ASCII) return zero */
  ASSERT_EQ(isalpha(0x80), 0);
  ASSERT_EQ(isalpha(0xFF), 0);
}

/* ---- isdigit ---- */

static void test_isdigit(void) {
  /* all digit characters return nonzero */
  for (int c = '0'; c <= '9'; c++)
    ASSERT_EQ(isdigit(c) != 0, 1);

  /* boundaries just outside the digit range */
  ASSERT_EQ(isdigit('/'), 0); /* '0' - 1 */
  ASSERT_EQ(isdigit(':'), 0); /* '9' + 1 */

  /* letters return zero */
  for (int c = 'a'; c <= 'z'; c++)
    ASSERT_EQ(isdigit(c), 0);
  for (int c = 'A'; c <= 'Z'; c++)
    ASSERT_EQ(isdigit(c), 0);

  /* common non-digit characters */
  ASSERT_EQ(isdigit(' '), 0);
  ASSERT_EQ(isdigit('\n'), 0);
  ASSERT_EQ(isdigit('\0'), 0);
  ASSERT_EQ(isdigit('!'), 0);

  /* EOF (-1) returns zero */
  ASSERT_EQ(isdigit(-1), 0);

  /* high bytes return zero */
  ASSERT_EQ(isdigit(0x80), 0);
  ASSERT_EQ(isdigit(0xFF), 0);
}

/* ---- main ---- */

int main(void) {
  test_isalpha();
  test_isdigit();

  printf("ctype.c: %d/%d tests passed\n", tests_run - tests_failed, tests_run);
  return tests_failed ? 1 : 0;
}
