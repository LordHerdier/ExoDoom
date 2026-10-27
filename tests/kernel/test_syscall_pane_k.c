/*
 * test_syscall_pane_k.c — exo_pane_set (#42, SCRUM-296): a launcher places
 * the context it holds in the split screen before starting it.
 *
 * Every case drives the real handler through exo_syscall_dispatch(), on a
 * context built by a real held launch (#36 with EXO_LAUNCH_HOLD) of the
 * launch-probe package, the way test_launch_pkg_k.c does. The screen is a
 * synthetic geometry, as in test_fb_layout_k.c: fb_layout.c reads only the
 * published geometry, never the framebuffer's memory, and fb_shadow.c sizes
 * its buffer from the pane.
 */

#include "kunit.h"
#include "context.h"
#include "vmm.h"
#include "page_alloc.h"
#include "libos_launch.h"
#include "exo_pkg.h"
#include "exo_syscall.h"
#include "syscall.h"
#include "string.h"
#include "fb_binding.h"
#include "fb_layout.h"
#include "fb_shadow.h"
#include "revoke.h"

#include <stdint.h>

extern void libos_launch_probe(void);
extern void libos_launch_probe_end(void);

/* Apart from the other launch suites' scratch ranges (test_launch_pkg_k.c
 * +0x2F000000, test_launch_multi_k.c +0x2F100000). */
#define PANE_PKG_VADDR (EXO_USER_VA_BASE + 0x2F200000ULL)

#define TEST_FB_BASE   0x00000000e4000000ull
#define TEST_FB_WIDTH  640u
#define TEST_FB_HEIGHT 400u
#define TEST_FB_PITCH  (TEST_FB_WIDTH * 4u)
#define TEST_FB_BPP    32u

#define PANE_CLEANUP_SCAN_IDS 4096

static fb_geometry_t boot_geometry;
static int           boot_had_fb;

int syscall_pane_suite_init(void)
{
    const fb_geometry_t *g = fb_binding_geometry();
    boot_had_fb = (g != NULL);
    if (boot_had_fb)
        boot_geometry = *g;

    fb_geometry_t t = {
        .phys_addr = TEST_FB_BASE,
        .width     = TEST_FB_WIDTH,
        .height    = TEST_FB_HEIGHT,
        .pitch     = TEST_FB_PITCH,
        .bpp       = TEST_FB_BPP,
    };
    fb_binding_init(&t);
    fb_layout_reset();
    return 0;
}

/* Same shape as launch_pkg_suite_cleanup(): a failed assertion mid-test must
 * not leak a context, a pane or the synthetic screen into a later suite. */
int syscall_pane_suite_cleanup(void)
{
    for (uint32_t offset = 0; offset < PANE_CLEANUP_SCAN_IDS; offset++) {
        page_owner_t id = (page_owner_t)(PAGE_OWNER_LIBOS + offset);
        if (id != PAGE_OWNER_LIBOS && context_lookup(id) != NULL) {
            fb_shadow_release(id);
            page_reclaim_all(id);
            context_destroy(id);
        }
    }
    context_switch_pending = 0;
    context_set_current(PAGE_OWNER_LIBOS);
    fb_layout_reset();
    fb_binding_init(boot_had_fb ? &boot_geometry : (const fb_geometry_t *)0);
    return 0;
}

static int64_t do_pane(uint64_t ctx, uint64_t x, uint64_t y, uint64_t w,
                       uint64_t h, uint64_t flags)
{
    return exo_syscall_dispatch(EXO_SYS_PANE_SET, ctx, x, y, w, h, flags);
}

static size_t probe_len(void)
{
    return (uintptr_t)&libos_launch_probe_end - (uintptr_t)&libos_launch_probe;
}

/* The scratch page the package is written to, owned by PAGE_OWNER_LIBOS (the
 * context the suite runs as) and mapped through the real #1/#2. */
