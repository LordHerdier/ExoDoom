/*
 * test_context_launch_rebind_k.c — the create-with-placeholder-pml4-then-
 * rebind launch pattern SCRUM-178 introduces (src/kernel.c's shell launch,
 * src/syscall_launch.c's WAD viewer launch).
 *
 * Neither test_context_k.c nor test_context_switch_k.c exercises this
 * specific sequence: both call context_create() with a pml4 from
 * vmm_create_address_space() that is never rebound afterward.  SCRUM-178's
 * two real launch sites instead call context_create() with a placeholder
 * pml4 (vmm_kernel_pml4()) purely to reserve an id and a table row, then
 * let libos_build_image() rebind that same id's vmm entry to the real
 * address space it builds ("vmm_bind_address_space() allows a rebind",
 * src/vmm.c). This suite proves that sequence lands correctly, and that a
 * context created this way is a completely ordinary context_switch_request()
 * participant afterward — the exact property that lets the shell (built
 * this way since SCRUM-178's kernel.c refactor) launch a second LibOS and
 * later be switched back to.
 */

#include "kunit.h"
#include "context.h"
#include "vmm.h"
#include "page_alloc.h"
#include "libos_launch.h"

#include <stdint.h>

extern void libos_launch_probe(void);
extern void libos_launch_probe_end(void);

/* Same shape as context_switch_suite_cleanup() (test_context_switch_k.c):
 * a launched context's code/data/stack pages need page_reclaim_all(), not
 * just context_destroy()'s vmm_destroy_address_space(), or a failed
 * assertion mid-test leaks pages into later suites. */
#define LAUNCH_REBIND_CLEANUP_SCAN_IDS 4096

int context_launch_rebind_suite_cleanup(void)
{
    for (uint32_t offset = 0; offset < LAUNCH_REBIND_CLEANUP_SCAN_IDS; offset++) {
        page_owner_t id = (page_owner_t)(PAGE_OWNER_LIBOS + offset);
        if (context_lookup(id) != NULL) {
            page_reclaim_all(id);
            context_destroy(id);
        }
    }
    return 0;
}

static page_owner_t create_and_launch(libos_image_t *img_out)
{
    page_owner_t id = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(vmm_kernel_pml4(), &id), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_pml4(id), vmm_kernel_pml4());

    size_t code_len = (uintptr_t)&libos_launch_probe_end -
                      (uintptr_t)&libos_launch_probe;
    CU_ASSERT_EQUAL(libos_build_image(id, (const void *)&libos_launch_probe,
                                      code_len, 0, 0, 0, img_out),
                   VMM_OK);
    CU_ASSERT_EQUAL(context_prime(id, img_out->entry_vaddr,
                                  img_out->stack_top_vaddr),
                   CONTEXT_OK);
    return id;
}

/* The rebind itself: libos_build_image() must replace the placeholder pml4
 * context_create() reserved, and both context.c's and vmm.c's view of "what
 * pml4 does this id run on" must agree afterward. */
static void test_libos_build_image_rebinds_placeholder(void)
{
    libos_image_t img;
    page_owner_t id = create_and_launch(&img);

    CU_ASSERT_NOT_EQUAL(img.pml4_phys, vmm_kernel_pml4());
    CU_ASSERT_EQUAL(context_pml4(id), img.pml4_phys);
    CU_ASSERT_EQUAL(vmm_address_space_for(id), img.pml4_phys);

    /* context_destroy() alone (not libos_destroy_image(), which would also
     * vmm_destroy_address_space() and make this ENOENT) -- same pattern
     * context_switch_suite_cleanup() documents: the code/data/stack pages
     * this leaves owned by `id` are swept by this suite's own cleanup via
     * page_reclaim_all(), not freed per test. */
    CU_ASSERT_EQUAL(context_destroy(id), CONTEXT_OK);
}

/* The property SCRUM-178 actually needs: a context launched via
 * create-then-rebind is switchable exactly like any other, in both
 * directions, once it is context_set_current(). This is what makes the
 * shell (built this way since SCRUM-178) a valid context_switch_request()
 * source rather than the boot-time default that refusal
 * (from == NULL in context_switch_request(), src/context.c) used to catch. */
static void test_create_then_rebind_contexts_switch_both_ways(void)
{
    libos_image_t img_a, img_b;
    page_owner_t shell_like = create_and_launch(&img_a);
    page_owner_t viewer_like = create_and_launch(&img_b);

    context_set_current(shell_like);
    context_set_state(shell_like, CONTEXT_STATE_RUNNING);

    /* SCRUM-179: context_switch_request() only *stages* the switch --
     * context_current() keeps naming the outgoing context until
     * context_switch_tail (src/context_switch.s) actually commits it,
     * which this test never drives. Simulate that commit by hand via
     * context_set_current(), the same seam this test already uses to
     * declare the very first running context above. */
    CU_ASSERT_EQUAL(context_switch_request(viewer_like), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_current(), shell_like);
    context_set_current(viewer_like);
    CU_ASSERT_EQUAL(context_current(), viewer_like);

    CU_ASSERT_EQUAL(context_next_ready(viewer_like), shell_like);
    CU_ASSERT_EQUAL(context_switch_request(shell_like), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_current(), viewer_like);
    context_set_current(shell_like);
    CU_ASSERT_EQUAL(context_current(), shell_like);

    /* Neither context_switch_request() call above was ever driven through a
     * real context_switch_tail commit -- both are only simulated by hand via
     * context_set_current() -- so context_switch_pending is still armed from
     * the second call, with context_switch_out_regs/_in_regs/_in_pml4/_in_id
     * all pointing at these two contexts. Left set, the next real switch
     * anywhere later in the run (e.g. test_syscall_bench_k.c's real ring-3
     * benchmarks) would take context_switch_tail using these now-destroyed
     * contexts' dangling regs pointers and a freed physical page as the PML4
     * to load into CR3 -- silently harmless before SCRUM-179 (nothing read
     * context_switch_in_id), but a real, visible corruption now that
     * context_switch_tail also commits it via context_set_current(). Same
     * fixup tests/kernel/test_kbd_ring.c and test_syscall_exit_k.c already
     * need for the same reason. */
    context_switch_pending = 0;

    /* Restore the default every other suite assumes (test_context_switch_k.c
     * and test_syscall_yield_k.c both do the same after driving a real
     * switch). */
    context_set_current(PAGE_OWNER_LIBOS);

    CU_ASSERT_EQUAL(context_destroy(shell_like), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_destroy(viewer_like), CONTEXT_OK);
}

void suite_context_launch_rebind_tests(CU_pSuite s)
{
    CU_add_test(s, "libos_build_image rebinds a placeholder pml4",
                test_libos_build_image_rebinds_placeholder);
    CU_add_test(s, "create-then-rebind contexts switch both ways",
                test_create_then_rebind_contexts_switch_both_ways);
}
