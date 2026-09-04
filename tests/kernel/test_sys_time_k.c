/*
 * test_sys_time_k.c — exo_get_ticks, the first end-to-end syscall (SCRUM-33).
 *
 * SCRUM-32's suite proved the entry path carries registers correctly using a
 * scratch handler. This proves a *real* handler answers correctly, from ring 3,
 * with the value the kernel itself reports.
 *
 * ── Ordering matters, and this suite must be registered LAST ───────────────
 *
 * The liveness test at the bottom starts the PIT and enables interrupts. Ring-3
 * code cannot survive an interrupt yet — there is no TSS, so the CPU has no
 * ring-0 stack to switch to and an IRQ taken at CPL 3 triple-faults the
 * machine (SCRUM-46). Every ring-3 test therefore has to run before interrupts
 * come on, which means this suite runs after the others and the liveness test
 * runs last within it. It disables interrupts again before returning, but the
 * IDT/PIC/PIT stay initialised for whatever follows.
 */

#include "kunit.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "pit.h"
#include "idt.h"
#include "pic.h"

extern uint64_t ring3_run(void (*entry)(void), void *user_stack_top);
extern void     ring3_ticks_probe(void);
extern int64_t  ring3_escape(uint64_t result, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);
extern void     irq0_stub(void);

#define SYS_ESCAPE  20          /* borrowed, matches ring3_probe.s */

static uint8_t user_stack[16384] __attribute__((aligned(16)));

static int64_t dispatch_ticks(void)
{
    return exo_syscall_dispatch(EXO_SYS_GET_TICKS, 0, 0, 0, 0, 0, 0);
}

/* ── Binding ─────────────────────────────────────────────────────────────── */

static void test_handler_is_bound(void)
{
    /* sys_time_init() runs in kernel_main before the test branch, so the
     * suite sees the same table a normal boot has. */
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_GET_TICKS));

    /* And the number no longer reports as unimplemented. */
    CU_ASSERT_NOT_EQUAL(dispatch_ticks(), -EXO_ENOSYS);
}

/* ── The acceptance criterion ────────────────────────────────────────────── */

static void test_matches_kernel_get_ticks_ms(void)
{
    /* Read both as close together as possible.  With the PIT not yet running
     * they are both 0, but the equality is checked again in the liveness test
     * below once the counter is actually moving. */
    uint32_t direct = kernel_get_ticks_ms();
    int64_t  viacall = dispatch_ticks();

    CU_ASSERT_EQUAL(viacall, (int64_t)direct);
}

static void test_never_negative(void)
{
    /* The handler widens from uint32_t deliberately: a signed 32-bit
     * intermediate would go negative after ~24.9 days of uptime and every
     * caller would read that as an -EXO_E* error. */
    CU_ASSERT(dispatch_ticks() >= 0);
}

static void test_takes_no_arguments(void)
{
    /* Zero-argument syscall: junk in the argument registers must be ignored,
     * not rejected.  A LibOS is entitled to leave anything there. */
    int64_t clean = exo_syscall_dispatch(EXO_SYS_GET_TICKS, 0, 0, 0, 0, 0, 0);
    int64_t junk  = exo_syscall_dispatch(EXO_SYS_GET_TICKS,
                                         0xDEADBEEF, 0xCAFEBABE, ~0ull,
                                         0x1234, 0x5678, 0x9ABC);

    CU_ASSERT(junk >= 0);
    CU_ASSERT_EQUAL(junk, clean);   /* still stopped, so identical */
}

/* ── From ring 3, through the real entry path ────────────────────────────── */

static void test_ring3_get_ticks(void)
{
    exo_syscall_register(SYS_ESCAPE, ring3_escape);

    uint32_t before = kernel_get_ticks_ms();
    uint64_t from_ring3 = ring3_run(ring3_ticks_probe,
                                    user_stack + sizeof(user_stack));
    uint32_t after = kernel_get_ticks_ms();

    exo_syscall_register(SYS_ESCAPE, 0);

    /* Not -EXO_ENOSYS sign-extended: a real handler ran. */
    CU_ASSERT((int64_t)from_ring3 >= 0);

    /* Bracketed by two direct reads.  Interrupts are still off here so all
     * three are equal, but written as a range so the assertion stays true if
     * this ever runs with the timer live. */
    CU_ASSERT(from_ring3 >= before);
    CU_ASSERT(from_ring3 <= after);
}

/* ── Liveness: the value is a real counter, not a constant ───────────────── */

static void test_ticks_advance(void)
{
    /*
     * Everything above would pass identically if the handler returned a
     * hardcoded 0, because the PIT is not running during tests and
     * kernel_get_ticks_ms() is also 0. This is the test that rules that out.
     *
     * MUST BE LAST: it enables interrupts, after which no ring-3 code can run
     * safely until SCRUM-46 provides a TSS. See the header comment.
     */
    idt_init();
    pic_remap();
    idt_set_gate(32, (uintptr_t)irq0_stub);
    pit_init(1000);
    __asm__ volatile ("sti");

    int64_t start = dispatch_ticks();
    int64_t now = start;

    /* Bounded spin rather than kernel_sleep_ms(): if IRQ0 never arrives, this
     * has to fail the assertion rather than hang the machine and turn into an
     * unexplained CI timeout. At 1000 Hz a tick is 1 ms, so this bound is
     * enormously generous even under QEMU. */
    for (uint64_t spins = 0; spins < 200000000ull && now == start; spins++) {
        __asm__ volatile ("pause");
        now = dispatch_ticks();
    }

    __asm__ volatile ("cli");

    /* The syscall observes the timer advancing. */
    CU_ASSERT(now > start);

    /* And still agrees with the kernel's own reading now that it is non-zero. */
    CU_ASSERT_EQUAL(dispatch_ticks(), (int64_t)kernel_get_ticks_ms());
}

void suite_sys_time_tests(CU_pSuite s)
{
    CU_add_test(s, "handler is bound",           test_handler_is_bound);
    CU_add_test(s, "matches kernel_get_ticks_ms", test_matches_kernel_get_ticks_ms);
    CU_add_test(s, "never negative",             test_never_negative);
    CU_add_test(s, "ignores argument registers", test_takes_no_arguments);
    CU_add_test(s, "ring3 get_ticks",            test_ring3_get_ticks);

    /* Last: enables interrupts, which forecloses any further ring-3 test. */
    CU_add_test(s, "ticks advance",              test_ticks_advance);
}
