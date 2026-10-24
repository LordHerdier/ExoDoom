/*
 * test_shell_firstboot_k.c -- SCRUM-234's first-boot decisions
 * (src/shell/shell_firstboot.h).
 *
 * Pure logic: which mount results count as "unformatted", and which keys
 * answer the format prompt. The shell's interactive half is verified by hand
 * (docs/filesystem.md "First boot"). That a fresh volume has two blocks in
 * use is already asserted by test_exofs_k.c; not repeated here. Registered
 * with (NULL, NULL) like "exo_admit".
 */

#include "kunit.h"

#include "shell/shell_firstboot.h"

static void test_mount_ok_is_present(void)
{
    CU_ASSERT_EQUAL(shell_firstboot_classify(0), SHELL_FIRSTBOOT_PRESENT);
}

static void test_blank_disk_is_unformatted(void)
{
    CU_ASSERT_EQUAL(shell_firstboot_classify(-EXO_EINVAL),
                    SHELL_FIRSTBOOT_UNFORMATTED);
}

static void test_other_errors_never_offer_a_format(void)
{
    CU_ASSERT_EQUAL(shell_firstboot_classify(-EXO_EBUSY), SHELL_FIRSTBOOT_ERROR);
    CU_ASSERT_EQUAL(shell_firstboot_classify(-EXO_ENOMEM), SHELL_FIRSTBOOT_ERROR);
    CU_ASSERT_EQUAL(shell_firstboot_classify(-EXO_EIO), SHELL_FIRSTBOOT_ERROR);
    CU_ASSERT_EQUAL(shell_firstboot_classify(-EXO_ENODEV), SHELL_FIRSTBOOT_ERROR);
}

static void test_only_y_answers_yes(void)
{
    CU_ASSERT_TRUE(shell_firstboot_answer_is_yes('y'));
    CU_ASSERT_TRUE(shell_firstboot_answer_is_yes('Y'));

    CU_ASSERT_FALSE(shell_firstboot_answer_is_yes('n'));
    CU_ASSERT_FALSE(shell_firstboot_answer_is_yes('N'));
    CU_ASSERT_FALSE(shell_firstboot_answer_is_yes('\n'));
    CU_ASSERT_FALSE(shell_firstboot_answer_is_yes(' '));
    CU_ASSERT_FALSE(shell_firstboot_answer_is_yes('0'));
    CU_ASSERT_FALSE(shell_firstboot_answer_is_yes('\0'));
}

void suite_shell_firstboot_tests(CU_pSuite s)
{
    CU_add_test(s, "a successful mount is a present volume",
                test_mount_ok_is_present);
    CU_add_test(s, "-EXO_EINVAL (blank disk) is unformatted",
                test_blank_disk_is_unformatted);
    CU_add_test(s, "busy / nomem / I/O errors never offer a format",
                test_other_errors_never_offer_a_format);
    CU_add_test(s, "only y/Y answers yes to the format prompt",
                test_only_y_answers_yes);
}
