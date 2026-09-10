/*
 * test_page_map_k.c — exo_page_map / exo_page_unmap (SCRUM-35) and the
 * ownership rule they enforce (SCRUM-153).
 *
 * Drives the real dispatch path, exo_syscall_dispatch(EXO_SYS_PAGE_MAP, ...) —
 * the route a ring-3 `syscall` takes once the entry stub has marshalled its
 * arguments.  The walker underneath is covered separately by test_vmm_k.c;
 * what is under test here is the protection rule of docs/syscall_spec.md §3.3:
 * a LibOS may map a page it owns, the framebuffer it has acquired, and nothing
 * else — the difference between a secure binding and an unprotected physical
 * mmap.
 */

#include "kunit.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "page_alloc.h"
#include "fb_binding.h"
#include "vmm.h"
#include "mmap.h"

/* The "another LibOS" whose pages the caller must not be able to reach. */
#define OTHER_LIBOS ((page_owner_t)(PAGE_OWNER_LIBOS + 1))

/* Scratch virtual addresses in the LibOS window, apart from test_vmm_k.c's. */
#define SCRATCH   (EXO_USER_VA_BASE + 0x20000000ULL)
#define SCRATCH_2 (EXO_USER_VA_BASE + 0x24000000ULL)

static int64_t do_map(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_MAP, vaddr, paddr, flags,
                                0, 0, 0);
}

static int64_t do_unmap(uint64_t vaddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, vaddr, 0, 0, 0, 0, 0);
}

static int64_t do_alloc(void)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
}

static int64_t do_free(uint64_t paddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_FREE, paddr, 0, 0, 0, 0, 0);
}

#define MAP_RW (EXO_PAGE_READ | EXO_PAGE_WRITE | EXO_PAGE_USER)

/*
 * The window has to be *empty* to be a window.  The kernel map is an identity
 * map, so a window starting below the top of physical memory overlaps real
 * kernel mappings: exo_page_map then answers -EXO_EINVAL (the address is
 * already taken) and exo_page_unmap would unmap the kernel's own RAM.  That is
 * exactly what a 4 GiB base did on an 8 GiB machine.
 *
 * Two assertions, because either alone is weak: no usable RAM region may reach
 * into the window (the invariant), and nothing may already be mapped at its
 * base (the observable consequence, which also catches a collision this test
 * did not think of).
 */
static void test_window_is_clear_of_kernel_mappings(void)
{
    uint32_t count = 0;
    const mmap_region_t *regions = mmap_get_regions(&count);
    uint64_t top = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t end = regions[i].base + regions[i].length;
        if (end > top)
            top = end;
    }

    CU_ASSERT(top <= EXO_USER_VA_BASE);

    uint64_t resolved = 0;
    CU_ASSERT_EQUAL(vmm_translate(EXO_USER_VA_BASE, &resolved, NULL),
                    VMM_ENOENT);
}

/* Without this the rest of the suite would only be re-proving the
 * dispatcher's -EXO_ENOSYS fallback. */
static void test_handlers_are_bound(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_PAGE_MAP));
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_PAGE_UNMAP));
}

/* The happy path end to end: allocate, map, use, unmap, free. */
static void test_map_own_page_round_trip(void)
{
    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    if (p <= 0)
        return;

    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)p, MAP_RW), 0);

    uint64_t resolved = 0;
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH, &resolved, NULL), VMM_OK);
    CU_ASSERT_EQUAL(resolved, (uint64_t)p);

    CU_ASSERT_EQUAL(do_unmap(SCRATCH), 0);
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH, &resolved, NULL), VMM_ENOENT);

    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
}

/*
 * The hole this story closes: a LibOS handing exo_page_map an arbitrary
 * physical address.  A page belonging to another context, and a kernel page,
 * are both -EXO_EPERM — and, crucially, are still unmapped afterwards.
 */
static void test_foreign_page_rejected(void)
{
    void *theirs = alloc_page_owned(OTHER_LIBOS);
    CU_ASSERT_PTR_NOT_NULL(theirs);
    if (theirs == NULL)
        return;

    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)(uintptr_t)theirs, MAP_RW),
                    -EXO_EPERM);

    uint64_t resolved = 0;
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH, &resolved, NULL), VMM_ENOENT);

    free_page_owned(theirs, OTHER_LIBOS);
}

