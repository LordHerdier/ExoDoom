/*
 * test_libc_gaps_k.c — SCRUM-65's tests for the libc surface Doom needs.
 *
 * Organised around docs/libc_audit.md rather than around the source files,
 * because that is where the requirement came from: the audit derived the
 * exact list of 52 external symbols from `nm` over the compiled engine, and
 * these tests cover the ones SCRUM-65 had to supply.
 *
 * Priority follows the audit's own §6 ordering, which is by what unblocks
 * the most rather than by call count -- so the precision tests come first
 * and are the longest, because that is the one gap the audit calls out as
 * stopping the game from starting at all.
 */

#include "kunit.h"

#include "errno.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* ---- printf precision: the audit's highest-severity finding ------------ */

static void test_precision_builds_doom_lump_names(void)
{
    /*
     * THE case from docs/libc_audit.md §3.1.
     *
     * hu_stuff.c:297 builds every HUD font lump name with
     * DEH_snprintf(buffer, 9, "STCFN%.3d", c). Before SCRUM-65 kvprintf had
     * no precision, so "%.3d" fell to the default: branch, which echoed the
     * characters verbatim AND DID NOT CONSUME THE VARARG -- the name came out
     * as "STCFN%.3" for every glyph, W_CacheLumpName found no such lump, and
     * Doom died in I_Error before the HUD ever drew.
     *
     * 33 is the first character HU_Init asks for ('!'), so "STCFN033" is
     * literally the first lump name the game looks up.
     */
    char buf[16];

    snprintf(buf, sizeof buf, "STCFN%.3d", 33);
    CU_ASSERT_STRING_EQUAL(buf, "STCFN033");

    snprintf(buf, sizeof buf, "STCFN%.3d", 122);
    CU_ASSERT_STRING_EQUAL(buf, "STCFN122");

    /* wi_stuff.c:1564 -- intermission level-name patches. Width AND
     * precision together, which is the form that exercises both paddings. */
    snprintf(buf, sizeof buf, "CWILV%2.2d", 0);
    CU_ASSERT_STRING_EQUAL(buf, "CWILV00");

    /* wi_stuff.c:1596 -- intermission animation lumps. */
    snprintf(buf, sizeof buf, "WIA%d%.2d%.2d", 0, 1, 7);
    CU_ASSERT_STRING_EQUAL(buf, "WIA00107");
}

static void test_precision_does_not_desync_the_arguments(void)
{
    /*
     * The half of §3.1 that is worse than the wrong text: an unconsumed
     * vararg shifts every LATER conversion in the same call. A wrong level
     * name is survivable; a shifted argument list is not.
     *
     * So this checks a precision conversion followed by more arguments --
     * if "%.3d" ever stops consuming its int again, the %s and %d after it
     * read the wrong slots and this fails loudly rather than subtly.
     */
    char buf[64];

    snprintf(buf, sizeof buf, "%.3d|%s|%d", 7, "tail", 42);
    CU_ASSERT_STRING_EQUAL(buf, "007|tail|42");

    snprintf(buf, sizeof buf, "%.2d %.2d %.2d", 1, 2, 3);
    CU_ASSERT_STRING_EQUAL(buf, "01 02 03");
}

static void test_precision_on_strings_truncates(void)
{
    /*
     * For %s the precision is a MAXIMUM, not a minimum -- the opposite of
     * what it means for %d. m_misc.c:226 uses "%.8s" to clip an 8-byte lump
     * name (which is NOT NUL-terminated in the WAD directory) into a warning
     * message, so reading past it is a real out-of-bounds read, not just
     * wrong output.
     */
    char buf[64];

    snprintf(buf, sizeof buf, "%.8s", "MAP01LONGNAME");
    CU_ASSERT_STRING_EQUAL(buf, "MAP01LON");

    /* A precision longer than the string changes nothing. */
    snprintf(buf, sizeof buf, "%.20s", "short");
    CU_ASSERT_STRING_EQUAL(buf, "short");

    /* Width and precision together: pad to 10, clip to 3. */
    snprintf(buf, sizeof buf, "[%10.3s]", "abcdefgh");
    CU_ASSERT_STRING_EQUAL(buf, "[       abc]");
}

