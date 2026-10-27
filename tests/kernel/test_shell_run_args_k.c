/*
 * test_shell_run_args_k.c — `run` argument text to launch params (SCRUM-301).
 *
 * src/shell/shell_run_args.c is ring-3 shell code; docker/scripts/build.sh
 * compiles a second copy into the TESTING kernel (same arrangement as
 * test_shell_err_k.c).
 */

#include "kunit.h"
#include "shell/shell_run_args.h"

#include <stddef.h>
#include <stdint.h>

static int buf_is(const uint8_t *buf, uint32_t len, const char *text)
{
    uint32_t i = 0;
    for (; text[i] != '\0'; i++) {
        if (i >= len || buf[i] != (uint8_t)text[i]) {
            return 0;
        }
    }
    for (; i < len; i++) {
        if (buf[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static void fill_ff(uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = 0xFF;
    }
}

static void test_arguments_are_copied_and_zero_padded(void)
{
    uint8_t buf[32];
    fill_ff(buf, sizeof(buf));
    CU_ASSERT_EQUAL(shell_run_args_fill("-iwad doom2.wad", sizeof(buf), buf),
                    SHELL_RUN_ARGS_OK);
    CU_ASSERT_TRUE(buf_is(buf, sizeof(buf), "-iwad doom2.wad"));
}

static void test_surrounding_spaces_are_dropped_inner_ones_kept(void)
{
    uint8_t buf[32];
    fill_ff(buf, sizeof(buf));
    CU_ASSERT_EQUAL(shell_run_args_fill("  -skill  4  ", sizeof(buf), buf),
                    SHELL_RUN_ARGS_OK);
    CU_ASSERT_TRUE(buf_is(buf, sizeof(buf), "-skill  4"));
}

static void test_no_arguments_clears_the_buffer(void)
{
    uint8_t buf[8];
    fill_ff(buf, sizeof(buf));
    CU_ASSERT_EQUAL(shell_run_args_fill("", sizeof(buf), buf), SHELL_RUN_ARGS_OK);
    CU_ASSERT_TRUE(buf_is(buf, sizeof(buf), ""));

    fill_ff(buf, sizeof(buf));
    CU_ASSERT_EQUAL(shell_run_args_fill("   ", sizeof(buf), buf),
                    SHELL_RUN_ARGS_OK);
    CU_ASSERT_TRUE(buf_is(buf, sizeof(buf), ""));
}

static void test_longest_fit_is_len_minus_one_and_never_truncates(void)
{
    uint8_t buf[8];
    fill_ff(buf, sizeof(buf));
    CU_ASSERT_EQUAL(shell_run_args_fill("1234567", sizeof(buf), buf),
                    SHELL_RUN_ARGS_OK);
    CU_ASSERT_TRUE(buf_is(buf, sizeof(buf), "1234567"));

    /* One more character does not fit with its NUL: refused, buffer as it was. */
    fill_ff(buf, sizeof(buf));
    CU_ASSERT_EQUAL(shell_run_args_fill("12345678", sizeof(buf), buf),
                    SHELL_RUN_ARGS_TOO_LONG);
    CU_ASSERT_EQUAL(buf[0], 0xFF);
    CU_ASSERT_EQUAL(buf[7], 0xFF);
}

static void test_package_without_params_takes_no_arguments(void)
{
    uint8_t buf[1] = { 0xFF };
    CU_ASSERT_EQUAL(shell_run_args_fill("-iwad x", 0, buf),
                    SHELL_RUN_ARGS_NO_PARAMS);
    CU_ASSERT_EQUAL(buf[0], 0xFF);

    /* Nothing typed is not an error, and writes nothing. */
    CU_ASSERT_EQUAL(shell_run_args_fill("", 0, buf), SHELL_RUN_ARGS_OK);
    CU_ASSERT_EQUAL(shell_run_args_fill("  ", 0, buf), SHELL_RUN_ARGS_OK);
    CU_ASSERT_EQUAL(buf[0], 0xFF);
}

void suite_shell_run_args_tests(CU_pSuite s)
{
    CU_add_test(s, "arguments are copied and zero-padded",
                test_arguments_are_copied_and_zero_padded);
    CU_add_test(s, "surrounding spaces are dropped, inner ones kept",
                test_surrounding_spaces_are_dropped_inner_ones_kept);
    CU_add_test(s, "no arguments clears the buffer",
                test_no_arguments_clears_the_buffer);
    CU_add_test(s, "the longest fit is one under the buffer, never truncated",
                test_longest_fit_is_len_minus_one_and_never_truncates);
    CU_add_test(s, "a package without params takes no arguments",
                test_package_without_params_takes_no_arguments);
}
