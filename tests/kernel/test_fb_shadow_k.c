/*
 * test_fb_shadow_k.c — per-context virtual framebuffers (SCRUM-112).
 *
 * fb_shadow.c is what replaced fb_binding.c's exclusive establish/reclaim
 * half for exo_fb_acquire (src/syscall_fb.c): every context gets its own
 * RAM-backed buffer, sized and positioned by the real framebuffer's
 * published geometry (fb_binding_geometry(), still the one source of truth
 * for that — see test_fb_binding_k.c's own header for the split). These
 * tests drive fb_shadow.c directly, the same layering test_fb_binding_k.c
 * and test_ownership_k.c already use for their own modules.
 *
 * A synthetic geometry, not the machine's real one, keeps page-count math
 * exact and independent of the video mode GRUB negotiated.
 */

#include "kunit.h"
#include "fb_shadow.h"
#include "fb_binding.h"
#include "page_alloc.h"
#include "syscall.h"

#include <stdint.h>

#define OTHER_LIBOS ((page_owner_t)(PAGE_OWNER_LIBOS + 1))
#define THIRD_LIBOS ((page_owner_t)(PAGE_OWNER_LIBOS + 2))

/* Small on purpose (4 pages exactly): keeps each test's contiguous
 * allocation cheap against a bitmap PMM shared with every other suite. */
#define TEST_FB_BASE    0x00000000e4000000ull
#define TEST_FB_WIDTH   32u
#define TEST_FB_HEIGHT  4u
#define TEST_FB_PITCH   4096u   /* width * pitch => exactly 4 pages */
#define TEST_FB_BPP     32u

static fb_geometry_t boot_geometry;
static int           boot_had_fb;

int fb_shadow_suite_init(void)
{
    const fb_geometry_t *g = fb_binding_geometry();

    boot_had_fb = (g != NULL);
    if (boot_had_fb)
        boot_geometry = *g;

    return 0;
}

int fb_shadow_suite_cleanup(void)
{
    /* SCRUM-187: fb_shadow_release() now frees the pages it allocated
     * itself, so no caller-side reclaim_pages_owned() sweep is needed (or
     * wanted) here anymore — see fb_shadow.h's header comment. */
    fb_shadow_release(syscall_current_context());
    fb_shadow_release(OTHER_LIBOS);
    fb_shadow_release(THIRD_LIBOS);

    /* Put the real framebuffer back, exactly as test_fb_binding_k.c's own
     * cleanup does. */
    fb_binding_init(boot_had_fb ? &boot_geometry : (const fb_geometry_t *)0);

    return 0;
}

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

