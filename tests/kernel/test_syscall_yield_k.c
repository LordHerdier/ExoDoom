/*
 * test_syscall_yield_k.c — real exo_yield syscall + round-robin policy
 * (SCRUM-109).
 *
 * tests/kernel/test_context_switch_k.c (SCRUM-108) already proves the raw
 * switch primitive (register/CR3 save-restore across
 * context_switch_request()) via a test-local SYS_SWITCH handler that takes
 * an explicit target context id -- that test is untouched by this ticket.
 *
 * This suite proves the *real, bound* exo_yield syscall (#19,
 * src/syscall_yield.c) end to end: no explicit target argument (the real
 * ABI, docs/syscall_spec.md §3.2 #19, takes none), the target is chosen by
 * context_next_ready()'s round-robin scan (src/context.c) instead. Same
 * two-context/two-address-space setup as test_context_switch_k.c (built the
 * same way, for the same reasons -- see that file's own comment), but the
 * probes (tests/kernel/yield_probe_a.s / _b.s) call the real exo_yield()
 * with no argument, and the real syscall_yield_init() handler is registered
 * instead of a test-local stand-in.
 */

#include "kunit.h"
#include "context.h"
#include "vmm.h"
#include "page_alloc.h"
#include "libos_launch.h"
#include "exo_syscall.h"
#include "syscall.h"
#include "syscall_yield.h"
#include "string.h"

#include <stdint.h>

extern void yield_probe_a(void);
extern void yield_probe_a_end(void);
extern void yield_probe_b(void);
extern void yield_probe_b_end(void);

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
#define SEED_RDI  0x7777777777777777ULL
#define SEED_RSI  0x8888888888888888ULL
#define SEED_RDX  0x9999999999999999ULL
#define SEED_R10  0xAAAAAAAAAAAAAAAAULL
#define SEED_R8   0xBBBBBBBBBBBBBBBBULL
#define SEED_R9   0xCCCCCCCCCCCCCCCCULL

/* Same shape as test_context_switch_k.c's own cleanup -- this suite's
 * contexts also have real code/data/stack pages mapped in, not just a bare
 * PML4, so page_reclaim_all() is needed alongside context_destroy(). */
#define SYSCALL_YIELD_CLEANUP_SCAN_IDS 4096

int syscall_yield_suite_cleanup(void)
{
    for (uint32_t offset = 0; offset < SYSCALL_YIELD_CLEANUP_SCAN_IDS; offset++) {
        page_owner_t id = (page_owner_t)(PAGE_OWNER_LIBOS + offset);
        if (context_lookup(id) != NULL) {
            page_reclaim_all(id);
            context_destroy(id);
        }
    }
    return 0;
}

/* Allocate one page owned by `owner`, copy `copy_len` bytes of `src` into it
 * (zeroing the rest), and map it at `vaddr` in `pml4`. Mirrors
 * test_context_switch_k.c's own map_one_page(). */
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

