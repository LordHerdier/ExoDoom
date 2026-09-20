/*
 * test_syscall_serial_k.c — exo_serial_write (SCRUM-50).
 *
 * Drives the real dispatch path, exo_syscall_dispatch(EXO_SYS_SERIAL_WRITE,
 * ...) — the route a ring-3 `syscall` takes once the entry stub has
 * marshalled its arguments.  The handler is bound in kernel_main by
 * syscall_serial_init() before run_tests().
 *
 * There is no way to observe the bytes that actually reach COM1 from inside
 * the harness — serial is write-only and QEMU's stdio backing has no
 * loopback the kernel can read — so what these tests can prove is the
 * contract at the syscall boundary: a legitimate buffer is accepted and
 * every byte reported written, and a buffer outside the LibOS window is
 * rejected with -EXO_EFAULT before serial.c ever sees it. Actually seeing
 * the bytes land on COM1 is a `make docker-run-kernel` / serial-log check
 * away, not something this suite automates.
 *
 * The scratch mapping below reuses exo_page_alloc/exo_page_map (already
 * bound) rather than calling page_alloc.c/vmm.c directly, the same reasoning
 * test_page_map_k.c gives: it keeps this suite testing the syscalls' real,
 * user-visible effect rather than an internal shortcut.
 */

#include "kunit.h"
#include "syscall.h"
#include "syscall_serial.h"
#include "exo_syscall.h"

#include <stdint.h>

/* Scratch virtual address in the LibOS window, apart from test_page_map_k.c's
 * and test_vmm_k.c's ranges. */
#define SCRATCH (EXO_USER_VA_BASE + 0x28000000ULL)

/* A second scratch address in this suite's own range, one page past SCRATCH,
 * that no test here ever maps -- the in-window-but-unmapped pointer SCRUM-186
 * reproduces the crash with. */
#define UNMAPPED (SCRATCH + 0x1000ULL)

static int64_t do_serial_write(uint64_t buf, uint64_t len)
{
    return exo_syscall_dispatch(EXO_SYS_SERIAL_WRITE, buf, len, 0, 0, 0, 0);
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
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_SERIAL_WRITE));
}

static void test_zero_length_write_is_a_noop(void)
{
    /* len == 0 needs no valid buf at all -- NULL is fine, there is nothing
     * to read. */
    CU_ASSERT_EQUAL(do_serial_write(0, 0), 0);
}

static void test_null_buffer_rejected(void)
{
    CU_ASSERT_EQUAL(do_serial_write(0, 1), -EXO_EFAULT);
}

static void test_buffer_below_window_rejected(void)
{
    CU_ASSERT_EQUAL(do_serial_write(EXO_USER_VA_BASE - 1, 1), -EXO_EFAULT);
}

static void test_buffer_spanning_window_end_rejected(void)
{
    /* Starts one byte before the window's end -- a valid single-byte write,
     * but two bytes runs off the top of the window. */
    CU_ASSERT_EQUAL(do_serial_write(EXO_USER_VA_END - 1, 2), -EXO_EFAULT);
}

static void test_oversized_write_rejected(void)
{
    /* The whole call runs with IF clear (syscall's FMASK) -- a real DoS
     * surface without a cap, not just a slow write. buf doesn't need to be
     * valid: the length check runs first, so this is rejected before the
     * window check ever looks at it. */
    CU_ASSERT_EQUAL(do_serial_write(0, SERIAL_WRITE_MAX_LEN + 1), -EXO_EINVAL);
}

static void test_max_length_at_cap_not_rejected_by_length_check(void)
{
    /* Exactly at the cap must not trip -EXO_EINVAL -- only fail on the
     * window check that necessarily follows a NULL buf, proving the cap
     * itself is inclusive ("exceeds", not "reaches"). */
    CU_ASSERT_EQUAL(do_serial_write(0, SERIAL_WRITE_MAX_LEN), -EXO_EFAULT);
}

static void test_valid_write_reports_full_length(void)
{
    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)p,
                          EXO_PAGE_WRITE | EXO_PAGE_USER), 0);

    char *buf = (char *)(uintptr_t)SCRATCH;
    const char msg[] = "hello from ring 3\n";
    for (size_t i = 0; i < sizeof(msg) - 1; i++)
        buf[i] = msg[i];

    CU_ASSERT_EQUAL(do_serial_write(SCRATCH, sizeof(msg) - 1),
                   (int64_t)(sizeof(msg) - 1));

    CU_ASSERT_EQUAL(do_unmap(SCRATCH), 0);
    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
}

/* SCRUM-186: an in-window pointer that was never exo_page_map'd used to reach
 * serial.c and take a fatal supervisor-mode page fault (the whole window is
 * reserved-but-unmapped by default). This is the exact crash repro -- it
 * must now come back cleanly as -EXO_EFAULT instead. */
static void test_unmapped_in_window_buffer_rejected(void)
{
    CU_ASSERT_EQUAL(do_serial_write(UNMAPPED, 1), -EXO_EFAULT);
}

/* Present but not user-accessible (flags 0: neither WRITE nor USER) is still
 * not memory ring 3 could have written -- the kernel-only identity map looks
 * exactly like this. */
static void test_mapped_kernel_only_buffer_rejected(void)
{
    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL(do_map(UNMAPPED, (uint64_t)p, 0), 0);

    CU_ASSERT_EQUAL(do_serial_write(UNMAPPED, 1), -EXO_EFAULT);

    CU_ASSERT_EQUAL(do_unmap(UNMAPPED), 0);
    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
}

/* exo_serial_write only *reads* buf -- a present, user-accessible page with
 * no WRITE bit is still a legitimate source, unlike exo_fb_acquire/
 * exo_kbd_poll which write *through* their pointer and do need WRITE. */
static void test_read_only_mapped_buffer_accepted(void)
{
    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL(do_map(UNMAPPED, (uint64_t)p, EXO_PAGE_USER), 0);

    char *buf = (char *)(uintptr_t)UNMAPPED;
    buf[0] = 'x';

    CU_ASSERT_EQUAL(do_serial_write(UNMAPPED, 1), 1);

    CU_ASSERT_EQUAL(do_unmap(UNMAPPED), 0);
    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
}

void suite_syscall_serial_tests(CU_pSuite s)
{
    CU_add_test(s, "handler is bound", test_handler_is_bound);
    CU_add_test(s, "zero-length write is a no-op", test_zero_length_write_is_a_noop);
    CU_add_test(s, "NULL buffer rejected", test_null_buffer_rejected);
    CU_add_test(s, "buffer below window rejected", test_buffer_below_window_rejected);
    CU_add_test(s, "buffer spanning window end rejected",
               test_buffer_spanning_window_end_rejected);
    CU_add_test(s, "oversized write rejected", test_oversized_write_rejected);
    CU_add_test(s, "max length at cap not rejected by length check",
               test_max_length_at_cap_not_rejected_by_length_check);
    CU_add_test(s, "valid write reports full length",
               test_valid_write_reports_full_length);
    CU_add_test(s, "unmapped in-window buffer rejected",
               test_unmapped_in_window_buffer_rejected);
    CU_add_test(s, "mapped kernel-only buffer rejected",
               test_mapped_kernel_only_buffer_rejected);
    CU_add_test(s, "read-only mapped buffer accepted",
               test_read_only_mapped_buffer_accepted);
}
