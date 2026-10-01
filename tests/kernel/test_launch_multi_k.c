/*
 * test_launch_multi_k.c — SCRUM-196: EXO_SYS_LAUNCH no longer tears down a
 * previous instance of the same app before creating a new one, so multiple
 * concurrent instances of one app (the SCRUM-195 dual-Doom prerequisite) are
 * now possible. This suite drives the real, bound sys_launch() handler
 * (src/syscall_launch.c) through exo_syscall_dispatch(EXO_SYS_LAUNCH, ...)
 * end to end -- the same direct-dispatch pattern test_exo_syscall_k.c
 * already uses for the app-id bounds-check cases, extended here to real
 * successful launches.
 *
 * EXO_LAUNCH_APP_SNAKE is used throughout rather than _DOOM: both flow
 * through the exact same generic sys_launch() handler, but Snake's blob is
 * far smaller and LAUNCH_NEEDS_WAD is unset for it, so this suite has no
 * multiboot WAD module dependency.
 *
 * Every test here first needs a real, live "foreground" context of its own
 * -- context_switch_request() inside sys_launch() refuses to arm a switch
 * unless find_slot(context_current()) resolves to something real (see that
 * function's own comment in src/context.c), and PAGE_OWNER_LIBOS has no such
 * row in a TESTING build unless some earlier suite happened to create one.
 * make_foreground() below builds one exactly like
 * test_context_launch_rebind_k.c's own create_and_launch() helper does,
 * using the same libos_launch_probe.s blob, and makes it
 * context_current()/CONTEXT_STATE_RUNNING by hand -- the same seam that file
 * documents using, for the same reason.
 */

#include "kunit.h"
#include "context.h"
#include "vmm.h"
#include "page_alloc.h"
#include "libos_launch.h"
#include "exo_syscall.h"
#include "syscall.h"

#include <stdint.h>

extern void libos_launch_probe(void);
extern void libos_launch_probe_end(void);

/* Same shape as context_launch_rebind_suite_cleanup() /
 * syscall_yield_suite_cleanup(): a failed assertion mid-test must not leak a
 * context (and its code/data/stack pages) into a later suite. */
#define LAUNCH_MULTI_CLEANUP_SCAN_IDS 4096

int launch_multi_suite_cleanup(void)
{
    for (uint32_t offset = 0; offset < LAUNCH_MULTI_CLEANUP_SCAN_IDS; offset++) {
        page_owner_t id = (page_owner_t)(PAGE_OWNER_LIBOS + offset);
        if (context_lookup(id) != NULL) {
            page_reclaim_all(id);
            context_destroy(id);
        }
    }
    /* Same fixup test_context_launch_rebind_k.c and test_syscall_yield_k.c
     * both need: a real EXO_SYS_LAUNCH success arms context_switch_pending
     * via context_switch_request(), and this suite never drives a real
     * context_switch_tail commit to consume it. Left set, the next real
     * switch anywhere later in the run would take context_switch_tail using
     * these now-destroyed contexts' dangling regs pointers. */
    context_switch_pending = 0;
    context_set_current(PAGE_OWNER_LIBOS);
    return 0;
}

/* Builds one real, live context and makes it the current/RUNNING context --
 * the "shell" stand-in every sys_launch() call in this suite launches from.
 * Same construction as test_context_launch_rebind_k.c's create_and_launch(). */
static page_owner_t make_foreground(void)
{
    page_owner_t id = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(vmm_kernel_pml4(), &id), CONTEXT_OK);

    libos_image_t img;
    size_t code_len = (uintptr_t)&libos_launch_probe_end -
                      (uintptr_t)&libos_launch_probe;
    CU_ASSERT_EQUAL(libos_build_image(id, (const void *)&libos_launch_probe,
                                      code_len, 0, 0, 0, &img),
                   VMM_OK);
    CU_ASSERT_EQUAL(context_prime(id, img.entry_vaddr, img.stack_top_vaddr),
                   CONTEXT_OK);

    context_set_current(id);
    context_set_state(id, CONTEXT_STATE_RUNNING);
    return id;
}

/* context_switch_request() never re-checks the outgoing side's state (only
 * the target's, see src/context.c), so context_current() staying `fg` across
 * repeated launches in this suite is enough on its own -- the only thing
 * that must be reset between two real launches in the same test is the
 * staged-but-never-committed switch from the previous one. */
static void settle_switch(page_owner_t fg)
{
    context_switch_pending = 0;
    context_set_current(fg);
}

static int64_t launch_snake(void)
{
    return exo_syscall_dispatch(EXO_SYS_LAUNCH, EXO_LAUNCH_APP_SNAKE,
                                0, 0, 0, 0, 0);
}

/* Finds the one live id in `list[0..n)` that isn't any of the (up to three)
 * ids to exclude -- used throughout to pick out "whatever context this
 * launch just created" without sys_launch() handing the id back directly
 * (it can't: the real ABI returns 0 on success, per exo_syscall.h). */
static page_owner_t find_new_id(const context_info_t *list, uint32_t n,
                                page_owner_t exclude_a, page_owner_t exclude_b,
                                page_owner_t exclude_c)
{
    for (uint32_t i = 0; i < n; i++) {
        page_owner_t id = list[i].id;
        if (id != exclude_a && id != exclude_b && id != exclude_c) {
            return id;
        }
    }
    return PAGE_OWNER_FREE;
}

