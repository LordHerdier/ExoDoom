/*
 * test_revoke_k.c — resource revocation / repossession (SCRUM-156).
 *
 * Secure binding (SCRUM-152 / -154) proves a LibOS cannot take what it was not
 * given.  These tests prove the converse: what the kernel gave, the kernel can
 * take back — and can take back *only* from the context it named.
 *
 * The three phases of docs/syscall_spec.md §3.6, for both resources that have
 * an owner today:
 *
 *   request  the mark lands in the ownership table and changes nothing else —
 *            the owner keeps the page or the screen and keeps using it;
 *   comply   returning a marked resource the ordinary way works and clears the
 *            mark with it, so a well-behaved LibOS is never punished;
 *   force    the kernel takes what was not returned, never touches a resource
 *            that has moved to another context, and never sweeps the kernel's
 *            own pages.
 *
 * Pages are allocated under OTHER_LIBOS rather than the v1 context id, so a
 * revoke_all() sweep here cannot reclaim a page another suite is holding.  The
 * framebuffer tests install a synthetic geometry and the suite's init/cleanup
 * pair restores whatever kernel_main published, exactly as the SCRUM-154 suite
 * does — the kernel console owns that screen for the rest of the boot.
 */

#include "kunit.h"
#include "revoke.h"
#include "page_alloc.h"
#include "fb_binding.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "vmm.h"
#include "libos_test_common.h"  /* TEST_OWNER_REVOKE_ADDRSPACE, SCRUM-159 */

#include <stdint.h>

/* A second, distinct LibOS id — the "another LibOS" of the multi-LibOS future.
 * v1 only ever runs PAGE_OWNER_LIBOS, so nothing outside this file holds a
 * page tagged with this id. */
#define OTHER_LIBOS ((page_owner_t)(PAGE_OWNER_LIBOS + 1))

/* Synthetic framebuffer, matching test_fb_binding_k.c: a base far above the
 * RAM QEMU gives us, so no address here can collide with a real page. */
#define TEST_FB_BASE    0x00000000e0000000ull
#define TEST_FB_WIDTH   1024u
#define TEST_FB_HEIGHT  768u
#define TEST_FB_PITCH   4096u
#define TEST_FB_BPP     32u

static fb_geometry_t boot_geometry;
static int           boot_had_fb;

/* Suite init/cleanup, passed to CU_add_suite in test_runner.c. */
int revoke_suite_init(void)
{
    const fb_geometry_t *g = fb_binding_geometry();

    boot_had_fb = (g != NULL);
    if (boot_had_fb)
        boot_geometry = *g;

    revoke_record_reset();

    return 0;
}

int revoke_suite_cleanup(void)
{
    /* Put the real framebuffer back, unbound, and leave no pages of ours
     * outstanding — a leak here would show up as a phantom allocation in
     * whatever runs next. */
    fb_binding_init(boot_had_fb ? &boot_geometry : (const fb_geometry_t *)0);
    (void)page_reclaim_all(OTHER_LIBOS);
    revoke_record_reset();

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

    /* fb_binding_init() also clears owner and mark, so every test that calls
     * this starts from "published, unheld, unmarked". */
    CU_ASSERT_EQUAL(fb_binding_init(&g), FB_BIND_OK);
}

/* ── Phase 1: request marks, and marking takes nothing ────────────────────── */

/* The defining property of the ask: after it, the page is still the owner's.
 * A revocation request that quietly confiscated the page would make the
 * "comply" phase unimplementable — there would be nothing left to return. */
static void test_request_marks_without_taking(void)
{
    void *p = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(p);

    revoke_res_t res = revoke_res_page((uint64_t)(uintptr_t)p);

    CU_ASSERT_FALSE(revoke_pending(res));
    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, res), REVOKE_OK);
    CU_ASSERT_TRUE(revoke_pending(res));

    /* Still allocated, still tagged with its owner: the mark lives in the high
     * bit of the tag and page_owner() must not leak it. */
    CU_ASSERT_EQUAL(page_owner(p), OTHER_LIBOS);
    CU_ASSERT_EQUAL(page_count_owned(OTHER_LIBOS), 1u);

    /* And a stranger still cannot free it — the mark neither adds nor removes
     * permission. */
    CU_ASSERT_EQUAL(free_page_owned(p, PAGE_OWNER_LIBOS), PAGE_FREE_EPERM);

    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, res), REVOKE_OK);
}

