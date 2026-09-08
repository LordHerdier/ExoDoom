/*
 * test_fb_binding_k.c — framebuffer secure binding (SCRUM-154).
 *
 * The framebuffer is the second resource to get an owner tag, after physical
 * pages (SCRUM-152 / test_ownership_k.c).  These tests cover the three halves
 * of the binding named in docs/syscall_spec.md §3.3:
 *
 *   establish — exo_fb_acquire records the caller as owner and answers
 *               -EXO_EBUSY to anyone else;
 *   enforce   — fb_binding_check_map, the permission gate SCRUM-153's
 *               exo_page_map consults before mapping a physical page;
 *   reclaim   — fb_binding_release, the hook SCRUM-155's exo_exit calls, after
 *               which a different context can acquire.
 *
 * Most tests install a synthetic geometry rather than using the machine's real
 * framebuffer, so the address arithmetic is exact and the assertions do not
 * depend on the video mode GRUB happened to pick.  The suite's init/cleanup
 * pair snapshots and restores whatever kernel_main published, so the rest of
 * the boot (and the framebuffer console) is unaffected.
 */

#include "kunit.h"
#include "fb_binding.h"
#include "page_alloc.h"
#include "syscall.h"
#include "exo_syscall.h"

#include <stdint.h>

/* A second, distinct LibOS id — the "another LibOS" of §3.2 #4.  v1 only ever
 * runs PAGE_OWNER_LIBOS; this id stands in as a foreign context. */
#define OTHER_LIBOS ((page_owner_t)(PAGE_OWNER_LIBOS + 1))

/* Synthetic framebuffer: 1024x768x32 at a base far above the RAM QEMU gives
 * us, so nothing here can collide with a real page.  pitch * height is a whole
 * number of 4 KiB pages, which keeps the boundary arithmetic below obvious. */
#define TEST_FB_BASE    0x00000000e0000000ull
#define TEST_FB_WIDTH   1024u
#define TEST_FB_HEIGHT  768u
#define TEST_FB_PITCH   4096u
#define TEST_FB_BPP     32u
#define TEST_FB_SIZE    ((uint64_t)TEST_FB_PITCH * TEST_FB_HEIGHT)

static fb_geometry_t boot_geometry;
static int           boot_had_fb;

/* Suite init/cleanup, passed to CU_add_suite in test_runner.c. */
int fb_binding_suite_init(void)
{
    const fb_geometry_t *g = fb_binding_geometry();

    boot_had_fb = (g != NULL);
    if (boot_had_fb)
        boot_geometry = *g;

    return 0;
}

int fb_binding_suite_cleanup(void)
{
    /* Put the real framebuffer back and drop any binding the tests took, so a
     * normal boot continues with the screen unowned. */
    fb_binding_init(boot_had_fb ? &boot_geometry : (const fb_geometry_t *)0);

    return 0;
}

/* Publish the synthetic framebuffer.  fb_binding_init() also clears the owner,
 * so every test that calls this starts from "unheld". */
static void install_test_fb(void)
{
    fb_geometry_t g = {
        .phys_addr = TEST_FB_BASE,
        .width     = TEST_FB_WIDTH,
        .height    = TEST_FB_HEIGHT,
        .pitch     = TEST_FB_PITCH,
        .bpp       = TEST_FB_BPP,
    };

    CU_ASSERT_EQUAL(fb_binding_init(&g), FB_BIND_OK);
}

static int64_t do_fb_acquire(exo_fb_info_t *info)
{
    return exo_syscall_dispatch(EXO_SYS_FB_ACQUIRE,
                                (uint64_t)(uintptr_t)info, 0, 0, 0, 0, 0);
}

/* ── Boot wiring ─────────────────────────────────────────────────────────── */

/* kernel_main must have bound #4 and published the machine's framebuffer;
 * without both, every assertion below would only be exercising the
 * dispatcher's -EXO_ENOSYS fallback. */
static void test_boot_published_the_framebuffer(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_FB_ACQUIRE));

    CU_ASSERT_TRUE(boot_had_fb);
    if (!boot_had_fb)
        return;

    CU_ASSERT_NOT_EQUAL(boot_geometry.phys_addr, 0);
    CU_ASSERT_NOT_EQUAL(boot_geometry.width, 0);
    CU_ASSERT_NOT_EQUAL(boot_geometry.height, 0);
    CU_ASSERT_TRUE(boot_geometry.pitch >= boot_geometry.width);
    /* Deliberately not pinned to 32: the video mode GRUB negotiates is
     * fb_init_bgrx8888()'s business, not the binding table's. */
    CU_ASSERT_NOT_EQUAL(boot_geometry.bpp, 0);
}

