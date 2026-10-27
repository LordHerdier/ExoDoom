/*
 * test_shell_complete_k.c — the shell's Tab-completion matching (SCRUM-274).
 *
 * src/shell/shell_complete.c is ring-3 shell code; docker/scripts/build.sh
 * compiles a second copy into the TESTING kernel (same arrangement as
 * test_shell_hist_k.c).
 */

#include "kunit.h"
#include "shell/shell_complete.h"

#include <stddef.h>

static int streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void classify(const char *line, shell_comp_ctx_t *c)
{
    shell_comp_classify(line, slen(line), c);
}

static void test_classify_command(void)
{
    shell_comp_ctx_t c;

    classify("sn", &c);
    CU_ASSERT_EQUAL(c.kind, SHELL_COMP_COMMAND);
    CU_ASSERT_EQUAL(c.leaf_off, 0);
    CU_ASSERT_EQUAL(c.leaf_len, 2);

    classify("", &c);
    CU_ASSERT_EQUAL(c.kind, SHELL_COMP_COMMAND);
    CU_ASSERT_EQUAL(c.leaf_len, 0);

    classify("  sn", &c);
    CU_ASSERT_EQUAL(c.kind, SHELL_COMP_COMMAND);
    CU_ASSERT_EQUAL(c.leaf_off, 2);
}

static void test_classify_verbs(void)
{
    shell_comp_ctx_t c;

    classify("run vol", &c);
    CU_ASSERT_EQUAL(c.kind, SHELL_COMP_PROGRAM);
    CU_ASSERT_EQUAL(c.leaf_off, 4);
    CU_ASSERT_EQUAL(c.leaf_len, 3);

    classify("run ", &c);
    CU_ASSERT_EQUAL(c.kind, SHELL_COMP_PROGRAM);
    CU_ASSERT_EQUAL(c.leaf_len, 0);

    classify("install free", &c);
    CU_ASSERT_EQUAL(c.kind, SHELL_COMP_INSTALL);
    CU_ASSERT_EQUAL(c.leaf_len, 4);

    classify("RUN vol", &c);
    CU_ASSERT_EQUAL(c.kind, SHELL_COMP_PROGRAM);

    classify("echo hi", &c);
    CU_ASSERT_EQUAL(c.kind, SHELL_COMP_NONE);

    classify("runx vol", &c);
    CU_ASSERT_EQUAL(c.kind, SHELL_COMP_NONE);
}

static void test_classify_paths(void)
{
    shell_comp_ctx_t c;

    classify("cat /mn", &c);
    CU_ASSERT_EQUAL(c.kind, SHELL_COMP_PATH);
    CU_ASSERT_EQUAL(c.dir_off, 4);
    CU_ASSERT_EQUAL(c.dir_len, 1);
    CU_ASSERT_EQUAL(c.leaf_off, 5);
    CU_ASSERT_EQUAL(c.leaf_len, 2);

    classify("ls a/b/c", &c);
    CU_ASSERT_EQUAL(c.dir_len, 4);
    CU_ASSERT_EQUAL(c.leaf_len, 1);

    classify("ls foo", &c);
    CU_ASSERT_EQUAL(c.dir_len, 0);
    CU_ASSERT_EQUAL(c.leaf_off, 3);

    classify("ls /mnt/", &c);
    CU_ASSERT_EQUAL(c.dir_len, 5);
    CU_ASSERT_EQUAL(c.leaf_len, 0);

    /* Every word after the verb is a path. */
    classify("cp a.txt b", &c);
    CU_ASSERT_EQUAL(c.kind, SHELL_COMP_PATH);
    CU_ASSERT_EQUAL(c.leaf_off, 9);

    classify("cd /m", &c);
    CU_ASSERT_EQUAL(c.kind, SHELL_COMP_DIR);
}

static void test_unique_command_gets_space(void)
{
    shell_comp_set_t s;
    char out[SHELL_COMP_NAME_LEN];
    const char *cmds[] = { "cat", "snake", "clear", "wadview" };

    shell_comp_init(&s);
    for (int i = 0; i < 4; i++) {
        shell_comp_add(&s, cmds[i], 0, "sn", 2);
    }
    CU_ASSERT_EQUAL(s.count, 1);
    CU_ASSERT_EQUAL(shell_comp_apply(&s, 2, out, sizeof(out)), 4);
    CU_ASSERT_TRUE(streq(out, "ake "));
}

static void test_common_prefix_and_candidates(void)
{
    shell_comp_set_t s;
    char out[SHELL_COMP_NAME_LEN];
    const char *cmds[] = { "cat", "cd", "clear", "clock", "cp", "snake" };

    shell_comp_init(&s);
    for (int i = 0; i < 6; i++) {
        shell_comp_add(&s, cmds[i], 0, "c", 1);
    }
    CU_ASSERT_EQUAL(s.count, 5);
    CU_ASSERT_TRUE(streq(s.name[0], "cat"));
    CU_ASSERT_TRUE(streq(s.name[4], "cp"));
    /* cat/cd/clear/clock/cp share only the typed "c". */
    CU_ASSERT_EQUAL(shell_comp_apply(&s, 1, out, sizeof(out)), 0);

    shell_comp_init(&s);
    shell_comp_add(&s, "clear", 0, "c", 1);
    shell_comp_add(&s, "clock", 0, "c", 1);
    CU_ASSERT_EQUAL(shell_comp_apply(&s, 1, out, sizeof(out)), 1);
    CU_ASSERT_TRUE(streq(out, "l"));
}