/* Marking is idempotent: a scheduler that asks twice has still asked once. */
static void test_request_is_idempotent(void)
{
    void *p = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(p);

    revoke_res_t res = revoke_res_page((uint64_t)(uintptr_t)p);

    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, res), REVOKE_OK);
    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, res), REVOKE_OK);
    CU_ASSERT_TRUE(revoke_pending(res));
    CU_ASSERT_EQUAL(page_owner(p), OTHER_LIBOS);

    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, res), REVOKE_OK);
}

/* You cannot ask a context for a resource it does not have.  Without this the
 * mark would be settable on a peer's page, and the next sweep would read as a
 * forced reclaim of something never granted. */
static void test_request_needs_the_named_holder(void)
{
    void *p = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(p);

    revoke_res_t res = revoke_res_page((uint64_t)(uintptr_t)p);

    CU_ASSERT_EQUAL(revoke_request(PAGE_OWNER_LIBOS, res), REVOKE_ENOENT);
    CU_ASSERT_EQUAL(revoke_request(PAGE_OWNER_FREE, res), REVOKE_ENOENT);
    CU_ASSERT_FALSE(revoke_pending(res));

    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, res), REVOKE_OK);

    /* Now free: nobody holds it, so nobody can be asked for it. */
    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, res), REVOKE_ENOENT);
    CU_ASSERT_FALSE(revoke_pending(res));
}

/* A malformed resource is EINVAL, distinct from ENOENT: "there is no such
 * page" and "that context does not hold this page" are different bugs. */
static void test_malformed_resources_are_einval(void)
{
    uint64_t past_pool = (uint64_t)page_alloc_pool_end();
    CU_ASSERT_NOT_EQUAL(past_pool, 0u);

    revoke_res_t out_of_range = revoke_res_page(past_pool);
    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, out_of_range), REVOKE_EINVAL);
    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, out_of_range), REVOKE_EINVAL);
    CU_ASSERT_FALSE(revoke_pending(out_of_range));

    void *p = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(p);

    revoke_res_t unaligned = revoke_res_page((uint64_t)(uintptr_t)p + 8);
    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, unaligned), REVOKE_EINVAL);
    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, unaligned), REVOKE_EINVAL);

    /* An unknown resource kind is malformed too, not silently a page. */
    revoke_res_t bogus_kind = revoke_res_page((uint64_t)(uintptr_t)p);
    bogus_kind.kind = (revoke_kind_t)99;
    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, bogus_kind), REVOKE_EINVAL);
    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, bogus_kind), REVOKE_EINVAL);
    CU_ASSERT_FALSE(revoke_pending(bogus_kind));

    /* None of that touched the page. */
    CU_ASSERT_EQUAL(page_owner(p), OTHER_LIBOS);
    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, revoke_res_page((uint64_t)(uintptr_t)p)),
                    REVOKE_OK);
}

/* A request can be taken back: the demand that prompted it may be satisfied
 * elsewhere, and a mark that outlives its reason turns the next sweep into a
 * forced reclaim nobody asked for. */
static void test_withdraw_clears_the_mark(void)
{
    void *p = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(p);

    revoke_res_t res = revoke_res_page((uint64_t)(uintptr_t)p);

    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, res), REVOKE_OK);

    /* Not the holder: the mark stays put. */
    CU_ASSERT_EQUAL(revoke_withdraw(PAGE_OWNER_LIBOS, res), REVOKE_ENOENT);
    CU_ASSERT_TRUE(revoke_pending(res));

    CU_ASSERT_EQUAL(revoke_withdraw(OTHER_LIBOS, res), REVOKE_OK);
    CU_ASSERT_FALSE(revoke_pending(res));
    CU_ASSERT_EQUAL(page_owner(p), OTHER_LIBOS);

    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, res), REVOKE_OK);
}

