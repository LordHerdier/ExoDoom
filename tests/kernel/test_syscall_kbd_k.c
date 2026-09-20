/*
 * test_syscall_kbd_k.c — exo_kbd_poll (SCRUM-39, #6).
 *
 * Drives the real dispatch path, exo_syscall_dispatch(EXO_SYS_KBD_POLL,
 * ...) — the route a ring-3 `syscall` takes once the entry stub has
 * marshalled its arguments. The handler is bound in kernel_main by
 * syscall_kbd_init() before run_tests(). Same shape as
 * test_syscall_serial_k.c: prove the contract at the syscall boundary
 * rather than exercise ring 3 directly, since dispatch is the boundary a
 * real ring-3 caller actually crosses.
 *
 * The keyboard ring itself (src/kbd_ring.c, SCRUM-18) is exercised directly
 * by tests/kernel/test_kbd_ring.c; this suite only needs kbd_enqueue/
 * kbd_reset (src/ps2.h) to put known events in front of the syscall, since
 * the ring is a single, kernel-global instance shared with the IRQ1 path.
 */

#include "kunit.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "ps2.h"

#include <stdint.h>

/* Scratch virtual address in the LibOS window, apart from other suites'
 * ranges (test_page_map_k.c, test_vmm_k.c, test_syscall_serial_k.c). */
#define SCRATCH (EXO_USER_VA_BASE + 0x30000000ULL)

/* A second scratch address in this suite's own range, one page past SCRATCH,
 * that no test here ever maps with EXO_PAGE_WRITE|EXO_PAGE_USER -- the
 * in-window-but-unmapped pointer SCRUM-186 reproduces the crash with. */
#define UNMAPPED (SCRATCH + 0x1000ULL)

static int64_t do_kbd_poll(uint64_t event_out)
{
    return exo_syscall_dispatch(EXO_SYS_KBD_POLL, event_out, 0, 0, 0, 0, 0);
}

static int64_t do_alloc(void)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
}

static int64_t do_free(uint64_t paddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_FREE, paddr, 0, 0, 0, 0, 0);
}

static int64_t do_map(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_MAP, vaddr, paddr, flags,
                                0, 0, 0);
}

static int64_t do_unmap(uint64_t vaddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, vaddr, 0, 0, 0, 0, 0);
}

/* The boot path must have bound this number; without this the rest of the
 * suite would only be re-proving the dispatcher's -EXO_ENOSYS fallback. */
static void test_handler_is_bound(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_KBD_POLL));
}

static void test_null_event_out_rejected(void)
{
    kbd_reset();
    /* Queue a real event so a non-conforming implementation returning 0
     * "ring empty" instead of -EXO_EFAULT would be caught rather than
     * masked by an empty ring. */
    kbd_event_t ev = { .pressed = 1, .key = (uint8_t)KEY_A, .modifiers = 0 };
    kbd_enqueue(ev);

    CU_ASSERT_EQUAL(do_kbd_poll(0), -EXO_EFAULT);
}

static void test_event_out_below_window_rejected(void)
{
    kbd_reset();
    CU_ASSERT_EQUAL(do_kbd_poll(EXO_USER_VA_BASE - 1), -EXO_EFAULT);
}

static void test_event_out_spanning_window_end_rejected(void)
{
    kbd_reset();
    /* exo_kbd_event_t is 4 bytes -- starting 3 bytes before the window's
     * end is the last address that still fits entirely inside it. */
    CU_ASSERT_EQUAL(do_kbd_poll(EXO_USER_VA_END - 2), -EXO_EFAULT);
}

static void test_valid_poll_empty_ring_returns_zero(void)
{
    kbd_reset();

    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)p,
                          EXO_PAGE_WRITE | EXO_PAGE_USER), 0);

    exo_kbd_event_t *out = (exo_kbd_event_t *)(uintptr_t)SCRATCH;
    out->pressed = 0xAB; out->key = 0xCD; out->modifiers = 0xEF;
    out->reserved = 0x12;

    CU_ASSERT_EQUAL(do_kbd_poll(SCRATCH), 0);

    /* An empty ring must leave *event_out untouched, same contract as
     * kbd_ring_pop. */
    CU_ASSERT_EQUAL(out->pressed, 0xAB);
    CU_ASSERT_EQUAL(out->key, 0xCD);
    CU_ASSERT_EQUAL(out->modifiers, 0xEF);

    CU_ASSERT_EQUAL(do_unmap(SCRATCH), 0);
    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
}