/* ── Establish ───────────────────────────────────────────────────────────── */

/* Acquire hands back the published geometry and records the caller as owner. */
static void test_acquire_binds_and_describes(void)
{
    install_test_fb();
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);

    exo_fb_info_t info = { 0, 0, 0, 0, 0, { 0xAA, 0xBB, 0xCC } };

    CU_ASSERT_EQUAL(do_fb_acquire(&info), 0);
    CU_ASSERT_EQUAL(fb_binding_owner(), syscall_current_context());

    CU_ASSERT_EQUAL(info.phys_addr, TEST_FB_BASE);
    CU_ASSERT_EQUAL(info.width,     TEST_FB_WIDTH);
    CU_ASSERT_EQUAL(info.height,    TEST_FB_HEIGHT);
    CU_ASSERT_EQUAL(info.pitch,     TEST_FB_PITCH);
    CU_ASSERT_EQUAL(info.bpp,       TEST_FB_BPP);

    /* ABI: the kernel zeroes the padding instead of handing the caller's own
     * bytes back as if they were kernel data. */
    CU_ASSERT_EQUAL(info.reserved[0], 0);
    CU_ASSERT_EQUAL(info.reserved[1], 0);
    CU_ASSERT_EQUAL(info.reserved[2], 0);
}

/* A framebuffer already held by another LibOS is -EXO_EBUSY, and the failed
 * acquire leaves the incumbent in place. */
static void test_second_acquirer_is_ebusy(void)
{
    install_test_fb();
    CU_ASSERT_EQUAL(fb_binding_acquire(OTHER_LIBOS), FB_BIND_OK);

    exo_fb_info_t info;

    CU_ASSERT_EQUAL(do_fb_acquire(&info), -EXO_EBUSY);
    CU_ASSERT_EQUAL(fb_binding_owner(), OTHER_LIBOS);
}

/* Re-acquiring what you already hold is not "another LibOS holds it": it
 * succeeds and re-fills the struct, so a LibOS re-running DG_Init is not
 * locked out of its own screen. */
static void test_owner_may_reacquire(void)
{
    install_test_fb();

    exo_fb_info_t first  = { 0, 0, 0, 0, 0, { 0, 0, 0 } };
    exo_fb_info_t second = { 0, 0, 0, 0, 0, { 0, 0, 0 } };

    CU_ASSERT_EQUAL(do_fb_acquire(&first), 0);
    CU_ASSERT_EQUAL(do_fb_acquire(&second), 0);

    CU_ASSERT_EQUAL(second.phys_addr, first.phys_addr);
    CU_ASSERT_EQUAL(fb_binding_owner(), syscall_current_context());
}

/* A bad info_out is rejected before the binding is taken — otherwise a caller
 * that passed garbage would own a screen it never received a handle to. */
static void test_null_info_faults_without_binding(void)
{
    install_test_fb();

    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_FB_ACQUIRE, 0, 0, 0, 0, 0, 0),
                    -EXO_EFAULT);
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);
}

/* Nobody can bind the framebuffer to PAGE_OWNER_FREE: that is the "unheld"
 * sentinel, and a binding to it would look free to the next caller. */
static void test_free_sentinel_cannot_own(void)
{
    install_test_fb();

    CU_ASSERT_EQUAL(fb_binding_acquire(PAGE_OWNER_FREE), FB_BIND_EBUSY);
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);
}

/* A machine with no framebuffer reports -EXO_ENODEV, not -EXO_ENOSYS: the
 * syscall exists, the hardware does not. */
static void test_headless_reports_enodev(void)
{
    CU_ASSERT_EQUAL(fb_binding_init((const fb_geometry_t *)0), FB_BIND_ENODEV);
    CU_ASSERT_PTR_NULL(fb_binding_geometry());

    exo_fb_info_t info;

    CU_ASSERT_EQUAL(do_fb_acquire(&info), -EXO_ENODEV);
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);

    /* With no framebuffer, no physical address is framebuffer memory, so the
     * map gate must not deny anything on its behalf. */
    CU_ASSERT_EQUAL(fb_binding_check_map(TEST_FB_BASE, PAGE_OWNER_LIBOS),
                    FB_MAP_NOT_FB);
}

