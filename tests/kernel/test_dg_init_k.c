/*
 * test_dg_init_k.c — SCRUM-73's tests for DG_Init and the WAD mount.
 *
 * SCRUM-73's acceptance is "Doom init sequence runs; serial shows WAD loaded
 * without I_Error".  The half worth automating is the one underneath that
 * sentence: DG_Init has to tell a real WAD from four bytes that merely look
 * like one, and it has to fail in a way that names which thing went wrong.
 *
 * Every WAD here is built byte by byte in this file rather than read from the
 * real freedoom2.wad module.  That is deliberate:
 *
 *   - The malformed cases cannot be obtained any other way.  A directory
 *     offset past the end of the file, a lump count that would overflow
 *     numlumps * 16, a PWAD where an IWAD was expected -- these are exactly
 *     the inputs that must be rejected, and none of them exists as a file to
 *     point at.
 *   - The real module is 28 MB and is only present on a normal boot; a
 *     TESTING build never gets a framebuffer or a GRUB module, so a test that
 *     depended on it would be untestable in CI by construction.
 *
 * DG_Init itself is drivable from ring 0 because it reports its outcome
 * through dg_init_result() instead of halting -- see its own comment in
 * src/doomgeneric_exo.c for why that is the shipped behaviour and not a test
 * affordance.
 */

#include "kunit.h"

#include "doom_wad.h"
#include "doomgeneric_exo.h"
#include "libos_wad_params.h"
#include "stdio.h"
#include "string.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* src/doomgeneric_exo.c's params global -- the struct the kernel-side
 * launcher patches via libos_launch_patch_params().  Writing it directly is
 * what stands in for that patch here. */
extern libos_wad_params_t g_doom_params;

/* ---- synthetic WADs --------------------------------------------------- */

/*
 * Big enough for a header plus a handful of directory entries.  A WAD is
 * 12 bytes of header (magic, numlumps, dirofs) followed by the lump data and
 * then a directory of 16-byte entries, so 256 bytes holds a valid two-lump
 * WAD with room left over for the out-of-bounds cases to point into.
 */
#define FAKE_WAD_LEN 256

static uint8_t fake_wad[FAKE_WAD_LEN];

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/*
 * Build a WAD header in fake_wad.  `magic` is "IWAD" or "PWAD"; the rest is
 * written raw so a test can supply values wad_init() must reject.
 */
static void make_wad(const char *magic, uint32_t numlumps, uint32_t dirofs)
{
    memset(fake_wad, 0, sizeof(fake_wad));

    fake_wad[0] = (uint8_t)magic[0];
    fake_wad[1] = (uint8_t)magic[1];
    fake_wad[2] = (uint8_t)magic[2];
    fake_wad[3] = (uint8_t)magic[3];

    put_le32(fake_wad + 4, numlumps);
    put_le32(fake_wad + 8, dirofs);
}

/* A WAD that should mount: 2 lumps, directory at offset 64, comfortably
 * inside FAKE_WAD_LEN (64 + 2*16 = 96). */
static void make_good_wad(const char *magic)
{
    make_wad(magic, 2, 64);
}

/* ---- the mount registry ----------------------------------------------- */

static void test_mount_accepts_iwad(void)
{
    const doom_wad_t *w;

    doom_wad_unmount();
    make_good_wad("IWAD");

    CU_ASSERT_EQUAL(doom_wad_mount(fake_wad, FAKE_WAD_LEN), DOOM_WAD_OK);

    w = doom_wad_mounted();
    CU_ASSERT_PTR_NOT_NULL(w);

    if (w != NULL) {
        CU_ASSERT_EQUAL(w->is_iwad, 1);
        CU_ASSERT_EQUAL(w->numlumps, 2);
        CU_ASSERT_EQUAL(w->size, FAKE_WAD_LEN);
        /* The parse must point back at the bytes handed in, not a copy --
         * there is nowhere to copy a real 28 MB WAD to. */
        CU_ASSERT_TRUE(w->wad.data == fake_wad);
    }

    doom_wad_unmount();
}

