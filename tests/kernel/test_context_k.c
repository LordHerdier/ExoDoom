/*
 * test_context_k.c — process/context table (SCRUM-107).
 *
 * Proves the acceptance criterion directly: the kernel can track 2+ LibOS
 * contexts' id/page-dir/registers/state at once. Each test builds its own
 * real address space(s) via vmm_create_address_space() (same primitive
 * test_vmm_k.c's address-space suite uses) and hands the raw PML4 to
 * context_create(), which is the thing actually under test here.
 *
 * context_destroy() fully tears an address space down (vmm_destroy_address_
 * space() under the hood, see context.c), so a test that creates but does
 * not itself destroy a context leaks a PML4 for the suite cleanup below to
 * catch -- the same shape test_vmm_k.c's ADDRSPACE_TEST_OWNER tests use,
 * except context.c hands out its own ids rather than a fixed sentinel, so
 * cleanup has to ask the table what's still live rather than naming one id.
 */

#include "kunit.h"
#include "context.h"
#include "vmm.h"
#include "page_alloc.h"

#include <stdint.h>

/* Sweep every context still live at suite end and tear it down, so a failed
 * assertion mid-test can't leak a PML4 into later suites. Registered as this
 * suite's CU_add_suite cleanup in test_runner.c.
 *
 * Bounded well past anything this suite could ever actually hand out --
 * context_create() only ever advances its id counter by CONTEXT_MAX (4) per
 * test at most, across a handful of tests in this one file -- rather than
 * walking the full 15-bit id space PAGE_OWNER_ID_MASK allows, which a linear
 * context_lookup() scan per id would make needlessly slow for no benefit
 * here. */
#define CONTEXT_CLEANUP_SCAN_IDS 4096

int context_suite_cleanup(void)
{
    for (uint32_t offset = 0; offset < CONTEXT_CLEANUP_SCAN_IDS; offset++) {
        page_owner_t id = (page_owner_t)(PAGE_OWNER_LIBOS + offset);
        if (context_lookup(id) != NULL) {
            context_destroy(id);
        }
    }
    return 0;
}

static uint64_t new_address_space(void)
{
    uint64_t phys = 0;
    CU_ASSERT_EQUAL(vmm_create_address_space(&phys), VMM_OK);
    CU_ASSERT_NOT_EQUAL(phys, 0);
    return phys;
}

/* context_create()'s id allocator must never hand out an id already bound
 * in vmm.c's own address-space registry -- src/kernel.c binds
 * PAGE_OWNER_LIBOS there directly (ahead of the TESTING branch, so this
 * holds on every boot including this test run), entirely outside
 * context.c's table, and context_create()'s id search starts at that exact
 * id. Regression test for the bug where find_slot() alone (checking only
 * context.c's table, which starts empty) let the very first
 * context_create() call silently steal and rebind PAGE_OWNER_LIBOS's
 * kernel-map binding with no error. */
static void test_create_never_steals_libos_binding(void)
{
    uint64_t kernel_pml4_before = vmm_address_space_for(PAGE_OWNER_LIBOS);
    CU_ASSERT_NOT_EQUAL(kernel_pml4_before, 0);

    uint64_t phys = new_address_space();
    page_owner_t id = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(phys, &id), CONTEXT_OK);

    CU_ASSERT_NOT_EQUAL(id, PAGE_OWNER_LIBOS);
    CU_ASSERT_EQUAL(vmm_address_space_for(PAGE_OWNER_LIBOS), kernel_pml4_before);

    CU_ASSERT_EQUAL(context_destroy(id), CONTEXT_OK);
    /* Still untouched after teardown too. */
    CU_ASSERT_EQUAL(vmm_address_space_for(PAGE_OWNER_LIBOS), kernel_pml4_before);
}

/* context_create() rejects a 0 pml4_phys rather than creating a live,
 * READY context that resolves to nothing -- 0 is the same sentinel
 * vmm_address_space_for()/context_pml4() use for "no binding". */
static void test_create_rejects_zero_pml4(void)
{
    page_owner_t id = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(0, &id), CONTEXT_EINVAL);
    CU_ASSERT_EQUAL(context_count(), 0);
}

