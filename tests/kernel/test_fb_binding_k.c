/*
 * test_fb_binding_k.c — framebuffer secure binding (SCRUM-154), fb_binding.c
 * itself.
 *
 * SCRUM-112 stopped exo_fb_acquire from ever calling fb_binding_acquire():
 * every caller now gets its own private virtual framebuffer
 * (src/fb_shadow.c, test_fb_shadow_k.c) instead of exclusive access to the
 * real one, so the real binding is permanently unheld in normal operation.
 * fb_binding.c's establish/enforce/reclaim API is otherwise unchanged and
 * still exactly what fb_binding_check_map() (SCRUM-153's exo_page_map gate)
 * relies on to keep the real framebuffer unmappable by any LibOS — these
 * tests exercise that API directly (fb_binding_acquire/_release/_check_map)
 * rather than through the syscall, which is the only thing that changed:
 * they are defense-in-depth coverage of the module itself now, not of what
 * exo_fb_acquire does with it. The handful of tests that *did* exercise
 * establish/reclaim through the real exo_fb_acquire dispatch were rewritten
 * for the new contract (no more -EXO_EBUSY, no more binding taken) rather
 * than removed outright.
 *
 * Most tests install a synthetic geometry rather than using the machine's real
 * framebuffer, so the address arithmetic is exact and the assertions do not
 * depend on the video mode GRUB happened to pick.  The suite's init/cleanup
 * pair snapshots and restores whatever kernel_main published, so the rest of
 * the boot (and the framebuffer console) is unaffected.
 *
 * exo_fb_acquire's info_out is now bounds-checked against the LibOS window
 * (SCRUM-54), so a real call through do_fb_acquire() needs a buffer inside
 * that window rather than an ordinary kernel-stack local — the same reason
 * test_syscall_serial_k.c's valid-write case maps a scratch page instead of
 * using a local array.  The suite maps one scratch page once in
 * fb_binding_suite_init() and every test that needs a real info_out reuses
 * it; tests that deliberately probe the bounds check (NULL, a kernel address,
 * a straddling address) still pass a raw address of their own choosing.
 */

#include "kunit.h"
#include "fb_binding.h"
#include "fb_shadow.h"
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

/* Scratch virtual address for a real info_out, apart from every other suite's
 * range (test_page_map_k.c: +0x20000000/+0x24000000, test_vmm_k.c:
 * +0x10000000, test_syscall_serial_k.c: +0x28000000). */
#define SCRATCH_INFO_VA (EXO_USER_VA_BASE + 0x2C000000ULL)

/* A second scratch address in this suite's own range, one page past
 * SCRATCH_INFO_VA, that fb_binding_suite_init() never maps -- the
 * in-window-but-unmapped pointer SCRUM-186 reproduces the crash with. */
#define UNMAPPED_INFO_VA (SCRATCH_INFO_VA + 0x1000ULL)

static fb_geometry_t boot_geometry;
static int           boot_had_fb;
static uint64_t      scratch_paddr;

/* Suite init/cleanup, passed to CU_add_suite in test_runner.c. */
int fb_binding_suite_init(void)
{
    const fb_geometry_t *g = fb_binding_geometry();

    boot_had_fb = (g != NULL);
    if (boot_had_fb)
        boot_geometry = *g;

    int64_t p = exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
    if (p <= 0)
        return -1;

    scratch_paddr = (uint64_t)p;
    if (exo_syscall_dispatch(EXO_SYS_PAGE_MAP, SCRATCH_INFO_VA, scratch_paddr,
                             EXO_PAGE_WRITE | EXO_PAGE_USER, 0, 0, 0) != 0)
        return -1;

    return 0;
}

int fb_binding_suite_cleanup(void)
{
    /* Put the real framebuffer back and drop any binding the tests took, so a
     * normal boot continues with the screen unowned. */
    fb_binding_init(boot_had_fb ? &boot_geometry : (const fb_geometry_t *)0);

    /* SCRUM-112: several tests below now call do_fb_acquire(), which
     * allocates a real shadow framebuffer (hundreds of PMM pages) owned by
     * whichever id acquired it. Drop the directory entries and return the
     * pages, so a later suite's own allocations are not competing with
     * leftover multi-hundred-page runs this suite made. fb_shadow_release()
     * frees exactly those pages itself now (SCRUM-187) -- no caller-side
     * reclaim_pages_owned() sweep needed, and using one here would risk
     * freeing this suite's own scratch_paddr below (the exact bug this
     * ticket fixed). */
    fb_shadow_release(syscall_current_context());
    fb_shadow_release(OTHER_LIBOS);

    exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, SCRATCH_INFO_VA, 0, 0, 0, 0, 0);
    exo_syscall_dispatch(EXO_SYS_PAGE_FREE, scratch_paddr, 0, 0, 0, 0, 0);

    return 0;
}

/* A real, in-window info_out every test that exercises a successful acquire
 * can share — see the file header for why a kernel-stack local no longer
 * works here. */
