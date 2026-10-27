/*
 * test_shell_err_k.c — the shell's errno-to-sentence mapping (SCRUM-271).
 *
 * src/shell/shell_err.c is ring-3 shell code; docker/scripts/build.sh
 * compiles a second copy into the TESTING kernel (same arrangement as
 * test_shell_help_k.c).
 */

#include "kunit.h"
#include "shell/shell_err.h"
#include "exo_errno.h"

#include <stddef.h>

static int streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void test_busy_has_advice(void)
{
    int m = 0;
    const char *t = shell_err_text(SHELL_OP_RUN, -EXO_EBUSY, &m);
    CU_ASSERT_EQUAL(m, 1);
    CU_ASSERT_TRUE(streq(t,
        "another program is using the disk -- Ctrl+Tab to it, or quit it first"));
    /* Same wording whichever command hit it. */
    CU_ASSERT_TRUE(streq(shell_err_text(SHELL_OP_DISK, -EXO_EBUSY, NULL), t));
    CU_ASSERT_TRUE(streq(shell_err_text(SHELL_OP_INSTALL, EXO_EBUSY, NULL), t));
}

static void test_enoent_depends_on_op(void)
{
    int m = 0;
    CU_ASSERT_TRUE(streq(shell_err_text(SHELL_OP_RUN, -EXO_ENOENT, &m),
                         "no such program (try 'programs')"));
    CU_ASSERT_EQUAL(m, 1);
    CU_ASSERT_TRUE(streq(shell_err_text(SHELL_OP_INSTALL, -EXO_ENOENT, &m),
                         "not on the install disk (type 'install' to list it)"));
    CU_ASSERT_TRUE(streq(shell_err_text(SHELL_OP_FS, -EXO_ENOENT, &m),
                         "no such file or directory"));
}

static void test_sign_is_ignored(void)
{
    CU_ASSERT_TRUE(streq(shell_err_text(SHELL_OP_FS, EXO_ENOSPC, NULL),
                         shell_err_text(SHELL_OP_FS, -EXO_ENOSPC, NULL)));
}

static void test_unmapped_falls_back(void)
{
    int m = 1;
    const char *t = shell_err_text(SHELL_OP_RUN, -9999, &m);
    CU_ASSERT_EQUAL(m, 0);
    CU_ASSERT_TRUE(streq(t, "launch failed"));
    t = shell_err_text(SHELL_OP_MOUNT, -9999, &m);
    CU_ASSERT_EQUAL(m, 0);
    CU_ASSERT_TRUE(streq(t, "could not mount the volume (run mkfs?)"));
}

static void test_every_op_has_text(void)
{
    for (int op = SHELL_OP_FS; op <= SHELL_OP_PROBE; op++) {
        const char *t = shell_err_text((shell_op_t)op, -9999, NULL);
        CU_ASSERT_TRUE(t != NULL && t[0] != '\0');
    }
    CU_ASSERT_TRUE(shell_err_lead(SHELL_OP_DISK) == NULL);
    CU_ASSERT_TRUE(streq(shell_err_lead(SHELL_OP_RUN), "run"));
}

void suite_shell_err_tests(CU_pSuite s)
{
    CU_add_test(s, "EBUSY says what to do", test_busy_has_advice);
    CU_add_test(s, "ENOENT wording depends on the command", test_enoent_depends_on_op);
    CU_add_test(s, "either sign of errno", test_sign_is_ignored);
    CU_add_test(s, "unmapped code falls back", test_unmapped_falls_back);
    CU_add_test(s, "every op has a fallback", test_every_op_has_text);
}
