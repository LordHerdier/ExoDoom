/*
 * test_pit_k.c — pit.c's deferred compositor flag (SCRUM-181).
 *
 * Drives irq0_handler() through a real IRQ0 window (the same sti/
 * kernel_sleep_ms/cli idiom test_syscall_pit_k.c's test_ticks_advance_over_
 * time already uses), rather than calling irq0_handler() directly, so this
 * exercises the same PIC/IDT/ISR path a real boot does.
 */

#include "kunit.h"
#include "pit.h"
#include "sleep.h"
#include "fb_compositor.h"

static void test_composite_pending_clears_on_read(void)
{
    /* Drain whatever an earlier suite/test left pending, then confirm the
     * very next read reports nothing -- clear-on-read, same shape as
     * pit_take_print_pending(). */
    pit_take_composite_pending();

    CU_ASSERT_EQUAL(pit_take_composite_pending(), 0);
}

static void test_composite_pending_set_after_real_ticks(void)
{
    pit_take_composite_pending();

    /* Local, not ambient -- see test_syscall_pit_k.c's file header for why
     * this suite can't assume IF's ambient state either way. 30ms comfortably
     * crosses one ~60 Hz composite period at the kernel's 1000 Hz pit_init
     * rate (~16-17 ticks). */
    __asm__ volatile ("sti");
    kernel_sleep_ms(30);
    __asm__ volatile ("cli");

    CU_ASSERT_EQUAL(pit_take_composite_pending(), 1);
    CU_ASSERT_EQUAL(pit_take_composite_pending(), 0);
}

static void test_compositor_service_safe_with_no_framebuffer(void)
{
    /* No framebuffer is ever acquired in a TESTING build, so
     * fb_compositor_tick() is a no-op underneath this either way -- this is
     * only proving fb_compositor_service()'s own wiring (the pending-flag
     * check) doesn't crash, whether or not a tick is actually pending. */
    pit_take_composite_pending();
    fb_compositor_service();

    __asm__ volatile ("sti");
    kernel_sleep_ms(30);
    __asm__ volatile ("cli");

    fb_compositor_service();

    CU_ASSERT_EQUAL(pit_take_composite_pending(), 0);
}

void suite_pit_tests(CU_pSuite s)
{
    CU_add_test(s, "composite pending clears on read",
               test_composite_pending_clears_on_read);
    CU_add_test(s, "composite pending set after real ticks",
               test_composite_pending_set_after_real_ticks);
    CU_add_test(s, "compositor service safe with no framebuffer",
               test_compositor_service_safe_with_no_framebuffer);
}