static exo_fb_info_t *scratch_info(void)
{
    return (exo_fb_info_t *)(uintptr_t)SCRATCH_INFO_VA;
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

/* ── Establish (SCRUM-112: now fb_shadow.c's job, not a binding) ──────────── */

/* Acquire hands back the published geometry, backed by the caller's own
 * private buffer rather than the real framebuffer — so the real binding
 * stays unheld, and the returned phys_addr is not the real FB's base. */
static void test_acquire_describes_a_private_buffer(void)
{
    install_test_fb();
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);

    exo_fb_info_t *info = scratch_info();
    *info = (exo_fb_info_t){ 0, 0, 0, 0, 0, { 0xAA, 0xBB, 0xCC } };

    CU_ASSERT_EQUAL(do_fb_acquire(info), 0);

    /* Nobody ever acquires the real binding anymore (src/syscall_fb.c). */
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);

    CU_ASSERT_NOT_EQUAL(info->phys_addr, TEST_FB_BASE);
    CU_ASSERT_EQUAL(info->width,     TEST_FB_WIDTH);
    CU_ASSERT_EQUAL(info->height,    TEST_FB_HEIGHT);
    CU_ASSERT_EQUAL(info->pitch,     TEST_FB_PITCH);
    CU_ASSERT_EQUAL(info->bpp,       TEST_FB_BPP);

    /* ABI: the kernel zeroes the padding instead of handing the caller's own
     * bytes back as if they were kernel data. */
    CU_ASSERT_EQUAL(info->reserved[0], 0);
    CU_ASSERT_EQUAL(info->reserved[1], 0);
    CU_ASSERT_EQUAL(info->reserved[2], 0);

    fb_shadow_release(syscall_current_context());
}

/* A second, distinct caller no longer gets -EXO_EBUSY: SCRUM-112 replaced
 * the single exclusive binding with a private buffer per caller. Simulating
 * "someone else already has one" via a direct fb_shadow_acquire() (the real
 * dispatch path always runs as this suite's one syscall_current_context())
 * proves the second acquirer still succeeds, with its own distinct
 * buffer. */
static void test_second_acquirer_also_succeeds(void)
{
    install_test_fb();

    exo_fb_info_t other_info;
    CU_ASSERT_EQUAL(fb_shadow_acquire(OTHER_LIBOS, &other_info), FB_SHADOW_OK);

    exo_fb_info_t *info = scratch_info();
    CU_ASSERT_EQUAL(do_fb_acquire(info), 0);

    CU_ASSERT_NOT_EQUAL(info->phys_addr, other_info.phys_addr);
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);

    fb_shadow_release(OTHER_LIBOS);
    fb_shadow_release(syscall_current_context());
}

/* Re-acquiring is idempotent: it succeeds and re-fills the struct with the
 * *same* buffer, so a LibOS re-running DG_Init is not handed a fresh, empty
 * surface it has to redraw from scratch. */
static void test_owner_may_reacquire(void)
{
    install_test_fb();

    exo_fb_info_t *info = scratch_info();

    CU_ASSERT_EQUAL(do_fb_acquire(info), 0);
    uint64_t first_phys_addr = info->phys_addr;

    CU_ASSERT_EQUAL(do_fb_acquire(info), 0);

    CU_ASSERT_EQUAL(info->phys_addr, first_phys_addr);

    fb_shadow_release(syscall_current_context());
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

/* A kernel-space info_out is rejected the same way NULL is (SCRUM-54): the
 * identity map means a kernel address is present and writable, so without
 * this check the kernel would write the struct into its own memory on
 * request rather than the caller's. */
static void test_kernel_address_info_faults_without_binding(void)
{
    install_test_fb();

    /* A real kernel address: the kernel image loads at 2M (src/linker.ld). */
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_FB_ACQUIRE, 0x200000, 0, 0,
                                         0, 0, 0),
                    -EXO_EFAULT);
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);

    /* One byte below the window. */
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_FB_ACQUIRE,
                                         EXO_USER_VA_BASE - 1, 0, 0, 0, 0, 0),
                    -EXO_EFAULT);
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);
}

/* A struct that starts inside the window but would straddle its end is
 * rejected too — exo_range_in_user_window checks base + len, not just base. */
static void test_info_straddling_window_end_faults(void)
{
    install_test_fb();

    uint64_t straddling = EXO_USER_VA_END - sizeof(exo_fb_info_t) + 1;

    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_FB_ACQUIRE, straddling, 0, 0,
                                         0, 0, 0),
                    -EXO_EFAULT);
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);
}

/* SCRUM-186: an in-window info_out that was never exo_page_map'd used to
 * reach the write in fb_shadow_acquire() and take a fatal supervisor-mode
 * page fault (the whole window is reserved-but-unmapped by default). This is
 * the exact crash repro -- it must now come back cleanly as -EXO_EFAULT
 * instead, with no binding taken. */
