/*
 * test_tss_k.c — TSS / RSP0 for CPL 3 exceptions (SCRUM-46).
 *
 * Two layers. The static checks confirm tss_init() actually did what it
 * claims -- TR loaded with the selector boot.s reserved, RSP0 pointing at
 * the kernel stack tss.c owns. The live test is the acceptance criterion
 * itself: fault for real from CPL 3 and prove the CPU switched onto that
 * stack, using the same hook-and-resume trick test_fault_k.c uses for its
 * ring-0 fault tests.
 */

#include "kunit.h"
#include "tss.h"
#include "fault.h"
#include "exo_syscall.h"

#include <stdint.h>

#define TSS_SELECTOR 0x30

static uint16_t read_tr(void) {
    uint16_t sel;
    __asm__ volatile ("str %0" : "=r"(sel));
    return sel;
}

static void test_tr_loaded_with_reserved_selector(void) {
    CU_ASSERT_EQUAL(read_tr(), TSS_SELECTOR);
}

static void test_rsp0_is_aligned_and_nonzero(void) {
    uint64_t rsp0 = tss_rsp0();
    CU_ASSERT_NOT_EQUAL(rsp0, 0);
    CU_ASSERT_EQUAL(rsp0 % 16, 0);
}

/* ── A real CPL-3 fault, caught and resumed ─────────────────────────────── */

extern uint64_t ring3_run(void (*entry)(void), void *user_stack_top);
extern int64_t  ring3_escape(uint64_t result, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);
extern void tss_fault_probe(void);
extern void tss_fault_probe_resume(void);

#define SYS_ESCAPE 20   /* EXO_SYS_EXIT, borrowed -- see ring3_probe.s */

static uint8_t user_stack[16384] __attribute__((aligned(16)));

static volatile int      hook_calls;
static volatile uint64_t seen_err;
static volatile uint64_t seen_cs;
static volatile uint64_t seen_frame_addr;

static int recording_hook(exception_frame_t *f, uint64_t cr2) {
    (void)cr2;
    hook_calls++;
    seen_err = f->error_code;
    seen_cs  = f->cs;
    seen_frame_addr = (uint64_t)(uintptr_t)f;

    f->rip = (uint64_t)(uintptr_t)&tss_fault_probe_resume;
    return 1;
}

static void test_ring3_fault_switches_to_tss_stack(void) {
    hook_calls = 0;
    seen_err = seen_cs = seen_frame_addr = 0;
    fault_set_hook(recording_hook);

    exo_syscall_register(SYS_ESCAPE, ring3_escape);
    uint64_t result = ring3_run(tss_fault_probe,
                                user_stack + sizeof(user_stack));
    exo_syscall_register(SYS_ESCAPE, 0);

    fault_set_hook(0);

    /* Reached the resume label and escaped cleanly -- above all, this proves
     * the machine did not triple-fault, which was the failure mode before
     * tss_init() existed. */
    CU_ASSERT_EQUAL(result, 0);
    CU_ASSERT_EQUAL(hook_calls, 1);

    /* The fault really did originate at CPL 3. */
    CU_ASSERT_EQUAL(seen_cs & 3, 3);
    CU_ASSERT_NOT_EQUAL(seen_err & PF_ERR_USER, 0);

    /* The frame the handler saw sits inside the TSS's kernel stack, not the
     * ring-3 probe's user stack or anywhere else -- the CPU built it there
     * because RSP0 pointed at it, which is the whole of what this ticket
     * delivers. */
    uint64_t stack_top    = tss_rsp0();
    uint64_t stack_bottom = stack_top - TSS_KERNEL_STACK_SIZE;
    CU_ASSERT_TRUE(seen_frame_addr >= stack_bottom &&
                   seen_frame_addr <  stack_top);

    uint64_t user_lo = (uint64_t)(uintptr_t)user_stack;
    uint64_t user_hi = user_lo + sizeof(user_stack);
    CU_ASSERT_FALSE(seen_frame_addr >= user_lo && seen_frame_addr < user_hi);
}

/* Clear the hook even if a test bailed out early -- leaving it installed
 * would make a later, genuine fault silently resume into a stale label
 * (same reasoning as test_fault_k.c's cleanup). */
int tss_suite_cleanup(void) {
    fault_set_hook(0);
    exo_syscall_register(SYS_ESCAPE, 0);
    return 0;
}

void suite_tss_tests(CU_pSuite s) {
    CU_add_test(s, "TR loaded with reserved selector",
                test_tr_loaded_with_reserved_selector);
    CU_add_test(s, "RSP0 is aligned and nonzero",
                test_rsp0_is_aligned_and_nonzero);
    CU_add_test(s, "ring3 fault switches to the TSS stack",
                test_ring3_fault_switches_to_tss_stack);
}
