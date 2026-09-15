/*
 * test_fault_k.c — page fault handler (SCRUM-17).
 *
 * Two layers, for two different risks.
 *
 * The decode tests are pure: fault_describe_err() is the only part of the
 * handler with branching logic in it, and it can be checked without faulting
 * at all.
 *
 * The live tests fault for real.  They lean on the TESTING-only hook in
 * fault.h: the hook records what the handler saw and repoints the saved RIP
 * at a fixup label, so iretq resumes past the faulting access instead of
 * halting the machine.  That is the same trick a kernel exception table uses,
 * and it is the only way to prove the stub's frame layout is right -- a
 * mismatch between PUSH_ALL_REGS and exception_frame_t would show up here as
 * a garbage RIP or a wrong error code, and nowhere else.
 */

#include "kunit.h"
#include "fault.h"
#include "vmm.h"
#include "exo_syscall.h"

#include <stdint.h>

/* ── Error-code decoding ─────────────────────────────────────────────────── */

static void test_describe_not_present_read_supervisor(void)
{
    char buf[64];
    CU_ASSERT_STRING_EQUAL(fault_describe_err(0x00, buf, sizeof buf),
                           "not-present read supervisor");
}

static void test_describe_not_present_write_supervisor(void)
{
    char buf[64];
    CU_ASSERT_STRING_EQUAL(fault_describe_err(PF_ERR_WRITE, buf, sizeof buf),
                           "not-present write supervisor");
}

static void test_describe_protection_write_user(void)
{
    char buf[64];
    uint64_t err = PF_ERR_PRESENT | PF_ERR_WRITE | PF_ERR_USER;
    CU_ASSERT_STRING_EQUAL(fault_describe_err(err, buf, sizeof buf),
                           "protection write user");
}

static void test_describe_reserved_bit(void)
{
    char buf[64];
    uint64_t err = PF_ERR_PRESENT | PF_ERR_RESERVED;
    CU_ASSERT_STRING_EQUAL(fault_describe_err(err, buf, sizeof buf),
                           "protection read supervisor reserved-bit");
}

static void test_describe_instruction_fetch(void)
{
    char buf[64];
    uint64_t err = PF_ERR_USER | PF_ERR_IFETCH;
    CU_ASSERT_STRING_EQUAL(fault_describe_err(err, buf, sizeof buf),
                           "not-present read user ifetch");
}

/* Truncation must still NUL-terminate rather than run off the buffer. */
static void test_describe_truncates_safely(void)
{
    char buf[8];
    for (unsigned i = 0; i < sizeof buf; i++)
        buf[i] = (char)0x7F;

    fault_describe_err(0x00, buf, sizeof buf);

    CU_ASSERT_STRING_EQUAL(buf, "not-pre");
    CU_ASSERT_EQUAL(buf[7], '\0');
}

/* n == 0 must not write at all. */
static void test_describe_zero_length(void)
{
    char buf[2] = { 0x7F, 0x7F };
    fault_describe_err(0x00, buf, 0);
    CU_ASSERT_EQUAL(buf[0], 0x7F);
}

/* ── A real fault, caught and resumed ───────────────────────── */

/*
 * Inside the LibOS window and guaranteed unmapped: exo_page_map confines a
 * LibOS to [EXO_USER_VA_BASE, EXO_USER_VA_END) and nothing has mapped
 * anything there during the test run.  Deliberately not page 0 -- that is the
 * NULL guard, and a fault on it would be indistinguishable from a bug in the
 * test itself.
 */
#define FAULT_VA (EXO_USER_VA_BASE + 0x1000ULL)

/* The faulting accesses live in tests/kernel/fault_probe.s, along with the
 * label each one resumes at.  See that file for why they are not written in C. */
extern void fault_probe_read(uint64_t va);
extern void fault_probe_read_resume(void);
extern void fault_probe_write(uint64_t va);
extern void fault_probe_write_resume(void);

static volatile int      hook_calls;
static volatile uint64_t seen_cr2;
static volatile uint64_t seen_err;
static volatile uint64_t seen_cs;
static volatile uint64_t seen_rip;
static volatile uint64_t resume_rip;

static int recording_hook(exception_frame_t *f, uint64_t cr2)
{
    hook_calls++;
    seen_cr2 = cr2;
    seen_err = f->error_code;
    seen_cs  = f->cs;
    seen_rip = f->rip;

    /* Resume at the probe's own resume label and tell the handler not to
     * halt.  A stale or zero resume_rip faults again at the new RIP; the
     * handler notices that the fault arrived at the address it just resumed
     * to, declines to resume a second time, and prints the diagnostic -- so a
     * mistake here ends in a reported halt rather than a silent QEMU hang. */
    f->rip = resume_rip;
    return 1;
}