static void test_valid_poll_dequeues_one_event(void)
{
    kbd_reset();

    kbd_event_t ev = { .pressed = 1, .key = (uint8_t)KEY_ENTER,
                       .modifiers = MOD_LSHIFT };
    kbd_enqueue(ev);

    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)p,
                          EXO_PAGE_WRITE | EXO_PAGE_USER), 0);

    exo_kbd_event_t *out = (exo_kbd_event_t *)(uintptr_t)SCRATCH;

    CU_ASSERT_EQUAL(do_kbd_poll(SCRATCH), 1);
    CU_ASSERT_EQUAL(out->pressed, 1);
    CU_ASSERT_EQUAL(out->key, (uint8_t)KEY_ENTER);
    CU_ASSERT_EQUAL(out->modifiers, MOD_LSHIFT);
    CU_ASSERT_EQUAL(out->reserved, 0);

    /* Ring is now empty. */
    CU_ASSERT_EQUAL(do_kbd_poll(SCRATCH), 0);

    CU_ASSERT_EQUAL(do_unmap(SCRATCH), 0);
    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
}

/* FIFO order must survive the syscall boundary, not just kbd_ring_pop
 * itself. */
static void test_valid_poll_preserves_fifo_order(void)
{
    kbd_reset();

    kbd_enqueue((kbd_event_t){ .pressed = 1, .key = (uint8_t)KEY_A, .modifiers = 0 });
    kbd_enqueue((kbd_event_t){ .pressed = 0, .key = (uint8_t)KEY_A, .modifiers = 0 });
    kbd_enqueue((kbd_event_t){ .pressed = 1, .key = (uint8_t)KEY_B, .modifiers = 0 });

    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)p,
                          EXO_PAGE_WRITE | EXO_PAGE_USER), 0);

    exo_kbd_event_t *out = (exo_kbd_event_t *)(uintptr_t)SCRATCH;

    CU_ASSERT_EQUAL(do_kbd_poll(SCRATCH), 1);
    CU_ASSERT_EQUAL(out->key, (uint8_t)KEY_A);
    CU_ASSERT_EQUAL(out->pressed, 1);

    CU_ASSERT_EQUAL(do_kbd_poll(SCRATCH), 1);
    CU_ASSERT_EQUAL(out->key, (uint8_t)KEY_A);
    CU_ASSERT_EQUAL(out->pressed, 0);

    CU_ASSERT_EQUAL(do_kbd_poll(SCRATCH), 1);
    CU_ASSERT_EQUAL(out->key, (uint8_t)KEY_B);
    CU_ASSERT_EQUAL(out->pressed, 1);

    CU_ASSERT_EQUAL(do_kbd_poll(SCRATCH), 0);

    CU_ASSERT_EQUAL(do_unmap(SCRATCH), 0);
    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
}

/* SCRUM-186: an in-window pointer that was never exo_page_map'd used to reach
 * the write to *event_out and take a fatal supervisor-mode page fault (the
 * whole window is reserved-but-unmapped by default). This is the exact crash
 * repro -- it must now come back cleanly as -EXO_EFAULT instead. A queued
 * event distinguishes that from a non-conforming "ring empty" 0, same
 * reasoning as test_null_event_out_rejected. */
static void test_unmapped_in_window_event_out_rejected(void)
{
    kbd_reset();
    kbd_event_t ev = { .pressed = 1, .key = (uint8_t)KEY_A, .modifiers = 0 };
    kbd_enqueue(ev);

    CU_ASSERT_EQUAL(do_kbd_poll(UNMAPPED), -EXO_EFAULT);
}

/* Mapped but read-only (EXO_PAGE_USER without EXO_PAGE_WRITE) is still not
 * somewhere the kernel may write *event_out through on the caller's behalf
 * -- unlike exo_serial_write's buf, this handler writes to its pointer. */
static void test_read_only_mapped_event_out_rejected(void)
{
    kbd_reset();
    kbd_event_t ev = { .pressed = 1, .key = (uint8_t)KEY_A, .modifiers = 0 };
    kbd_enqueue(ev);

    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL(do_map(UNMAPPED, (uint64_t)p, EXO_PAGE_USER), 0);

    CU_ASSERT_EQUAL(do_kbd_poll(UNMAPPED), -EXO_EFAULT);

    CU_ASSERT_EQUAL(do_unmap(UNMAPPED), 0);
    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
}

void suite_syscall_kbd_tests(CU_pSuite s)
{
    CU_add_test(s, "handler is bound", test_handler_is_bound);
    CU_add_test(s, "NULL event_out rejected with events queued",
               test_null_event_out_rejected);
    CU_add_test(s, "event_out below window rejected",
               test_event_out_below_window_rejected);
    CU_add_test(s, "event_out spanning window end rejected",
               test_event_out_spanning_window_end_rejected);
    CU_add_test(s, "valid poll of empty ring returns 0",
               test_valid_poll_empty_ring_returns_zero);
    CU_add_test(s, "valid poll dequeues one event",
               test_valid_poll_dequeues_one_event);
    CU_add_test(s, "valid poll preserves FIFO order",
               test_valid_poll_preserves_fifo_order);
    CU_add_test(s, "unmapped in-window event_out rejected",
               test_unmapped_in_window_event_out_rejected);
    CU_add_test(s, "read-only mapped event_out rejected",
               test_read_only_mapped_event_out_rejected);
}
