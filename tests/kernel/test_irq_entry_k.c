/*
 * test_irq_entry_k.c — hardware interrupts taken at CPL 3 (SCRUM-170).
 *
 * test_tss_k.c already proved a CPL-3 *exception* (a deliberate page fault)
 * switches to TSS.RSP0. This is the analogous live test for a CPL-3
 * *hardware interrupt*, which SCRUM-47's libos_enter() deliberately left
 * unverified by launching with RFLAGS.IF clear the whole time -- see
 * src/libos_enter.s's own comment. libos_enter_irq() (SCRUM-170) is the
 * second entry point that sets IF instead, so PIT/keyboard IRQs can land
 * while CPL-3 code runs.
 *
 * The probe (tests/kernel/irq_entry_probe.s) busy-waits on exo_get_ticks()
 * until real time has passed. kernel_main never `sti`s ahead of
 * run_tests() (test_syscall_pit_k.c's file comment), so IRQ0 can only be
 * recognised during the exact window libos_enter_irq()'s iretq sets IF for
 * -- ticks advancing at all is already proof an IRQ landed while the CPU
 * was at CPL 3. src/pit.c's irq0_handler additionally records, in TESTING
 * builds only, the stack pointer it observed on entry
 * (pit_irq0_last_rsp()) -- the same "which stack did the CPU actually use"
 * check test_tss_k.c makes for its fault frame, proving the interrupt
 * really did switch to TSS.RSP0 rather than continuing on whatever stack
 * ring-3 code happened to be running on.
 */

#include "kunit.h"
#include "libos_launch.h"
#include "libos_test_common.h"
#include "vmm.h"
#include "fault.h"
#include "exo_syscall.h"
#include "syscall.h"
#include "syscall_pit.h"
#include "pit.h"
#include "tss.h"

#include <stdint.h>

#define IRQ_ENTRY_TEST_OWNER TEST_OWNER_IRQ_ENTRY
#define RESULT_MARKER        0x1120DEADULL /* must match irq_entry_probe.s */

extern uint64_t libos_enter_irq(uint64_t entry_vaddr, uint64_t stack_top_vaddr);
extern void libos_irq_entry_probe(void);
extern void libos_irq_entry_probe_end(void);

static volatile int fault_count;

static int recording_hook(exception_frame_t *f, uint64_t cr2) {
    (void)f; (void)cr2;
    fault_count++;
    return 0;
}

static void test_irq_at_cpl3_switches_to_tss_stack_and_resumes(void) {
    size_t code_len = (uintptr_t)&libos_irq_entry_probe_end -
                      (uintptr_t)&libos_irq_entry_probe;

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(IRQ_ENTRY_TEST_OWNER,
                                      (const void *)&libos_irq_entry_probe,
                                      code_len, 0, 0, 0, &img),
                   VMM_OK);

    pit_irq0_reset_last_rsp();
    fault_count = 0;
    fault_set_hook(recording_hook);
    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, libos_return);

    CU_ASSERT_EQUAL(vmm_switch_address_space(img.pml4_phys), VMM_OK);
    uint64_t result = libos_enter_irq(img.entry_vaddr, img.stack_top_vaddr);
    CU_ASSERT_EQUAL(vmm_switch_address_space(vmm_kernel_pml4()), VMM_OK);

    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, 0);
    fault_set_hook(0);

    /* Reached the syscall and escaped cleanly -- no triple fault, and no
     * unexpected fault along the way (a broken RSP0 switch on IRQ entry
     * corrupts whatever it lands on, which tends to show up as exactly
     * that). The probe only reaches this point after its busy-wait loop
     * observed real tick advancement, which is only possible if IRQ0 was
     * recognised while it ran -- i.e. at CPL 3, since nothing in this test
     * binary sets IF anywhere else. */
    CU_ASSERT_EQUAL(result, RESULT_MARKER);
    CU_ASSERT_EQUAL(fault_count, 0);

    /* The interrupt really did switch to TSS.RSP0: irq0_handler's own stack
     * frame, captured the last time it ran, sits inside the TSS's kernel
     * stack range -- not the LibOS's ring-3 stack and not wherever the
     * kernel's own RSP happened to be before libos_enter_irq() was called. */
    uint64_t last_rsp = pit_irq0_last_rsp();
    CU_ASSERT_NOT_EQUAL(last_rsp, 0);

    uint64_t stack_top    = tss_rsp0();
    uint64_t stack_bottom = stack_top - TSS_KERNEL_STACK_SIZE;
    CU_ASSERT_TRUE(last_rsp >= stack_bottom && last_rsp < stack_top);

    libos_destroy_image(IRQ_ENTRY_TEST_OWNER, &img);
}

/* Leaves nothing behind even if an assertion above failed mid-test and
 * skipped its own cleanup -- see libos_test_common.h. */
int irq_entry_suite_cleanup(void) {
    pit_irq0_reset_last_rsp();
    libos_test_teardown_owner(IRQ_ENTRY_TEST_OWNER);
    return 0;
}

void suite_irq_entry_tests(CU_pSuite s) {
    CU_add_test(s, "IRQ0 taken at CPL 3 switches to the TSS stack and resumes",
                test_irq_at_cpl3_switches_to_tss_stack_and_resumes);
}
