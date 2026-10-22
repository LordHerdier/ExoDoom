/*
 * test_rtc_k.c — CMOS RTC driver and exo_rtc_read (SCRUM-249).
 *
 * Three layers, matching docs/drivers/rtc.md:
 *   - the pure decoders and rtc_to_unix(), no hardware;
 *   - the live clock, which QEMU backs with the host's;
 *   - exo_rtc_read (#35), through the dispatcher with a mapped scratch page
 *     (the shape of test_syscall_stat_k.c) and from a real ring-3 probe
 *     (tests/kernel/rtc_probe.s) that must get errors, not faults, for a
 *     NULL and a kernel-space pointer.
 *
 * The expected Unix times were computed independently (Python's
 * calendar.timegm), not with the code under test.
 */

#include "kunit.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "rtc.h"
#include "sleep.h"
#include "vmm.h"
#include "page_alloc.h"
#include "libos_launch.h"
#include "libos_test_common.h"

#include <stdint.h>
#include <stddef.h>

#define SCRATCH  (EXO_USER_VA_BASE + 0x2A000000ULL)
#define UNMAPPED (SCRATCH + 0x1000ULL)
#define KERNEL_PTR 0x200000ULL   /* the kernel image; see rtc_probe.s */

static int64_t do_rtc(uint64_t out)
{
    return exo_syscall_dispatch(EXO_SYS_RTC_READ, out, 0, 0, 0, 0, 0);
}

static struct rtc_time mk(uint16_t y, uint8_t mo, uint8_t d,
                          uint8_t h, uint8_t mi, uint8_t s)
{
    struct rtc_time t = { y, mo, d, h, mi, s };
    return t;
}

/* ── Pure: BCD ─────────────────────────────────────────────────────────── */

static void test_bcd_decodes(void)
{
    CU_ASSERT_EQUAL(rtc_bcd_to_bin(0x00), 0);
    CU_ASSERT_EQUAL(rtc_bcd_to_bin(0x09), 9);
    CU_ASSERT_EQUAL(rtc_bcd_to_bin(0x10), 10);
    CU_ASSERT_EQUAL(rtc_bcd_to_bin(0x59), 59);
    CU_ASSERT_EQUAL(rtc_bcd_to_bin(0x99), 99);
}

static void test_bcd_validity(void)
{
    CU_ASSERT_TRUE(rtc_bcd_valid(0x00));
    CU_ASSERT_TRUE(rtc_bcd_valid(0x99));
    CU_ASSERT_FALSE(rtc_bcd_valid(0x0A));
    CU_ASSERT_FALSE(rtc_bcd_valid(0xA0));
    CU_ASSERT_FALSE(rtc_bcd_valid(0x1F));
}

/* ── Pure: hours ───────────────────────────────────────────────────────── */

static void test_hour_24h_binary(void)
{
    CU_ASSERT_EQUAL(rtc_decode_hour(0, 0, 0), 0);
    CU_ASSERT_EQUAL(rtc_decode_hour(13, 0, 0), 13);
    CU_ASSERT_EQUAL(rtc_decode_hour(23, 0, 0), 23);
    CU_ASSERT_EQUAL(rtc_decode_hour(24, 0, 0), 0xFF);
}

static void test_hour_24h_bcd(void)
{
    CU_ASSERT_EQUAL(rtc_decode_hour(0x00, 1, 0), 0);
    CU_ASSERT_EQUAL(rtc_decode_hour(0x09, 1, 0), 9);
    CU_ASSERT_EQUAL(rtc_decode_hour(0x23, 1, 0), 23);
    CU_ASSERT_EQUAL(rtc_decode_hour(0x24, 1, 0), 0xFF);
    CU_ASSERT_EQUAL(rtc_decode_hour(0x1A, 1, 0), 0xFF);
}

/* 12-hour mode: PM is bit 7 of the hours byte, and 12 AM / 12 PM are the
 * two cases a naive "+12 if PM" gets wrong. */
static void test_hour_12h_bcd(void)
{
    CU_ASSERT_EQUAL(rtc_decode_hour(0x12, 1, 1), 0);           /* 12 AM */
    CU_ASSERT_EQUAL(rtc_decode_hour(0x01, 1, 1), 1);           /*  1 AM */
    CU_ASSERT_EQUAL(rtc_decode_hour(0x11, 1, 1), 11);          /* 11 AM */
    CU_ASSERT_EQUAL(rtc_decode_hour(0x12 | 0x80, 1, 1), 12);   /* 12 PM */
    CU_ASSERT_EQUAL(rtc_decode_hour(0x01 | 0x80, 1, 1), 13);   /*  1 PM */
    CU_ASSERT_EQUAL(rtc_decode_hour(0x11 | 0x80, 1, 1), 23);   /* 11 PM */
    CU_ASSERT_EQUAL(rtc_decode_hour(0x00, 1, 1), 0xFF);
    CU_ASSERT_EQUAL(rtc_decode_hour(0x13, 1, 1), 0xFF);
}