static void test_unmapped_info_faults_without_binding(void)
{
    install_test_fb();

    CU_ASSERT_EQUAL(do_fb_acquire((exo_fb_info_t *)(uintptr_t)UNMAPPED_INFO_VA),
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

    CU_ASSERT_EQUAL(do_fb_acquire(scratch_info()), -EXO_ENODEV);
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

/* fb_binding.c's own release/re-acquire mechanics, exercised directly
 * (SCRUM-112: nothing in the normal exo_fb_acquire path takes this binding
 * anymore, so there is no longer a real caller to route this through — see
 * this file's header comment). Kept as coverage of the module itself: it is
 * still what fb_binding_check_map() denies against, and still what
 * SCRUM-155's exo_exit calls unconditionally. */
static void test_release_lets_another_context_acquire(void)
{
    install_test_fb();
    page_owner_t me = syscall_current_context();

    CU_ASSERT_EQUAL(fb_binding_acquire(OTHER_LIBOS), FB_BIND_OK);
    CU_ASSERT_EQUAL(fb_binding_acquire(me), FB_BIND_EBUSY);

    fb_binding_release(OTHER_LIBOS);
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);

    CU_ASSERT_EQUAL(fb_binding_acquire(me), FB_BIND_OK);
    CU_ASSERT_EQUAL(fb_binding_owner(), me);

    /* And the new owner can map it, which the previous one no longer can. */
    CU_ASSERT_EQUAL(fb_binding_check_map(TEST_FB_BASE, me), FB_MAP_ALLOW);
    CU_ASSERT_EQUAL(fb_binding_check_map(TEST_FB_BASE, OTHER_LIBOS),
                    FB_MAP_DENY);

    fb_binding_release(me);
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

/* SCRUM-187 regression: fb_binding_suite_init()'s own scratch page
 * (scratch_paddr, mapped at SCRATCH_INFO_VA for this whole suite's life)
 * shares its owner id with the shadow framebuffer -- exo_page_alloc always
 * tags a page with syscall_current_context(), which is also who acquires the
 * shadow buffer below, and stays that way for the rest of the suite. Before
 * this ticket, every test's cleanup paired fb_shadow_release(me) with a
 * caller-side reclaim_pages_owned(me) sweep to return the shadow buffer's
 * pages -- but that sweep frees *every* page `me` owns, scratch_paddr
 * included, since scratch_paddr legitimately stays tagged `me` for the whole
 * suite. A later EXO_SYS_PAGE_ALLOC could then be handed that identical
 * physical page straight back (observed on serial as
 * "alloc p=0x1EC0000 scratch_paddr=0x1EC0000"). There is no fix that keeps
 * calling reclaim_pages_owned(me) here -- it is unconditionally dangerous
 * whenever `me` legitimately holds any other page, which this suite's own
 * scratch page always does. The actual fix is that fb_shadow_release() now
 * frees exactly the pages it allocated itself, so nothing needs to sweep by
 * owner id here at all: a plain fb_shadow_release(me), by itself, must
 * leave scratch_paddr untouched. */
static void test_shadow_release_does_not_free_unrelated_pages(void)
{
    install_test_fb();
    page_owner_t me = syscall_current_context();

    CU_ASSERT_EQUAL(page_owner((void *)(uintptr_t)scratch_paddr), me);

    exo_fb_info_t *info = scratch_info();
    CU_ASSERT_EQUAL(do_fb_acquire(info), 0);

    fb_shadow_release(me);

    CU_ASSERT_EQUAL(page_owner((void *)(uintptr_t)scratch_paddr), me);

    int64_t fresh = exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
    CU_ASSERT_TRUE(fresh > 0);
    CU_ASSERT_NOT_EQUAL((uint64_t)fresh, scratch_paddr);
    if (fresh > 0)
        exo_syscall_dispatch(EXO_SYS_PAGE_FREE, (uint64_t)fresh, 0, 0, 0, 0, 0);
}

void suite_fb_binding_tests(CU_pSuite s)
{
    CU_add_test(s, "boot published the framebuffer",
                test_boot_published_the_framebuffer);
    CU_add_test(s, "acquire describes a private buffer",
                test_acquire_describes_a_private_buffer);
    CU_add_test(s, "second acquirer also succeeds",
                test_second_acquirer_also_succeeds);
    CU_add_test(s, "owner may re-acquire", test_owner_may_reacquire);
    CU_add_test(s, "null info faults without binding",
                test_null_info_faults_without_binding);
    CU_add_test(s, "kernel address info faults without binding",
                test_kernel_address_info_faults_without_binding);
    CU_add_test(s, "info straddling window end faults",
                test_info_straddling_window_end_faults);
    CU_add_test(s, "unmapped info faults without binding",
                test_unmapped_info_faults_without_binding);
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
    CU_add_test(s, "shadow release does not free unrelated pages",
                test_shadow_release_does_not_free_unrelated_pages);
}