static void test_case_insensitive(void)
{
    shell_comp_set_t s;
    char out[SHELL_COMP_NAME_LEN];

    shell_comp_init(&s);
    CU_ASSERT_EQUAL(shell_comp_add(&s, "FreeDoom2.wad", 0, "free", 4), 1);
    CU_ASSERT_EQUAL(shell_comp_add(&s, "freedm.wad", 0, "FREE", 4), 1);
    CU_ASSERT_EQUAL(shell_comp_apply(&s, 4, out, sizeof(out)), 1);
    CU_ASSERT_TRUE(streq(out, "D"));

    shell_comp_init(&s);
    shell_comp_add(&s, "FreeDoom2.wad", 0, "free", 4);
    CU_ASSERT_EQUAL(shell_comp_apply(&s, 4, out, sizeof(out)), 10);
    CU_ASSERT_TRUE(streq(out, "Doom2.wad "));
}

static void test_directory_gets_slash(void)
{
    shell_comp_set_t s;
    char out[SHELL_COMP_NAME_LEN];

    shell_comp_init(&s);
    shell_comp_add(&s, "mnt", 1, "m", 1);
    CU_ASSERT_EQUAL(shell_comp_apply(&s, 1, out, sizeof(out)), 3);
    CU_ASSERT_TRUE(streq(out, "nt/"));
}

static void test_no_match_and_empty_prefix(void)
{
    shell_comp_set_t s;
    char out[SHELL_COMP_NAME_LEN];

    shell_comp_init(&s);
    CU_ASSERT_EQUAL(shell_comp_add(&s, "snake", 0, "z", 1), 0);
    CU_ASSERT_EQUAL(shell_comp_apply(&s, 1, out, sizeof(out)), 0);
    CU_ASSERT_TRUE(streq(out, ""));

    /* Empty prefix matches everything; the common prefix may be empty. */
    shell_comp_add(&s, "a.txt", 0, "", 0);
    shell_comp_add(&s, "b.txt", 0, "", 0);
    CU_ASSERT_EQUAL(s.count, 2);
    CU_ASSERT_EQUAL(shell_comp_apply(&s, 0, out, sizeof(out)), 0);
}

static void test_duplicates_and_long_names_skipped(void)
{
    shell_comp_set_t s;
    char big[SHELL_COMP_NAME_LEN + 4];

    shell_comp_init(&s);
    CU_ASSERT_EQUAL(shell_comp_add(&s, "a", 0, "", 0), 1);
    CU_ASSERT_EQUAL(shell_comp_add(&s, "a", 0, "", 0), 0);
    CU_ASSERT_EQUAL(s.count, 1);

    for (size_t i = 0; i < sizeof(big) - 1; i++) {
        big[i] = 'x';
    }
    big[sizeof(big) - 1] = '\0';
    CU_ASSERT_EQUAL(shell_comp_add(&s, big, 0, "", 0), 0);
    CU_ASSERT_EQUAL(s.count, 1);
}

static void test_overflow(void)
{
    shell_comp_set_t s;
    char name[3] = { 'a', 'a', '\0' };

    shell_comp_init(&s);
    for (unsigned i = 0; i < SHELL_COMP_MAX + 3; i++) {
        name[1] = (char)('a' + i);
        shell_comp_add(&s, name, 0, "a", 1);
    }
    CU_ASSERT_EQUAL(s.count, SHELL_COMP_MAX);
    CU_ASSERT_TRUE(s.overflow);
}

static void test_apply_bounded(void)
{
    shell_comp_set_t s;
    char out[4];

    shell_comp_init(&s);
    shell_comp_add(&s, "freedoom2.wad", 0, "f", 1);
    CU_ASSERT_EQUAL(shell_comp_apply(&s, 1, out, sizeof(out)), 3);
    CU_ASSERT_TRUE(streq(out, "ree"));
}

static void test_is_program(void)
{
    CU_ASSERT_TRUE(shell_comp_is_program("voldemoa.exo"));
    CU_ASSERT_TRUE(shell_comp_is_program("a.exo"));
    CU_ASSERT_FALSE(shell_comp_is_program(".exo"));
    CU_ASSERT_FALSE(shell_comp_is_program("freedoom2.wad"));
    CU_ASSERT_FALSE(shell_comp_is_program("exo"));
}

void suite_shell_complete_tests(CU_pSuite s)
{
    CU_add_test(s, "classify: first word is a command", test_classify_command);
    CU_add_test(s, "classify: run/install/other verbs", test_classify_verbs);
    CU_add_test(s, "classify: path words split at last slash", test_classify_paths);
    CU_add_test(s, "unique command completes with a space", test_unique_command_gets_space);
    CU_add_test(s, "several matches complete the common prefix", test_common_prefix_and_candidates);
    CU_add_test(s, "matching is case-insensitive", test_case_insensitive);
    CU_add_test(s, "directory completes with a slash", test_directory_gets_slash);
    CU_add_test(s, "no match / empty prefix", test_no_match_and_empty_prefix);
    CU_add_test(s, "duplicates and long names skipped", test_duplicates_and_long_names_skipped);
    CU_add_test(s, "candidate set overflow is flagged", test_overflow);
    CU_add_test(s, "apply respects the output size", test_apply_bounded);
    CU_add_test(s, "program name rule", test_is_program);
}