static void test_precision_edge_cases(void)
{
    char buf[64];

    /* C99: precision 0 applied to a value of 0 produces NO characters. */
    snprintf(buf, sizeof buf, "[%.0d]", 0);
    CU_ASSERT_STRING_EQUAL(buf, "[]");

    /* ...but a non-zero value still prints. */
    snprintf(buf, sizeof buf, "[%.0d]", 5);
    CU_ASSERT_STRING_EQUAL(buf, "[5]");

    /* An explicit precision disables zero-padding (C99 7.19.6.1p6) -- both
     * would otherwise pad and the field would be twice as wide. */
    snprintf(buf, sizeof buf, "[%08.3d]", 7);
    CU_ASSERT_STRING_EQUAL(buf, "[     007]");

    /* Precision wider than the number pads with zeros, not spaces. */
    snprintf(buf, sizeof buf, "%.6d", -42);
    CU_ASSERT_STRING_EQUAL(buf, "-000042");
}

/* ---- the rest of the conversions SCRUM-65 added ------------------------ */

static void test_new_conversions(void)
{
    char buf[64];

    /* %X and %o, neither of which existed before. */
    snprintf(buf, sizeof buf, "%X", 0xDEADu);
    CU_ASSERT_STRING_EQUAL(buf, "DEAD");
    snprintf(buf, sizeof buf, "%x", 0xDEADu);
    CU_ASSERT_STRING_EQUAL(buf, "dead");
    snprintf(buf, sizeof buf, "%o", 8u);
    CU_ASSERT_STRING_EQUAL(buf, "10");

    /* '#' alternate form. */
    snprintf(buf, sizeof buf, "%#x", 255u);
    CU_ASSERT_STRING_EQUAL(buf, "0xff");

    /* '+' and ' ' sign flags. */
    snprintf(buf, sizeof buf, "%+d/% d", 5, 5);
    CU_ASSERT_STRING_EQUAL(buf, "+5/ 5");

    /* Length modifiers. Doom writes %li in a handful of places; the reason
     * they matter is the same as precision's -- an unparsed modifier used to
     * reach default: and leave the argument unconsumed. */
    snprintf(buf, sizeof buf, "%ld", (long)-1234567890L);
    CU_ASSERT_STRING_EQUAL(buf, "-1234567890");
    snprintf(buf, sizeof buf, "%lu", (unsigned long)4294967295UL);
    CU_ASSERT_STRING_EQUAL(buf, "4294967295");
    snprintf(buf, sizeof buf, "%zu", (size_t)12345);
    CU_ASSERT_STRING_EQUAL(buf, "12345");

    /* '*' width and precision taken from the argument list. */
    snprintf(buf, sizeof buf, "[%*d]", 6, 42);
    CU_ASSERT_STRING_EQUAL(buf, "[    42]");
    snprintf(buf, sizeof buf, "[%.*s]", 3, "abcdef");
    CU_ASSERT_STRING_EQUAL(buf, "[abc]");
}

/* ---- snprintf's contract ---------------------------------------------- */

static void test_snprintf_return_and_truncation(void)
{
    char buf[8];
    int  n;

    /* Returns what it WOULD have written, not what it did -- m_misc.c's
     * M_StringJoin sizes its allocation from exactly this number, so a
     * return clamped to the buffer would make every such allocation short. */
    n = snprintf(buf, sizeof buf, "0123456789");
    CU_ASSERT_EQUAL(n, 10);
    CU_ASSERT_STRING_EQUAL(buf, "0123456");  /* 7 chars + NUL */

    /* Exact fit. */
    n = snprintf(buf, sizeof buf, "1234567");
    CU_ASSERT_EQUAL(n, 7);
    CU_ASSERT_STRING_EQUAL(buf, "1234567");

    /* size 0 must write nothing at all and may be handed a NULL buffer --
     * the "how long would this be?" idiom. */
    n = snprintf(0, 0, "abc%d", 42);
    CU_ASSERT_EQUAL(n, 5);
}