static uint64_t map_package(size_t *total)
{
    int64_t paddr = exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
    CU_ASSERT_TRUE(paddr > 0);
    if (paddr <= 0)
        return 0;
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_PAGE_MAP, PANE_PKG_VADDR,
                                         (uint64_t)paddr,
                                         EXO_PAGE_READ | EXO_PAGE_WRITE |
                                             EXO_PAGE_USER, 0, 0, 0), 0);
    memset((void *)(uintptr_t)PANE_PKG_VADDR, 0, VMM_PAGE_SIZE);

    exo_pkg_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic             = EXO_PKG_MAGIC;
    h.version           = EXO_PKG_VERSION;
    h.code_len          = (uint32_t)probe_len();
    h.stack_pages       = EXO_PKG_MAX_STACK_PAGES;
    h.min_pages         = 20;
    h.recommended_pages = 36;
    CU_ASSERT_EQUAL(exo_pkg_pack((void *)(uintptr_t)PANE_PKG_VADDR,
                                 VMM_PAGE_SIZE, &h,
                                 (const void *)&libos_launch_probe, NULL,
                                 total), 0);
    return (uint64_t)paddr;
}

static void unmap_package(uint64_t paddr)
{
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, PANE_PKG_VADDR,
                                         0, 0, 0, 0, 0), 0);
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_PAGE_FREE, paddr,
                                         0, 0, 0, 0, 0), 0);
}

/* A real, live, current "launcher", as test_launch_pkg_k.c's
 * make_foreground() builds one. */
static page_owner_t make_launcher(void)
{
    page_owner_t id = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(vmm_kernel_pml4(), &id), CONTEXT_OK);
    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(id, (const void *)&libos_launch_probe,
                                      probe_len(), 0, 0, 0, &img), VMM_OK);
    CU_ASSERT_EQUAL(context_prime(id, img.entry_vaddr, img.stack_top_vaddr),
                    CONTEXT_OK);
    context_set_current(id);
    context_set_state(id, CONTEXT_STATE_RUNNING);
    return id;
}

static void drop_launcher(page_owner_t l)
{
    context_switch_pending = 0;
    context_set_current(PAGE_OWNER_LIBOS);
    revoke_all(l);
    context_destroy(l);
}

/* Hold a launch of the probe package as the current context; its id. */
static page_owner_t hold(size_t total)
{
    int64_t id = exo_syscall_dispatch(EXO_SYS_LAUNCH_PKG, PANE_PKG_VADDR, total,
                                      EXO_LAUNCH_HOLD, 0, 0, 0);
    CU_ASSERT_TRUE(id > 0);
    return id > 0 ? (page_owner_t)id : PAGE_OWNER_FREE;
}

static void rect_is(page_owner_t who, uint32_t x, uint32_t y, uint32_t w,
                    uint32_t h)
{
    fb_rect_t r;
    CU_ASSERT_EQUAL(fb_layout_get(who, &r), 0);
    CU_ASSERT_EQUAL(r.x, x);
    CU_ASSERT_EQUAL(r.y, y);
    CU_ASSERT_EQUAL(r.w, w);
    CU_ASSERT_EQUAL(r.h, h);
}

/* The acceptance case: hold, place in the top half, start; the program's
 * framebuffer is the pane's size with a tight pitch. */
static void test_held_context_gets_a_pane_sized_framebuffer(void)
{
    size_t total = 0;
    uint64_t paddr = map_package(&total);
    page_owner_t l = make_launcher();
    page_owner_t c = hold(total);

    CU_ASSERT_EQUAL(do_pane(c, 0, 0, TEST_FB_WIDTH, TEST_FB_HEIGHT / 2, 0), 0);
    rect_is(c, 0, 0, TEST_FB_WIDTH, TEST_FB_HEIGHT / 2);

    /* Moving and resizing before the program runs is fine. */
    CU_ASSERT_EQUAL(do_pane(c, 0, TEST_FB_HEIGHT / 2, TEST_FB_WIDTH,
                            TEST_FB_HEIGHT / 2, 0), 0);
    rect_is(c, 0, TEST_FB_HEIGHT / 2, TEST_FB_WIDTH, TEST_FB_HEIGHT / 2);

    /* Start it; the pane stays. What its exo_fb_acquire would report is
     * fb_shadow_acquire()'s answer for it. */
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_LAUNCH_START, c, 0, 0, 0, 0, 0),
                    0);
    context_switch_pending = 0;
    context_set_current(l);
    rect_is(c, 0, TEST_FB_HEIGHT / 2, TEST_FB_WIDTH, TEST_FB_HEIGHT / 2);

    exo_fb_info_t info;
    CU_ASSERT_EQUAL(fb_shadow_acquire(c, &info), FB_SHADOW_OK);
    CU_ASSERT_EQUAL(info.width,  TEST_FB_WIDTH);
    CU_ASSERT_EQUAL(info.height, TEST_FB_HEIGHT / 2);
    CU_ASSERT_EQUAL(info.pitch,  TEST_FB_WIDTH * 4u);

    /* Once started it is no longer a hold: placing it is not a launcher's
     * to do any more. */
    CU_ASSERT_EQUAL(do_pane(c, 0, 0, 10, 10, 0), -EXO_ENOENT);

    /* Exit/revocation takes the pane with the framebuffer. */
    fb_shadow_release(c);
    fb_rect_t r;
    CU_ASSERT_NOT_EQUAL(fb_layout_get(c, &r), 0);

    revoke_all(c);
    context_destroy(c);
    drop_launcher(l);
    unmap_package(paddr);
}

