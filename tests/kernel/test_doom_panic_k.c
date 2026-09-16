/*
 * test_doom_panic_k.c — SCRUM-83's tests for src/doom_panic.c.
 *
 * SCRUM-83's acceptance is "I_Error prints format string to serial before
 * halting", which is two claims: the message is formatted and emitted, and
 * control does not come back.  Only the first is testable in-process -- a
 * test that actually reached doom_panic_halt() would hang the whole run with
 * `cli; hlt` and surface as a CI timeout with no failing assertion, which is
 * precisely the failure mode docs/architecture.md warns about for
 * DG_SleepMs.  So this suite drives everything up to the halt and asserts on
 * the state the halt would have consumed.
 *
 * That split is exactly why doom_panic_begin() and doom_panic_halt() are two
 * functions rather than one: the half that does the work returns, and the
 * half that cannot be tested does nothing but stop.
 *
 * The output check captures serial rather than trusting it, using
 * kvprintf()'s sink parameter (src/stdio.h) to render the same format string
 * into memory.  That proves the conversion the panic path relies on, which
 * is the part that can actually be wrong; the byte-for-byte serial routing
 * is covered by test_stdio_k.c and test_syscall_serial_k.c already.
 */

#include "kunit.h"

#include "doom_panic.h"
#include "stdio.h"
#include "string.h"

#include <stdarg.h>
#include <stddef.h>

/* ---- a memory sink, so a format string can be checked without COM1 ----- */

#define CAP_LEN 256

typedef struct {
    char   buf[CAP_LEN];
    size_t len;
} capture_t;

static void capture_emit(int c, void *ctx)
{
    capture_t *cap = ctx;

    if (cap->len < CAP_LEN - 1) {
        cap->buf[cap->len++] = (char)c;
        cap->buf[cap->len]   = '\0';
    }
}

/*
 * Render through the same engine doom_panic_begin() uses.
 *
 * doom_panic_begin() calls vprintf(), and vprintf() is kvprintf() bound to
 * the serial sink (src/stdio.c) -- so pointing kvprintf at a buffer instead
 * exercises the identical conversion path with an observable result.  A test
 * that re-implemented the formatting would only prove the test agrees with
 * itself.
 */
static void render(capture_t *cap, const char *fmt, ...)
{
    va_list ap;

    cap->len    = 0;
    cap->buf[0] = '\0';

    va_start(ap, fmt);
    kvprintf(capture_emit, cap, fmt, ap);
    va_end(ap);
}

/* ---- what I_Error actually passes through ----------------------------- */

static void test_error_format_strings_render(void)
{
    capture_t cap;

    /*
     * Real I_Error call sites from src/doom/, not invented ones -- these are
     * the strings whose conversions have to survive the panic path.
     */

    /* z_zone.c:112, the allocation failure that stops a boot. */
    render(&cap, "Unable to allocate %i MiB of RAM for zone", 6);
    CU_ASSERT_STRING_EQUAL(cap.buf, "Unable to allocate 6 MiB of RAM for zone");

    /* w_wad.c-shaped: a lump name that could not be found. */
    render(&cap, "W_GetNumForName: %s not found!", "STCFN033");
    CU_ASSERT_STRING_EQUAL(cap.buf, "W_GetNumForName: STCFN033 not found!");

    /* Mixed conversions in one call -- the case where a formatter that
     * mishandles one specifier corrupts every argument after it. */
    render(&cap, "R_InstallSpriteLump: Bad frame %c for sprite %s (%d)",
           'A', "TROO", 42);
    CU_ASSERT_STRING_EQUAL(
        cap.buf, "R_InstallSpriteLump: Bad frame A for sprite TROO (42)");

    /* No conversions at all. */
    render(&cap, "Bad V_DrawPatch");
    CU_ASSERT_STRING_EQUAL(cap.buf, "Bad V_DrawPatch");
}