static void test_kernel_page_rejected(void)
{
    void *kp = alloc_page();          /* PAGE_OWNER_KERNEL */
    CU_ASSERT_PTR_NOT_NULL(kp);
    if (kp == NULL)
        return;

    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)(uintptr_t)kp, MAP_RW),
                    -EXO_EPERM);

    free_page(kp);
}

/* A free page is nobody's to hand out — only exo_page_alloc creates the
 * binding that makes a page mappable. */
static void test_unowned_page_rejected(void)
{
    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    if (p <= 0)
        return;
    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);   /* now PAGE_OWNER_FREE */

    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)p, MAP_RW), -EXO_EPERM);
}

/*
 * "Map over it" must not be a way around exo_page_unmap's ownership check.
 * Two independent rules close that: the walker refuses to repoint a live
 * mapping at a different physical page at all (VMM_EEXIST -> -EXO_EINVAL), and
 * unmapping it first is refused because the page is another context's
 * (-EXO_EPERM).  A LibOS moving a window over its *own* pages unmaps and
 * re-maps; a LibOS reaching for a peer's mapping gets nowhere either way.
 */
static void test_remap_over_foreign_mapping_rejected(void)
{
    void *theirs = alloc_page_owned(OTHER_LIBOS);
    int64_t mine = do_alloc();
    CU_ASSERT_PTR_NOT_NULL(theirs);
    CU_ASSERT(mine > 0);
    if (theirs == NULL || mine <= 0)
        return;

    /* Put the other context's page there behind the syscall's back — the
     * syscall would (rightly) not have let the caller do it. */
    CU_ASSERT_EQUAL(vmm_map_page(SCRATCH_2, (uint64_t)(uintptr_t)theirs,
                                 VMM_WRITE | VMM_USER), VMM_OK);

    CU_ASSERT_EQUAL(do_map(SCRATCH_2, (uint64_t)mine, MAP_RW), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_unmap(SCRATCH_2), -EXO_EPERM);

    /* The foreign mapping is still intact — a refused call changes nothing. */
    uint64_t resolved = 0;
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_2, &resolved, NULL), VMM_OK);
    CU_ASSERT_EQUAL(resolved, (uint64_t)(uintptr_t)theirs);

    CU_ASSERT_EQUAL(vmm_unmap_page(SCRATCH_2), VMM_OK);
    free_page_owned(theirs, OTHER_LIBOS);
    CU_ASSERT_EQUAL(do_free((uint64_t)mine), 0);
}

/* The kernel lives below 4 GiB, in the boot identity map, in the same page
 * tables these syscalls edit.  A LibOS may own a page and still not be allowed
 * to install it over kernel text — hence the window. */
static void test_vaddr_outside_window_rejected(void)
{
    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    if (p <= 0)
        return;

    /* The kernel's own load address, and the last page below the window. */
    CU_ASSERT_EQUAL(do_map(0x200000ULL, (uint64_t)p, MAP_RW), -EXO_EPERM);
    CU_ASSERT_EQUAL(do_map(EXO_USER_VA_BASE - VMM_PAGE_SIZE, (uint64_t)p,
                           MAP_RW), -EXO_EPERM);
    CU_ASSERT_EQUAL(do_unmap(0x200000ULL), -EXO_EPERM);

    /* Nothing about the kernel's mapping of itself moved. */
    uint64_t resolved = 0;
    CU_ASSERT_EQUAL(vmm_translate(0x200000ULL, &resolved, NULL), VMM_OK);
    CU_ASSERT_EQUAL(resolved, 0x200000ULL);

    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
}

static void test_bad_arguments_rejected(void)
{
    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    if (p <= 0)
        return;

    CU_ASSERT_EQUAL(do_map(SCRATCH + 1, (uint64_t)p, MAP_RW), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)p + 8, MAP_RW), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)p, 1u << 20), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_unmap(SCRATCH + 8), -EXO_EINVAL);

    /* Nothing mapped there is a bad argument, not a silent success. */
    CU_ASSERT_EQUAL(do_unmap(SCRATCH), -EXO_EINVAL);

    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
}

/*
 * The framebuffer is the case generic page ownership cannot decide: its pages
 * are MMIO, outside the PMM's pool, so page_owner() calls them FREE.  Mapping
 * them follows the binding instead (§3.5) — which is how DG_Init gets pixels
 * on screen and how nobody else does.
 */
