/*
 * test_fault_contain_k.c — a faulting ring-3 program is ended, not fatal
 * (SCRUM-283).
 *
 * Unlike test_fault_k.c / test_kernel_mem_fault_k.c this installs no fault
 * hook: every fault here goes through the real page_fault_handler /
 * gp_fault_handler -> fault_terminate_current() -> context_teardown() ->
 * context_resume_noreturn() path. Each round builds a victim context that
 * takes one fault and a primed survivor; the survivor reaching
 * libos_return() with its marker proves control really left the exception
 * frame, switched address spaces and resumed another program in ring 3.
 *
 * Reuses context_switch_k's hand-built contexts (two context_create()'d
 * address spaces, one page each for code/data/stack) rather than
 * libos_build_image(), which binds the address space to its own owner.
 * Vectors other than #PF/#GP are SCRUM-283 step 4.
 */

#include "kunit.h"
#include "context.h"
#include "vmm.h"
#include "page_alloc.h"
#include "libos_launch.h"
#include "exo_syscall.h"
#include "syscall.h"
#include "string.h"

#include <stdint.h>

extern void fault_contain_victim(void);
extern void fault_contain_victim_end(void);
extern void fault_contain_survivor(void);
extern void fault_contain_survivor_end(void);

#define SURVIVOR_MARKER 0x600DC0DEULL /* must match fault_contain_probe.s */
#define FAULT_KINDS     5             /* selector values the victim handles */
#define LOOP_ROUNDS     25            /* 5 rounds of every kind */

#define CLEANUP_SCAN_IDS 4096

int fault_contain_suite_cleanup(void)
{
    for (uint32_t offset = 0; offset < CLEANUP_SCAN_IDS; offset++) {
        page_owner_t id = (page_owner_t)(PAGE_OWNER_LIBOS + offset);
        if (context_lookup(id) != NULL) {
            page_reclaim_all(id);
            context_destroy(id);
        }
    }
    return 0;
}

static uint64_t map_page(page_owner_t owner, uint64_t *pml4, uint64_t vaddr,
                         uint64_t flags, const void *src, size_t len)
{
    void *page = alloc_page_owned(owner);
    CU_ASSERT_PTR_NOT_NULL(page);
    if (page == NULL)
        return 0;
    memset(page, 0, VMM_PAGE_SIZE);
    if (len > 0)
        memcpy(page, src, len);
    CU_ASSERT_EQUAL(
        vmm_map_page_in(pml4, vaddr, (uint64_t)(uintptr_t)page, flags), VMM_OK);
    return (uint64_t)(uintptr_t)page;
}

/* A context with `code` at the code window, a data page and a stack. Returns
 * the data page's physical address (a valid ring-0 pointer: identity map). */
static uint64_t make_context(const void *code, size_t len, page_owner_t *id,
                             uint64_t *pml4_phys)
{
    CU_ASSERT_EQUAL(vmm_create_address_space(pml4_phys), VMM_OK);
    CU_ASSERT_EQUAL(context_create(*pml4_phys, id), CONTEXT_OK);
    uint64_t *pml4 = (uint64_t *)(uintptr_t)*pml4_phys;

    map_page(*id, pml4, LIBOS_LAUNCH_CODE_VADDR, VMM_PRESENT | VMM_USER,
             code, len);
    uint64_t data = map_page(*id, pml4, LIBOS_LAUNCH_DATA_VADDR,
                             VMM_PRESENT | VMM_USER | VMM_WRITE, NULL, 0);
    map_page(*id, pml4, LIBOS_LAUNCH_STACK_VADDR,
             VMM_PRESENT | VMM_USER | VMM_WRITE, NULL, 0);
    return data;
}

/* One round: victim takes fault `kind`, survivor must run. Asserts the
 * teardown and hand-off; leaves both rows for the caller to reap. */