/* Two contexts, each with its own address space, tracked at once -- the
 * acceptance criterion itself. */
static void test_two_contexts_simultaneously(void)
{
    uint64_t phys_a = new_address_space();
    uint64_t phys_b = new_address_space();
    CU_ASSERT_NOT_EQUAL(phys_a, phys_b);

    page_owner_t id_a = PAGE_OWNER_FREE, id_b = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(phys_a, &id_a), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_create(phys_b, &id_b), CONTEXT_OK);
    CU_ASSERT_NOT_EQUAL(id_a, id_b);

    CU_ASSERT_EQUAL(context_count(), 2);

    CU_ASSERT_EQUAL(context_pml4(id_a), phys_a);
    CU_ASSERT_EQUAL(context_pml4(id_b), phys_b);
    /* Cross-checked against the layer context.c itself calls into. */
    CU_ASSERT_EQUAL(vmm_address_space_for(id_a), phys_a);
    CU_ASSERT_EQUAL(vmm_address_space_for(id_b), phys_b);

    context_t *ca = context_lookup(id_a);
    context_t *cb = context_lookup(id_b);
    CU_ASSERT_PTR_NOT_NULL(ca);
    CU_ASSERT_PTR_NOT_NULL(cb);
    CU_ASSERT_EQUAL(ca->state, CONTEXT_STATE_READY);
    CU_ASSERT_EQUAL(cb->state, CONTEXT_STATE_READY);

    CU_ASSERT_EQUAL(context_destroy(id_a), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_count(), 1);
    CU_ASSERT_PTR_NULL(context_lookup(id_a));
    /* id_a's PML4 is gone with it -- the registry no longer resolves it. */
    CU_ASSERT_EQUAL(vmm_address_space_for(id_a), 0);

    CU_ASSERT_EQUAL(context_destroy(id_b), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_count(), 0);
}

/* A freshly created context starts with a zeroed register area -- the
 * save slot SCRUM-108's switch will fill in, not touched by anything yet. */
static void test_new_context_regs_are_zeroed(void)
{
    uint64_t phys = new_address_space();
    page_owner_t id = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(phys, &id), CONTEXT_OK);

    context_t *c = context_lookup(id);
    CU_ASSERT_PTR_NOT_NULL(c);
    CU_ASSERT_EQUAL(c->regs.rsp, 0);
    CU_ASSERT_EQUAL(c->regs.rip, 0);
    CU_ASSERT_EQUAL(c->regs.rflags, 0);
    CU_ASSERT_EQUAL(c->regs.rbx, 0);
    CU_ASSERT_EQUAL(c->regs.r15, 0);

    CU_ASSERT_EQUAL(context_destroy(id), CONTEXT_OK);
}

/* context_set_state moves a context through the states SCRUM-108's switch
 * will need -- nothing enforces a transition order at this layer, since
 * that policy belongs to the scheduler this ticket doesn't build. */
static void test_state_transitions(void)
{
    uint64_t phys = new_address_space();
    page_owner_t id = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(phys, &id), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_lookup(id)->state, CONTEXT_STATE_READY);

    CU_ASSERT_EQUAL(context_set_state(id, CONTEXT_STATE_RUNNING), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_lookup(id)->state, CONTEXT_STATE_RUNNING);

    CU_ASSERT_EQUAL(context_set_state(id, CONTEXT_STATE_BLOCKED), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_lookup(id)->state, CONTEXT_STATE_BLOCKED);

    CU_ASSERT_EQUAL(context_destroy(id), CONTEXT_OK);
}

/* An id that never named a live context (or no longer does) is CONTEXT_ENOENT
 * everywhere, never a stale/garbage read. */
static void test_unknown_id_is_enoent(void)
{
    page_owner_t bogus = (page_owner_t)(PAGE_OWNER_LIBOS + 1000);

    CU_ASSERT_PTR_NULL(context_lookup(bogus));
    CU_ASSERT_EQUAL(context_pml4(bogus), 0);
    CU_ASSERT_EQUAL(context_set_state(bogus, CONTEXT_STATE_RUNNING),
                    CONTEXT_ENOENT);
    CU_ASSERT_EQUAL(context_destroy(bogus), CONTEXT_ENOENT);
}