/* ---- the four string functions ---------------------------------------- */

static void test_strncmp(void)
{
    CU_ASSERT_EQUAL(strncmp("abc", "abc", 3), 0);
    CU_ASSERT_EQUAL(strncmp("abc", "abd", 2), 0);   /* stops before the diff */
    CU_ASSERT_TRUE(strncmp("abc", "abd", 3) < 0);
    CU_ASSERT_TRUE(strncmp("abd", "abc", 3) > 0);
    CU_ASSERT_EQUAL(strncmp("", "", 5), 0);
    /* n == 0 compares nothing and is always equal. */
    CU_ASSERT_EQUAL(strncmp("x", "y", 0), 0);
    /* Stops at a NUL even with n left over. */
    CU_ASSERT_EQUAL(strncmp("ab", "ab", 10), 0);
}

static void test_strrchr(void)
{
    const char *path = "/usr/share/doom/freedoom2.wad";

    CU_ASSERT_PTR_NOT_NULL(strrchr(path, '/'));
    CU_ASSERT_STRING_EQUAL(strrchr(path, '/'), "/freedoom2.wad");
    CU_ASSERT_STRING_EQUAL(strrchr(path, '.'), ".wad");
    CU_ASSERT_PTR_NULL(strrchr(path, 'Z'));

    /* The terminator counts as part of the string: strrchr(s, 0) points at
     * it. Getting this wrong is the classic strrchr bug, and a `while (*s)`
     * loop gets it wrong. */
    CU_ASSERT_PTR_NOT_NULL(strrchr("abc", '\0'));
    CU_ASSERT_EQUAL(*strrchr("abc", '\0'), '\0');
}

static void test_strstr(void)
{
    const char *hay = "MAP01 VERTEXES LINEDEFS";

    CU_ASSERT_STRING_EQUAL(strstr(hay, "VERTEXES"), "VERTEXES LINEDEFS");
    CU_ASSERT_PTR_NULL(strstr(hay, "SECTORS"));
    /* The empty needle matches at position 0 -- standard, and easy to get
     * wrong by accident rather than on purpose. */
    CU_ASSERT_TRUE(strstr(hay, "") == hay);
    /* A partial match that then fails must not consume the restart point. */
    CU_ASSERT_STRING_EQUAL(strstr("aaab", "aab"), "aab");
}

static void test_strdup(void)
{
    const char *src = "freedoom2.wad";
    char       *dup = strdup(src);

    CU_ASSERT_PTR_NOT_NULL(dup);
    if (dup != 0) {
        CU_ASSERT_STRING_EQUAL(dup, src);
        /* A copy, not an alias -- the point of the function. */
        CU_ASSERT_TRUE(dup != src);
        dup[0] = 'F';
        CU_ASSERT_EQUAL(src[0], 'f');
        free(dup);
    }

    dup = strdup("");
    CU_ASSERT_PTR_NOT_NULL(dup);
    if (dup != 0) {
        CU_ASSERT_EQUAL(dup[0], '\0');
        free(dup);
    }
}

/* ---- calloc ----------------------------------------------------------- */

static void test_calloc(void)
{
    unsigned char *p = calloc(16, 4);
    int            i;
    int            all_zero = 1;

    CU_ASSERT_PTR_NOT_NULL(p);
    if (p != 0) {
        for (i = 0; i < 64; i++) {
            if (p[i] != 0) {
                all_zero = 0;
            }
        }
        CU_ASSERT_TRUE(all_zero);
        free(p);
    }

    /*
     * The overflow guard, which is the only reason calloc exists as
     * something other than malloc+memset. nmemb * size wrapping would
     * produce a small allocation that the caller then writes nmemb*size
     * bytes into -- a heap overflow driven by arithmetic, not by a bad
     * length check.
     */
    CU_ASSERT_PTR_NULL(calloc((size_t)-1 / 2, 4));
}

/* ---- the FILE* layer --------------------------------------------------- */