/* Someone else's hold: -EPERM, and the pane is unchanged. */
static void test_only_the_launcher_may_place_its_hold(void)
{
    size_t total = 0;
    uint64_t paddr = map_package(&total);
    page_owner_t l = make_launcher();
    page_owner_t c = hold(total);
    CU_ASSERT_EQUAL(do_pane(c, 0, 0, 100, 100, 0), 0);

    page_owner_t other = make_launcher();      /* now the current context */
    CU_ASSERT_EQUAL(do_pane(c, 10, 10, 50, 50, 0), -EXO_EPERM);
    CU_ASSERT_EQUAL(do_pane(c, 0, 0, 0, 0, EXO_PANE_CLEAR), -EXO_EPERM);
    rect_is(c, 0, 0, 100, 100);

    /* The program itself is not its own launcher either. */
    context_set_current(c);
    CU_ASSERT_EQUAL(do_pane(c, 10, 10, 50, 50, 0), -EXO_EPERM);
    rect_is(c, 0, 0, 100, 100);

    context_set_current(other);
    revoke_all(other);
    context_destroy(other);
    context_set_current(l);
    revoke_all(c);
    context_destroy(c);
    drop_launcher(l);
    unmap_package(paddr);
}

static void test_unknown_context_is_enoent(void)
{
    page_owner_t l = make_launcher();
    CU_ASSERT_EQUAL(do_pane(l, 0, 0, 10, 10, 0), -EXO_ENOENT);   /* not a hold */
    CU_ASSERT_EQUAL(do_pane(0, 0, 0, 10, 10, 0), -EXO_ENOENT);
    CU_ASSERT_EQUAL(do_pane(0xFFFFull, 0, 0, 10, 10, 0), -EXO_ENOENT);
    CU_ASSERT_EQUAL(do_pane(1ull << 32, 0, 0, 10, 10, 0), -EXO_ENOENT);
    CU_ASSERT_EQUAL(fb_layout_count(), 0);
    drop_launcher(l);
}

static void test_bad_rects_are_einval(void)
{
    size_t total = 0;
    uint64_t paddr = map_package(&total);
    page_owner_t l = make_launcher();
    page_owner_t c = hold(total);

    CU_ASSERT_EQUAL(do_pane(c, 0, 0, 0, 10, 0), -EXO_EINVAL);        /* empty */
    CU_ASSERT_EQUAL(do_pane(c, 0, 0, 10, 0, 0), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_pane(c, 600, 0, 41, 10, 0), -EXO_EINVAL);     /* off right */
    CU_ASSERT_EQUAL(do_pane(c, 0, 399, 10, 2, 0), -EXO_EINVAL);      /* off bottom */
    CU_ASSERT_EQUAL(do_pane(c, (1ull << 32) | 0, 0, 10, 10, 0), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_pane(c, 0, 0, (1ull << 32) | 10, 10, 0), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_pane(c, 0, 0, 10, 10, 2), -EXO_EINVAL);       /* flag */
    fb_rect_t r;
    CU_ASSERT_NOT_EQUAL(fb_layout_get(c, &r), 0);

    /* No framebuffer at all: nothing to be a pane of. */
    fb_binding_init((const fb_geometry_t *)0);
    CU_ASSERT_EQUAL(do_pane(c, 0, 0, 10, 10, 0), -EXO_EINVAL);
    syscall_pane_suite_init();

    revoke_all(c);
    context_destroy(c);
    drop_launcher(l);
    unmap_package(paddr);
}

