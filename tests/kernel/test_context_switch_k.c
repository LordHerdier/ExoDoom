/*
 * test_context_switch_k.c — context switch: save/restore registers + CR3
 * swap (SCRUM-108).
 *
 * Proves the acceptance criterion directly: switching between two LibOS
 * instances preserves all state correctly. Builds two real, separate
 * address spaces via vmm_create_address_space() + context_create() (the
 * same two-step test_context_k.c already uses), maps a tiny hand-written
 * ring-3 probe's code/data/stack into each by hand (not
 * libos_build_image(), which insists on binding the address space to its
 * caller-chosen owner itself -- context_create() has already done that),
 * then launches side A via the ordinary libos_enter() and lets the two
 * probes ping-pong through context_switch_request() via a test-local
 * temporary syscall handler, the same EXO_SYS_YIELD/EXO_SYS_EXIT borrow
 * pattern the rest of this tree already uses for libos_return().
 *
 * See tests/kernel/context_switch_probe_a.s / _b.s for what the launched
 * code actually does and its data layout.
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

extern void context_switch_probe_a(void);
extern void context_switch_probe_a_end(void);
extern void context_switch_probe_b(void);
extern void context_switch_probe_b_end(void);

#define SYS_SWITCH      EXO_SYS_YIELD
#define RESULT_MARKER   0x600DC0DEULL

/* Must match the probes' own .set values. */
#define MARK1     0xA000000000000001ULL
#define MARK2     0xA000000000000002ULL
#define MARK_B    0xB000000000000001ULL
#define SEED_RBX  0x1111111111111111ULL
#define SEED_RBP  0x2222222222222222ULL
#define SEED_R12  0x3333333333333333ULL
#define SEED_R13  0x4444444444444444ULL
#define SEED_R14  0x5555555555555555ULL
#define SEED_R15  0x6666666666666666ULL

/* Sweep every context this suite could have left live and free everything
 * it still owns -- the same shape context_suite_cleanup() (test_context_k.c)
 * uses, extended with page_reclaim_all() because, unlike that suite, this
 * one's contexts have real code/data/stack pages mapped into them (not just
 * a bare PML4), the same reason libos_test_teardown_owner() calls it. */
#define CONTEXT_SWITCH_CLEANUP_SCAN_IDS 4096

int context_switch_suite_cleanup(void)
{
    for (uint32_t offset = 0; offset < CONTEXT_SWITCH_CLEANUP_SCAN_IDS; offset++) {
        page_owner_t id = (page_owner_t)(PAGE_OWNER_LIBOS + offset);
        if (context_lookup(id) != NULL) {
            page_reclaim_all(id);
            context_destroy(id);
        }
    }
    return 0;
}

static int switch_calls;
static int switch_rc;

/* Test-local handler for SYS_SWITCH (borrowing EXO_SYS_YIELD, still unbound
 * in production -- exo_yield itself is SCRUM-109's job). `a1` is the target
 * context id, written by each probe from its own data page's peer_id field.
 * Registered/unregistered around the single launch below, same as
 * libos_return()/LIBOS_RETURN_SYSCALL_NUM already is throughout this tree. */
static int64_t test_switch_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    switch_calls++;
    switch_rc = context_switch_request((page_owner_t)a1);
    return 0;
}

/* Allocate one page owned by `owner`, copy `copy_len` bytes of `src` into it
 * (zeroing the rest), and map it at `vaddr` in `pml4`. Mirrors
 * libos_launch.c's load_region() for the single-page case every probe here
 * needs. Returns the page's physical address (also a valid ring-0 pointer,
 * since it stays identity-mapped -- the same technique
 * test_libos_launch_k.c's test_build_image_places_code_and_data uses to
 * check what actually landed). */
static uint64_t map_one_page(page_owner_t owner, uint64_t *pml4,
                             uint64_t vaddr, uint64_t flags,
                             const void *src, size_t copy_len)
{
    void *page = alloc_page_owned(owner);
    CU_ASSERT_PTR_NOT_NULL(page);
    if (copy_len > 0) {
        memcpy(page, src, copy_len);
    }
    if (copy_len < VMM_PAGE_SIZE) {
        memset((unsigned char *)page + copy_len, 0, VMM_PAGE_SIZE - copy_len);
    }
    CU_ASSERT_EQUAL(
        vmm_map_page_in(pml4, vaddr, (uint64_t)(uintptr_t)page, flags),
        VMM_OK);
    return (uint64_t)(uintptr_t)page;
}