/* Destroying frees the slot (and the id) for reuse by a later create --
 * the table is keyed on "is this id live right now", not a monotonic
 * history. */
static void test_destroyed_id_is_reusable(void)
{
    uint64_t phys_1 = new_address_space();
    page_owner_t id = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(phys_1, &id), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_destroy(id), CONTEXT_OK);
    CU_ASSERT_PTR_NULL(context_lookup(id));

    uint64_t phys_2 = new_address_space();
    page_owner_t id_2 = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(phys_2, &id_2), CONTEXT_OK);
    CU_ASSERT_PTR_NOT_NULL(context_lookup(id_2));

    CU_ASSERT_EQUAL(context_destroy(id_2), CONTEXT_OK);
}

/* The table has no more room than vmm's own address-space registry can
 * actually back (CONTEXT_MAX == VMM_MAX_ADDRESS_SPACES - 1, by design --
 * see context.h: one of vmm's VMM_MAX_ADDRESS_SPACES slots is permanently
 * occupied by src/kernel.c's boot-time PAGE_OWNER_LIBOS binding) -- the
 * (CONTEXT_MAX+1)th concurrent context is refused cleanly rather than
 * silently evicting one already tracked or, worse, silently rebinding
 * PAGE_OWNER_LIBOS's own entry. */
static void test_table_full_is_enomem(void)
{
    page_owner_t ids[CONTEXT_MAX];
    uint64_t phys[CONTEXT_MAX];

    for (int i = 0; i < CONTEXT_MAX; i++) {
        phys[i] = new_address_space();
        CU_ASSERT_EQUAL(context_create(phys[i], &ids[i]), CONTEXT_OK);
    }
    CU_ASSERT_EQUAL(context_count(), CONTEXT_MAX);

    uint64_t extra_phys = new_address_space();
    page_owner_t extra_id = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(extra_phys, &extra_id), CONTEXT_ENOMEM);
    CU_ASSERT_EQUAL(context_count(), CONTEXT_MAX);

    /* The address space context_create() refused to bind is still this
     * test's to clean up -- context.c never touched it. vmm's own registry
     * has exactly zero free slots right now too (CONTEXT_MAX contexts here
     * plus the permanent PAGE_OWNER_LIBOS entry account for all
     * VMM_MAX_ADDRESS_SPACES of them), so there is still no free registry
     * slot to bind extra_phys into until one of the CONTEXT_MAX contexts
     * above gives one back first. */
    CU_ASSERT_EQUAL(context_destroy(ids[0]), CONTEXT_OK);
    CU_ASSERT_EQUAL(vmm_bind_address_space(
                        (page_owner_t)(PAGE_OWNER_LIBOS + 500), extra_phys),
                    VMM_OK);
    CU_ASSERT_EQUAL(vmm_destroy_address_space(
                        (page_owner_t)(PAGE_OWNER_LIBOS + 500)), VMM_OK);

    for (int i = 1; i < CONTEXT_MAX; i++) {
        CU_ASSERT_EQUAL(context_destroy(ids[i]), CONTEXT_OK);
    }
    CU_ASSERT_EQUAL(context_count(), 0);
}

void suite_context_tests(CU_pSuite s)
{
    CU_add_test(s, "create never steals the LibOS binding",
               test_create_never_steals_libos_binding);
    CU_add_test(s, "create rejects zero pml4", test_create_rejects_zero_pml4);
    CU_add_test(s, "two contexts tracked simultaneously",
               test_two_contexts_simultaneously);
    CU_add_test(s, "new context has zeroed regs", test_new_context_regs_are_zeroed);
    CU_add_test(s, "state transitions", test_state_transitions);
    CU_add_test(s, "unknown id is ENOENT", test_unknown_id_is_enoent);
    CU_add_test(s, "destroyed id is reusable", test_destroyed_id_is_reusable);
    CU_add_test(s, "table full is ENOMEM", test_table_full_is_enomem);
}
