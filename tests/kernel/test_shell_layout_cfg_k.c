/*
 * test_shell_layout_cfg_k.c — the layout manifest parser (SCRUM-294).
 *
 * src/shell/shell_layout_cfg.c is ring-3 shell code; docker/scripts/build.sh
 * compiles a second copy into the TESTING kernel so the format can be checked
 * here, as test_shell_mount_k.c does for the mount table. Every validation
 * error has a case that pins its line number, and the fuzz cases feed it
 * truncated, oversized and random input to show it fails cleanly.
 */

#include "kunit.h"
#include "context.h"
#include "exo_volume.h"
#include "fb_layout.h"
#include "shell/shell_layout_cfg.h"
#include "string.h"

#include <stddef.h>
#include <stdint.h>

_Static_assert(SHELL_LAYOUT_MAX_PANES == FB_LAYOUT_MAX_PANES,
               "a manifest must not declare more panes than the kernel table holds");
_Static_assert(SHELL_LAYOUT_NAME_LEN == CONTEXT_NAME_MAX,
               "a pane name must fit as a context name");

/* The ticket's example, with the save-volume slots it would really use. */
static const char TWO_PANES[] =
    "# dual Doom: one instance per keyboard\n"
    "\n"
    "pane doom1  program=doom.exo  rect=0,0,100%,50%    input=ps2     volume=2\n"
    "pane doom2  program=doom.exo  rect=0,50%,100%,50%  input=usb:0   volume=3\n";

static shell_layout_t     g_l;
static shell_layout_err_t g_e;
static shell_layout_rect_t g_r[SHELL_LAYOUT_MAX_PANES];

static int parse(const char *s)
{
    return shell_layout_parse(s, strlen(s), &g_l, &g_e);
}

static void rect_is(uint32_t i, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    CU_ASSERT_EQUAL(g_r[i].x, x);
    CU_ASSERT_EQUAL(g_r[i].y, y);
    CU_ASSERT_EQUAL(g_r[i].w, w);
    CU_ASSERT_EQUAL(g_r[i].h, h);
}

/* `s` fails to parse with `code` on line `line`. */
static void parse_fails(const char *s, int code, uint32_t line)
{
    CU_ASSERT_EQUAL(parse(s), code);
    CU_ASSERT_EQUAL((int)g_e.code, code);
    CU_ASSERT_EQUAL(g_e.line, line);
}

/* `s` parses, and resolving it on w x h fails with `code` on line `line`. */
static void resolve_fails(const char *s, uint32_t w, uint32_t h, int code,
                          uint32_t line)
{
    CU_ASSERT_EQUAL(parse(s), 0);
    CU_ASSERT_EQUAL(shell_layout_resolve(&g_l, w, h, g_r, &g_e), code);
    CU_ASSERT_EQUAL((int)g_e.code, code);
    CU_ASSERT_EQUAL(g_e.line, line);
}

static void test_example_parses(void)
{
    CU_ASSERT_EQUAL(parse(TWO_PANES), 0);
    CU_ASSERT_EQUAL(g_l.count, 2);

    const shell_layout_pane_t *a = &g_l.pane[0];
    const shell_layout_pane_t *b = &g_l.pane[1];
    CU_ASSERT_STRING_EQUAL(a->name, "doom1");
    CU_ASSERT_STRING_EQUAL(a->program, "doom.exo");
    CU_ASSERT_EQUAL(a->input.kind, SHELL_LAYOUT_IN_PS2);
    CU_ASSERT_EQUAL(a->has_volume, 1);
    CU_ASSERT_EQUAL(a->volume, 2);
    CU_ASSERT_EQUAL(a->line, 3);

    CU_ASSERT_STRING_EQUAL(b->name, "doom2");
    CU_ASSERT_EQUAL(b->input.kind, SHELL_LAYOUT_IN_USB_IDX);
    CU_ASSERT_EQUAL(b->input.index, 0);
    CU_ASSERT_EQUAL(b->volume, 3);
    CU_ASSERT_EQUAL(b->line, 4);
    CU_ASSERT_EQUAL(b->rect[1].value, 50);
    CU_ASSERT_EQUAL(b->rect[1].pct, 1);
    CU_ASSERT_EQUAL(b->rect[0].pct, 0);
}

