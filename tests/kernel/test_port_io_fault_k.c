/*
 * test_port_io_fault_k.c -- ring-3 port I/O is walled off from a LibOS
 * (SCRUM-56).
 *
 * Together with test_libos_launch_k.c's proof that a deliberate fault inside
 * a real LibOS address space can be caught and resumed, this is the other
 * half of "an exokernel, not a monolithic kernel with a syscall table": a
 * LibOS running at CPL 3 cannot reach hardware directly, full stop, and only
 * a bound syscall can get there on its behalf.
 *
 * The trap itself needs no new kernel mechanism -- tss_init() (SCRUM-46,
 * src/tss.c) already points TSS.iomap_base one byte past the TSS segment
 * limit, so any port access below IOPL (always 0; nothing ever raises it)
 * reads as "no I/O permission bitmap" and takes #GP at CPL 3 regardless of
 * the port or the value in %al. What SCRUM-56 actually adds is a real #GP
 * handler (gp_fault_handler/gpf_stub, src/fault.c/isr.s) to replace the
 * silent error_stub re-fault loop vector 13 used before, and this test that
 * asserts the wall for real instead of just trusting the TSS comment.
 */

#include "kunit.h"
#include "libos_launch.h"
#include "libos_test_common.h"
#include "vmm.h"
#include "fault.h"
#include "exo_syscall.h"
#include "syscall.h"

#include <stdint.h>

#define PORT_IO_FAULT_TEST_OWNER TEST_OWNER_PORT_IO_FAULT

#define RESULT_MARKER 0xBADD0170ULL /* must match port_io_fault_probe.s */

extern void port_io_fault_probe(void);
extern void port_io_fault_probe_resume(void);
extern void port_io_fault_probe_end(void);

static volatile int      hook_calls;
static volatile uint64_t seen_cs;
static volatile uint64_t resume_rip;

static int recording_hook(exception_frame_t *f, uint64_t cr2) {
    (void)cr2;
    hook_calls++;
    seen_cs = f->cs;
    f->rip = resume_rip;
    return 1;
}

static void test_ring3_port_io_faults(void) {
    size_t code_len = (uintptr_t)&port_io_fault_probe_end -
                      (uintptr_t)&port_io_fault_probe;
    uint64_t resume_offset = (uintptr_t)&port_io_fault_probe_resume -
                             (uintptr_t)&port_io_fault_probe;

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(PORT_IO_FAULT_TEST_OWNER,
                                      (const void *)&port_io_fault_probe,
                                      code_len, 0, 0, 0, &img),
                   VMM_OK);

    /* The copied blob's resume label sits at the same offset from its own
     * base as it does in this file's linked copy -- the one thing that
     * survives the relocation into the LibOS window. */
    resume_rip = img.entry_vaddr + resume_offset;

    hook_calls = 0;
    seen_cs = 0;
    fault_set_hook(recording_hook);
    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, libos_return);

    CU_ASSERT_EQUAL(vmm_switch_address_space(img.pml4_phys), VMM_OK);
    uint64_t result = libos_enter(img.entry_vaddr, img.stack_top_vaddr);
    CU_ASSERT_EQUAL(vmm_switch_address_space(vmm_kernel_pml4()), VMM_OK);

    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, 0);
    fault_set_hook(0);

    /* Reached the resume label and escaped cleanly -- the #GP was caught by
     * gp_fault_handler rather than silently re-faulting forever (the old
     * error_stub behaviour) or triple-faulting the machine. */
    CU_ASSERT_EQUAL(result, RESULT_MARKER);
    CU_ASSERT_EQUAL(hook_calls, 1);

    /* The fault really originated at CPL 3. */
    CU_ASSERT_EQUAL(seen_cs & 3, 3);

    libos_destroy_image(PORT_IO_FAULT_TEST_OWNER, &img);
}

/* Leaves nothing behind even if an assertion above failed mid-test and
 * skipped its own cleanup -- see libos_test_common.h. */
int port_io_fault_suite_cleanup(void) {
    libos_test_teardown_owner(PORT_IO_FAULT_TEST_OWNER);
    return 0;
}

void suite_port_io_fault_tests(CU_pSuite s) {
    CU_add_test(s, "ring3 port I/O traps #GP", test_ring3_port_io_faults);
}
