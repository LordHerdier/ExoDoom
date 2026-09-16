#include "kunit.h"

#include "context.h"
#include "exo_syscall.h"
#include "fb_binding.h"
#include "fb_shadow.h"
#include "page_alloc.h"
#include "syscall.h"
#include "syscall_exit.h"
#include "vmm.h"

static void test_exit_handler_is_bound(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_EXIT));
}

static void test_exit_reclaims_pages_and_framebuffer(void)
{
    page_owner_t owner = syscall_current_context();

    void *p1 = alloc_page_owned(owner);
    void *p2 = alloc_page_owned(owner);

    CU_ASSERT_PTR_NOT_NULL(p1);
    CU_ASSERT_PTR_NOT_NULL(p2);

    CU_ASSERT_EQUAL(fb_binding_acquire(owner), FB_BIND_OK);
    CU_ASSERT_EQUAL(fb_binding_owner(), owner);

    /* SCRUM-112: exo_exit must also drop this context's virtual framebuffer
     * directory entry — fb_shadow_acquire() is a real acquire here (a real
     * framebuffer must be published for it to succeed at all; see
     * suite_syscall_exit_tests()'s registration in test_runner.c, which runs
     * after boot has published one). */
    exo_fb_info_t shadow_info;
    int shadow_rc = fb_shadow_acquire(owner, &shadow_info);
    CU_ASSERT_EQUAL(shadow_rc, FB_SHADOW_OK);

    uint64_t shadow_phys;
    if (shadow_rc == FB_SHADOW_OK) {
        CU_ASSERT_EQUAL(fb_shadow_lookup(owner, &shadow_phys), 0);
    }

    CU_ASSERT_EQUAL(
        exo_syscall_dispatch(EXO_SYS_EXIT, 7, 0, 0, 0, 0, 0),
        0);

    CU_ASSERT_EQUAL(page_owner(p1), PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(page_owner(p2), PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(fb_binding_owner(), PAGE_OWNER_FREE);
    if (shadow_rc == FB_SHADOW_OK) {
        CU_ASSERT_NOT_EQUAL(fb_shadow_lookup(owner, &shadow_phys), 0);
    }
}

static void test_exit_does_not_reclaim_kernel_page(void)
{
    void *kp = alloc_page();

    CU_ASSERT_PTR_NOT_NULL(kp);
    CU_ASSERT_EQUAL(page_owner(kp), PAGE_OWNER_KERNEL);

    CU_ASSERT_EQUAL(
        exo_syscall_dispatch(EXO_SYS_EXIT, 0, 0, 0, 0, 0, 0),
        0);

    CU_ASSERT_EQUAL(page_owner(kp), PAGE_OWNER_KERNEL);

    CU_ASSERT_EQUAL(free_page_checked(kp), 0);
}

/* SCRUM-111 regression: the context exo_exit() hands off from must not come
 * back as a round-robin target. Before this ticket's fix, context_switch_
 * request() left it CONTEXT_STATE_READY -- indistinguishable from a context
 * that voluntarily exo_yield()'d and is perfectly safe to resume -- even
 * though exo_exit() has already freed everything it owned. See src/
 * syscall_exit.c's own top comment for the full hazard (a round-robin into
 * this row, e.g. via the Ctrl+Tab hotkey, ran whatever garbage the freed
 * pages now held). */
static void test_exit_blocks_outgoing_context_from_round_robin(void)
{
    uint64_t phys_a = 0, phys_b = 0;
    page_owner_t id_a = PAGE_OWNER_FREE, id_b = PAGE_OWNER_FREE;

    CU_ASSERT_EQUAL(vmm_create_address_space(&phys_a), VMM_OK);
    CU_ASSERT_EQUAL(vmm_create_address_space(&phys_b), VMM_OK);
    CU_ASSERT_EQUAL(context_create(phys_a, &id_a), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_create(phys_b, &id_b), CONTEXT_OK);

    context_set_current(id_a);
    CU_ASSERT_EQUAL(context_set_state(id_a, CONTEXT_STATE_RUNNING), CONTEXT_OK);

    CU_ASSERT_EQUAL(
        exo_syscall_dispatch(EXO_SYS_EXIT, 0, 0, 0, 0, 0, 0),
        0);

    /* Handed off to id_b, but id_a is BLOCKED, not READY -- context_next_
     * ready() already skips BLOCKED rows, so it must not come back. */
    CU_ASSERT_EQUAL(context_current(), id_b);
    CU_ASSERT_EQUAL(context_lookup(id_a)->state, CONTEXT_STATE_BLOCKED);
    CU_ASSERT_EQUAL(context_next_ready(id_b), PAGE_OWNER_FREE);

    /* exo_exit()'s own context_switch_request() call armed context_switch_
     * pending; nothing in this test drives it through a real syscall trip to
     * consume it, so it must not leak into a later suite's real switch (see
     * tests/kernel/test_kbd_ring.c's SCRUM-111 tests for the same fixup). */
    context_switch_pending = 0;

    CU_ASSERT_EQUAL(context_destroy(id_a), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_destroy(id_b), CONTEXT_OK);
    context_set_current(PAGE_OWNER_LIBOS);
}

void suite_syscall_exit_tests(CU_pSuite s)
{
    CU_add_test(s, "handler is bound",
                test_exit_handler_is_bound);
    CU_add_test(s, "exit reclaims pages and framebuffer",
                test_exit_reclaims_pages_and_framebuffer);
    CU_add_test(s, "exit preserves kernel pages",
                test_exit_does_not_reclaim_kernel_page);
    CU_add_test(s, "exit blocks outgoing context from round-robin",
                test_exit_blocks_outgoing_context_from_round_robin);
}