static void test_switch_preserves_state_both_ways(void)
{
    uint64_t phys_a = 0, phys_b = 0;
    CU_ASSERT_EQUAL(vmm_create_address_space(&phys_a), VMM_OK);
    CU_ASSERT_EQUAL(vmm_create_address_space(&phys_b), VMM_OK);

    page_owner_t id_a = PAGE_OWNER_FREE, id_b = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(phys_a, &id_a), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_create(phys_b, &id_b), CONTEXT_OK);

    uint64_t *pml4_a = (uint64_t *)(uintptr_t)phys_a;
    uint64_t *pml4_b = (uint64_t *)(uintptr_t)phys_b;

    size_t code_len_a = (uintptr_t)&context_switch_probe_a_end -
                        (uintptr_t)&context_switch_probe_a;
    size_t code_len_b = (uintptr_t)&context_switch_probe_b_end -
                        (uintptr_t)&context_switch_probe_b;

    map_one_page(id_a, pml4_a, LIBOS_LAUNCH_CODE_VADDR,
                VMM_PRESENT | VMM_USER,
                (const void *)&context_switch_probe_a, code_len_a);
    map_one_page(id_b, pml4_b, LIBOS_LAUNCH_CODE_VADDR,
                VMM_PRESENT | VMM_USER,
                (const void *)&context_switch_probe_b, code_len_b);

    uint64_t data_paddr_a = map_one_page(id_a, pml4_a, LIBOS_LAUNCH_DATA_VADDR,
                                         VMM_PRESENT | VMM_USER | VMM_WRITE,
                                         NULL, 0);
    uint64_t data_paddr_b = map_one_page(id_b, pml4_b, LIBOS_LAUNCH_DATA_VADDR,
                                         VMM_PRESENT | VMM_USER | VMM_WRITE,
                                         NULL, 0);

    map_one_page(id_a, pml4_a, LIBOS_LAUNCH_STACK_VADDR,
                VMM_PRESENT | VMM_USER | VMM_WRITE, NULL, 0);
    map_one_page(id_b, pml4_b, LIBOS_LAUNCH_STACK_VADDR,
                VMM_PRESENT | VMM_USER | VMM_WRITE, NULL, 0);

    /* peer_id at data+0x00 for each side -- still identity-mapped for the
     * kernel right now, so this is an ordinary write. */
    *(uint64_t *)(uintptr_t)data_paddr_a = id_b;
    *(uint64_t *)(uintptr_t)data_paddr_b = id_a;

    /* Side B is never launched directly -- prime it so its first resume (via
     * context_switch_tail, not libos_enter()) has somewhere to go. Side A is
     * launched directly below, so it needs no priming. */
    CU_ASSERT_EQUAL(context_prime(id_b, LIBOS_LAUNCH_CODE_VADDR,
                                  LIBOS_LAUNCH_STACK_VADDR + VMM_PAGE_SIZE),
                   CONTEXT_OK);

    /* Declare A the running context before entering it directly -- the seam
     * a first dispatch uses, since context_switch_request() itself always
     * switches *away from* whatever context_current() already names. */
    context_set_current(id_a);
    CU_ASSERT_EQUAL(context_set_state(id_a, CONTEXT_STATE_RUNNING), CONTEXT_OK);

    switch_calls = 0;
    switch_rc = CONTEXT_ENOENT;
    exo_syscall_register(SYS_SWITCH, test_switch_handler);
    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, libos_return);

    CU_ASSERT_EQUAL(vmm_switch_address_space(phys_a), VMM_OK);
    uint64_t result = libos_enter(LIBOS_LAUNCH_CODE_VADDR,
                                  LIBOS_LAUNCH_STACK_VADDR + VMM_PAGE_SIZE);
    CU_ASSERT_EQUAL(vmm_switch_address_space(vmm_kernel_pml4()), VMM_OK);

    exo_syscall_register(SYS_SWITCH, 0);
    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, 0);

    /* A really did unwind all the way back out via libos_return(), and both
     * switches (A->B, then B->A) were requested and accepted. */
    CU_ASSERT_EQUAL(result, RESULT_MARKER);
    CU_ASSERT_EQUAL(switch_calls, 2);
    CU_ASSERT_EQUAL(switch_rc, CONTEXT_OK);

    /* Control really did come back to A, not just fall through. */
    CU_ASSERT_EQUAL(context_current(), id_a);
    CU_ASSERT_EQUAL(context_lookup(id_a)->state, CONTEXT_STATE_RUNNING);
    CU_ASSERT_EQUAL(context_lookup(id_b)->state, CONTEXT_STATE_READY);

    /* A's own trace: wrote MARK1 before switching away, then -- after
     * resuming on the other side of two switches -- read back exactly the
     * callee-saved values it seeded before ever yielding. This is the
     * acceptance criterion itself: register state survived the round trip. */
    uint64_t *trace_a = (uint64_t *)(uintptr_t)data_paddr_a;
    CU_ASSERT_EQUAL(trace_a[1], MARK1);
    CU_ASSERT_EQUAL(trace_a[2], SEED_RBX);
    CU_ASSERT_EQUAL(trace_a[3], SEED_RBP);
    CU_ASSERT_EQUAL(trace_a[4], SEED_R12);
    CU_ASSERT_EQUAL(trace_a[5], SEED_R13);
    CU_ASSERT_EQUAL(trace_a[6], SEED_R14);
    CU_ASSERT_EQUAL(trace_a[7], SEED_R15);
    CU_ASSERT_EQUAL(trace_a[8], MARK2);

    /* B genuinely ran too -- proves the CR3 swap and iretq into a *primed*
     * (never libos_enter()'d) context actually reached ring 3 on B's own
     * address space and data page. */
    uint64_t *trace_b = (uint64_t *)(uintptr_t)data_paddr_b;
    CU_ASSERT_EQUAL(trace_b[1], MARK_B);

    CU_ASSERT_EQUAL(context_destroy(id_a), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_destroy(id_b), CONTEXT_OK);
    /* id_a no longer names a live context -- restore the default so later
     * suites' syscall_current_context() calls see PAGE_OWNER_LIBOS, not a
     * stale, now-destroyed id. */
    context_set_current(PAGE_OWNER_LIBOS);
}