/* A WAD-shaped blob: "IWAD", 0 lumps, directory at 12. Content is arbitrary;
 * these tests are about the stream, not the parse. */
static const unsigned char blob_bytes[] = {
    'I', 'W', 'A', 'D', 0, 0, 0, 0, 12, 0, 0, 0,
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'
};

/* Same shape, different payload, so a test can tell which of two
 * registrations under the same name actually won. */
static const unsigned char blob_bytes_alt[] = {
    'I', 'W', 'A', 'D', 0, 0, 0, 0, 12, 0, 0, 0,
    'Z', 'Y', 'X', 'W', 'V', 'U', 'T', 'S'
};

static void test_fopen_finds_a_registered_blob(void)
{
    FILE *f;

    exo_file_reset_blobs();
    CU_ASSERT_EQUAL(exo_file_register_blob("freedoom2.wad", blob_bytes,
                                           sizeof blob_bytes), 0);

    /*
     * Matched on the basename, not the whole path. Doom builds WAD paths by
     * joining a directory it discovered (d_iwad.c) onto a filename, so the
     * string reaching fopen is a path while what was registered is a name --
     * matching the tail is what lets the two meet without the registrar
     * having to predict the path Doom will invent.
     */
    f = fopen("/usr/share/doom/freedoom2.wad", "rb");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != 0) {
        fclose(f);
    }

    /* Bare name works too. */
    f = fopen("freedoom2.wad", "r");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f != 0) {
        fclose(f);
    }

    /* An unregistered name is ENOENT, not a crash. */
    CU_ASSERT_PTR_NULL(fopen("doom2.wad", "r"));

    exo_file_reset_blobs();
}

static void test_fopen_refuses_to_write(void)
{
    exo_file_reset_blobs();
    exo_file_register_blob("freedoom2.wad", blob_bytes, sizeof blob_bytes);

    /*
     * A write mode fails, and failing is the point. Doom writes a config
     * file and savegames; a successful-looking write that went nowhere would
     * surface much later as a config that never persists or a savegame that
     * reloads as garbage. Returning NULL puts the failure at the open, where
     * M_SaveDefaults and P_SaveGame already handle it.
     */
    CU_ASSERT_PTR_NULL(fopen("freedoom2.wad", "w"));
    CU_ASSERT_PTR_NULL(fopen("freedoom2.wad", "wb"));
    CU_ASSERT_PTR_NULL(fopen("freedoom2.wad", "a"));

    exo_file_reset_blobs();
}

static void test_fread_fseek_ftell(void)
{
    FILE         *f;
    unsigned char buf[8];
    size_t        got;

    exo_file_reset_blobs();
    exo_file_register_blob("test.wad", blob_bytes, sizeof blob_bytes);

    f = fopen("test.wad", "rb");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f == 0) {
        return;
    }

    CU_ASSERT_EQUAL(ftell(f), 0L);

    got = fread(buf, 1, 4, f);
    CU_ASSERT_EQUAL(got, 4u);
    CU_ASSERT_EQUAL(buf[0], 'I');
    CU_ASSERT_EQUAL(buf[3], 'D');
    CU_ASSERT_EQUAL(ftell(f), 4L);

    /* This is exactly W_StdC_Read's access pattern: seek absolute, then
     * read. */
    CU_ASSERT_EQUAL(fseek(f, 12, SEEK_SET), 0);
    got = fread(buf, 1, 4, f);
    CU_ASSERT_EQUAL(got, 4u);
    CU_ASSERT_EQUAL(buf[0], 'A');
    CU_ASSERT_EQUAL(buf[3], 'D');

    CU_ASSERT_EQUAL(fseek(f, -2, SEEK_CUR), 0);
    CU_ASSERT_EQUAL(ftell(f), 14L);

    CU_ASSERT_EQUAL(fseek(f, 0, SEEK_END), 0);
    CU_ASSERT_EQUAL(ftell(f), (long)sizeof blob_bytes);

    /* Seeking outside the blob is refused -- which is also what stops a
     * later fread from computing a negative available length. */
    CU_ASSERT_EQUAL(fseek(f, 1, SEEK_END), -1);
    CU_ASSERT_EQUAL(fseek(f, -1, SEEK_SET), -1);

    fclose(f);
    exo_file_reset_blobs();
}

