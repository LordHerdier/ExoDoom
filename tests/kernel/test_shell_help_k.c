/*
 * test_shell_help_k.c — the shell's help matcher and row formatter
 * (SCRUM-263).
 *
 * src/shell/shell_help.c is ring-3 shell code; docker/scripts/build.sh
 * compiles a second copy into the TESTING kernel so it can be checked here
 * without driving the shell's never-returning input loop (see
 * test_shell_libos_k.c for why that loop has no automated test). The
 * fixture below stands in for the real table in src/shell/shell_main.c,
 * which is static to that file.
 */

#include "kunit.h"
#include "shell/shell_help.h"

#include <stddef.h>

static const shell_help_entry_t fixture[] = {
    { "about", SHELL_GRP_SYSTEM, "About", "usage: about\n" },
    { "cat",   SHELL_GRP_FS,     "Cat",   "usage: cat <path>\n" },
    { "cd",    SHELL_GRP_FS,     "Cd",    "usage: cd [path]\n" },
    { "clear", SHELL_GRP_SYSTEM, "Clear", "usage: clear\n" },
    { "clock", SHELL_GRP_APPS,   "Clock", "usage: clock\n" },
    { "cp",    SHELL_GRP_FS,     "Cp",    "usage: cp <src> <dst>\n" },
    { "dd",    SHELL_GRP_DISK,   "Dd",    "usage: dd ...\n" },
    { "disk",  SHELL_GRP_DISK,   "Disk",  "usage: disk\n" },
    { "doom",  SHELL_GRP_APPS,   "Doom",  "usage: doom\n" },
};
#define FIXTURE_N (sizeof(fixture) / sizeof(fixture[0]))

/* Bitmask of which fixture rows match q. */
static unsigned match_mask(const char *arg)
{
    shell_help_query_t q = shell_help_query_parse(arg);
    unsigned m = 0;
    for (size_t i = 0; i < FIXTURE_N; i++) {
        if (shell_help_entry_matches(&fixture[i], &q)) {
            m |= 1u << i;
        }
    }
    return m;
}

static void test_ci_token_eq(void)
{
    CU_ASSERT_TRUE(shell_ci_token_eq("ls", 2, "ls"));
    CU_ASSERT_TRUE(shell_ci_token_eq("LS", 2, "ls"));
    CU_ASSERT_TRUE(shell_ci_token_eq("Ls foo", 2, "ls"));
    CU_ASSERT_FALSE(shell_ci_token_eq("l", 1, "ls"));
    CU_ASSERT_FALSE(shell_ci_token_eq("lsx", 3, "ls"));
    CU_ASSERT_FALSE(shell_ci_token_eq("", 0, "ls"));
}

static void test_ci_prefix(void)
{
    CU_ASSERT_TRUE(shell_ci_prefix("clock", "c", 1));
    CU_ASSERT_TRUE(shell_ci_prefix("clock", "CL", 2));
    CU_ASSERT_TRUE(shell_ci_prefix("clock", "clock", 5));
    CU_ASSERT_TRUE(shell_ci_prefix("clock", "", 0));
    CU_ASSERT_FALSE(shell_ci_prefix("clock", "clocks", 6));
    CU_ASSERT_FALSE(shell_ci_prefix("cat", "d", 1));
}

static void test_group_names(void)
{
    for (int g = 0; g < SHELL_GRP_COUNT; g++) {
        const char *name = shell_group_name((shell_group_t)g);
        size_t len = 0;
        while (name[len] != '\0') {
            len++;
        }
        shell_group_t back = SHELL_GRP_COUNT;
        CU_ASSERT_TRUE(shell_group_parse(name, len, &back));
        CU_ASSERT_EQUAL(back, (shell_group_t)g);
    }
    shell_group_t g;
    CU_ASSERT_TRUE(shell_group_parse("fs", 2, &g));
    CU_ASSERT_EQUAL(g, SHELL_GRP_FS);
    CU_ASSERT_FALSE(shell_group_parse("f", 1, &g));
    CU_ASSERT_STRING_EQUAL(shell_group_name(SHELL_GRP_COUNT), "?");
}

static void test_query_parse_kinds(void)
{
    CU_ASSERT_EQUAL(shell_help_query_parse("").kind, SHELL_HELP_Q_INDEX);
    CU_ASSERT_EQUAL(shell_help_query_parse("   ").kind, SHELL_HELP_Q_INDEX);
    CU_ASSERT_EQUAL(shell_help_query_parse("all").kind, SHELL_HELP_Q_ALL);
    CU_ASSERT_EQUAL(shell_help_query_parse("ALL").kind, SHELL_HELP_Q_ALL);
    CU_ASSERT_EQUAL(shell_help_query_parse("Fs").kind, SHELL_HELP_Q_GROUP);
    CU_ASSERT_EQUAL(shell_help_query_parse("cat").kind, SHELL_HELP_Q_WORD);
    CU_ASSERT_EQUAL(shell_help_query_parse("c*").kind, SHELL_HELP_Q_PREFIX);
    /* A trailing '*' always means a prefix, even on a group name. */
    CU_ASSERT_EQUAL(shell_help_query_parse("disk*").kind, SHELL_HELP_Q_PREFIX);
}

