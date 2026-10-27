/*
 * test_doom_args_k.c — Doom's argv from the launch params (SCRUM-301).
 *
 * src/doom_args.c is pure string handling in src/, so the kernel build links
 * it and these tests drive it from ring 0.
 */

#include "kunit.h"
#include "doom_args.h"
#include "doomgeneric_exo.h"

#include <stddef.h>
#include <stdint.h>

#define TEXT_MAX 64u
#define SLOTS    DOOM_ARGV_SLOTS(TEXT_MAX)

static int streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int split(const char *text, char **argv, char *store)
{
    return doom_args_split("exodoom", text, TEXT_MAX, store, TEXT_MAX + 1,
                           argv, SLOTS);
}

/* The params buffer as the kernel leaves it: the text, then zeros. */
static void load(char *buf, const char *text)
{
    uint32_t i = 0;
    for (; text[i] != '\0'; i++) {
        buf[i] = text[i];
    }
    for (; i < TEXT_MAX; i++) {
        buf[i] = '\0';
    }
}

static void test_no_arguments_is_just_the_program_name(void)
{
    char text[TEXT_MAX], store[TEXT_MAX + 1], *argv[SLOTS];
    load(text, "");
    CU_ASSERT_EQUAL(split(text, argv, store), 1);
    CU_ASSERT_TRUE(streq(argv[0], "exodoom"));
    CU_ASSERT_PTR_NULL(argv[1]);

    /* The unlaunched default is a single space. */
    load(text, " ");
    CU_ASSERT_EQUAL(split(text, argv, store), 1);
    CU_ASSERT_PTR_NULL(argv[1]);
}

static void test_tokens_split_on_spaces(void)
{
    char text[TEXT_MAX], store[TEXT_MAX + 1], *argv[SLOTS];
    load(text, "-iwad doom2.wad");
    CU_ASSERT_EQUAL(split(text, argv, store), 3);
    CU_ASSERT_TRUE(streq(argv[0], "exodoom"));
    CU_ASSERT_TRUE(streq(argv[1], "-iwad"));
    CU_ASSERT_TRUE(streq(argv[2], "doom2.wad"));
    CU_ASSERT_PTR_NULL(argv[3]);
}

static void test_extra_spaces_make_no_empty_tokens(void)
{
    char text[TEXT_MAX], store[TEXT_MAX + 1], *argv[SLOTS];
    load(text, "  -warp   1  2 ");
    CU_ASSERT_EQUAL(split(text, argv, store), 4);
    CU_ASSERT_TRUE(streq(argv[1], "-warp"));
    CU_ASSERT_TRUE(streq(argv[2], "1"));
    CU_ASSERT_TRUE(streq(argv[3], "2"));
    CU_ASSERT_PTR_NULL(argv[4]);
}

static void test_split_leaves_the_text_alone(void)
{
    char text[TEXT_MAX], store[TEXT_MAX + 1], *argv[SLOTS];
    load(text, "-iwad doom2.wad");
    split(text, argv, store);
    CU_ASSERT_TRUE(streq(text, "-iwad doom2.wad"));
}

static void test_as_many_tokens_as_the_text_can_hold_all_fit(void)
{
    /* "a a a ..." filling the whole buffer: the worst case DOOM_ARGV_SLOTS
     * promises room for. */
    char text[TEXT_MAX], store[TEXT_MAX + 1], *argv[SLOTS];
    for (uint32_t i = 0; i < TEXT_MAX; i++) {
        text[i] = (i % 2 == 0) ? 'a' : ' ';
    }
    CU_ASSERT_EQUAL(split(text, argv, store), (int)(TEXT_MAX / 2) + 1);
    CU_ASSERT_PTR_NULL(argv[TEXT_MAX / 2 + 1]);
}

static void test_unterminated_text_is_read_only_to_its_length(void)
{
    char text[4] = { 'a', 'b', ' ', 'c' }; /* no NUL anywhere */
    char store[5], *argv[DOOM_ARGV_SLOTS(4)];
    CU_ASSERT_EQUAL(doom_args_split("exodoom", text, 4, store, sizeof(store),
                                    argv, DOOM_ARGV_SLOTS(4)), 3);
    CU_ASSERT_TRUE(streq(argv[1], "ab"));
    CU_ASSERT_TRUE(streq(argv[2], "c"));
}