static void test_yield_round_robins_both_ways(void)
{
    uint64_t phys_a = 0, phys_b = 0;
    CU_ASSERT_EQUAL(vmm_create_address_space(&phys_a), VMM_OK);
    CU_ASSERT_EQUAL(vmm_create_address_space(&phys_b), VMM_OK);

    page_owner_t id_a = PAGE_OWNER_FREE, id_b = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(phys_a, &id_a), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_create(phys_b, &id_b), CONTEXT_OK);

    uint64_t *pml4_a = (uint64_t *)(uintptr_t)phys_a;
    uint64_t *pml4_b = (uint64_t *)(uintptr_t)phys_b;

    size_t code_len_a = (uintptr_t)&yield_probe_a_end -
                        (uintptr_t)&yield_probe_a;
    size_t code_len_b = (uintptr_t)&yield_probe_b_end -
                        (uintptr_t)&yield_probe_b;

    map_one_page(id_a, pml4_a, LIBOS_LAUNCH_CODE_VADDR,
                VMM_PRESENT | VMM_USER,
                (const void *)&yield_probe_a, code_len_a);
    map_one_page(id_b, pml4_b, LIBOS_LAUNCH_CODE_VADDR,
                VMM_PRESENT | VMM_USER,
                (const void *)&yield_probe_b, code_len_b);

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

    /* Side B is never launched directly -- prime it so its first resume (via
     * context_switch_tail, not libos_enter()) has somewhere to go. Side A is
     * launched directly below, so it needs no priming. */
    CU_ASSERT_EQUAL(context_prime(id_b, LIBOS_LAUNCH_CODE_VADDR,
                                  LIBOS_LAUNCH_STACK_VADDR + VMM_PAGE_SIZE),
                   CONTEXT_OK);

    /* Declare A the running context before entering it directly -- the seam
     * a first dispatch uses, since context_switch_request() itself always
     * switches *away from* whatever context_current() already names. B is
     * the only other READY context, so context_next_ready(id_a) round-robins
     * straight to it, and vice versa on B's own yield. */
    context_set_current(id_a);
    CU_ASSERT_EQUAL(context_set_state(id_a, CONTEXT_STATE_RUNNING), CONTEXT_OK);

    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, libos_return);
    syscall_yield_init();

    CU_ASSERT_EQUAL(vmm_switch_address_space(phys_a), VMM_OK);
    uint64_t result = libos_enter(LIBOS_LAUNCH_CODE_VADDR,
                                  LIBOS_LAUNCH_STACK_VADDR + VMM_PAGE_SIZE);
    CU_ASSERT_EQUAL(vmm_switch_address_space(vmm_kernel_pml4()), VMM_OK);

    exo_syscall_register(EXO_SYS_YIELD, 0);
    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, 0);

    /* A really did unwind all the way back out via libos_return(), after
     * round-robining to B and back. */
    CU_ASSERT_EQUAL(result, RESULT_MARKER);

    /* Control really did come back to A, not just fall through. */
    CU_ASSERT_EQUAL(context_current(), id_a);
    CU_ASSERT_EQUAL(context_lookup(id_a)->state, CONTEXT_STATE_RUNNING);
    CU_ASSERT_EQUAL(context_lookup(id_b)->state, CONTEXT_STATE_READY);

    /* A's own trace: wrote MARK1 before yielding away, then -- after
     * resuming on the other side of two switches -- read back exactly the
     * callee-saved values it seeded before ever yielding. This is the
     * acceptance criterion itself: "LibOS can voluntarily yield; other
     * LibOS resumes", proven through the real syscall and policy, not a
     * test-registered stand-in. */
    uint64_t *trace_a = (uint64_t *)(uintptr_t)data_paddr_a;
    CU_ASSERT_EQUAL(trace_a[0], MARK1);
    CU_ASSERT_EQUAL(trace_a[1], SEED_RBX);
    CU_ASSERT_EQUAL(trace_a[2], SEED_RBP);
    CU_ASSERT_EQUAL(trace_a[3], SEED_R12);
    CU_ASSERT_EQUAL(trace_a[4], SEED_R13);
    CU_ASSERT_EQUAL(trace_a[5], SEED_R14);
    CU_ASSERT_EQUAL(trace_a[6], SEED_R15);
    CU_ASSERT_EQUAL(trace_a[7], MARK2);

    /* SCRUM-178 regression: docs/syscall_spec.md's ABI promises these six
     * argument registers survive an *ordinary* syscall untouched, and that
     * has to hold just as well when the syscall happens to trigger a real
     * context switch -- compiled C (src/libos_wad_viewer.c's
     * `for (;;) { exo_yield(); }`) is entitled to keep a value live across
     * the call either way. Before context_regs_t/context_switch_tail
     * carried these, only the SysV callee-saved set (checked above) and
     * RAX survived a real switch. */
    CU_ASSERT_EQUAL(trace_a[8],  SEED_RDI);
    CU_ASSERT_EQUAL(trace_a[9],  SEED_RSI);
    CU_ASSERT_EQUAL(trace_a[10], SEED_RDX);
    CU_ASSERT_EQUAL(trace_a[11], SEED_R10);
    CU_ASSERT_EQUAL(trace_a[12], SEED_R8);
    CU_ASSERT_EQUAL(trace_a[13], SEED_R9);

    /* B genuinely ran too -- proves round-robin actually picked it as A's
     * yield target, on B's own address space and data page. */
    uint64_t *trace_b = (uint64_t *)(uintptr_t)data_paddr_b;
    CU_ASSERT_EQUAL(trace_b[0], MARK_B);

    CU_ASSERT_EQUAL(context_destroy(id_a), CONTEXT_OK);
    CU_ASSERT_EQUAL(context_destroy(id_b), CONTEXT_OK);
    /* id_a no longer names a live context -- restore the default so later
     * suites' syscall_current_context() calls see PAGE_OWNER_LIBOS, not a
     * stale, now-destroyed id. */
    context_set_current(PAGE_OWNER_LIBOS);
}

/* With no other READY context, exo_yield() is a no-op: it returns 0 and the
 * caller just carries on, rather than erroring or hanging -- the collapsed
 * "nothing to switch to" case both context_next_ready() and
 * context_switch_request() can produce (see src/syscall_yield.c's own
 * comment). */
static void test_yield_alone_is_noop(void)
{
    uint64_t phys_a = 0;
    CU_ASSERT_EQUAL(vmm_create_address_space(&phys_a), VMM_OK);
    page_owner_t id_a = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(phys_a, &id_a), CONTEXT_OK);

    context_set_current(id_a);
    CU_ASSERT_EQUAL(context_set_state(id_a, CONTEXT_STATE_RUNNING), CONTEXT_OK);

    CU_ASSERT_EQUAL(context_next_ready(id_a), PAGE_OWNER_FREE);

    syscall_yield_init();
    int64_t rc = exo_syscall_dispatch(EXO_SYS_YIELD, 0, 0, 0, 0, 0, 0);
    exo_syscall_register(EXO_SYS_YIELD, 0);

    CU_ASSERT_EQUAL(rc, 0);
    CU_ASSERT_EQUAL(context_current(), id_a);
    CU_ASSERT_EQUAL(context_lookup(id_a)->state, CONTEXT_STATE_RUNNING);

    CU_ASSERT_EQUAL(context_destroy(id_a), CONTEXT_OK);
    context_set_current(PAGE_OWNER_LIBOS);
}

void suite_syscall_yield_tests(CU_pSuite s)
{
    CU_add_test(s, "exo_yield round-robins both ways",
               test_yield_round_robins_both_ways);
    CU_add_test(s, "exo_yield with nothing else runnable is a no-op",
               test_yield_alone_is_noop);
}