static void test_framebuffer_follows_the_binding(void)
{
    const fb_geometry_t *geom = fb_binding_geometry();
    CU_ASSERT_PTR_NOT_NULL(geom);
    if (geom == NULL)
        return;

    page_owner_t me = syscall_current_context();
    uint64_t fb_page = geom->phys_addr & ~(uint64_t)(VMM_PAGE_SIZE - 1);

    /* Unheld: not public property. */
    fb_binding_release(fb_binding_owner());
    CU_ASSERT_EQUAL(do_map(SCRATCH, fb_page, MAP_RW), -EXO_EPERM);

    /* Held by someone else: still refused. */
    fb_binding_acquire(OTHER_LIBOS);
    CU_ASSERT_EQUAL(do_map(SCRATCH, fb_page, MAP_RW), -EXO_EPERM);

    /* Held by the caller: allowed, and unmappable again by the caller. */
    fb_binding_release(OTHER_LIBOS);
    CU_ASSERT_EQUAL(fb_binding_acquire(me), FB_BIND_OK);
    CU_ASSERT_EQUAL(do_map(SCRATCH, fb_page, MAP_RW), 0);

    uint64_t resolved = 0;
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH, &resolved, NULL), VMM_OK);
    CU_ASSERT_EQUAL(resolved, fb_page);

    CU_ASSERT_EQUAL(do_unmap(SCRATCH), 0);
    fb_binding_release(me);
}

/*
 * Losing the framebuffer must not leave the LibOS holding an address it can
 * never clean up.  The kernel repossessing the screen (§3.6) does not walk the
 * page tables, so the mapping outlives the binding; if unmapping it then
 * required the binding, that virtual address would be dead space for the rest
 * of the context's life.
 */
static void test_fb_mapping_removable_after_reclaim(void)
{
    const fb_geometry_t *geom = fb_binding_geometry();
    CU_ASSERT_PTR_NOT_NULL(geom);
    if (geom == NULL)
        return;

    page_owner_t me = syscall_current_context();
    uint64_t fb_page = geom->phys_addr & ~(uint64_t)(VMM_PAGE_SIZE - 1);

    CU_ASSERT_EQUAL(fb_binding_acquire(me), FB_BIND_OK);
    CU_ASSERT_EQUAL(do_map(SCRATCH_2, fb_page, MAP_RW), 0);

    /* The kernel takes the screen back; the mapping is untouched by that. */
    CU_ASSERT_EQUAL(fb_binding_reclaim(me), FB_REVOKE_OK);
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);

    /* Mapping it again is refused — the binding is gone... */
    CU_ASSERT_EQUAL(do_map(SCRATCH_2, fb_page, MAP_RW), -EXO_EPERM);

    /* ...but letting go of it is not. */
    CU_ASSERT_EQUAL(do_unmap(SCRATCH_2), 0);

    uint64_t resolved = 0;
    CU_ASSERT_EQUAL(vmm_translate(SCRATCH_2, &resolved, NULL), VMM_ENOENT);
}

/* The suite borrows the framebuffer binding and allocates under a second
 * context id; neither may outlive it, or the boot console downstream loses the
 * screen and the revocation demo's page accounting shifts under it. */
int page_map_suite_cleanup(void)
{
    fb_binding_release(fb_binding_owner());
    page_reclaim_all(OTHER_LIBOS);
    return 0;
}

void suite_page_map_tests(CU_pSuite s)
{
    CU_add_test(s, "handlers are bound",         test_handlers_are_bound);
    CU_add_test(s, "window is clear of kernel mappings",
                test_window_is_clear_of_kernel_mappings);
    CU_add_test(s, "map own page round trip",    test_map_own_page_round_trip);
    CU_add_test(s, "foreign page rejected",      test_foreign_page_rejected);
    CU_add_test(s, "kernel page rejected",       test_kernel_page_rejected);
    CU_add_test(s, "unowned page rejected",      test_unowned_page_rejected);
    CU_add_test(s, "remap over foreign rejected", test_remap_over_foreign_mapping_rejected);
    CU_add_test(s, "vaddr outside window",       test_vaddr_outside_window_rejected);
    CU_add_test(s, "bad arguments rejected",     test_bad_arguments_rejected);
    CU_add_test(s, "framebuffer follows binding", test_framebuffer_follows_the_binding);
    CU_add_test(s, "fb mapping removable after reclaim",
                test_fb_mapping_removable_after_reclaim);
}