static void test_full_table_is_enospc(void)
{
    size_t total = 0;
    uint64_t paddr = map_package(&total);
    page_owner_t l = make_launcher();
    page_owner_t c = hold(total);

    /* Fill every pane with stand-in owners that are not live contexts. */
    for (int i = 0; i < FB_LAYOUT_MAX_PANES; i++) {
        fb_rect_t r = { (uint32_t)i * 10u, 0, 10, 10 };
        CU_ASSERT_EQUAL(fb_layout_set((page_owner_t)(PAGE_OWNER_LIBOS + 3000 + i),
                                      &r), FB_LAYOUT_OK);
    }
    CU_ASSERT_EQUAL(do_pane(c, 0, 100, 10, 10, 0), -EXO_ENOSPC);
    fb_layout_reset();
    CU_ASSERT_EQUAL(do_pane(c, 0, 100, 10, 10, 0), 0);

    revoke_all(c);
    context_destroy(c);
    drop_launcher(l);
    unmap_package(paddr);
}

/* A context that already holds a framebuffer of another size cannot be
 * resized under it. */
static void test_resize_after_acquire_is_ebusy(void)
{
    size_t total = 0;
    uint64_t paddr = map_package(&total);
    page_owner_t l = make_launcher();
    page_owner_t c = hold(total);

    CU_ASSERT_EQUAL(do_pane(c, 0, 0, 320, 200, 0), 0);
    exo_fb_info_t info;
    CU_ASSERT_EQUAL(fb_shadow_acquire(c, &info), FB_SHADOW_OK);
    CU_ASSERT_EQUAL(do_pane(c, 0, 0, 640, 200, 0), -EXO_EBUSY);
    rect_is(c, 0, 0, 320, 200);
    CU_ASSERT_EQUAL(do_pane(c, 320, 200, 320, 200, 0), 0);    /* a move is fine */
    rect_is(c, 320, 200, 320, 200);

    fb_shadow_release(c);
    revoke_all(c);
    context_destroy(c);
    drop_launcher(l);
    unmap_package(paddr);
}

/* Undoing a placement, and dropping a hold that was never started: neither
 * leaves a pane behind for a dead or reused id. */
static void test_clear_and_dropped_hold_leave_no_pane(void)
{
    size_t total = 0;
    uint64_t paddr = map_package(&total);
    page_owner_t l = make_launcher();
    page_owner_t c = hold(total);
    fb_rect_t r;

    CU_ASSERT_EQUAL(do_pane(c, 0, 0, 100, 100, 0), 0);
    CU_ASSERT_EQUAL(do_pane(c, 0, 0, 0, 0, EXO_PANE_CLEAR), 0);
    CU_ASSERT_NOT_EQUAL(fb_layout_get(c, &r), 0);
    CU_ASSERT_EQUAL(do_pane(c, 0, 0, 0, 0, EXO_PANE_CLEAR), 0);   /* none: ok */

    /* Placed again, then abandoned: the launcher's next launch drops the
     * unstarted hold, and the pane with it. */
    CU_ASSERT_EQUAL(do_pane(c, 0, 0, 100, 100, 0), 0);
    CU_ASSERT_EQUAL(fb_layout_count(), 1);
    page_owner_t c2 = hold(total);
    CU_ASSERT_NOT_EQUAL(c2, PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(context_lookup(c) == NULL || c2 == c, 1);
    if (c2 != c)
        CU_ASSERT_NOT_EQUAL(fb_layout_get(c, &r), 0);
    CU_ASSERT_NOT_EQUAL(fb_layout_get(c2, &r), 0);  /* the new hold has none */
    CU_ASSERT_EQUAL(fb_layout_count(), 0);

    revoke_all(c2);
    context_destroy(c2);
    drop_launcher(l);
    unmap_package(paddr);
}

void suite_syscall_pane_tests(CU_pSuite s)
{
    CU_add_test(s, "held context gets a pane-sized framebuffer",
                test_held_context_gets_a_pane_sized_framebuffer);
    CU_add_test(s, "only the launcher may place its hold",
                test_only_the_launcher_may_place_its_hold);
    CU_add_test(s, "unknown context is ENOENT", test_unknown_context_is_enoent);
    CU_add_test(s, "bad rects are EINVAL", test_bad_rects_are_einval);
    CU_add_test(s, "full pane table is ENOSPC", test_full_table_is_enospc);
    CU_add_test(s, "resize after acquire is EBUSY",
                test_resize_after_acquire_is_ebusy);
    CU_add_test(s, "clear and a dropped hold leave no pane",
                test_clear_and_dropped_hold_leave_no_pane);
}