static void test_example_resolves(void)
{
    CU_ASSERT_EQUAL(parse(TWO_PANES), 0);

    CU_ASSERT_EQUAL(shell_layout_resolve(&g_l, 640, 480, g_r, &g_e), 0);
    rect_is(0, 0, 0, 640, 240);
    rect_is(1, 0, 240, 640, 240);

    CU_ASSERT_EQUAL(shell_layout_resolve(&g_l, 1024, 768, g_r, &g_e), 0);
    rect_is(0, 0, 0, 1024, 384);
    rect_is(1, 0, 384, 1024, 384);
}

/* Percentages that tile must still tile on a screen they do not divide:
 * 0%+50% and 50%+50% of 1023 are 0..511 and 511..1023, no gap, no overlap. */
static void test_percent_tiles_odd_screen(void)
{
    CU_ASSERT_EQUAL(parse("pane l program=a rect=0,0,50%,100%\n"
                          "pane r program=b rect=50%,0,50%,100%\n"), 0);
    CU_ASSERT_EQUAL(shell_layout_resolve(&g_l, 1023, 767, g_r, &g_e), 0);
    rect_is(0, 0, 0, 511, 767);
    rect_is(1, 511, 0, 512, 767);

    CU_ASSERT_EQUAL(parse("pane t program=a rect=0,0,100%,33%\n"
                          "pane m program=b rect=0,33%,100%,34%\n"
                          "pane b program=c rect=0,67%,100%,33%\n"), 0);
    CU_ASSERT_EQUAL(shell_layout_resolve(&g_l, 640, 481, g_r, &g_e), 0);
    CU_ASSERT_EQUAL(g_r[0].y + g_r[0].h, g_r[1].y);
    CU_ASSERT_EQUAL(g_r[1].y + g_r[1].h, g_r[2].y);
    CU_ASSERT_EQUAL(g_r[2].y + g_r[2].h, 481);
}

static void test_pixels_and_mixed_units(void)
{
    CU_ASSERT_EQUAL(parse("pane a program=x rect=10,20,300,200\n"
                          "pane b program=y rect=50%,20,25%,200\n"), 0);
    CU_ASSERT_EQUAL(shell_layout_resolve(&g_l, 800, 600, g_r, &g_e), 0);
    rect_is(0, 10, 20, 300, 200);
    rect_is(1, 400, 20, 200, 200);
}

static void test_layout_details(void)
{
    /* CRLF, tabs, a trailing comment, no newline at the end, keys in any
     * order, input and volume optional. */
    CU_ASSERT_EQUAL(parse("pane\ta\trect=0,0,1,1 program=p.exo # note\r\n"
                          "  # indented comment\r\n"
                          "pane b input=none program=q rect=1,0,1,1"), 0);
    CU_ASSERT_EQUAL(g_l.count, 2);
    CU_ASSERT_STRING_EQUAL(g_l.pane[0].program, "p.exo");
    CU_ASSERT_EQUAL(g_l.pane[0].input.kind, SHELL_LAYOUT_IN_NONE);
    CU_ASSERT_EQUAL(g_l.pane[0].has_volume, 0);
    CU_ASSERT_EQUAL(g_l.pane[1].input.kind, SHELL_LAYOUT_IN_NONE);
    CU_ASSERT_EQUAL(g_l.pane[1].line, 3);

    /* Nothing at all is a valid, empty layout. */
    CU_ASSERT_EQUAL(parse(""), 0);
    CU_ASSERT_EQUAL(g_l.count, 0);
    CU_ASSERT_EQUAL(parse("# only a comment\n\n\n"), 0);
    CU_ASSERT_EQUAL(g_l.count, 0);
    CU_ASSERT_EQUAL(shell_layout_parse(NULL, 0, &g_l, NULL), 0);
}