static void test_hour_12h_binary(void)
{
    CU_ASSERT_EQUAL(rtc_decode_hour(12, 0, 1), 0);
    CU_ASSERT_EQUAL(rtc_decode_hour(12 | 0x80, 0, 1), 12);
    CU_ASSERT_EQUAL(rtc_decode_hour(7 | 0x80, 0, 1), 19);
}

/* ── Pure: rtc_to_unix ─────────────────────────────────────────────────── */

static void test_unix_epoch(void)
{
    struct rtc_time t = mk(1970, 1, 1, 0, 0, 0);
    CU_ASSERT_EQUAL(rtc_to_unix(&t), 0);
}

static void test_unix_leap_day_2024(void)
{
    struct rtc_time a = mk(2024, 2, 29, 0, 0, 0);
    struct rtc_time b = mk(2024, 2, 29, 12, 34, 56);
    CU_ASSERT_EQUAL(rtc_to_unix(&a), 1709164800ULL);
    CU_ASSERT_EQUAL(rtc_to_unix(&b), 1709210096ULL);
}

static void test_unix_century_boundaries(void)
{
    struct rtc_time y1999 = mk(1999, 12, 31, 23, 59, 59);
    struct rtc_time y2000 = mk(2000, 1, 1, 0, 0, 0);
    struct rtc_time leap2000 = mk(2000, 2, 29, 0, 0, 0);
    CU_ASSERT_EQUAL(rtc_to_unix(&y1999), 946684799ULL);
    CU_ASSERT_EQUAL(rtc_to_unix(&y2000), 946684800ULL);
    CU_ASSERT_EQUAL(rtc_to_unix(&leap2000), 951782400ULL);

    /* 2100 is divisible by 100 but not 400: not a leap year, so 28 Feb to
     * 1 Mar is one day, not two. */
    struct rtc_time feb28 = mk(2100, 2, 28, 0, 0, 0);
    struct rtc_time mar1 = mk(2100, 3, 1, 0, 0, 0);
    CU_ASSERT_EQUAL(rtc_to_unix(&feb28), 4107456000ULL);
    CU_ASSERT_EQUAL(rtc_to_unix(&mar1), 4107542400ULL);
}

static void test_unix_past_2038(void)
{
    struct rtc_time t = mk(2038, 1, 19, 3, 14, 8);   /* 2^31 */
    CU_ASSERT_EQUAL(rtc_to_unix(&t), 2147483648ULL);
}

/* ── Live ──────────────────────────────────────────────────────────────── */

static int in_range(const struct rtc_time *t)
{
    return t->year >= 2000 && t->year <= 2099 &&
           t->month >= 1 && t->month <= 12 &&
           t->day >= 1 && t->day <= 31 &&
           t->hour <= 23 && t->minute <= 59 && t->second <= 59;
}

static void test_live_read_in_range(void)
{
    struct rtc_time t;
    CU_ASSERT_EQUAL(rtc_read(&t), 0);
    CU_ASSERT_TRUE(in_range(&t));
}

/* QEMU's RTC follows the host clock. Compare unix times, not fields, so a
 * minute/day rollover between the reads cannot fail it. */
static void test_live_reads_are_monotonic(void)
{
    struct rtc_time a, b;
    CU_ASSERT_EQUAL(rtc_read(&a), 0);

    /* The RTC ticks once a second, so this wait cannot be shortened. IF is
     * clear ambiently (test_syscall_pit_k.c's header), and kernel_sleep_ms()
     * halts until IRQ0 -- without sti it never returns. Local, not ambient. */
    __asm__ volatile ("sti");
    kernel_sleep_ms(1100);
    __asm__ volatile ("cli");

    CU_ASSERT_EQUAL(rtc_read(&b), 0);

    uint64_t ua = rtc_to_unix(&a), ub = rtc_to_unix(&b);
    CU_ASSERT_TRUE(ub >= ua);
    CU_ASSERT_TRUE(ub - ua >= 1);
    CU_ASSERT_TRUE(ub - ua <= 5);
}

/* ── Syscall (dispatcher) ──────────────────────────────────────────────── */

static void test_handler_is_bound(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_RTC_READ));
}

static void test_syscall_null_rejected(void)
{
    CU_ASSERT_EQUAL(do_rtc(0), -EXO_EFAULT);
}

static void test_syscall_kernel_pointer_rejected(void)
{
    CU_ASSERT_EQUAL(do_rtc(KERNEL_PTR), -EXO_EFAULT);
}