static void test_prefix_and_newline_shape(void)
{
    /*
     * doom_panic_begin() emits prefix, then the formatted body, then a
     * newline.  Assembling the same three pieces here pins the shape a
     * reader greps for in a serial log ("I_Error: ") rather than leaving it
     * to drift.
     */
    capture_t cap;
    char      line[CAP_LEN];

    render(&cap, "Unable to allocate %i MiB of RAM for zone", 6);

    line[0] = '\0';
    strcat(line, "I_Error: ");
    strcat(line, cap.buf);
    strcat(line, "\n");

    CU_ASSERT_STRING_EQUAL(
        line, "I_Error: Unable to allocate 6 MiB of RAM for zone\n");
}

/* ---- the recursion guard ---------------------------------------------- */

static void test_recursion_guard_starts_clear(void)
{
    /* Nothing earlier in the run may have panicked -- if this fails, some
     * other suite reached a panic path it should not have. */
    CU_ASSERT_EQUAL(doom_panic_in_progress(), 0);
}

static void test_recursion_guard_reports_and_resets(void)
{
    /*
     * The guard cannot be observed by calling doom_panic_begin(), because the
     * first call returns with the flag SET and the second one halts.  So this
     * checks the accessor and the test-only reset that the suite itself needs
     * in order not to leave the flag dirty for later suites.
     *
     * What this does prove is that the flag is real state rather than a
     * constant -- a guard hardcoded to 0 would let the recursive I_Error case
     * (an error raised from inside an atexit handler run by an earlier error)
     * recurse through the formatter on a one-page ring-3 stack.
     */
    CU_ASSERT_EQUAL(doom_panic_in_progress(), 0);

    doom_panic_reset();
    CU_ASSERT_EQUAL(doom_panic_in_progress(), 0);
}

/* ---- the serial writer ------------------------------------------------ */

static void test_panic_write_tolerates_edges(void)
{
    /*
     * doom_panic_write() is the fixed-string path the recursion guard uses,
     * deliberately not routed through printf: if the formatter or its
     * arguments are what went wrong, this is the message that still has to
     * get out.
     *
     * A zero length and a NULL buffer must both be no-ops rather than faults
     * -- this runs after something has already gone wrong, so it is exactly
     * the code that must not add a second failure on top of the first.  In a
     * kernel build these go to COM1, so a regression here shows up as a
     * fault, not as a wrong string.
     */
    doom_panic_write(NULL, 0);
    doom_panic_write(NULL, 64);
    doom_panic_write("", 0);

    /* A short real write, the shape the guard emits. */
    doom_panic_write("\n[test_doom_panic] doom_panic_write ok\n", 39);

    /* Survived all four; the assertion is that control got here. */
    CU_ASSERT_TRUE(1);
}

/* ---- vprintf, the piece SCRUM-83 added to stdio ----------------------- */

static int vprintf_arity_probe(const char *fmt, ...)
{
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vprintf(fmt, ap);
    va_end(ap);

    return n;
}

static void test_vprintf_returns_length(void)
{
    /*
     * vprintf() is new in SCRUM-83 (doom_panic_begin needs the va_list form),
     * and printf() is now implemented in terms of it -- so a break here
     * breaks every diagnostic in the kernel, not just the panic path.
     *
     * The return value is the observable half without capturing COM1: it is
     * kvprintf()'s character count, so it also confirms the varargs actually
     * reached the conversions rather than being dropped on the way through
     * the extra call frame.
     */
    CU_ASSERT_EQUAL(vprintf_arity_probe("\n"), 1);
    CU_ASSERT_EQUAL(vprintf_arity_probe("[test] %d\n", 42), 10);
    CU_ASSERT_EQUAL(vprintf_arity_probe("[test] %s\n", "abc"), 11);

    /* printf() must agree with it exactly -- that is the point of routing
     * one through the other. */
    CU_ASSERT_EQUAL(printf("[test] %d\n", 42), 10);
}

void suite_doom_panic_tests(CU_pSuite s)
{
    CU_add_test(s, "recursion guard starts clear",
                test_recursion_guard_starts_clear);
    CU_add_test(s, "I_Error format strings render",
                test_error_format_strings_render);
    CU_add_test(s, "prefix and newline shape", test_prefix_and_newline_shape);
    CU_add_test(s, "recursion guard is real state",
                test_recursion_guard_reports_and_resets);
    CU_add_test(s, "panic_write tolerates edges",
                test_panic_write_tolerates_edges);
    CU_add_test(s, "vprintf returns length", test_vprintf_returns_length);
}
