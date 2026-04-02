/*
 * test_timer_k.c — Kernel-side CUnit tests for src/pit.c and src/sleep.c.
 *
 * Requires the PIT and IDT to be initialised before this suite runs.
 * kernel_main sets them up in the TESTING path before calling run_tests().
 */

#include <stdint.h>
#include "kunit.h"
#include "pit.h"
#include "sleep.h"

static void test_ticks_nonzero(void)
{
    /* After a brief sleep the millisecond counter must have advanced. */
    kernel_sleep_ms(10);
    CU_ASSERT(kernel_get_ticks_ms() > 0);
}

static void test_ticks_monotonic(void)
{
    /* A second read after a sleep must be strictly greater than the first. */
    uint32_t t1 = kernel_get_ticks_ms();
    kernel_sleep_ms(10);
    uint32_t t2 = kernel_get_ticks_ms();
    CU_ASSERT(t2 > t1);
}

static void test_ticks_multiple_reads(void)
{
    /* Five successive reads separated by short sleeps must never go backwards. */
    uint32_t prev = kernel_get_ticks_ms();
    int i;
    for (i = 0; i < 5; i++) {
        kernel_sleep_ms(5);
        uint32_t cur = kernel_get_ticks_ms();
        CU_ASSERT(cur >= prev);
        prev = cur;
    }
}

static void test_sleep_1000ms_accuracy(void)
{
    /* kernel_sleep_ms(1000) must complete within 1000 ± 20 ms. */
    uint32_t t1 = kernel_get_ticks_ms();
    kernel_sleep_ms(1000);
    uint32_t t2 = kernel_get_ticks_ms();
    uint32_t elapsed = t2 - t1;
    CU_ASSERT(elapsed >= 980);
    CU_ASSERT(elapsed <= 1020);
}

/* ---- Suite registration ----------------------------------------------- */

void suite_timer_tests(CU_pSuite s)
{
    CU_add_test(s, "ticks_nonzero",         test_ticks_nonzero);
    CU_add_test(s, "ticks_monotonic",       test_ticks_monotonic);
    CU_add_test(s, "ticks_multiple_reads",  test_ticks_multiple_reads);
    CU_add_test(s, "sleep_1000ms_accuracy", test_sleep_1000ms_accuracy);
}
