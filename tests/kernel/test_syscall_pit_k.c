/*
 * test_syscall_pit_k.c — exo_get_ticks (SCRUM-172).
 *
 * Drives the real dispatch path, exo_syscall_dispatch(EXO_SYS_GET_TICKS,
 * ...), and proves it reports the live PIT tick count rather than a fixed
 * value: kernel_main now wires pic_remap()/idt_set_gate(32)/pit_init() ahead
 * of the TESTING branch (see src/kernel.c) so ticks exist to observe from
 * inside a test build at all.
 *
 * What kernel_main deliberately does *not* do is a blanket `sti` before
 * run_tests(): the fault/tss/libos_launch/libos_main suites drive real
 * ring-3 faults with RFLAGS.IF hardcoded clear and their hook-driven resume
 * (src/fault.c) restores exactly that, so ambient IF is not something a
 * later suite can assume either way. test_ticks_advance_over_time enables
 * interrupts for just the span of its own wait and restores them after --
 * the only test here that needs a hardware IRQ to actually land, since a
 * busy hlt loop (kernel_sleep_ms, src/sleep.c) is the sole way to observe
 * time passing in here; there is no mock clock to advance by hand.
 */

#include "kunit.h"
#include "syscall.h"
#include "syscall_pit.h"
#include "exo_syscall.h"
#include "sleep.h"
#include "pit.h"

#include <stdint.h>

static int64_t do_get_ticks(void)
{
    return exo_syscall_dispatch(EXO_SYS_GET_TICKS, 0, 0, 0, 0, 0, 0);
}

static void test_handler_is_bound(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_GET_TICKS));
}

static void test_ticks_never_negative(void)
{
    CU_ASSERT(do_get_ticks() >= 0);
}

static void test_ticks_match_kernel_get_ticks_ms(void)
{
    /* Same source, sampled back to back -- must never disagree by more than
     * the time the two calls themselves take. */
    uint32_t before = kernel_get_ticks_ms();
    int64_t via_syscall = do_get_ticks();
    uint32_t after = kernel_get_ticks_ms();

    CU_ASSERT(via_syscall >= (int64_t)before);
    CU_ASSERT(via_syscall <= (int64_t)after);
}

static void test_ticks_advance_over_time(void)
{
    int64_t start = do_get_ticks();

    /* Local, not ambient: see the file header. Restored afterward so this
     * test's IRQ0 window doesn't leak into whatever suite runs next. */
    __asm__ volatile ("sti");
    kernel_sleep_ms(20);
    __asm__ volatile ("cli");

    int64_t elapsed = do_get_ticks();

    CU_ASSERT(elapsed > start);
}

void suite_syscall_pit_tests(CU_pSuite s)
{
    CU_add_test(s, "handler is bound", test_handler_is_bound);
    CU_add_test(s, "ticks never negative", test_ticks_never_negative);
    CU_add_test(s, "ticks match kernel_get_ticks_ms",
               test_ticks_match_kernel_get_ticks_ms);
    CU_add_test(s, "ticks advance over time", test_ticks_advance_over_time);
}