/* ── Phase 2: compliance ──────────────────────────────────────────────────── */

/* The whole point of asking first: a LibOS that gives the page back does so
 * through the ordinary free path, and the mark disappears with the tag.  If
 * free_page_owned() compared the raw tag instead of the owner id, a marked
 * page would answer -EPERM to its own owner and compliance would be
 * impossible. */
static void test_owner_may_return_a_marked_page(void)
{
    void *p = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(p);

    revoke_res_t res = revoke_res_page((uint64_t)(uintptr_t)p);

    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, res), REVOKE_OK);
    CU_ASSERT_EQUAL(free_page_owned(p, OTHER_LIBOS), PAGE_FREE_OK);

    CU_ASSERT_EQUAL(page_owner(p), PAGE_OWNER_FREE);
    CU_ASSERT_FALSE(revoke_pending(res));
}

/* After compliance the force step finds nothing to take, and says so — this is
 * how the kernel distinguishes "I had to seize it" from "asking was enough". */
static void test_force_after_compliance_takes_nothing(void)
{
    void *p = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(p);

    revoke_res_t res = revoke_res_page((uint64_t)(uintptr_t)p);

    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, res), REVOKE_OK);
    CU_ASSERT_EQUAL(free_page_owned(p, OTHER_LIBOS), PAGE_FREE_OK);

    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, res), REVOKE_RETURNED);
    CU_ASSERT_EQUAL(page_owner(p), PAGE_OWNER_FREE);
}

/* ── Phase 3: force ───────────────────────────────────────────────────────── */

/* A LibOS that ignores the ask loses the page anyway.  Forcing twice is not a
 * double free: the second call reports there was nothing left to take. */
static void test_force_reclaims_a_page_still_held(void)
{
    void *p = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(p);

    revoke_res_t res = revoke_res_page((uint64_t)(uintptr_t)p);

    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, res), REVOKE_OK);
    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, res), REVOKE_OK);

    CU_ASSERT_EQUAL(page_owner(p), PAGE_OWNER_FREE);
    CU_ASSERT_FALSE(revoke_pending(res));
    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, res), REVOKE_RETURNED);
}

/* Force works without a request: exo_exit reclamation (SCRUM-155) is an
 * unconditional take, and the two-phase sequence is the polite route to the
 * same call. */
static void test_force_needs_no_prior_request(void)
{
    void *p = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(p);

    revoke_res_t res = revoke_res_page((uint64_t)(uintptr_t)p);

    CU_ASSERT_FALSE(revoke_pending(res));
    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, res), REVOKE_OK);
    CU_ASSERT_EQUAL(page_owner(p), PAGE_OWNER_FREE);
}

/* Revocation is scoped to the context it names.  Reclaiming on behalf of A
 * must never free a page B holds — otherwise "revoke" would be a syscall-free
 * way to free a peer's memory, which is the exact hole secure binding closes. */
static void test_force_is_scoped_to_the_named_context(void)
{
    void *p = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(p);

    revoke_res_t res = revoke_res_page((uint64_t)(uintptr_t)p);

    CU_ASSERT_EQUAL(revoke_force(PAGE_OWNER_LIBOS, res), REVOKE_RETURNED);
    CU_ASSERT_EQUAL(page_owner(p), OTHER_LIBOS);
    CU_ASSERT_EQUAL(page_count_owned(OTHER_LIBOS), 1u);

    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, res), REVOKE_OK);
}

/* Kernel memory is not revocable: the bitmap, the owner table, the kernel image
 * and the WAD module all carry PAGE_OWNER_KERNEL.  Two distinct cases, and the
 * second is the one that bites — a LibOS naming a kernel page is refused by the
 * ordinary owner check, but naming PAGE_OWNER_KERNEL *as the holder* would
 * satisfy that check, so the single-resource path has to refuse the id the way
 * the sweep does.  Without it the page lands back in the pool and the kernel's
 * own free_page() double-frees it. */