/* A geometry that cannot describe a real framebuffer is refused outright,
 * rather than being published as a zero-length or wrapped range that
 * fb_binding_contains() would then have to reason about. */
static void test_degenerate_geometry_refused(void)
{
    fb_geometry_t g = {
        .phys_addr = TEST_FB_BASE,
        .width     = TEST_FB_WIDTH,
        .height    = TEST_FB_HEIGHT,
        .pitch     = TEST_FB_PITCH,
        .bpp       = TEST_FB_BPP,
    };

    g.height = 0;
    CU_ASSERT_EQUAL(fb_binding_init(&g), FB_BIND_ENODEV);
    g.height = TEST_FB_HEIGHT;

    g.pitch = 0;
    CU_ASSERT_EQUAL(fb_binding_init(&g), FB_BIND_ENODEV);
    g.pitch = TEST_FB_PITCH;

    g.phys_addr = 0;
    CU_ASSERT_EQUAL(fb_binding_init(&g), FB_BIND_ENODEV);
    g.phys_addr = TEST_FB_BASE;

    /* An extent that runs off the end of the physical address space would
     * wrap and make every address "framebuffer memory". */
    g.phys_addr = UINT64_MAX - 4096;
    CU_ASSERT_EQUAL(fb_binding_init(&g), FB_BIND_ENODEV);

    CU_ASSERT_PTR_NULL(fb_binding_geometry());
}

/* ── Enforce: the exo_page_map gate (SCRUM-153) ──────────────────────────── */

/* The whole point of the binding: framebuffer pages are mappable only by the
 * LibOS that acquired them. */
static void test_map_of_fb_pages_needs_the_binding(void)
{
    install_test_fb();

    /* Unheld is not public property — the LibOS must acquire first. */
    CU_ASSERT_EQUAL(fb_binding_check_map(TEST_FB_BASE, PAGE_OWNER_LIBOS),
                    FB_MAP_DENY);

    CU_ASSERT_EQUAL(fb_binding_acquire(PAGE_OWNER_LIBOS), FB_BIND_OK);

    CU_ASSERT_EQUAL(fb_binding_check_map(TEST_FB_BASE, PAGE_OWNER_LIBOS),
                    FB_MAP_ALLOW);
    CU_ASSERT_EQUAL(fb_binding_check_map(TEST_FB_BASE + TEST_FB_SIZE - 1,
                                         PAGE_OWNER_LIBOS),
                    FB_MAP_ALLOW);

    /* Another LibOS gets nothing, and neither does the kernel: there is no
     * bypass, only the binding. */
    CU_ASSERT_EQUAL(fb_binding_check_map(TEST_FB_BASE, OTHER_LIBOS),
                    FB_MAP_DENY);
    CU_ASSERT_EQUAL(fb_binding_check_map(TEST_FB_BASE, PAGE_OWNER_KERNEL),
                    FB_MAP_DENY);
    CU_ASSERT_EQUAL(fb_binding_check_map(TEST_FB_BASE, PAGE_OWNER_FREE),
                    FB_MAP_DENY);
}

/* Addresses outside the framebuffer are not this table's business: the caller
 * falls through to generic page ownership for them. */
static void test_non_fb_addresses_fall_through(void)
{
    install_test_fb();
    CU_ASSERT_EQUAL(fb_binding_acquire(PAGE_OWNER_LIBOS), FB_BIND_OK);

    CU_ASSERT_EQUAL(fb_binding_check_map(TEST_FB_BASE - 4096, PAGE_OWNER_LIBOS),
                    FB_MAP_NOT_FB);
    CU_ASSERT_EQUAL(fb_binding_check_map(TEST_FB_BASE + TEST_FB_SIZE,
                                         PAGE_OWNER_LIBOS),
                    FB_MAP_NOT_FB);

    /* Owning the framebuffer grants nothing anywhere else. */
    CU_ASSERT_EQUAL(fb_binding_check_map(0x100000ull, PAGE_OWNER_LIBOS),
                    FB_MAP_NOT_FB);
}

/* Permission is decided per 4 KiB page, so a framebuffer whose base is not
 * page-aligned still covers its first and last partial pages. */