static void test_fread_counts_elements_and_flags_eof(void)
{
    FILE         *f;
    unsigned char buf[32];

    exo_file_reset_blobs();
    exo_file_register_blob("test.wad", blob_bytes, sizeof blob_bytes);

    f = fopen("test.wad", "rb");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f == 0) {
        return;
    }

    CU_ASSERT_FALSE(feof(f));

    /*
     * fread returns whole ELEMENTS, not bytes. 20 bytes of blob asked for as
     * 8-byte elements is 2 complete ones with 4 bytes left over -- a partial
     * final element is not counted. Returning 3 (or 20) would make a short
     * read look complete.
     */
    CU_ASSERT_EQUAL(fread(buf, 8, 4, f), 2u);
    CU_ASSERT_TRUE(feof(f) != 0);

    /* A successful seek clears EOF, per C99 -- W_StdC_Read seeks and reads
     * in a loop, and a sticky EOF would stop the second read. */
    CU_ASSERT_EQUAL(fseek(f, 0, SEEK_SET), 0);
    CU_ASSERT_FALSE(feof(f));

    fclose(f);
    exo_file_reset_blobs();
}

static void test_stdout_stderr_exist_and_survive_fclose(void)
{
    CU_ASSERT_PTR_NOT_NULL(stdout);
    CU_ASSERT_PTR_NOT_NULL(stderr);

    /* fprintf to the console returns the character count, like printf. */
    CU_ASSERT_EQUAL(fprintf(stderr, "[test] %d\n", 42), 10);

    /*
     * Closing them must be harmless: Doom's I_Quit path can reach fclose on a
     * stream it did not open, and these two are static objects rather than
     * table entries. Closing the underlying storage would take the
     * diagnostic channel down mid-shutdown.
     */
    CU_ASSERT_EQUAL(fclose(stderr), 0);
    CU_ASSERT_EQUAL(fprintf(stderr, ""), 0);
    CU_ASSERT_PTR_NOT_NULL(stderr);
}

static void test_filesystem_absent_functions_say_so(void)
{
    /*
     * remove/rename/mkdir all fail, rather than returning 0 and claiming
     * something happened. docs/libc_audit.md §6 suggests mkdir could be a
     * no-op returning 0; this does not, because M_MakeDirectory is called
     * before writing into that directory and a successful-looking mkdir just
     * moves the failure one step further from its cause.
     */
    CU_ASSERT_EQUAL(remove("/tmp/x"), -1);
    CU_ASSERT_EQUAL(rename("/tmp/x", "/tmp/y"), -1);

    /* system() reports "no shell" -- 0 for the NULL probe (there is no
     * interpreter), -1 for an actual command (it cannot be run). */
    CU_ASSERT_EQUAL(system(0), 0);
    CU_ASSERT_EQUAL(system("/usr/bin/zenity --help"), -1);
}

/* ---- sscanf ------------------------------------------------------------ */