static void run_round(int kind, page_owner_t *victim_out,
                      page_owner_t *survivor_out)
{
    page_owner_t victim = PAGE_OWNER_FREE, survivor = PAGE_OWNER_FREE;
    uint64_t pml4_v = 0, pml4_s = 0;

    uint64_t data_v = make_context(
        (const void *)&fault_contain_victim,
        (uintptr_t)&fault_contain_victim_end - (uintptr_t)&fault_contain_victim,
        &victim, &pml4_v);
    make_context(
        (const void *)&fault_contain_survivor,
        (uintptr_t)&fault_contain_survivor_end -
            (uintptr_t)&fault_contain_survivor,
        &survivor, &pml4_s);

    *(uint64_t *)(uintptr_t)data_v = (uint64_t)kind;
    CU_ASSERT_EQUAL(context_prime(survivor, LIBOS_LAUNCH_CODE_VADDR,
                                  LIBOS_LAUNCH_STACK_VADDR + VMM_PAGE_SIZE),
                    CONTEXT_OK);
    CU_ASSERT_TRUE(page_count_owned(victim) > 0);

    context_set_current(victim);
    CU_ASSERT_EQUAL(context_set_state(victim, CONTEXT_STATE_RUNNING),
                    CONTEXT_OK);
    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, libos_return);

    CU_ASSERT_EQUAL(vmm_switch_address_space(pml4_v), VMM_OK);
    uint64_t result = libos_enter(LIBOS_LAUNCH_CODE_VADDR,
                                  LIBOS_LAUNCH_STACK_VADDR + VMM_PAGE_SIZE);
    CU_ASSERT_EQUAL(vmm_switch_address_space(vmm_kernel_pml4()), VMM_OK);
    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, 0);

    /* The survivor ran: the machine did not halt. */
    CU_ASSERT_EQUAL(result, SURVIVOR_MARKER);
    CU_ASSERT_EQUAL(context_current(), survivor);
    CU_ASSERT_EQUAL(context_lookup(survivor)->state, CONTEXT_STATE_RUNNING);

    /* The victim is dead and resourceless, but its row stays BLOCKED so
     * round-robin skips it until the next launch reaps it (SCRUM-111). */
    CU_ASSERT_EQUAL(context_lookup(victim)->state, CONTEXT_STATE_BLOCKED);
    CU_ASSERT_EQUAL(page_count_owned(victim), 0);
    CU_ASSERT_EQUAL(context_next_ready(survivor), PAGE_OWNER_FREE);

    *victim_out = victim;
    *survivor_out = survivor;
}

static void reap(page_owner_t victim, page_owner_t survivor)
{
    CU_ASSERT_EQUAL(context_destroy(victim), CONTEXT_OK);
    page_reclaim_all(survivor);
    CU_ASSERT_EQUAL(context_destroy(survivor), CONTEXT_OK);
    context_set_current(PAGE_OWNER_LIBOS);
}

static void test_each_fault_kind_is_contained(void)
{
    for (int kind = 0; kind < FAULT_KINDS; kind++) {
        page_owner_t v, s;
        run_round(kind, &v, &s);
        reap(v, s);
    }
}

/* Launch -> fault -> reap, repeatedly: neither pages nor context rows may
 * leak across rounds. */
static void test_fault_loop_does_not_leak(void)
{
    uint32_t free_before = 0, free_after = 0, ignore;
    page_alloc_totals(&ignore, &free_before, &ignore, &ignore);
    uint32_t rows_before = context_count();

    for (int i = 0; i < LOOP_ROUNDS; i++) {
        page_owner_t v, s;
        run_round(i % FAULT_KINDS, &v, &s);
        reap(v, s);
    }

    page_alloc_totals(&ignore, &free_after, &ignore, &ignore);
    CU_ASSERT_EQUAL(free_after, free_before);
    CU_ASSERT_EQUAL(context_count(), rows_before);
}

void suite_fault_contain_tests(CU_pSuite s)
{
    CU_add_test(s, "each ring-3 fault kind ends the program, not the machine",
                test_each_fault_kind_is_contained);
    CU_add_test(s, "launch-fault loop leaks no pages or context rows",
                test_fault_loop_does_not_leak);
}
