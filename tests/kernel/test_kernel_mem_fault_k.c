/*
 * test_kernel_mem_fault_k.c -- a LibOS cannot read or write kernel memory
 * (SCRUM-55).
 *
 * Together with test_port_io_fault_k.c's proof that ring-3 port I/O is
 * walled off, this is the other half of "an exokernel, not a monolithic
 * kernel with a syscall table": a LibOS running at CPL 3, in its own real
 * address space, cannot reach the kernel's own memory either.
 *
 * Before SCRUM-55, TESTING builds mapped the *entire* kernel identity range
 * user-accessible (src/vmm.c's KERNEL_MAP_USER), because tests/kernel/
 * ring3_probe.s and tss_fault_probe.s predate SCRUM-48's per-LibOS address
 * spaces and execute directly against the kernel's own .text. Since every
 * address space shares that same kernel subtree (vmm_create_address_space's
 * own comment), that blanket exposure meant a fresh LibOS window saw kernel
 * memory as user-accessible too -- the isolation this suite asserts did not
 * actually hold under test. SCRUM-55 tightens KERNEL_MAP_USER back to
 * supervisor-only for everything except those two legacy probes' own narrow
 * code ranges (src/vmm.c's expose_ring3_legacy_probes()), which is what
 * makes this suite mean something.
 */

#include "kunit.h"
#include "libos_launch.h"
#include "libos_test_common.h"
#include "vmm.h"
#include "fault.h"
#include "exo_syscall.h"
#include "syscall.h"

#include <stdint.h>

#define KERNEL_MEM_FAULT_TEST_OWNER TEST_OWNER_KERNEL_MEM_FAULT

#define RESULT_MARKER 0xFEEDFACEULL /* must match kernel_mem_fault_probe.s */

extern void kernel_mem_fault_probe(void);
extern void kernel_mem_fault_probe_read_resume(void);
extern void kernel_mem_fault_probe_write_resume(void);
extern void kernel_mem_fault_probe_end(void);

static volatile int      hook_calls;
static volatile uint64_t seen_err[2];
static volatile uint64_t seen_cs[2];
static volatile uint64_t resume_rip[2];

static int recording_hook(exception_frame_t *f, uint64_t cr2) {
    (void)cr2;

    if (hook_calls < 2) {
        seen_err[hook_calls] = f->error_code;
        seen_cs[hook_calls]  = f->cs;
        f->rip = resume_rip[hook_calls];
    }
    hook_calls++;
    return 1;
}

static void test_ring3_kernel_mem_faults(void) {
    size_t code_len = (uintptr_t)&kernel_mem_fault_probe_end -
                      (uintptr_t)&kernel_mem_fault_probe;
    uint64_t read_resume_offset =
        (uintptr_t)&kernel_mem_fault_probe_read_resume -
        (uintptr_t)&kernel_mem_fault_probe;
    uint64_t write_resume_offset =
        (uintptr_t)&kernel_mem_fault_probe_write_resume -
        (uintptr_t)&kernel_mem_fault_probe;

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(KERNEL_MEM_FAULT_TEST_OWNER,
                                      (const void *)&kernel_mem_fault_probe,
                                      code_len, 0, 0, 0, &img),
                   VMM_OK);

    /* The copied blob's resume labels sit at the same offset from its own
     * base as they do in this file's linked copy -- the one thing that
     * survives the relocation into the LibOS window. */
    resume_rip[0] = img.entry_vaddr + read_resume_offset;
    resume_rip[1] = img.entry_vaddr + write_resume_offset;

    hook_calls = 0;
    seen_err[0] = seen_err[1] = seen_cs[0] = seen_cs[1] = 0;
    fault_set_hook(recording_hook);
    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, libos_return);

    CU_ASSERT_EQUAL(vmm_switch_address_space(img.pml4_phys), VMM_OK);
    uint64_t result = libos_enter(img.entry_vaddr, img.stack_top_vaddr);
    CU_ASSERT_EQUAL(vmm_switch_address_space(vmm_kernel_pml4()), VMM_OK);

    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, 0);
    fault_set_hook(0);

    /* Reached the final resume label and escaped cleanly -- both the read
     * and the write faulted, were caught, and execution kept going rather
     * than the machine triple-faulting or the access silently succeeding. */
    CU_ASSERT_EQUAL(result, RESULT_MARKER);
    CU_ASSERT_EQUAL(hook_calls, 2);

    /* The read: really CPL 3, and a *protection* fault -- the page is
     * present, just off-limits, not coincidentally unmapped. */
    CU_ASSERT_EQUAL(seen_cs[0] & 3, 3);
    CU_ASSERT_NOT_EQUAL(seen_err[0] & PF_ERR_PRESENT, 0);
    CU_ASSERT_EQUAL(seen_err[0] & PF_ERR_WRITE, 0);
    CU_ASSERT_NOT_EQUAL(seen_err[0] & PF_ERR_USER, 0);

    /* The write: same address, same everything, but the write bit set. */
    CU_ASSERT_EQUAL(seen_cs[1] & 3, 3);
    CU_ASSERT_NOT_EQUAL(seen_err[1] & PF_ERR_PRESENT, 0);
    CU_ASSERT_NOT_EQUAL(seen_err[1] & PF_ERR_WRITE, 0);
    CU_ASSERT_NOT_EQUAL(seen_err[1] & PF_ERR_USER, 0);

    libos_destroy_image(KERNEL_MEM_FAULT_TEST_OWNER, &img);
}

/* Leaves nothing behind even if an assertion above failed mid-test and
 * skipped its own cleanup -- see libos_test_common.h. */
int kernel_mem_fault_suite_cleanup(void) {
    libos_test_teardown_owner(KERNEL_MEM_FAULT_TEST_OWNER);
    return 0;
}

void suite_kernel_mem_fault_tests(CU_pSuite s) {
    CU_add_test(s, "ring3 kernel memory access faults",
                test_ring3_kernel_mem_faults);
}
