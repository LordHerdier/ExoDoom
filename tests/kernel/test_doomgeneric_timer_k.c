/*
 * test_doomgeneric_timer_k.c — DG_GetTicksMs / DG_SleepMs (SCRUM-74).
 *
 * Drives src/doomgeneric_exo.c, which under -DEXO_KERNEL reaches the timer
 * through exo_syscall_dispatch(EXO_SYS_GET_TICKS, ...) rather than a real
 * `syscall` instruction -- see that file's header for why the two builds
 * differ. So this suite proves the callbacks sit correctly on top of the
 * dispatch path; tests/kernel/test_syscall_pit_k.c already proves the
 * dispatch path itself reports live PIT time, and
 * tests/kernel/test_libc_shim_probe_k.c proves compiled ring-3 code can make
 * a genuine bound syscall.
 *
 * ── Interrupts ─────────────────────────────────────────────────────────
 *
 * kernel_main performs no blanket `sti` before run_tests(): the fault, tss,
 * libos_launch and libos_main suites drive real ring-3 faults with RFLAGS.IF
 * hardcoded clear, and their hook-driven resume restores exactly that, so
 * ambient IF is not something any suite may assume in either direction.
 *
 * That matters more here than anywhere else in the tree. DG_SleepMs spins on
 * a counter only IRQ0 advances, so calling it with interrupts off does not
 * fail -- it never returns, and CI reports a 30-second timeout with no
 * failing assertion to point at. Every test below that waits therefore
 * enables interrupts for just the span of its own wait and clears them
 * again afterwards, the same shape test_syscall_pit_k.c uses, so this
 * suite's IRQ0 window cannot leak into whatever suite runs next.
 *
 * Waits are kept short (20 ms) for the same reason: the whole boot has 30
 * seconds.
 */

#include "kunit.h"
#include "doomgeneric_exo.h"
#include "syscall.h"
#include "syscall_pit.h"
#include "exo_syscall.h"
#include "pit.h"

#include <stdint.h>

/* Long enough that a 1000 Hz PIT lands ~20 IRQ0s inside it, so the "advanced"
 * checks are not riding on a single tick boundary; short enough to be
 * invisible against the test boot's budget. */
#define WAIT_MS 20u

static void test_get_ticks_matches_kernel_clock(void)
{
    /* Sandwich: DG_GetTicksMs must report the same clock the kernel does, so
     * its answer cannot fall outside a pair of readings taken either side of
     * it. Catches a DG_GetTicksMs wired to a different source, or to a
     * constant outside the current tick value. */
    uint32_t before = kernel_get_ticks_ms();
    uint32_t via_dg = DG_GetTicksMs();
    uint32_t after  = kernel_get_ticks_ms();

    CU_ASSERT(via_dg >= before);
    CU_ASSERT(via_dg <= after);
}

static void test_get_ticks_advances(void)
{
    /* A constant passes the sandwich above whenever the clock is stopped, so
     * prove the value actually moves. This is the check SCRUM-33's acceptance
     * criterion calls out by name: an equality test alone is satisfied by a
     * hardcoded return. */
    uint32_t start = DG_GetTicksMs();

    __asm__ volatile ("sti");
    while ((uint32_t)(kernel_get_ticks_ms() - start) < WAIT_MS) {
        __asm__ volatile ("hlt");
    }
    __asm__ volatile ("cli");

    CU_ASSERT(DG_GetTicksMs() > start);
}

static void test_sleep_waits_at_least_the_requested_time(void)
{
    /* The actual subject of the ticket: DG_SleepMs must not return early.
     * Measured against kernel_get_ticks_ms rather than DG_GetTicksMs so the
     * thing under test is not also the thing keeping score -- a DG_SleepMs
     * that returned immediately AND a DG_GetTicksMs stuck at a constant
     * would agree with each other and pass. */
    __asm__ volatile ("sti");
    uint32_t start = kernel_get_ticks_ms();
    DG_SleepMs(WAIT_MS);
    uint32_t elapsed = kernel_get_ticks_ms() - start;
    __asm__ volatile ("cli");

    CU_ASSERT(elapsed >= WAIT_MS);
}

static void test_sleep_zero_returns_immediately(void)
{
    /* DG_SleepMs(0) must fall straight out of the loop rather than wait for
     * the next tick -- Doom's D_DoomLoop reaches I_Sleep with a computed
     * delay that is routinely 0, so a zero-wait that costs a tick would cap
     * the frame rate at the PIT frequency.
     *
     * Deliberately run with interrupts OFF: with the clock frozen, a correct
     * implementation still returns (0 elapsed is already >= 0), while one
     * that waited for a tick edge would hang here. That makes this the one
     * test in the suite that needs no `sti`, and it is a stronger check for
     * being run that way. */
    uint32_t start = kernel_get_ticks_ms();
    DG_SleepMs(0);
    uint32_t elapsed = kernel_get_ticks_ms() - start;

    CU_ASSERT(elapsed < WAIT_MS);
}

static void test_get_ticks_is_monotonic(void)
{
    /* Doom's I_GetTime subtracts a stored basetime from every reading and
     * feeds the result to unsigned arithmetic, so a clock that ever went
     * backwards would not read as "a small negative" but as roughly 49 days.
     * Cheap to check, expensive to debug from the symptom. */
    __asm__ volatile ("sti");

    uint32_t prev = DG_GetTicksMs();
    int ok = 1;

    for (int i = 0; i < 64; i++) {
        uint32_t now = DG_GetTicksMs();
        if (now < prev) {
            ok = 0;
            break;
        }
        prev = now;
    }

    __asm__ volatile ("cli");

    CU_ASSERT(ok);
}

void suite_doomgeneric_timer_tests(CU_pSuite s)
{
    CU_add_test(s, "DG_GetTicksMs matches the kernel clock",
                test_get_ticks_matches_kernel_clock);
    CU_add_test(s, "DG_GetTicksMs advances over time",
                test_get_ticks_advances);
    CU_add_test(s, "DG_GetTicksMs never goes backwards",
                test_get_ticks_is_monotonic);
    CU_add_test(s, "DG_SleepMs waits at least the requested time",
                test_sleep_waits_at_least_the_requested_time);
    CU_add_test(s, "DG_SleepMs(0) returns immediately",
                test_sleep_zero_returns_immediately);
}