static void test_inputs(void)
{
    char buf[32];

    CU_ASSERT_EQUAL(parse("pane a program=x rect=0,0,1,1 input=usb:0627:00Ff\n"
                          "pane b program=x rect=1,0,1,1 input=usb:15\n"
                          "pane c program=x rect=2,0,1,1 input=none\n"
                          "pane d program=x rect=3,0,1,1 input=none\n"), 0);
    CU_ASSERT_EQUAL(g_l.pane[0].input.kind, SHELL_LAYOUT_IN_USB_ID);
    CU_ASSERT_EQUAL(g_l.pane[0].input.vendor, 0x0627);
    CU_ASSERT_EQUAL(g_l.pane[0].input.product, 0x00ff);
    CU_ASSERT_EQUAL(g_l.pane[1].input.index, 15);

    shell_layout_input_str(&g_l.pane[0].input, buf, sizeof(buf));
    CU_ASSERT_STRING_EQUAL(buf, "usb:0627:00ff");
    shell_layout_input_str(&g_l.pane[1].input, buf, sizeof(buf));
    CU_ASSERT_STRING_EQUAL(buf, "usb:15");
    shell_layout_input_str(&g_l.pane[2].input, buf, sizeof(buf));
    CU_ASSERT_STRING_EQUAL(buf, "none");
    CU_ASSERT_EQUAL(shell_layout_input_str(&g_l.pane[0].input, buf, 5), 4);
    CU_ASSERT_STRING_EQUAL(buf, "usb:");
}