static void test_force_cannot_take_a_kernel_page(void)
{
    void *kp = alloc_page();               /* PAGE_OWNER_KERNEL */
    CU_ASSERT_PTR_NOT_NULL(kp);

    revoke_res_t res = revoke_res_page((uint64_t)(uintptr_t)kp);

    /* A LibOS asking for it: refused because it does not hold the page. */
    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, res), REVOKE_ENOENT);
    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, res), REVOKE_RETURNED);
    CU_ASSERT_EQUAL(page_owner(kp), PAGE_OWNER_KERNEL);

    /* Naming the kernel itself as the holder: refused because PAGE_OWNER_KERNEL
     * is not a revocable context, mirroring revoke_all()/page_reclaim_all(). */
    CU_ASSERT_EQUAL(revoke_request(PAGE_OWNER_KERNEL, res), REVOKE_ENOENT);
    CU_ASSERT_EQUAL(revoke_withdraw(PAGE_OWNER_KERNEL, res), REVOKE_ENOENT);
    CU_ASSERT_EQUAL(revoke_force(PAGE_OWNER_KERNEL, res), REVOKE_RETURNED);
    CU_ASSERT_EQUAL(page_owner(kp), PAGE_OWNER_KERNEL);
    CU_ASSERT_FALSE(revoke_pending(res));

    /* Still the kernel's, so its own free path still works exactly once. */
    CU_ASSERT_EQUAL(free_page_checked(kp), 0);
    CU_ASSERT_EQUAL(page_owner(kp), PAGE_OWNER_FREE);
}

/*
 * SCRUM-159: revoke_force removes not just the ownership tag but the target's
 * own mapping of the page it reclaims, so it can never again reach memory the
 * PMM is now free to hand to somebody else. Needs a real bound address space
 * to have anything to unmap out of, unlike every other test in this suite
 * (plain page ownership only) -- see libos_test_common.h for why this one
 * test uses TEST_OWNER_REVOKE_ADDRSPACE instead of OTHER_LIBOS.
 */
