/*
 * test_syscall_mouse_k.c — exo_mouse_poll (SCRUM-52, #7).
 *
 * Drives the real dispatch path, exo_syscall_dispatch(EXO_SYS_MOUSE_POLL,
 * ...) — the route a ring-3 `syscall` takes once the entry stub has
 * marshalled its arguments. The handler is bound in kernel_main by
 * syscall_mouse_init() before run_tests(). Same shape as
 * test_syscall_kbd_k.c: prove the contract at the syscall boundary rather
 * than exercise ring 3 directly, since dispatch is the boundary a real
 * ring-3 caller actually crosses.
 *
 * The packet decoder/accumulator itself (src/ps2_mouse.c, SCRUM-52) is
 * exercised directly by tests/kernel/test_ps2_mouse_k.c; this suite only
 * needs ps2_mouse_process_byte()/ps2_mouse_reset() (src/ps2_mouse.h) to put
 * known state in front of the syscall, since the accumulator is a single
 * kernel-global instance shared with the IRQ12 path.
 */

#include "kunit.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "ps2_mouse.h"

#include <stdint.h>

/* Scratch virtual address in the LibOS window, apart from other suites'
 * ranges (test_page_map_k.c, test_vmm_k.c, test_syscall_serial_k.c,
 * test_syscall_kbd_k.c). */
#define SCRATCH (EXO_USER_VA_BASE + 0x31000000ULL)

/* A second scratch address in this suite's own range, one page past SCRATCH,
 * that no test here ever maps with EXO_PAGE_WRITE|EXO_PAGE_USER -- the
 * in-window-but-unmapped pointer SCRUM-186 reproduces the crash with. */
#define UNMAPPED (SCRATCH + 0x1000ULL)

/* Builds a real 3-byte packet from signed movement values (-128..127) so
 * callers can write the delta they mean instead of hand-encoding sign bits
 * and two's-complement bytes. */
static void feed_packet(uint8_t buttons, int dx, int dy)
{
    uint8_t b0 = (uint8_t)(0x08 | (buttons & 0x07));
    if (dx < 0) b0 |= 0x10;
    if (dy < 0) b0 |= 0x20;

    ps2_mouse_process_byte(b0);
    ps2_mouse_process_byte((uint8_t)dx);
    ps2_mouse_process_byte((uint8_t)dy);
}

static int64_t do_mouse_poll(uint64_t state_out)
{
    return exo_syscall_dispatch(EXO_SYS_MOUSE_POLL, state_out, 0, 0, 0, 0, 0);
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
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_MOUSE_POLL));
}

static void test_null_state_out_rejected(void)
{
    ps2_mouse_reset();
    feed_packet(0, 1, 1);

    CU_ASSERT_EQUAL(do_mouse_poll(0), -EXO_EFAULT);
}

static void test_state_out_below_window_rejected(void)
{
    ps2_mouse_reset();
    CU_ASSERT_EQUAL(do_mouse_poll(EXO_USER_VA_BASE - 1), -EXO_EFAULT);
}

static void test_state_out_spanning_window_end_rejected(void)
{
    ps2_mouse_reset();
    /* exo_mouse_state_t is 6 bytes -- the last start address that still fits
     * entirely inside the window is EXO_USER_VA_END - 6, so 4 bytes before
     * the end spans 2 bytes past it and must be rejected. */
    CU_ASSERT_EQUAL(do_mouse_poll(EXO_USER_VA_END - 4), -EXO_EFAULT);
}

static void test_valid_poll_reports_accumulated_state(void)
{
    ps2_mouse_reset();
    feed_packet(0x03 /* left + right */, 5, -6);

    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)p,
                          EXO_PAGE_WRITE | EXO_PAGE_USER), 0);

    exo_mouse_state_t *out = (exo_mouse_state_t *)(uintptr_t)SCRATCH;
    out->dx = 0x7AAA; out->dy = 0x7AAA; out->buttons = 0xAA; out->reserved = 0xAA;

    CU_ASSERT_EQUAL(do_mouse_poll(SCRATCH), 0);
    CU_ASSERT_EQUAL(out->dx, 5);
    CU_ASSERT_EQUAL(out->dy, -6);
    CU_ASSERT_EQUAL(out->buttons, 0x03);
    CU_ASSERT_EQUAL(out->reserved, 0);

    CU_ASSERT_EQUAL(do_unmap(SCRATCH), 0);
    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
}

/* A poll resets dx/dy but leaves the last-reported buttons level in place --
 * matches ps2_mouse_poll()'s own contract, proven again here at the syscall
 * boundary. */
static void test_second_poll_resets_deltas_not_buttons(void)
{
    ps2_mouse_reset();
    feed_packet(0x01, 4, 4);

    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)p,
                          EXO_PAGE_WRITE | EXO_PAGE_USER), 0);

    exo_mouse_state_t *out = (exo_mouse_state_t *)(uintptr_t)SCRATCH;

    CU_ASSERT_EQUAL(do_mouse_poll(SCRATCH), 0);
    CU_ASSERT_EQUAL(out->dx, 4);
    CU_ASSERT_EQUAL(out->buttons, 0x01);

    CU_ASSERT_EQUAL(do_mouse_poll(SCRATCH), 0);
    CU_ASSERT_EQUAL(out->dx, 0);
    CU_ASSERT_EQUAL(out->dy, 0);
    CU_ASSERT_EQUAL(out->buttons, 0x01);

    CU_ASSERT_EQUAL(do_unmap(SCRATCH), 0);
    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
}

/* SCRUM-186-style repro (see test_syscall_kbd_k.c's own comment): an
 * in-window pointer that was never exo_page_map'd must be rejected before
 * the write, not take a fatal supervisor-mode page fault. */
static void test_unmapped_in_window_state_out_rejected(void)
{
    ps2_mouse_reset();
    feed_packet(0, 1, 1);

    CU_ASSERT_EQUAL(do_mouse_poll(UNMAPPED), -EXO_EFAULT);
}

/* Mapped but read-only (EXO_PAGE_USER without EXO_PAGE_WRITE) is still not
 * somewhere the kernel may write *state_out through on the caller's behalf. */
static void test_read_only_mapped_state_out_rejected(void)
{
    ps2_mouse_reset();
    feed_packet(0, 1, 1);

    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL(do_map(UNMAPPED, (uint64_t)p, EXO_PAGE_USER), 0);

    CU_ASSERT_EQUAL(do_mouse_poll(UNMAPPED), -EXO_EFAULT);

    CU_ASSERT_EQUAL(do_unmap(UNMAPPED), 0);
    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
}

void suite_syscall_mouse_tests(CU_pSuite s)
{
    CU_add_test(s, "handler is bound", test_handler_is_bound);
    CU_add_test(s, "NULL state_out rejected with a real packet queued",
               test_null_state_out_rejected);
    CU_add_test(s, "state_out below window rejected",
               test_state_out_below_window_rejected);
    CU_add_test(s, "state_out spanning window end rejected",
               test_state_out_spanning_window_end_rejected);
    CU_add_test(s, "valid poll reports accumulated state",
               test_valid_poll_reports_accumulated_state);
    CU_add_test(s, "second poll resets deltas not buttons",
               test_second_poll_resets_deltas_not_buttons);
    CU_add_test(s, "unmapped in-window state_out rejected",
               test_unmapped_in_window_state_out_rejected);
    CU_add_test(s, "read-only mapped state_out rejected",
               test_read_only_mapped_state_out_rejected);
}