/* Acquire allocates a buffer matching the published geometry, zeroed. */
static void test_acquire_allocates_and_zeroes(void)
{
    install_test_fb();

    exo_fb_info_t info;
    CU_ASSERT_EQUAL(fb_shadow_acquire(PAGE_OWNER_LIBOS, &info), FB_SHADOW_OK);

    CU_ASSERT_EQUAL(info.width,  TEST_FB_WIDTH);
    CU_ASSERT_EQUAL(info.height, TEST_FB_HEIGHT);
    CU_ASSERT_EQUAL(info.pitch,  TEST_FB_PITCH);
    CU_ASSERT_EQUAL(info.bpp,    TEST_FB_BPP);
    CU_ASSERT_NOT_EQUAL(info.phys_addr, 0);

    /* Owned by the caller, like any other allocated page. */
    CU_ASSERT_EQUAL(page_owner((void *)(uintptr_t)info.phys_addr),
                    PAGE_OWNER_LIBOS);

    uint8_t *buf = (uint8_t *)(uintptr_t)info.phys_addr;
    uint64_t fb_bytes = (uint64_t)TEST_FB_PITCH * TEST_FB_HEIGHT;
    int all_zero = 1;
    for (uint64_t i = 0; i < fb_bytes; i++) {
        if (buf[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    CU_ASSERT_TRUE(all_zero);

    uint64_t phys_base;
    CU_ASSERT_EQUAL(fb_shadow_lookup(PAGE_OWNER_LIBOS, &phys_base), 0);
    CU_ASSERT_EQUAL(phys_base, info.phys_addr);
}

/* Re-acquiring is idempotent: same caller, same buffer back. */
static void test_reacquire_is_idempotent(void)
{
    install_test_fb();

    exo_fb_info_t first, second;
    CU_ASSERT_EQUAL(fb_shadow_acquire(PAGE_OWNER_LIBOS, &first), FB_SHADOW_OK);
    CU_ASSERT_EQUAL(fb_shadow_acquire(PAGE_OWNER_LIBOS, &second), FB_SHADOW_OK);

    CU_ASSERT_EQUAL(first.phys_addr, second.phys_addr);
}

/* Two different contexts get two distinct, non-overlapping buffers — the
 * whole point of SCRUM-112 over the single exclusive binding it replaces. */
static void test_two_contexts_get_distinct_buffers(void)
{
    install_test_fb();

    exo_fb_info_t a, b;
    CU_ASSERT_EQUAL(fb_shadow_acquire(PAGE_OWNER_LIBOS, &a), FB_SHADOW_OK);
    CU_ASSERT_EQUAL(fb_shadow_acquire(OTHER_LIBOS, &b), FB_SHADOW_OK);

    CU_ASSERT_NOT_EQUAL(a.phys_addr, b.phys_addr);

    uint64_t fb_bytes = (uint64_t)TEST_FB_PITCH * TEST_FB_HEIGHT;
    uint64_t a_end = a.phys_addr + fb_bytes;
    uint64_t b_end = b.phys_addr + fb_bytes;
    CU_ASSERT_TRUE(a_end <= b.phys_addr || b_end <= a.phys_addr);

    CU_ASSERT_EQUAL(page_owner((void *)(uintptr_t)a.phys_addr),
                    PAGE_OWNER_LIBOS);
    CU_ASSERT_EQUAL(page_owner((void *)(uintptr_t)b.phys_addr), OTHER_LIBOS);
}

/* Release clears the directory entry and frees the pages it allocated
 * (SCRUM-187) — see this file's header and src/fb_shadow.h's own top
 * comment. */
static void test_release_frees_the_pages(void)
{
    install_test_fb();

    exo_fb_info_t info;
    CU_ASSERT_EQUAL(fb_shadow_acquire(OTHER_LIBOS, &info), FB_SHADOW_OK);

    uint64_t phys_base;
    CU_ASSERT_EQUAL(fb_shadow_lookup(OTHER_LIBOS, &phys_base), 0);

    fb_shadow_release(OTHER_LIBOS);
    CU_ASSERT_NOT_EQUAL(fb_shadow_lookup(OTHER_LIBOS, &phys_base), 0);

    /* Freed, not just released from the directory. */
    CU_ASSERT_EQUAL(page_owner((void *)(uintptr_t)info.phys_addr),
                    PAGE_OWNER_FREE);

    /* Nothing left for a caller-side sweep to find. */
    CU_ASSERT_EQUAL(reclaim_pages_owned(OTHER_LIBOS), 0u);
}

/* No published framebuffer, no geometry to size a buffer from. */
static void test_headless_reports_enodev(void)
{
    CU_ASSERT_EQUAL(fb_binding_init((const fb_geometry_t *)0), FB_BIND_ENODEV);

    /* THIRD_LIBOS, not PAGE_OWNER_LIBOS/OTHER_LIBOS: those two already hold
     * slots from earlier tests in this suite (never released until
     * fb_shadow_suite_cleanup()), so reusing either here would find that
     * pre-existing slot rather than proving a failed acquire creates
     * nothing. */
    exo_fb_info_t info;
    CU_ASSERT_EQUAL(fb_shadow_acquire(THIRD_LIBOS, &info), FB_SHADOW_ENODEV);

    uint64_t phys_base;
    CU_ASSERT_NOT_EQUAL(fb_shadow_lookup(THIRD_LIBOS, &phys_base), 0);
}

/* A context that never acquired has nothing to look up. */
static void test_lookup_of_unacquired_context_fails(void)
{
    install_test_fb();

    uint64_t phys_base;
    CU_ASSERT_NOT_EQUAL(fb_shadow_lookup(OTHER_LIBOS, &phys_base), 0);
}

void suite_fb_shadow_tests(CU_pSuite s)
{
    CU_add_test(s, "acquire allocates and zeroes",
                test_acquire_allocates_and_zeroes);
    CU_add_test(s, "reacquire is idempotent", test_reacquire_is_idempotent);
    CU_add_test(s, "two contexts get distinct buffers",
                test_two_contexts_get_distinct_buffers);
    CU_add_test(s, "release frees the pages", test_release_frees_the_pages);
    CU_add_test(s, "headless reports ENODEV", test_headless_reports_enodev);
    CU_add_test(s, "lookup of unacquired context fails",
                test_lookup_of_unacquired_context_fails);
}