static void test_force_unmaps_the_page(void)
{
    uint64_t pml4_phys = 0;
    CU_ASSERT_EQUAL(vmm_create_address_space(&pml4_phys), VMM_OK);
    if (pml4_phys == 0)
        return;
    CU_ASSERT_EQUAL(vmm_bind_address_space(TEST_OWNER_REVOKE_ADDRSPACE, pml4_phys),
                    VMM_OK);
    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;

    void *p = alloc_page_owned(TEST_OWNER_REVOKE_ADDRSPACE);
    CU_ASSERT_PTR_NOT_NULL(p);
    if (p == NULL) {
        vmm_destroy_address_space(TEST_OWNER_REVOKE_ADDRSPACE);
        return;
    }

    const uint64_t vaddr = EXO_USER_VA_BASE + 0x50000000ULL;
    CU_ASSERT_EQUAL(vmm_map_page_in(pml4, vaddr, (uint64_t)(uintptr_t)p,
                                    VMM_WRITE | VMM_USER), VMM_OK);

    uint64_t resolved = 0;
    CU_ASSERT_EQUAL(vmm_translate_in(pml4, vaddr, &resolved, NULL), VMM_OK);
    CU_ASSERT_EQUAL(resolved, (uint64_t)(uintptr_t)p);

    revoke_res_t res = revoke_res_page((uint64_t)(uintptr_t)p);
    CU_ASSERT_EQUAL(revoke_force(TEST_OWNER_REVOKE_ADDRSPACE, res), REVOKE_OK);

    CU_ASSERT_EQUAL(page_owner(p), PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(vmm_translate_in(pml4, vaddr, &resolved, NULL), VMM_ENOENT);

    CU_ASSERT_EQUAL(vmm_destroy_address_space(TEST_OWNER_REVOKE_ADDRSPACE), VMM_OK);
}

/* ── revoke_all: the v1 policy ────────────────────────────────────────────── */

/* What exo_exit will call.  Everything the context holds goes back, and
 * nothing else moves. */
static void test_revoke_all_sweeps_one_context(void)
{
    CU_ASSERT_EQUAL(page_count_owned(OTHER_LIBOS), 0u);

    void *a = alloc_page_owned(OTHER_LIBOS);
    void *b = alloc_page_owned(OTHER_LIBOS);
    void *c = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(a);
    CU_ASSERT_PTR_NOT_NULL(b);
    CU_ASSERT_PTR_NOT_NULL(c);

    /* A page belonging to the v1 LibOS and one belonging to the kernel: the
     * sweep must step over both. */
    void *other = alloc_page_owned(PAGE_OWNER_LIBOS);
    void *kp    = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(other);
    CU_ASSERT_PTR_NOT_NULL(kp);

    CU_ASSERT_EQUAL(page_count_owned(OTHER_LIBOS), 3u);

    /* One of them marked, to show the sweep does not care either way. */
    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS,
                                   revoke_res_page((uint64_t)(uintptr_t)b)),
                    REVOKE_OK);

    CU_ASSERT_EQUAL(revoke_all(OTHER_LIBOS), 3u);

    CU_ASSERT_EQUAL(page_count_owned(OTHER_LIBOS), 0u);
    CU_ASSERT_EQUAL(page_owner(a), PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(page_owner(b), PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(page_owner(c), PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(page_owner(other), PAGE_OWNER_LIBOS);
    CU_ASSERT_EQUAL(page_owner(kp), PAGE_OWNER_KERNEL);

    CU_ASSERT_EQUAL(free_page_owned(other, PAGE_OWNER_LIBOS), PAGE_FREE_OK);
    free_page(kp);
}

/*
 * SCRUM-159: revoke_all wipes every mapping left in the swept context's own
 * window in the same pass, not just the pages it hands back to the PMM --
 * page_reclaim_all() never reports which addresses it took, so this is what
 * makes a repossessed shadow framebuffer (or any other page the context had
 * mapped) unreachable without the sweep needing to enumerate them one by one.
 */
static void test_revoke_all_unmaps_everything(void)
{
    uint64_t pml4_phys = 0;
    CU_ASSERT_EQUAL(vmm_create_address_space(&pml4_phys), VMM_OK);
    if (pml4_phys == 0)
        return;
    CU_ASSERT_EQUAL(vmm_bind_address_space(TEST_OWNER_REVOKE_ADDRSPACE, pml4_phys),
                    VMM_OK);
    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;

    void *a = alloc_page_owned(TEST_OWNER_REVOKE_ADDRSPACE);
    void *b = alloc_page_owned(TEST_OWNER_REVOKE_ADDRSPACE);
    CU_ASSERT_PTR_NOT_NULL(a);
    CU_ASSERT_PTR_NOT_NULL(b);
    if (a == NULL || b == NULL) {
        vmm_destroy_address_space(TEST_OWNER_REVOKE_ADDRSPACE);
        return;
    }

    const uint64_t va_a = EXO_USER_VA_BASE + 0x50008000ULL;
    const uint64_t va_b = EXO_USER_VA_BASE + 0x5000C000ULL;
    CU_ASSERT_EQUAL(vmm_map_page_in(pml4, va_a, (uint64_t)(uintptr_t)a,
                                    VMM_WRITE | VMM_USER), VMM_OK);
    CU_ASSERT_EQUAL(vmm_map_page_in(pml4, va_b, (uint64_t)(uintptr_t)b,
                                    VMM_WRITE | VMM_USER), VMM_OK);

    CU_ASSERT_EQUAL(revoke_all(TEST_OWNER_REVOKE_ADDRSPACE), 2u);

    CU_ASSERT_EQUAL(vmm_translate_in(pml4, va_a, NULL, NULL), VMM_ENOENT);
    CU_ASSERT_EQUAL(vmm_translate_in(pml4, va_b, NULL, NULL), VMM_ENOENT);

    CU_ASSERT_EQUAL(vmm_destroy_address_space(TEST_OWNER_REVOKE_ADDRSPACE), VMM_OK);
}

/* The sweep that must never happen.  PAGE_OWNER_KERNEL names every reserved
 * page on the machine — the bitmap, the owner table, the kernel image, the WAD
 * module — so a sweep of it would free the system out from under itself, and
 * PAGE_OWNER_FREE names every unallocated page.  Both are refused outright. */
static void test_revoke_all_refuses_reserved_ids(void)
{
    uint32_t kernel_pages = page_count_owned(PAGE_OWNER_KERNEL);
    CU_ASSERT_NOT_EQUAL(kernel_pages, 0u);

    void *kp = alloc_page();
    CU_ASSERT_PTR_NOT_NULL(kp);

    CU_ASSERT_EQUAL(revoke_all(PAGE_OWNER_KERNEL), 0u);
    CU_ASSERT_EQUAL(revoke_all(PAGE_OWNER_FREE), 0u);

    CU_ASSERT_EQUAL(page_count_owned(PAGE_OWNER_KERNEL), kernel_pages + 1u);
    CU_ASSERT_EQUAL(page_owner(kp), PAGE_OWNER_KERNEL);

    free_page(kp);
}

/* ── The framebuffer ──────────────────────────────────────────────────────── */

/* The mark is an ask here too: a marked owner still holds the screen and may
 * still map its pages, which is what lets it finish the frame it is drawing
 * before handing the display over. */
static void test_fb_request_marks_without_taking(void)
{
    install_test_fb();
    CU_ASSERT_EQUAL(fb_binding_acquire(OTHER_LIBOS), FB_BIND_OK);

    revoke_res_t fb = revoke_res_fb();

    CU_ASSERT_FALSE(revoke_pending(fb));
    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, fb), REVOKE_OK);
    CU_ASSERT_TRUE(revoke_pending(fb));

    CU_ASSERT_EQUAL(fb_binding_owner(), OTHER_LIBOS);
    CU_ASSERT_EQUAL(fb_binding_check_map(TEST_FB_BASE, OTHER_LIBOS),
                    FB_MAP_ALLOW);
    /* And it is still refused to everyone else. */
    CU_ASSERT_EQUAL(fb_binding_check_map(TEST_FB_BASE, PAGE_OWNER_LIBOS),
                    FB_MAP_DENY);
}