static void test_mount_accepts_pwad_and_flags_it(void)
{
    const doom_wad_t *w;

    doom_wad_unmount();
    make_good_wad("PWAD");

    /* Accepted, not refused: Doom's own d_iwad.c decides whether it has a
     * playable game.  Refusing here would turn "this is a patch WAD" into
     * "DG_Init rejected your file", which is a worse diagnosis of the same
     * problem -- and this project has already shipped a PWAD as an IWAD once
     * (docs/architecture.md sec7). */
    CU_ASSERT_EQUAL(doom_wad_mount(fake_wad, FAKE_WAD_LEN), DOOM_WAD_OK);

    w = doom_wad_mounted();
    CU_ASSERT_PTR_NOT_NULL(w);
    if (w != NULL) {
        CU_ASSERT_EQUAL(w->is_iwad, 0);
    }

    doom_wad_unmount();
}

static void test_mount_rejects_missing_module(void)
{
    doom_wad_unmount();

    /* Its own code, because it is the one failure that says nothing about
     * the WAD: it means no multiboot module tag reached us, so the thing to
     * look at is src/grub.cfg and build.sh's ISO staging. */
    CU_ASSERT_EQUAL(doom_wad_mount(0, FAKE_WAD_LEN), DOOM_WAD_ENOENT);
    CU_ASSERT_EQUAL(doom_wad_mount(fake_wad, 0), DOOM_WAD_ENOENT);
    CU_ASSERT_PTR_NULL(doom_wad_mounted());
}

static void test_mount_rejects_oversize(void)
{
    doom_wad_unmount();
    make_good_wad("IWAD");

    /*
     * Refused BEFORE the parse, not after.  libos_map_wad() will not map past
     * LIBOS_WAD_MAX_BYTES, so a size beyond that means the tail of the range
     * is not actually mapped -- and wad_init() would read a directory offset
     * out of a header that IS mapped, then bounds-check lumps against a size
     * covering pages that fault on touch.
     */
    CU_ASSERT_EQUAL(doom_wad_mount(fake_wad, DOOM_WAD_MAX_BYTES + 1),
                    DOOM_WAD_ETOOBIG);
    CU_ASSERT_PTR_NULL(doom_wad_mounted());
}

static void test_mount_rejects_bad_magic(void)
{
    doom_wad_unmount();

    /* Not a WAD at all. */
    make_wad("JUNK", 2, 64);
    CU_ASSERT_EQUAL(doom_wad_mount(fake_wad, FAKE_WAD_LEN), DOOM_WAD_EINVAL);

    /* Right first byte, wrong rest -- catches a magic check that only looks
     * at 'I' or 'P'. */
    make_wad("IXAD", 2, 64);
    CU_ASSERT_EQUAL(doom_wad_mount(fake_wad, FAKE_WAD_LEN), DOOM_WAD_EINVAL);

    CU_ASSERT_PTR_NULL(doom_wad_mounted());
}

static void test_mount_rejects_bad_directory(void)
{
    doom_wad_unmount();

    /* Directory starting inside the header itself. */
    make_wad("IWAD", 2, 4);
    CU_ASSERT_EQUAL(doom_wad_mount(fake_wad, FAKE_WAD_LEN), DOOM_WAD_EINVAL);

    /* Directory offset past the end of the file. */
    make_wad("IWAD", 2, FAKE_WAD_LEN + 16);
    CU_ASSERT_EQUAL(doom_wad_mount(fake_wad, FAKE_WAD_LEN), DOOM_WAD_EINVAL);

    /* A lump count whose directory would run off the end.  Offset 64 leaves
     * 192 bytes, i.e. room for 12 entries -- 13 is one too many. */
    make_wad("IWAD", 13, 64);
    CU_ASSERT_EQUAL(doom_wad_mount(fake_wad, FAKE_WAD_LEN), DOOM_WAD_EINVAL);

    /* The overflow case: numlumps * 16 wraps in 32 bits.  A mount that
     * computed the directory extent as a product rather than a division
     * would wave this through and then read far outside the mapping. */
    make_wad("IWAD", 0xFFFFFFFFu, 64);
    CU_ASSERT_EQUAL(doom_wad_mount(fake_wad, FAKE_WAD_LEN), DOOM_WAD_EINVAL);

    CU_ASSERT_PTR_NULL(doom_wad_mounted());
}