static void test_sscanf_m_strtoint_forms(void)
{
    /*
     * The four probes m_misc.c:192-195's M_StrToInt makes, in order. They
     * are tried in sequence until one returns 1, so each has to fail on
     * input meant for a later one -- getting that wrong makes every hex
     * value parse as a decimal 0.
     */
    int v;

    CU_ASSERT_EQUAL(sscanf(" 0x1f", " 0x%x", &v), 1);
    CU_ASSERT_EQUAL(v, 31);

    CU_ASSERT_EQUAL(sscanf(" 0X1F", " 0X%x", &v), 1);
    CU_ASSERT_EQUAL(v, 31);

    CU_ASSERT_EQUAL(sscanf(" 017", " 0%o", &v), 1);
    CU_ASSERT_EQUAL(v, 15);

    CU_ASSERT_EQUAL(sscanf(" -42", " %d", &v), 1);
    CU_ASSERT_EQUAL(v, -42);

    /* m_config.c:1721/1723 -- a hex default, then the %i fallback whose
     * auto-detection has to recognise the 0x prefix by itself. */
    CU_ASSERT_EQUAL(sscanf("ff", "%x", &v), 1);
    CU_ASSERT_EQUAL(v, 255);
    CU_ASSERT_EQUAL(sscanf("0x20", "%i", &v), 1);
    CU_ASSERT_EQUAL(v, 32);
    CU_ASSERT_EQUAL(sscanf("020", "%i", &v), 1);
    CU_ASSERT_EQUAL(v, 16);   /* leading 0 means octal */
    CU_ASSERT_EQUAL(sscanf("20", "%i", &v), 1);
    CU_ASSERT_EQUAL(v, 20);
}

static void test_sscanf_reports_what_it_assigned(void)
{
    int  a = -1, b = -1;
    char word[16];

    CU_ASSERT_EQUAL(sscanf("10 20", "%d %d", &a, &b), 2);
    CU_ASSERT_EQUAL(a, 10);
    CU_ASSERT_EQUAL(b, 20);

    /* A matching failure stops the scan and reports only what was assigned
     * before it -- b must be left alone. */
    b = -1;
    CU_ASSERT_EQUAL(sscanf("10 xx", "%d %d", &a, &b), 1);
    CU_ASSERT_EQUAL(b, -1);

    /* EOF, not 0, when the input ran out before any conversion. m_config.c's
     * read loop uses exactly that distinction as its terminator. */
    CU_ASSERT_EQUAL(sscanf("", "%d", &a), EOF);

    /* %s stops at whitespace and terminates; width bounds it. */
    CU_ASSERT_EQUAL(sscanf("mouse_speed 12", "%15s", word), 1);
    CU_ASSERT_STRING_EQUAL(word, "mouse_speed");

    CU_ASSERT_EQUAL(sscanf("abcdefgh", "%3s", word), 1);
    CU_ASSERT_STRING_EQUAL(word, "abc");

    /* Assignment suppression consumes without storing. */
    a = -1;
    CU_ASSERT_EQUAL(sscanf("99 7", "%*d %d", &a), 1);
    CU_ASSERT_EQUAL(a, 7);
}

static void test_register_blob_replace_matches_lookup(void)
{
    FILE         *f;
    unsigned char buf[8];
    size_t        got;

    exo_file_reset_blobs();

    CU_ASSERT_EQUAL(exo_file_register_blob("freedoom2.wad", blob_bytes,
                                           sizeof blob_bytes), 0);

    /*
     * The same name in different case must REPLACE the slot, not add a
     * second one.
     *
     * find_blob() matches case-insensitively, so a shadowing second slot
     * would be exactly as matchable as the first, and which one a later
     * fopen returned would come down to slot order -- the replace-in-place
     * contract the registration loop exists to provide would silently not
     * hold. Registering with strcmp while looking up with strcasecmp gave
     * the two functions different notions of "the same name"; this is what
     * pins them together.
     */
    CU_ASSERT_EQUAL(exo_file_register_blob("FreeDoom2.WAD", blob_bytes_alt,
                                           sizeof blob_bytes_alt), 0);

    /* The replacement's payload is what a read returns -- which is the
     * observable half of "replaced, not shadowed". */
    f = fopen("/some/where/FREEDOOM2.WAD", "rb");
    CU_ASSERT_PTR_NOT_NULL(f);
    if (f == 0) {
        exo_file_reset_blobs();
        return;
    }

    CU_ASSERT_EQUAL(fseek(f, 12, SEEK_SET), 0);
    got = fread(buf, 1, 4, f);
    CU_ASSERT_EQUAL(got, 4u);
    CU_ASSERT_EQUAL(buf[0], 'Z');
    CU_ASSERT_EQUAL(buf[3], 'W');

    fclose(f);
    exo_file_reset_blobs();
}