/* A target that is not a live READY context is refused, and refusing it
 * must not corrupt context_current() or either side's state. */
static void test_switch_rejects_bad_target(void)
{
    uint64_t phys_a = 0;
    CU_ASSERT_EQUAL(vmm_create_address_space(&phys_a), VMM_OK);
    page_owner_t id_a = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(phys_a, &id_a), CONTEXT_OK);

    context_set_current(id_a);
    CU_ASSERT_EQUAL(context_set_state(id_a, CONTEXT_STATE_RUNNING), CONTEXT_OK);

    page_owner_t bogus = (page_owner_t)(PAGE_OWNER_LIBOS + 2000);
    CU_ASSERT_EQUAL(context_switch_request(bogus), CONTEXT_ENOENT);
    CU_ASSERT_EQUAL(context_current(), id_a);
    CU_ASSERT_EQUAL(context_lookup(id_a)->state, CONTEXT_STATE_RUNNING);

    /* A BLOCKED context is live but not a valid switch target either. */
    uint64_t phys_b = 0;
    CU_ASSERT_EQUAL(vmm_create_address_space(&phys_b), VMM_OK);
    page_owner_t id_b = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(phys_b, &id_b), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_set_state(id_b, CONTEXT_STATE_BLOCKED), CONTEXT_OK);

    CU_ASSERT_EQUAL(context_switch_request(id_b), CONTEXT_ENOENT);
    CU_ASSERT_EQUAL(context_current(), id_a);
    CU_ASSERT_EQUAL(context_lookup(id_a)->state, CONTEXT_STATE_RUNNING);
    CU_ASSERT_EQUAL(context_lookup(id_b)->state, CONTEXT_STATE_BLOCKED);

    CU_ASSERT_EQUAL(context_destroy(id_a), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_destroy(id_b), CONTEXT_OK);
    context_set_current(PAGE_OWNER_LIBOS);
}

void suite_context_switch_tests(CU_pSuite s)
{
    CU_add_test(s, "switch preserves state both ways",
               test_switch_preserves_state_both_ways);
    CU_add_test(s, "switch rejects a bad target",
               test_switch_rejects_bad_target);
}