static void test_failed_mount_leaves_nothing_behind(void)
{
    /*
     * The caller of a failed mount is on its way to an error path, and
     * anything still reading the registry during that shutdown must see
     * "nothing mounted" rather than a half-written parse from the attempt
     * that just failed.
     */
    doom_wad_unmount();
    make_good_wad("IWAD");
    CU_ASSERT_EQUAL(doom_wad_mount(fake_wad, FAKE_WAD_LEN), DOOM_WAD_OK);
    CU_ASSERT_PTR_NOT_NULL(doom_wad_mounted());

    /* A failed mount must not disturb the good one already in place... */
    make_wad("JUNK", 2, 64);
    CU_ASSERT_EQUAL(doom_wad_mount(fake_wad, FAKE_WAD_LEN), DOOM_WAD_EINVAL);
    CU_ASSERT_PTR_NOT_NULL(doom_wad_mounted());

    /* ...and an explicit unmount must clear it. */
    doom_wad_unmount();
    CU_ASSERT_PTR_NULL(doom_wad_mounted());
}

static void test_strerror_is_always_a_string(void)
{
    /* Used in DG_Init's serial report, so a NULL here would turn a
     * diagnostic into a fault on the diagnostic path. */
    CU_ASSERT_PTR_NOT_NULL(doom_wad_strerror(DOOM_WAD_OK));
    CU_ASSERT_PTR_NOT_NULL(doom_wad_strerror(DOOM_WAD_ENOENT));
    CU_ASSERT_PTR_NOT_NULL(doom_wad_strerror(DOOM_WAD_ETOOBIG));
    CU_ASSERT_PTR_NOT_NULL(doom_wad_strerror(DOOM_WAD_EINVAL));
    CU_ASSERT_PTR_NOT_NULL(doom_wad_strerror(12345));
    CU_ASSERT_STRING_EQUAL(doom_wad_strerror(DOOM_WAD_OK), "ok");
}

/* ---- DG_Init itself ---------------------------------------------------- */

/*
 * Each DG_Init case writes g_doom_params directly.  On a real launch the
 * kernel-side handler patches that struct through img.data_paddrs[0]
 * (libos_launch_patch_params(), src/libos_launch.h); from ring 0 the global
 * is simply in reach, so this exercises the identical code path DG_Init
 * takes without needing a LibOS launch.
 */
static void set_params(uint64_t vaddr, uint64_t size)
{
    g_doom_params.wad_vaddr = vaddr;
    g_doom_params.wad_size  = size;
}

static void test_dg_init_mounts_a_good_wad(void)
{
    doom_wad_unmount();
    make_good_wad("IWAD");
    set_params((uint64_t)(uintptr_t)fake_wad, FAKE_WAD_LEN);

    DG_Init();

    CU_ASSERT_EQUAL(dg_init_result(), DOOM_WAD_OK);
    CU_ASSERT_PTR_NOT_NULL(doom_wad_mounted());

    doom_wad_unmount();
}

static void test_dg_init_reports_unpatched_params(void)
{
    /*
     * The sentinel case: nobody called libos_launch_patch_params(), most
     * likely because src/doomgeneric_exo.c was not first in the link order
     * for the Doom target, so its params global did not land at offset 0 of
     * .data.
     *
     * This gets its own outcome rather than being allowed to fall through to
     * a mount attempt, because a sentinel address dereferenced as a WAD
     * header is a fault, not a diagnosis -- and it is a launcher bug, which
     * sends you to a completely different file than a bad WAD would.
     */
    doom_wad_unmount();
    set_params(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL);

    DG_Init();

    CU_ASSERT_EQUAL(dg_init_result(), DOOM_WAD_ENOENT);
    CU_ASSERT_PTR_NULL(doom_wad_mounted());
}

static void test_dg_init_reports_oversize_before_truncating(void)
{
    /*
     * wad_t is 32-bit throughout (a WAD directory stores 32-bit file
     * offsets), so DG_Init has to narrow the 64-bit param.  The guard runs
     * first so an absurd size is reported as absurd rather than silently
     * wrapping into a plausible one -- 0x1_0000_0100 truncates to 256, which
     * is exactly FAKE_WAD_LEN and would otherwise have mounted cleanly.
     */
    doom_wad_unmount();
    make_good_wad("IWAD");
    set_params((uint64_t)(uintptr_t)fake_wad, 0x100000100ULL);

    DG_Init();

    CU_ASSERT_EQUAL(dg_init_result(), DOOM_WAD_ETOOBIG);
    CU_ASSERT_PTR_NULL(doom_wad_mounted());
}