/* Asking a context that does not hold the screen — and asking on a machine
 * that has no screen at all — is ENOENT, never a mark on nothing. */
static void test_fb_request_needs_the_holder(void)
{
    install_test_fb();
    CU_ASSERT_EQUAL(fb_binding_acquire(OTHER_LIBOS), FB_BIND_OK);

    revoke_res_t fb = revoke_res_fb();

    CU_ASSERT_EQUAL(revoke_request(PAGE_OWNER_LIBOS, fb), REVOKE_ENOENT);
    CU_ASSERT_EQUAL(revoke_request(PAGE_OWNER_FREE, fb), REVOKE_ENOENT);
    CU_ASSERT_FALSE(revoke_pending(fb));

    /* Unheld. */
    install_test_fb();
    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, fb), REVOKE_ENOENT);
    CU_ASSERT_FALSE(revoke_pending(fb));

    /* Headless. */
    CU_ASSERT_EQUAL(fb_binding_init((const fb_geometry_t *)0), FB_BIND_ENODEV);
    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, fb), REVOKE_ENOENT);
    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, fb), REVOKE_RETURNED);
    CU_ASSERT_FALSE(revoke_pending(fb));
}

/* Compliance for the framebuffer is fb_binding_release(), and it must clear
 * the mark: otherwise the next context to acquire would inherit a pending
 * revocation it was never asked about. */
static void test_fb_release_clears_the_mark(void)
{
    install_test_fb();
    CU_ASSERT_EQUAL(fb_binding_acquire(OTHER_LIBOS), FB_BIND_OK);

    revoke_res_t fb = revoke_res_fb();

    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, fb), REVOKE_OK);
    fb_binding_release(OTHER_LIBOS);

    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);
    CU_ASSERT_FALSE(revoke_pending(fb));
    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, fb), REVOKE_RETURNED);

    /* The next owner starts unmarked. */
    CU_ASSERT_EQUAL(fb_binding_acquire(PAGE_OWNER_LIBOS), FB_BIND_OK);
    CU_ASSERT_FALSE(revoke_pending(fb));
}