static void test_err_line_too_long(void)
{
    static char line[SHELL_LAYOUT_LINE_MAX + 2];
    memset(line, 'x', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    char text[sizeof(line) + 40];
    text[0] = '\0';
    strcat(text, "pane a program=x rect=0,0,1,1\n");
    strcat(text, line);
    parse_fails(text, SHELL_LAYOUT_E_LINE_TOO_LONG, 2);

    /* Exactly the limit is fine as far as length goes (it is then not a
     * pane line, which is the next error). */
    line[SHELL_LAYOUT_LINE_MAX] = '\0';
    parse_fails(line, SHELL_LAYOUT_E_NOT_PANE, 1);
}

static void test_err_bad_char(void)
{
    parse_fails("pane a program=x rect=0,0,1,1\npane b program=\x01 rect=1,0,1,1\n",
                SHELL_LAYOUT_E_BAD_CHAR, 2);
    parse_fails("pane a program=caf\xc3\xa9 rect=0,0,1,1\n",
                SHELL_LAYOUT_E_BAD_CHAR, 1);
    /* A '\r' is only a line ending at the end of a line. */
    parse_fails("pane a\rprogram=x rect=0,0,1,1\n", SHELL_LAYOUT_E_BAD_CHAR, 1);
    /* An embedded NUL is just another bad byte, not the end of the text. */
    static const char nul[] = "pane a program=x rect=0,0,1,1\n\0pane b\n";
    CU_ASSERT_EQUAL(shell_layout_parse(nul, sizeof(nul) - 1, &g_l, &g_e),
                    SHELL_LAYOUT_E_BAD_CHAR);
    CU_ASSERT_EQUAL(g_e.line, 2);
    /* ...but a comment may hold anything. */
    CU_ASSERT_EQUAL(parse("# caf\xc3\xa9 \x01\npane a program=x rect=0,0,1,1\n"), 0);
}

static void test_err_not_pane(void)
{
    parse_fails("\n\npanel a program=x rect=0,0,1,1\n", SHELL_LAYOUT_E_NOT_PANE, 3);
    CU_ASSERT_STRING_EQUAL(g_e.what, "panel");
    parse_fails("PANE a program=x rect=0,0,1,1\n", SHELL_LAYOUT_E_NOT_PANE, 1);
}

static void test_err_names(void)
{
    parse_fails("pane\n", SHELL_LAYOUT_E_NO_NAME, 1);
    parse_fails("pane   # name forgotten\n", SHELL_LAYOUT_E_NO_NAME, 1);
    parse_fails("pane abcdefghijklmnop program=x rect=0,0,1,1\n",
                SHELL_LAYOUT_E_BAD_NAME, 1);
    parse_fails("pane a/b program=x rect=0,0,1,1\n", SHELL_LAYOUT_E_BAD_NAME, 1);
    /* 15 characters is the longest that still fits a context name. */
    CU_ASSERT_EQUAL(parse("pane abcdefghijklmno program=x rect=0,0,1,1\n"), 0);

    parse_fails("pane a program=x rect=0,0,1,1\n"
                "pane b program=x rect=1,0,1,1\n"
                "pane a program=y rect=2,0,1,1\n",
                SHELL_LAYOUT_E_DUP_NAME, 3);
    CU_ASSERT_EQUAL(g_e.other_line, 1);
    CU_ASSERT_STRING_EQUAL(g_e.what, "a");
}

static void test_err_keys(void)
{
    parse_fails("pane a program=x rect=0,0,1,1 colour=red\n",
                SHELL_LAYOUT_E_UNKNOWN_KEY, 1);
    CU_ASSERT_STRING_EQUAL(g_e.what, "colour");
    parse_fails("pane a program=x rect=0,0,1,1 fullscreen\n",
                SHELL_LAYOUT_E_UNKNOWN_KEY, 1);
    CU_ASSERT_STRING_EQUAL(g_e.what, "fullscreen");
    parse_fails("pane a program=x rect=0,0,1,1 Program=y\n",
                SHELL_LAYOUT_E_UNKNOWN_KEY, 1);
    parse_fails("pane a program=x rect=0,0,1,1 rect=1,1,1,1\n",
                SHELL_LAYOUT_E_DUP_KEY, 1);
    CU_ASSERT_STRING_EQUAL(g_e.what, "rect");
    parse_fails("pane a rect=0,0,1,1\n", SHELL_LAYOUT_E_NO_PROGRAM, 1);
    parse_fails("pane a program=x\n", SHELL_LAYOUT_E_NO_RECT, 1);
}

static void test_err_bad_program(void)
{
    parse_fails("pane a program= rect=0,0,1,1\n", SHELL_LAYOUT_E_BAD_PROGRAM, 1);

    char text[SHELL_LAYOUT_PROG_LEN + 64];
    text[0] = '\0';
    strcat(text, "pane a rect=0,0,1,1 program=");
    size_t at = strlen(text);
    memset(text + at, 'p', SHELL_LAYOUT_PROG_LEN);       /* one too many */
    text[at + SHELL_LAYOUT_PROG_LEN] = '\0';
    parse_fails(text, SHELL_LAYOUT_E_BAD_PROGRAM, 1);
    /* Quoted tokens are cut short. */
    CU_ASSERT_EQUAL(strlen(g_e.what), SHELL_LAYOUT_WHAT_LEN - 1);
    text[at + SHELL_LAYOUT_PROG_LEN - 1] = '\0';
    CU_ASSERT_EQUAL(parse(text), 0);
}

static void test_err_bad_rect(void)
{
    static const char *const bad[] = {
        "rect=0,0,1",          /* three values      */
        "rect=0,0,1,1,1",      /* five values       */
        "rect=0,,1,1",         /* empty value       */
        "rect=0,0,1,1,",       /* trailing comma    */
        "rect=0,0,101%,1",     /* over 100%         */
        "rect=0,0,65536,1",    /* over the px cap   */
        "rect=0,0,999999,1",   /* too many digits   */
        "rect=-1,0,1,1",       /* sign              */
        "rect=0,0,1%%,1",      /* doubled %         */
        "rect=a,0,1,1",
        "rect=",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char text[96];
        text[0] = '\0';
        strcat(text, "pane a program=x rect=0,0,1,1\npane b program=x ");
        strcat(text, bad[i]);
        parse_fails(text, SHELL_LAYOUT_E_BAD_RECT, 2);
    }
    CU_ASSERT_EQUAL(parse("pane a program=x rect=0,0,65535,100%\n"), 0);
}

static void test_err_bad_input(void)
{
    static const char *const bad[] = {
        "input=usb", "input=usb:", "input=usb:16", "input=usb:123",
        "input=usb:x", "input=usb:627:1", "input=usb:0627:001",
        "input=usb:0627:00001", "input=usb:0627:00g1", "input=usb:0627",
        "input=PS2", "input=ps/2", "input=",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char text[96];
        text[0] = '\0';
        strcat(text, "\npane a program=x rect=0,0,1,1 ");
        strcat(text, bad[i]);
        parse_fails(text, SHELL_LAYOUT_E_BAD_INPUT, 2);
    }
}

static void test_err_dup_input(void)
{
    parse_fails("pane a program=x rect=0,0,1,1 input=ps2\n"
                "pane b program=x rect=1,0,1,1 input=usb:0\n"
                "pane c program=x rect=2,0,1,1 input=ps2\n",
                SHELL_LAYOUT_E_DUP_INPUT, 3);
    CU_ASSERT_EQUAL(g_e.other_line, 1);
    CU_ASSERT_STRING_EQUAL(g_e.what, "ps2");

    parse_fails("pane a program=x rect=0,0,1,1 input=usb:0627:0001\n"
                "pane b program=x rect=1,0,1,1 input=usb:0627:0001\n",
                SHELL_LAYOUT_E_DUP_INPUT, 2);
    CU_ASSERT_STRING_EQUAL(g_e.what, "usb:0627:0001");

    /* Different keyboards, and any number of panes with none. */
    CU_ASSERT_EQUAL(parse("pane a program=x rect=0,0,1,1 input=usb:0\n"
                          "pane b program=x rect=1,0,1,1 input=usb:1\n"
                          "pane c program=x rect=2,0,1,1\n"
                          "pane d program=x rect=3,0,1,1\n"), 0);
}

static void test_err_volume(void)
{
    parse_fails("pane a program=x rect=0,0,1,1 volume=8\n",
                SHELL_LAYOUT_E_BAD_VOLUME, 1);
    parse_fails("pane a program=x rect=0,0,1,1 volume=\n",
                SHELL_LAYOUT_E_BAD_VOLUME, 1);
    parse_fails("pane a program=x rect=0,0,1,1 volume=1x\n",
                SHELL_LAYOUT_E_BAD_VOLUME, 1);
    CU_ASSERT_EQUAL(parse("pane a program=x rect=0,0,1,1 volume=7\n"), 0);
    CU_ASSERT_EQUAL(EXO_VOLUME_MAX_SLOTS, 8);

    parse_fails("pane a program=x rect=0,0,1,1 volume=2\n"
                "pane b program=x rect=1,0,1,1 volume=2\n",
                SHELL_LAYOUT_E_DUP_VOLUME, 2);
    CU_ASSERT_EQUAL(g_e.other_line, 1);
}

static void test_err_too_many(void)
{
    char text[SHELL_LAYOUT_MAX_PANES * 48 + 64];
    text[0] = '\0';
    for (uint32_t i = 0; i <= SHELL_LAYOUT_MAX_PANES; i++) {
        char line[48] = "pane pN program=x rect=N,0,1,1\n";
        line[6] = (char)('0' + i);       /* the name's N  */
        line[23] = (char)('0' + i);      /* rect's x      */
        strcat(text, line);
        if (i + 1 == SHELL_LAYOUT_MAX_PANES) {
            CU_ASSERT_EQUAL(parse(text), 0);
            CU_ASSERT_EQUAL(g_l.count, SHELL_LAYOUT_MAX_PANES);
        }
    }
    parse_fails(text, SHELL_LAYOUT_E_TOO_MANY, SHELL_LAYOUT_MAX_PANES + 1);
}

static void test_err_resolve(void)
{
    resolve_fails("pane a program=x rect=0,0,0,100\n", 640, 480,
                  SHELL_LAYOUT_E_RECT_EMPTY, 1);
    /* 1% of a 50-pixel-high screen is no rows at all. */
    resolve_fails("\npane a program=x rect=0,0,100%,1%\n", 640, 50,
                  SHELL_LAYOUT_E_RECT_EMPTY, 2);
    resolve_fails("pane a program=x rect=600,0,100,100\n", 640, 480,
                  SHELL_LAYOUT_E_RECT_OFFSCREEN, 1);
    resolve_fails("pane a program=x rect=0,0,10,10\n"
                  "pane b program=x rect=0,50%,100%,51%\n", 640, 480,
                  SHELL_LAYOUT_E_RECT_OFFSCREEN, 2);
    resolve_fails("pane a program=x rect=640,0,1,1\n", 640, 480,
                  SHELL_LAYOUT_E_RECT_OFFSCREEN, 1);
    /* Fits a big screen, not a small one: checked at apply time, not parse. */
    CU_ASSERT_EQUAL(parse("pane a program=x rect=0,0,800,600\n"), 0);
    CU_ASSERT_EQUAL(shell_layout_resolve(&g_l, 1024, 768, g_r, &g_e), 0);
    CU_ASSERT_EQUAL(shell_layout_resolve(&g_l, 640, 480, g_r, &g_e),
                    SHELL_LAYOUT_E_RECT_OFFSCREEN);

    resolve_fails("pane a program=x rect=0,0,50%,100%\n"
                  "pane b program=x rect=0,0,10,10\n"
                  "pane c program=x rect=40%,0,60%,100%\n", 640, 480,
                  SHELL_LAYOUT_E_OVERLAP, 2);
    CU_ASSERT_EQUAL(g_e.other_line, 1);
    CU_ASSERT_STRING_EQUAL(g_e.what, "b");
    CU_ASSERT_EQUAL(g_e.screen_w, 640);
    CU_ASSERT_EQUAL(g_e.screen_h, 480);

    /* Touching edges are not an overlap. */
    CU_ASSERT_EQUAL(parse("pane a program=x rect=0,0,320,480\n"
                          "pane b program=x rect=320,0,320,480\n"), 0);
    CU_ASSERT_EQUAL(shell_layout_resolve(&g_l, 640, 480, g_r, NULL), 0);
}

static void test_format_err(void)
{
    char msg[160];

    parse("pane a program=x rect=0,0,1,1\npane b program=x rect=1,0,1,1 colour=red\n");
    shell_layout_format_err(msg, sizeof(msg), SHELL_LAYOUT_DEFAULT_PATH, &g_e);
    CU_ASSERT_STRING_EQUAL(msg, "/etc/layout.cfg line 2: unknown setting "
                                "'colour' (expected program=, rect=, input= "
                                "or volume=)");

    parse("pane a program=x rect=0,0,1,1\npane a program=x rect=1,0,1,1\n");
    shell_layout_format_err(msg, sizeof(msg), NULL, &g_e);
    CU_ASSERT_STRING_EQUAL(msg, "layout line 2: pane name 'a' is already used "
                                "on line 1");

    resolve_fails("pane top program=x rect=0,0,100%,60%\n"
                  "pane bot program=x rect=0,50%,100%,50%\n", 640, 480,
                  SHELL_LAYOUT_E_OVERLAP, 2);
    shell_layout_format_err(msg, sizeof(msg), "l.cfg", &g_e);
    CU_ASSERT_STRING_EQUAL(msg, "l.cfg line 2: pane 'bot' overlaps the pane "
                                "on line 1 on a 640x480 screen");

    /* Truncates rather than overruns, and always terminates. */
    CU_ASSERT_EQUAL(shell_layout_format_err(msg, 8, "l.cfg", &g_e), 7);
    CU_ASSERT_STRING_EQUAL(msg, "l.cfg l");
    CU_ASSERT_EQUAL(shell_layout_format_err(msg, 0, "l.cfg", &g_e), 0);

    /* Every code has a message of its own. */
    shell_layout_err_t e;
    memset(&e, 0, sizeof(e));
    for (int c = SHELL_LAYOUT_E_LINE_TOO_LONG; c < SHELL_LAYOUT_E_COUNT; c++) {
        e.code = (shell_layout_code_t)c;
        e.line = 1;
        shell_layout_format_err(msg, sizeof(msg), "f", &e);
        CU_ASSERT_TRUE(strlen(msg) > strlen("f line 1: "));
        CU_ASSERT_TRUE(strstr(msg, "invalid layout") == NULL);
    }
}

/* Every outcome is OK or a known code with a line in range, and nothing
 * reads past `len` (the buffer below ends exactly at the text). */
static int outcome_sane(const char *text, size_t len)
{
    int rc = shell_layout_parse(text, len, &g_l, &g_e);
    if (rc < 0 || rc >= SHELL_LAYOUT_E_COUNT || rc != (int)g_e.code) {
        return 0;
    }
    if (rc == 0) {
        rc = shell_layout_resolve(&g_l, 640, 480, g_r, &g_e);
        if (rc < 0 || rc >= SHELL_LAYOUT_E_COUNT) {
            return 0;
        }
        return 1;
    }
    char msg[200];
    shell_layout_format_err(msg, sizeof(msg), "f", &g_e);
    return g_e.line >= 1 && strlen(msg) < sizeof(msg);
}

static void test_fuzz_truncated(void)
{
    static char buf[sizeof(TWO_PANES)];
    size_t full = sizeof(TWO_PANES) - 1;
    int bad = 0;
    for (size_t n = 0; n <= full; n++) {
        memcpy(buf, TWO_PANES, n);
        if (!outcome_sane(buf, n)) {
            bad++;
        }
    }
    CU_ASSERT_EQUAL(bad, 0);
}

static void test_fuzz_long_lines(void)
{
    static char big[4096];
    memset(big, 'a', sizeof(big));
    CU_ASSERT_EQUAL(shell_layout_parse(big, sizeof(big), &g_l, &g_e),
                    SHELL_LAYOUT_E_LINE_TOO_LONG);
    CU_ASSERT_EQUAL(g_e.line, 1);

    /* A 4 KB comment is still a too-long line: the cap is on the line. */
    big[0] = '#';
    CU_ASSERT_EQUAL(shell_layout_parse(big, sizeof(big), &g_l, &g_e),
                    SHELL_LAYOUT_E_LINE_TOO_LONG);

    /* A single 4 KB token on a valid-looking line. */
    memcpy(big, "pane a program=", 15);
    CU_ASSERT_EQUAL(shell_layout_parse(big, sizeof(big), &g_l, &g_e),
                    SHELL_LAYOUT_E_LINE_TOO_LONG);

    /* Thousands of short lines are fine. */
    for (size_t i = 0; i < sizeof(big); i += 2) {
        big[i] = '#';
        big[i + 1] = '\n';
    }
    CU_ASSERT_EQUAL(shell_layout_parse(big, sizeof(big), &g_l, &g_e), 0);
}

static void test_fuzz_garbage(void)
{
    static char buf[512];
    uint32_t seed = 0x2945u;
    int bad = 0;

    for (int it = 0; it < 2000; it++) {
        size_t n = 0;
        seed = seed * 1103515245u + 12345u;
        n = (seed >> 16) % sizeof(buf);
        for (size_t k = 0; k < n; k++) {
            seed = seed * 1103515245u + 12345u;
            if (it & 1) {
                buf[k] = (char)(seed >> 16);                  /* raw bytes */
            } else {
                /* A valid file with some characters swapped for ones that
                 * matter to the grammar, to reach deeper than raw bytes. */
                static const char pick[] = " ,%:=#\n\r9azp";
                buf[k] = TWO_PANES[k % (sizeof(TWO_PANES) - 1)];
                if (((seed >> 16) & 15u) == 0) {
                    buf[k] = pick[(seed >> 20) % (sizeof(pick) - 1)];
                }
            }
        }
        if (!outcome_sane(buf, n)) {
            bad++;
        }
    }
    CU_ASSERT_EQUAL(bad, 0);
}

void suite_shell_layout_tests(CU_pSuite s)
{
    CU_add_test(s, "example: parses", test_example_parses);
    CU_add_test(s, "example: resolves on 640x480 and 1024x768",
                test_example_resolves);
    CU_add_test(s, "percentages tile an odd-sized screen",
                test_percent_tiles_odd_screen);
    CU_add_test(s, "pixels and mixed units", test_pixels_and_mixed_units);
    CU_add_test(s, "comments, CRLF, tabs, no final newline",
                test_layout_details);
    CU_add_test(s, "input sources", test_inputs);
    CU_add_test(s, "error: line too long", test_err_line_too_long);
    CU_add_test(s, "error: bad character", test_err_bad_char);
    CU_add_test(s, "error: not a pane line", test_err_not_pane);
    CU_add_test(s, "error: names", test_err_names);
    CU_add_test(s, "error: keys", test_err_keys);
    CU_add_test(s, "error: bad program", test_err_bad_program);
    CU_add_test(s, "error: bad rect", test_err_bad_rect);
    CU_add_test(s, "error: bad input", test_err_bad_input);
    CU_add_test(s, "error: input bound twice", test_err_dup_input);
    CU_add_test(s, "error: volume", test_err_volume);
    CU_add_test(s, "error: too many panes", test_err_too_many);
    CU_add_test(s, "error: resolve (empty, off-screen, overlap)",
                test_err_resolve);
    CU_add_test(s, "error messages", test_format_err);
    CU_add_test(s, "fuzz: every truncation", test_fuzz_truncated);
    CU_add_test(s, "fuzz: 4 KB lines", test_fuzz_long_lines);
    CU_add_test(s, "fuzz: random bytes", test_fuzz_garbage);
}