static void test_dg_init_reports_a_bad_wad(void)
{
    doom_wad_unmount();
    make_wad("JUNK", 2, 64);
    set_params((uint64_t)(uintptr_t)fake_wad, FAKE_WAD_LEN);

    DG_Init();

    /* Reports and returns -- it does not halt.  A missing or malformed IWAD
     * is what Doom's own D_DoomMain/W_AddFile path exists to diagnose, with
     * engine context this layer does not have. */
    CU_ASSERT_EQUAL(dg_init_result(), DOOM_WAD_EINVAL);
    CU_ASSERT_PTR_NULL(doom_wad_mounted());
}

/*
 * The oversize message has to survive a 64-bit size, and kvprintf has no
 * length modifiers -- no %llu.  Rendering DG_Init's own format string through
 * the same engine, into a buffer, is the only way to check that from a test:
 * DG_Init writes to COM1, which a unit test cannot read back.
 *
 * This is not hypothetical.  The first version of that message used
 * (unsigned)wad_size and reported a 0x1_0000_0100 byte WAD as "256 bytes
 * exceeds the 67108864 byte window" -- the low half, which happens to be a
 * perfectly plausible size, describing a failure it does not explain.
 */
#define OVERSIZE_CAP 160

typedef struct {
    char   buf[OVERSIZE_CAP];
    size_t len;
} oversize_capture_t;

static void oversize_emit(int c, void *ctx)
{
    oversize_capture_t *cap = ctx;

    if (cap->len < OVERSIZE_CAP - 1) {
        cap->buf[cap->len++] = (char)c;
        cap->buf[cap->len]   = '\0';
    }
}

static void render_oversize(oversize_capture_t *cap, const char *fmt, ...)
{
    va_list ap;

    cap->len    = 0;
    cap->buf[0] = '\0';

    va_start(ap, fmt);
    kvprintf(oversize_emit, cap, fmt, ap);
    va_end(ap);
}

static void test_oversize_message_keeps_all_64_bits(void)
{
    oversize_capture_t cap;
    uint64_t           size = 0x100000100ULL;

    render_oversize(&cap,
                    "DG_Init: WAD size 0x%08x%08x bytes exceeds the %u byte "
                    "window.\n",
                    (unsigned)(size >> 32),
                    (unsigned)(size & 0xFFFFFFFFu),
                    (unsigned)DOOM_WAD_MAX_BYTES);

    CU_ASSERT_STRING_EQUAL(cap.buf,
                           "DG_Init: WAD size 0x0000000100000100 bytes "
                           "exceeds the 67108864 byte window.\n");

    /* The bug this replaced: the low half alone reads as a valid size. */
    CU_ASSERT_EQUAL((unsigned)(size & 0xFFFFFFFFu), 256u);
}

void suite_dg_init_tests(CU_pSuite s)
{
    CU_add_test(s, "mount accepts IWAD", test_mount_accepts_iwad);
    CU_add_test(s, "mount accepts PWAD and flags it",
                test_mount_accepts_pwad_and_flags_it);
    CU_add_test(s, "mount rejects missing module",
                test_mount_rejects_missing_module);
    CU_add_test(s, "mount rejects oversize", test_mount_rejects_oversize);
    CU_add_test(s, "mount rejects bad magic", test_mount_rejects_bad_magic);
    CU_add_test(s, "mount rejects bad directory",
                test_mount_rejects_bad_directory);
    CU_add_test(s, "failed mount leaves nothing behind",
                test_failed_mount_leaves_nothing_behind);
    CU_add_test(s, "strerror is always a string",
                test_strerror_is_always_a_string);
    CU_add_test(s, "DG_Init mounts a good WAD", test_dg_init_mounts_a_good_wad);
    CU_add_test(s, "DG_Init reports unpatched params",
                test_dg_init_reports_unpatched_params);
    CU_add_test(s, "DG_Init reports oversize before truncating",
                test_dg_init_reports_oversize_before_truncating);
    CU_add_test(s, "DG_Init reports a bad WAD", test_dg_init_reports_a_bad_wad);
    CU_add_test(s, "oversize message keeps all 64 bits",
                test_oversize_message_keeps_all_64_bits);
}