static void test_extent_is_page_granular(void)
{
    fb_geometry_t g = {
        .phys_addr = TEST_FB_BASE + 0x10,   /* deliberately unaligned */
        .width     = TEST_FB_WIDTH,
        .height    = TEST_FB_HEIGHT,
        .pitch     = TEST_FB_PITCH,
        .bpp       = TEST_FB_BPP,
    };

    CU_ASSERT_EQUAL(fb_binding_init(&g), FB_BIND_OK);

    /* The slop before the base shares a page with it, so it is framebuffer. */
    CU_ASSERT_TRUE(fb_binding_contains(TEST_FB_BASE));
    CU_ASSERT_TRUE(fb_binding_contains(TEST_FB_BASE + 0x10));
    /* Last byte in use, and the rest of the page it sits in. */
    CU_ASSERT_TRUE(fb_binding_contains(TEST_FB_BASE + 0x10 + TEST_FB_SIZE - 1));
    CU_ASSERT_TRUE(fb_binding_contains(TEST_FB_BASE + TEST_FB_SIZE + 0xFFF));
    /* One page past the rounded-up end is not. */
    CU_ASSERT_FALSE(fb_binding_contains(TEST_FB_BASE + TEST_FB_SIZE + 0x1000));
    CU_ASSERT_FALSE(fb_binding_contains(TEST_FB_BASE - 1));
}

/* ── Reclaim ─────────────────────────────────────────────────────────────── */

/* SCRUM-155's exo_exit calls this: after the owner is torn down the screen is
 * free and a different LibOS can take it.  A LibOS that dies holding the
 * framebuffer must not lock the display for the rest of the boot. */
static void test_release_lets_another_context_acquire(void)
{
    install_test_fb();
    CU_ASSERT_EQUAL(fb_binding_acquire(OTHER_LIBOS), FB_BIND_OK);

    exo_fb_info_t info;
    CU_ASSERT_EQUAL(do_fb_acquire(&info), -EXO_EBUSY);

    fb_binding_release(OTHER_LIBOS);
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);

    CU_ASSERT_EQUAL(do_fb_acquire(&info), 0);
    CU_ASSERT_EQUAL(info.phys_addr, TEST_FB_BASE);
    CU_ASSERT_EQUAL(fb_binding_owner(), syscall_current_context());

    /* And the new owner can map it, which the previous one no longer can. */
    CU_ASSERT_EQUAL(fb_binding_check_map(TEST_FB_BASE, syscall_current_context()),
                    FB_MAP_ALLOW);
    CU_ASSERT_EQUAL(fb_binding_check_map(TEST_FB_BASE, OTHER_LIBOS),
                    FB_MAP_DENY);
}

/* Release is scoped to the owner: reclaiming context A must not hand away the
 * framebuffer context B is holding, and releasing what you do not hold is a
 * no-op so reclamation can call it unconditionally. */
static void test_release_by_non_owner_is_a_noop(void)
{
    install_test_fb();
    CU_ASSERT_EQUAL(fb_binding_acquire(OTHER_LIBOS), FB_BIND_OK);

    fb_binding_release(PAGE_OWNER_LIBOS);
    CU_ASSERT_EQUAL(fb_binding_owner(), OTHER_LIBOS);

    fb_binding_release(PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(fb_binding_owner(), OTHER_LIBOS);

    fb_binding_release(OTHER_LIBOS);
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);
}

void suite_fb_binding_tests(CU_pSuite s)
{
    CU_add_test(s, "boot published the framebuffer",
                test_boot_published_the_framebuffer);
    CU_add_test(s, "acquire binds and describes",
                test_acquire_binds_and_describes);
    CU_add_test(s, "second acquirer gets EBUSY",
                test_second_acquirer_is_ebusy);
    CU_add_test(s, "owner may re-acquire", test_owner_may_reacquire);
    CU_add_test(s, "null info faults without binding",
                test_null_info_faults_without_binding);
    CU_add_test(s, "FREE sentinel cannot own", test_free_sentinel_cannot_own);
    CU_add_test(s, "headless reports ENODEV", test_headless_reports_enodev);
    CU_add_test(s, "degenerate geometry refused",
                test_degenerate_geometry_refused);
    CU_add_test(s, "map of FB pages needs the binding",
                test_map_of_fb_pages_needs_the_binding);
    CU_add_test(s, "non-FB addresses fall through",
                test_non_fb_addresses_fall_through);
    CU_add_test(s, "extent is page granular", test_extent_is_page_granular);
    CU_add_test(s, "release lets another context acquire",
                test_release_lets_another_context_acquire);
    CU_add_test(s, "release by non-owner is a no-op",
                test_release_by_non_owner_is_a_noop);
}