static void test_sscanf_float_honours_field_width(void)
{
    char word[16];

    /*
     * The float conversion reads at most `width` characters, the way every
     * other conversion here already did (C99 7.21.6.2p3).
     *
     * This asserts on the SCAN POSITION rather than on a converted value,
     * because it has to: this TU is compiled -mno-sse, so store_double_bits
     * writes nothing at all in a kernel build (src/stdio.c) and there is no
     * double to inspect. The position is the half that could silently go
     * wrong anyway -- exo_parse_f64 stops only at a character that cannot
     * continue a number, so a width the conversion ignored would not fail,
     * it would consume more input and hand the NEXT conversion a tail
     * beginning somewhere it does not expect. The value side is covered by
     * test_fpconv_k.c, which drives exo_parse_f64 directly on bit patterns.
     *
     * Assignment suppression (%*Nf) is what makes that testable without a
     * double destination at all.
     */
    word[0] = '\0';
    CU_ASSERT_EQUAL(sscanf("12.5", "%*2f%s", word), 1);
    CU_ASSERT_STRING_EQUAL(word, ".5");

    /*
     * The exponent case, where ignoring width does the most damage: "1.5e3"
     * is a single token to the parser, so a %3f has to stop after "1.5" and
     * leave "e3" behind rather than consume a value a thousand times larger
     * and report success either way.
     */
    word[0] = '\0';
    CU_ASSERT_EQUAL(sscanf("1.5e3", "%*3f%s", word), 1);
    CU_ASSERT_STRING_EQUAL(word, "e3");

    /* No width is still unbounded -- the pre-existing behaviour, unchanged. */
    word[0] = '\0';
    CU_ASSERT_EQUAL(sscanf("1.5e3 tail", "%*f %s", word), 1);
    CU_ASSERT_STRING_EQUAL(word, "tail");

    /* A width that outruns the input is not an error; it just stops at the
     * end of what is there. */
    word[0] = '\0';
    CU_ASSERT_EQUAL(sscanf("2.5 rest", "%*99f %s", word), 1);
    CU_ASSERT_STRING_EQUAL(word, "rest");
}

void suite_libc_gaps_tests(CU_pSuite s)
{
    CU_add_test(s, "precision builds Doom lump names",
                test_precision_builds_doom_lump_names);
    CU_add_test(s, "precision does not desync arguments",
                test_precision_does_not_desync_the_arguments);
    CU_add_test(s, "precision on strings truncates",
                test_precision_on_strings_truncates);
    CU_add_test(s, "precision edge cases", test_precision_edge_cases);
    CU_add_test(s, "new conversions", test_new_conversions);
    CU_add_test(s, "snprintf return and truncation",
                test_snprintf_return_and_truncation);
    CU_add_test(s, "strncmp", test_strncmp);
    CU_add_test(s, "register_blob replace matches lookup",
                test_register_blob_replace_matches_lookup);
    CU_add_test(s, "sscanf %f honours field width",
                test_sscanf_float_honours_field_width);
    CU_add_test(s, "strrchr", test_strrchr);
    CU_add_test(s, "strstr", test_strstr);
    CU_add_test(s, "strdup", test_strdup);
    CU_add_test(s, "calloc", test_calloc);
    CU_add_test(s, "fopen finds a registered blob",
                test_fopen_finds_a_registered_blob);
    CU_add_test(s, "fopen refuses to write", test_fopen_refuses_to_write);
    CU_add_test(s, "fread/fseek/ftell", test_fread_fseek_ftell);
    CU_add_test(s, "fread counts elements and flags EOF",
                test_fread_counts_elements_and_flags_eof);
    CU_add_test(s, "stdout/stderr survive fclose",
                test_stdout_stderr_exist_and_survive_fclose);
    CU_add_test(s, "absent filesystem says so",
                test_filesystem_absent_functions_say_so);
    CU_add_test(s, "sscanf M_StrToInt forms", test_sscanf_m_strtoint_forms);
    CU_add_test(s, "sscanf reports what it assigned",
                test_sscanf_reports_what_it_assigned);
}