static void test_two_launches_of_same_app_yield_two_live_contexts(void)
{
    page_owner_t fg = make_foreground();

    CU_ASSERT_EQUAL(launch_snake(), 0);

    context_info_t live[CONTEXT_MAX];
    uint32_t n = context_list(live, CONTEXT_MAX);
    page_owner_t first_id = find_new_id(live, n, fg, PAGE_OWNER_FREE,
                                        PAGE_OWNER_FREE);
    CU_ASSERT_NOT_EQUAL(first_id, PAGE_OWNER_FREE);
    CU_ASSERT_PTR_NOT_NULL(context_lookup(first_id));

    settle_switch(fg);
    CU_ASSERT_EQUAL(launch_snake(), 0);

    n = context_list(live, CONTEXT_MAX);
    page_owner_t second_id = find_new_id(live, n, fg, first_id,
                                         PAGE_OWNER_FREE);
    CU_ASSERT_NOT_EQUAL(second_id, PAGE_OWNER_FREE);
    CU_ASSERT_NOT_EQUAL(second_id, first_id);

    /* Both instances are still live -- the whole point of this ticket. The
     * old last_id-keyed guard would have destroyed `first_id` the moment the
     * second launch ran. */
    CU_ASSERT_PTR_NOT_NULL(context_lookup(first_id));
    CU_ASSERT_PTR_NOT_NULL(context_lookup(second_id));

    settle_switch(fg);
}

/* exo_exit() (src/syscall_exit.c) leaves an exited context's row behind,
 * downgraded to CONTEXT_STATE_BLOCKED rather than destroyed (SCRUM-111).
 * sys_launch()'s table-wide sweep must reclaim a row in that state on the
 * next launch of ANYTHING, while leaving a still-live (READY) row from a
 * concurrent instance alone. */
static void test_blocked_context_reclaimed_but_live_one_survives(void)
{
    page_owner_t fg = make_foreground();

    CU_ASSERT_EQUAL(launch_snake(), 0);
    context_info_t live[CONTEXT_MAX];
    uint32_t n = context_list(live, CONTEXT_MAX);
    page_owner_t exited_id = find_new_id(live, n, fg, PAGE_OWNER_FREE,
                                         PAGE_OWNER_FREE);
    CU_ASSERT_NOT_EQUAL(exited_id, PAGE_OWNER_FREE);
    settle_switch(fg);

    /* Simulate exo_exit()'s own end state for this row. */
    CU_ASSERT_EQUAL(context_set_state(exited_id, CONTEXT_STATE_BLOCKED),
                   CONTEXT_OK);

    /* A second, unrelated live instance that must survive the sweep below. */
    CU_ASSERT_EQUAL(launch_snake(), 0);
    n = context_list(live, CONTEXT_MAX);
    page_owner_t still_ready_id = find_new_id(live, n, fg, exited_id,
                                              PAGE_OWNER_FREE);
    CU_ASSERT_NOT_EQUAL(still_ready_id, PAGE_OWNER_FREE);
    settle_switch(fg);

    /* Third launch's sweep must reclaim the BLOCKED row from the first
     * launch, while leaving the still-READY row from the second alone. */
    CU_ASSERT_EQUAL(launch_snake(), 0);

    CU_ASSERT_PTR_NULL(context_lookup(exited_id));
    CU_ASSERT_PTR_NOT_NULL(context_lookup(still_ready_id));

    settle_switch(fg);
}

/* CONTEXT_MAX (14) live rows total. Launching past that must fail cleanly --
 * context_create()'s existing CONTEXT_ENOMEM, surfaced as -EXO_ENOMEM -- not
 * corrupt state, per this ticket's acceptance criteria. */
static void test_launch_past_capacity_fails_gracefully(void)
{
    page_owner_t fg = make_foreground();

    uint32_t launched = 0;
    int64_t rc = 0;
    for (uint32_t i = 0; i < CONTEXT_MAX; i++) {
        rc = launch_snake();
        settle_switch(fg);
        if (rc != 0) {
            break;
        }
        launched++;
    }

    /* fg itself holds one of the CONTEXT_MAX rows, so at most CONTEXT_MAX-1
     * further launches can ever succeed; capacity must actually have been
     * hit within this loop rather than the loop simply running out. */
    CU_ASSERT_TRUE(launched > 0);
    CU_ASSERT_TRUE(launched < CONTEXT_MAX);
    CU_ASSERT_EQUAL(rc, -EXO_ENOMEM);

    /* The failure must be clean: fg's own row is untouched and still
     * resolves, and a further call keeps failing the same way rather than
     * doing something worse. */
    CU_ASSERT_PTR_NOT_NULL(context_lookup(fg));
    CU_ASSERT_EQUAL(launch_snake(), -EXO_ENOMEM);
    settle_switch(fg);
}

void suite_launch_multi_tests(CU_pSuite s)
{
    CU_add_test(s, "two launches of the same app yield two live contexts",
                test_two_launches_of_same_app_yield_two_live_contexts);
    CU_add_test(s, "a blocked context is reclaimed but a live one survives",
                test_blocked_context_reclaimed_but_live_one_survives);
    CU_add_test(s, "launching past context table capacity fails gracefully",
                test_launch_past_capacity_fails_gracefully);
}