static void test_query_parse_word(void)
{
    shell_help_query_t q = shell_help_query_parse("cl* extra");
    CU_ASSERT_EQUAL(q.kind, SHELL_HELP_Q_PREFIX);
    CU_ASSERT_EQUAL(q.word_len, 2);
    CU_ASSERT_TRUE(shell_ci_token_eq(q.word, q.word_len, "cl"));

    q = shell_help_query_parse("disk");
    CU_ASSERT_EQUAL(q.kind, SHELL_HELP_Q_GROUP);
    CU_ASSERT_EQUAL(q.group, SHELL_GRP_DISK);

    q = shell_help_query_parse("*");
    CU_ASSERT_EQUAL(q.kind, SHELL_HELP_Q_PREFIX);
    CU_ASSERT_EQUAL(q.word_len, 0);
}

static void test_prefix_matches(void)
{
    /* cat, cd, clear, clock, cp = fixture rows 1..5 */
    CU_ASSERT_EQUAL(match_mask("c"), 0x3Eu);
    CU_ASSERT_EQUAL(match_mask("c*"), 0x3Eu);
    CU_ASSERT_EQUAL(match_mask("C*"), 0x3Eu);
    CU_ASSERT_EQUAL(match_mask("cl"), 0x18u);
    CU_ASSERT_EQUAL(match_mask("zz"), 0u);
    CU_ASSERT_EQUAL(match_mask("*"), (1u << FIXTURE_N) - 1u);
}

static void test_group_and_all_matches(void)
{
    CU_ASSERT_EQUAL(match_mask("all"), (1u << FIXTURE_N) - 1u);
    CU_ASSERT_EQUAL(match_mask("fs"), 0x26u);     /* cat, cd, cp */
    CU_ASSERT_EQUAL(match_mask("disk"), 0xC0u);   /* dd, disk */
    CU_ASSERT_EQUAL(match_mask("apps"), 0x110u);  /* clock, doom */
    CU_ASSERT_EQUAL(match_mask("system"), 0x09u); /* about, clear */
    CU_ASSERT_EQUAL(match_mask(""), 0u);          /* index lists nothing */
}

static void test_format_row(void)
{
    char buf[64];
    size_t n = shell_help_format_row(&fixture[1], buf, sizeof(buf));
    CU_ASSERT_STRING_EQUAL(buf, "CAT       FS      Cat\n");
    CU_ASSERT_EQUAL(n, 22);

    n = shell_help_format_row(&fixture[0], buf, sizeof(buf));
    CU_ASSERT_STRING_EQUAL(buf, "ABOUT     SYSTEM  About\n");
}

static void test_format_row_long_name(void)
{
    static const shell_help_entry_t e = {
        "abcdefghijkl", SHELL_GRP_SYSTEM, "x", ""
    };
    char buf[64];
    shell_help_format_row(&e, buf, sizeof(buf));
    /* A name past its column still gets one separating space. */
    CU_ASSERT_STRING_EQUAL(buf, "ABCDEFGHIJKL SYSTEM  x\n");
}

static void test_format_row_truncates(void)
{
    char buf[8];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = 'Z';
    }
    size_t n = shell_help_format_row(&fixture[1], buf, 6);
    CU_ASSERT_EQUAL(n, 5);
    CU_ASSERT_STRING_EQUAL(buf, "CAT  ");
    CU_ASSERT_EQUAL(buf[6], 'Z'); /* nothing written past len */

    CU_ASSERT_EQUAL(shell_help_format_row(&fixture[1], buf, 0), 0);
    CU_ASSERT_EQUAL(buf[0], 'C'); /* len 0 writes nothing at all */
}

void suite_shell_help_tests(CU_pSuite s)
{
    CU_add_test(s, "case-insensitive token equality", test_ci_token_eq);
    CU_add_test(s, "case-insensitive prefix", test_ci_prefix);
    CU_add_test(s, "group names round-trip", test_group_names);
    CU_add_test(s, "query parse: kinds", test_query_parse_kinds);
    CU_add_test(s, "query parse: word and star", test_query_parse_word);
    CU_add_test(s, "prefix matching", test_prefix_matches);
    CU_add_test(s, "group and all matching", test_group_and_all_matches);
    CU_add_test(s, "row format", test_format_row);
    CU_add_test(s, "row format: long name", test_format_row_long_name);
    CU_add_test(s, "row format: truncation", test_format_row_truncates);
}