static void test_too_small_argv_or_store_is_an_error_not_an_overrun(void)
{
    char text[TEXT_MAX], store[TEXT_MAX + 1], *argv[SLOTS];
    load(text, "-iwad doom2.wad");

    CU_ASSERT_EQUAL(doom_args_split("exodoom", text, TEXT_MAX, store,
                                    sizeof(store), argv, 3), -1);
    CU_ASSERT_TRUE(streq(argv[0], "exodoom"));
    CU_ASSERT_PTR_NULL(argv[1]);

    CU_ASSERT_EQUAL(doom_args_split("exodoom", text, TEXT_MAX, store, 8,
                                    argv, SLOTS), -1);
    CU_ASSERT_TRUE(streq(argv[0], "exodoom"));
    CU_ASSERT_PTR_NULL(argv[2]);
}

static void test_value_finds_the_token_after_a_flag(void)
{
    char text[TEXT_MAX], out[32];
    load(text, "-skill 4 -iwad  hacx.wad -warp 1");
    CU_ASSERT_EQUAL(doom_args_value(text, TEXT_MAX, "-iwad", out, sizeof(out)), 1);
    CU_ASSERT_TRUE(streq(out, "hacx.wad"));
    CU_ASSERT_EQUAL(doom_args_value(text, TEXT_MAX, "-skill", out, sizeof(out)), 1);
    CU_ASSERT_TRUE(streq(out, "4"));
}

static void test_value_misses_leave_out_empty(void)
{
    char text[TEXT_MAX], out[32];
    out[0] = 'x';

    load(text, "-skill 4");
    CU_ASSERT_EQUAL(doom_args_value(text, TEXT_MAX, "-iwad", out, sizeof(out)), 0);
    CU_ASSERT_EQUAL(out[0], '\0');

    /* A flag with nothing after it, and a prefix that is not the flag. */
    load(text, "-iwad");
    CU_ASSERT_EQUAL(doom_args_value(text, TEXT_MAX, "-iwad", out, sizeof(out)), 0);
    load(text, "-iwadx foo.wad");
    CU_ASSERT_EQUAL(doom_args_value(text, TEXT_MAX, "-iwad", out, sizeof(out)), 0);
    load(text, "-iw foo.wad");
    CU_ASSERT_EQUAL(doom_args_value(text, TEXT_MAX, "-iwad", out, sizeof(out)), 0);

    /* A value that does not fit is a miss, never a cut-off name. */
    load(text, "-iwad doom2.wad");
    CU_ASSERT_EQUAL(doom_args_value(text, TEXT_MAX, "-iwad", out, 9), 0);
    CU_ASSERT_EQUAL(doom_args_value(text, TEXT_MAX, "-iwad", out, 10), 1);
}

static void test_the_launch_buffer_starts_empty(void)
{
    /* Nothing patched it in a kernel test: DG_Init must see no -iwad. */
    char out[16];
    CU_ASSERT_EQUAL(doom_args_value(dg_launch_args(), DG_LAUNCH_ARGS_MAX,
                                    "-iwad", out, sizeof(out)), 0);
}

void suite_doom_args_tests(CU_pSuite s)
{
    CU_add_test(s, "no arguments is just the program name",
                test_no_arguments_is_just_the_program_name);
    CU_add_test(s, "tokens split on spaces", test_tokens_split_on_spaces);
    CU_add_test(s, "extra spaces make no empty tokens",
                test_extra_spaces_make_no_empty_tokens);
    CU_add_test(s, "splitting leaves the text alone",
                test_split_leaves_the_text_alone);
    CU_add_test(s, "as many tokens as the text can hold all fit",
                test_as_many_tokens_as_the_text_can_hold_all_fit);
    CU_add_test(s, "unterminated text is read only to its length",
                test_unterminated_text_is_read_only_to_its_length);
    CU_add_test(s, "a too-small argv or store is an error, not an overrun",
                test_too_small_argv_or_store_is_an_error_not_an_overrun);
    CU_add_test(s, "value finds the token after a flag",
                test_value_finds_the_token_after_a_flag);
    CU_add_test(s, "value misses leave the output empty",
                test_value_misses_leave_out_empty);
    CU_add_test(s, "the launch buffer starts without an -iwad",
                test_the_launch_buffer_starts_empty);
}