/* A context that dies holding the (real) binding must not lock it for the
 * rest of the boot: forcing it hands it to the next acquirer.
 *
 * SCRUM-112: the real exo_fb_acquire syscall no longer routes through this
 * binding at all (src/syscall_fb.c calls fb_shadow_acquire() instead, which
 * never fails with -EXO_EBUSY), so the exclusivity this test proves is
 * exercised directly against fb_binding.c rather than through the syscall —
 * see test_fb_binding_k.c's file header for the same reasoning. */
static void test_fb_force_frees_the_binding(void)
{
    install_test_fb();
    page_owner_t me = syscall_current_context();

    CU_ASSERT_EQUAL(fb_binding_acquire(OTHER_LIBOS), FB_BIND_OK);

    revoke_res_t fb = revoke_res_fb();

    CU_ASSERT_EQUAL(fb_binding_acquire(me), FB_BIND_EBUSY);

    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, fb), REVOKE_OK);
    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, fb), REVOKE_OK);
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);
    CU_ASSERT_FALSE(revoke_pending(fb));

    /* Freed, so a different context can take it now. */
    CU_ASSERT_EQUAL(fb_binding_acquire(me), FB_BIND_OK);
    CU_ASSERT_EQUAL(fb_binding_owner(), me);

    fb_binding_release(me);
}

/* Scoped, exactly as for pages: reclaiming for A must leave B's binding
 * alone. */
static void test_fb_force_is_scoped_to_the_holder(void)
{
    install_test_fb();
    CU_ASSERT_EQUAL(fb_binding_acquire(OTHER_LIBOS), FB_BIND_OK);

    CU_ASSERT_EQUAL(revoke_force(PAGE_OWNER_LIBOS, revoke_res_fb()),
                    REVOKE_RETURNED);
    CU_ASSERT_EQUAL(fb_binding_owner(), OTHER_LIBOS);

    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, revoke_res_fb()), REVOKE_OK);
}

/* The sweep covers every kind of resource, not just pages — a context that
 * exits holding both must lose both in one call. */
static void test_revoke_all_takes_pages_and_framebuffer(void)
{
    install_test_fb();
    CU_ASSERT_EQUAL(fb_binding_acquire(OTHER_LIBOS), FB_BIND_OK);

    CU_ASSERT_PTR_NOT_NULL(alloc_page_owned(OTHER_LIBOS));
    CU_ASSERT_PTR_NOT_NULL(alloc_page_owned(OTHER_LIBOS));

    /* 2 pages + 1 framebuffer. */
    CU_ASSERT_EQUAL(revoke_all(OTHER_LIBOS), 3u);

    CU_ASSERT_EQUAL(page_count_owned(OTHER_LIBOS), 0u);
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);

    /* A second sweep of a context that holds nothing takes nothing. */
    CU_ASSERT_EQUAL(revoke_all(OTHER_LIBOS), 0u);
}

/* ── The repossession record ──────────────────────────────────────────────── */

/* The audit trail: how often the kernel asked, how often asking was enough,
 * and how often it had to take the resource itself. */