static void test_syscall_unmapped_in_window_rejected(void)
{
    CU_ASSERT_EQUAL(do_rtc(UNMAPPED), -EXO_EFAULT);
}

static void test_syscall_writes_the_time(void)
{
    int64_t p = exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_PAGE_MAP, SCRATCH,
                        (uint64_t)p, EXO_PAGE_WRITE | EXO_PAGE_USER, 0, 0, 0),
                    0);

    /* A struct that ends exactly at the page end is fine; one byte further
     * is not. */
    uint64_t last = SCRATCH + 0x1000 - sizeof(exo_rtc_time_t);
    CU_ASSERT_EQUAL(do_rtc(last + 1), -EXO_EFAULT);
    CU_ASSERT_EQUAL(do_rtc(last), 0);

    exo_rtc_time_t *out = (exo_rtc_time_t *)(uintptr_t)SCRATCH;
    CU_ASSERT_EQUAL(do_rtc(SCRATCH), 0);

    struct rtc_time t = { out->year, out->month, out->day,
                          out->hour, out->minute, out->second };
    CU_ASSERT_TRUE(in_range(&t));
    CU_ASSERT_EQUAL(out->reserved, 0);
    CU_ASSERT_EQUAL(out->unix_secs, rtc_to_unix(&t));

    struct rtc_time now;
    CU_ASSERT_EQUAL(rtc_read(&now), 0);
    uint64_t un = rtc_to_unix(&now);
    CU_ASSERT_TRUE(un >= out->unix_secs && un - out->unix_secs <= 2);

    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, SCRATCH,
                                         0, 0, 0, 0, 0), 0);
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_PAGE_FREE, (uint64_t)p,
                                         0, 0, 0, 0, 0), 0);
}

/* ── Syscall (real ring 3) ─────────────────────────────────────────────── */

extern void rtc_probe(void);
extern void rtc_probe_end(void);

/* A real LibOS issues three real `syscall`s: a good buffer, NULL, and a
 * kernel address. Bit n of the result is set when call n+1 behaved; getting
 * a result at all means none of them faulted. */
static void test_ring3_probe(void)
{
    static uint8_t buf[sizeof(exo_rtc_time_t)];

    size_t code_len = (uintptr_t)&rtc_probe_end - (uintptr_t)&rtc_probe;

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(TEST_OWNER_RTC, (const void *)&rtc_probe,
                                      code_len, buf, sizeof(buf), 0, &img),
                    VMM_OK);

    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, libos_return);
    CU_ASSERT_EQUAL(vmm_switch_address_space(img.pml4_phys), VMM_OK);
    uint64_t result = libos_enter(img.entry_vaddr, img.stack_top_vaddr);
    CU_ASSERT_EQUAL(vmm_switch_address_space(vmm_kernel_pml4()), VMM_OK);
    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, 0);

    CU_ASSERT_EQUAL(result, 7);
    libos_destroy_image(TEST_OWNER_RTC, &img);
}

/* ── Registration ──────────────────────────────────────────────────────── */

void suite_rtc_tests(CU_pSuite s)
{
    CU_add_test(s, "BCD decodes", test_bcd_decodes);
    CU_add_test(s, "BCD validity", test_bcd_validity);
    CU_add_test(s, "hours: 24-hour binary", test_hour_24h_binary);
    CU_add_test(s, "hours: 24-hour BCD", test_hour_24h_bcd);
    CU_add_test(s, "hours: 12-hour BCD incl. 12 AM / 12 PM",
                test_hour_12h_bcd);
    CU_add_test(s, "hours: 12-hour binary", test_hour_12h_binary);
    CU_add_test(s, "unix: epoch", test_unix_epoch);
    CU_add_test(s, "unix: leap day 2024-02-29", test_unix_leap_day_2024);
    CU_add_test(s, "unix: century boundaries", test_unix_century_boundaries);
    CU_add_test(s, "unix: past 2038", test_unix_past_2038);
    CU_add_test(s, "live read is in range", test_live_read_in_range);
    CU_add_test(s, "live reads a second apart are monotonic",
                test_live_reads_are_monotonic);
    CU_add_test(s, "syscall handler is bound", test_handler_is_bound);
    CU_add_test(s, "syscall rejects NULL", test_syscall_null_rejected);
    CU_add_test(s, "syscall rejects a kernel pointer",
                test_syscall_kernel_pointer_rejected);
    CU_add_test(s, "syscall rejects an unmapped in-window pointer",
                test_syscall_unmapped_in_window_rejected);
    CU_add_test(s, "syscall writes the time", test_syscall_writes_the_time);
    CU_add_test(s, "ring 3 gets errors, not faults", test_ring3_probe);
}