static void arm(uint64_t resume_at)
{
    hook_calls = 0;
    seen_cr2 = seen_err = seen_cs = seen_rip = 0;
    resume_rip = resume_at;
    fault_set_hook(recording_hook);
}

static void test_read_fault_reports_address_and_cause(void)
{
    /* The address really is unmapped -- otherwise the test proves nothing. */
    CU_ASSERT_EQUAL(vmm_translate(FAULT_VA, 0, 0), VMM_ENOENT);

    arm((uint64_t)(uintptr_t)&fault_probe_read_resume);
    fault_probe_read(FAULT_VA);
    fault_set_hook(0);

    CU_ASSERT_EQUAL(hook_calls, 1);
    CU_ASSERT_EQUAL(seen_cr2, FAULT_VA);
    CU_ASSERT_EQUAL(seen_err & PF_ERR_PRESENT, 0);   /* not present   */
    CU_ASSERT_EQUAL(seen_err & PF_ERR_WRITE,   0);   /* it was a read */
    CU_ASSERT_EQUAL(seen_err & PF_ERR_USER,    0);   /* ring 0        */
    CU_ASSERT_EQUAL(seen_cs & 3, 0);                 /* CPL 0         */
}

static void test_write_fault_sets_write_bit(void)
{
    arm((uint64_t)(uintptr_t)&fault_probe_write_resume);
    fault_probe_write(FAULT_VA + 0x40);
    fault_set_hook(0);

    CU_ASSERT_EQUAL(hook_calls, 1);
    CU_ASSERT_EQUAL(seen_cr2, FAULT_VA + 0x40);
    CU_ASSERT_EQUAL(seen_err & PF_ERR_PRESENT, 0);
    CU_ASSERT_NOT_EQUAL(seen_err & PF_ERR_WRITE, 0);
}

/*
 * #PF is a fault, not a trap: the saved RIP is the faulting instruction, which
 * for the read probe is its very first one.  This is the assertion that proves
 * PUSH_ALL_REGS and exception_frame_t agree -- any disagreement puts some
 * other register where rip belongs, and it will not equal this address.
 */
static void test_frame_rip_is_the_faulting_instruction(void)
{
    arm((uint64_t)(uintptr_t)&fault_probe_read_resume);
    fault_probe_read(FAULT_VA + 0x80);
    fault_set_hook(0);

    CU_ASSERT_EQUAL(hook_calls, 1);
    CU_ASSERT_EQUAL(seen_rip, (uint64_t)(uintptr_t)&fault_probe_read);
}

/* A mapped address must not fault -- the probe is only a probe because the
 * address is unmapped, and this pins that the hook is not firing on its own. */
static void test_mapped_address_does_not_fault(void)
{
    uint64_t here = (uint64_t)(uintptr_t)&fault_probe_read;

    arm((uint64_t)(uintptr_t)&fault_probe_read_resume);
    fault_probe_read(here);          /* kernel text: identity-mapped */
    fault_set_hook(0);

    CU_ASSERT_EQUAL(hook_calls, 0);
}

/* Clear the hook even if a test bailed out early -- leaving it installed
 * would make a later, genuine fault silently resume into a stale label. */
int fault_suite_cleanup(void)
{
    fault_set_hook(0);
    return 0;
}

void suite_fault_tests(CU_pSuite s)
{
    CU_add_test(s, "describe: not-present read supervisor",
                test_describe_not_present_read_supervisor);
    CU_add_test(s, "describe: not-present write supervisor",
                test_describe_not_present_write_supervisor);
    CU_add_test(s, "describe: protection write user",
                test_describe_protection_write_user);
    CU_add_test(s, "describe: reserved bit", test_describe_reserved_bit);
    CU_add_test(s, "describe: instruction fetch",
                test_describe_instruction_fetch);
    CU_add_test(s, "describe: truncates safely",
                test_describe_truncates_safely);
    CU_add_test(s, "describe: zero-length buffer", test_describe_zero_length);

    CU_add_test(s, "live: read fault reports address and cause",
                test_read_fault_reports_address_and_cause);
    CU_add_test(s, "live: write fault sets the write bit",
                test_write_fault_sets_write_bit);
    CU_add_test(s, "live: frame rip is the faulting instruction",
                test_frame_rip_is_the_faulting_instruction);
    CU_add_test(s, "live: a mapped address does not fault",
                test_mapped_address_does_not_fault);
}