static void test_record_counts_asks_and_takes(void)
{
    revoke_record_reset();

    const revoke_record_t *rec = revoke_record();
    CU_ASSERT_EQUAL(rec->requested, 0u);
    CU_ASSERT_EQUAL(rec->forced, 0u);
    CU_ASSERT_EQUAL(rec->returned, 0u);

    /* One page the LibOS gives back after being asked. */
    void *complied = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(complied);
    revoke_res_t cres = revoke_res_page((uint64_t)(uintptr_t)complied);
    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, cres), REVOKE_OK);
    CU_ASSERT_EQUAL(free_page_owned(complied, OTHER_LIBOS), PAGE_FREE_OK);
    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, cres), REVOKE_RETURNED);

    /* One it ignores. */
    void *kept = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(kept);
    revoke_res_t kres = revoke_res_page((uint64_t)(uintptr_t)kept);
    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, kres), REVOKE_OK);
    CU_ASSERT_EQUAL(revoke_force(OTHER_LIBOS, kres), REVOKE_OK);

    /* One ask taken back. */
    void *spared = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(spared);
    revoke_res_t sres = revoke_res_page((uint64_t)(uintptr_t)spared);
    CU_ASSERT_EQUAL(revoke_request(OTHER_LIBOS, sres), REVOKE_OK);
    CU_ASSERT_EQUAL(revoke_withdraw(OTHER_LIBOS, sres), REVOKE_OK);

    CU_ASSERT_EQUAL(rec->requested, 3u);
    CU_ASSERT_EQUAL(rec->withdrawn, 1u);
    CU_ASSERT_EQUAL(rec->returned, 1u);
    CU_ASSERT_EQUAL(rec->forced, 1u);
    CU_ASSERT_EQUAL(rec->pages_reclaimed, 1u);
    CU_ASSERT_EQUAL(rec->fb_reclaimed, 0u);

    /* A failed ask is not counted: it never reached an owner. */
    CU_ASSERT_EQUAL(revoke_request(PAGE_OWNER_LIBOS, sres), REVOKE_ENOENT);
    CU_ASSERT_EQUAL(rec->requested, 3u);

    /* The sweep adds the framebuffer and the page still outstanding. */
    install_test_fb();
    CU_ASSERT_EQUAL(fb_binding_acquire(OTHER_LIBOS), FB_BIND_OK);
    CU_ASSERT_EQUAL(revoke_all(OTHER_LIBOS), 2u);

    CU_ASSERT_EQUAL(rec->pages_reclaimed, 2u);
    CU_ASSERT_EQUAL(rec->fb_reclaimed, 1u);
    CU_ASSERT_EQUAL(rec->forced, 3u);

    revoke_record_reset();
    CU_ASSERT_EQUAL(rec->requested, 0u);
    CU_ASSERT_EQUAL(rec->forced, 0u);
    CU_ASSERT_EQUAL(rec->pages_reclaimed, 0u);
}

void suite_revoke_tests(CU_pSuite s)
{
    CU_add_test(s, "request marks without taking",
                test_request_marks_without_taking);
    CU_add_test(s, "request is idempotent", test_request_is_idempotent);
    CU_add_test(s, "request needs the named holder",
                test_request_needs_the_named_holder);
    CU_add_test(s, "malformed resources are EINVAL",
                test_malformed_resources_are_einval);
    CU_add_test(s, "withdraw clears the mark", test_withdraw_clears_the_mark);
    CU_add_test(s, "owner may return a marked page",
                test_owner_may_return_a_marked_page);
    CU_add_test(s, "force after compliance takes nothing",
                test_force_after_compliance_takes_nothing);
    CU_add_test(s, "force reclaims a page still held",
                test_force_reclaims_a_page_still_held);
    CU_add_test(s, "force needs no prior request",
                test_force_needs_no_prior_request);
    CU_add_test(s, "force is scoped to the named context",
                test_force_is_scoped_to_the_named_context);
    CU_add_test(s, "force cannot take a kernel page",
                test_force_cannot_take_a_kernel_page);
    CU_add_test(s, "force unmaps the page", test_force_unmaps_the_page);
    CU_add_test(s, "revoke_all sweeps one context",
                test_revoke_all_sweeps_one_context);
    CU_add_test(s, "revoke_all unmaps everything",
                test_revoke_all_unmaps_everything);
    CU_add_test(s, "revoke_all refuses reserved ids",
                test_revoke_all_refuses_reserved_ids);
    CU_add_test(s, "FB request marks without taking",
                test_fb_request_marks_without_taking);
    CU_add_test(s, "FB request needs the holder",
                test_fb_request_needs_the_holder);
    CU_add_test(s, "FB release clears the mark",
                test_fb_release_clears_the_mark);
    CU_add_test(s, "FB force frees the binding",
                test_fb_force_frees_the_binding);
    CU_add_test(s, "FB force is scoped to the holder",
                test_fb_force_is_scoped_to_the_holder);
    CU_add_test(s, "revoke_all takes pages and framebuffer",
                test_revoke_all_takes_pages_and_framebuffer);
    CU_add_test(s, "record counts asks and takes",
                test_record_counts_asks_and_takes);
}
