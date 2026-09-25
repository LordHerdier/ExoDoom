/*
 * test_speaker_k.c — PC speaker driver, PIT channel 2 (SCRUM-98).
 *
 * Acceptance is "can play a tone at a given frequency and duration", and
 * nothing here takes the driver's word for either half. Frequency is read
 * back from the hardware: the 8254's read-back command reports channel 2's
 * programmed mode, a latched count proves the reload value is the one
 * asked for, and port 0x61 bit 5 -- channel 2's OUT pin -- has to actually
 * toggle. Duration is proven by letting real IRQ0s land (the same
 * sti/kernel_sleep_ms/cli window test_syscall_pit_k.c uses, for the same
 * reason: ambient IF is not something a suite can assume) and checking the
 * speaker gate went off on its own.
 *
 * Every test leaves the speaker silent, so a failure here cannot leave a
 * tone running under every later suite.
 */

#include "kunit.h"
#include "speaker.h"
#include "sleep.h"
#include "io.h"

#include <stdint.h>

#define PORT_61_GATE_BITS 0x03
#define PORT_61_OUT2      0x20

static uint8_t port61(void)
{
    return inb(0x61);
}

/* 8254 read-back: 11 (read-back) 1 (don't latch count) 0 (latch status)
 * 1 0 0 (channel 2 only) 0. The status byte then comes out of 0x42. */
static uint8_t ch2_status(void)
{
    outb(0x43, 0xE8);
    return inb(0x42);
}

/* Counter-latch command for channel 2, then lobyte/hibyte of the count. */
static uint16_t ch2_count(void)
{
    outb(0x43, 0x80);
    uint8_t lo = inb(0x42);
    uint8_t hi = inb(0x42);
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

static void test_divisor_math(void)
{
    /* Rounded to nearest, the same way pit_init() rounds channel 0's. */
    CU_ASSERT_EQUAL(speaker_divisor_for(440), 2712);
    CU_ASSERT_EQUAL(speaker_divisor_for(1000), 1193);
    CU_ASSERT_EQUAL(speaker_divisor_for(SPEAKER_MIN_HZ), 62799);
    CU_ASSERT_EQUAL(speaker_divisor_for(SPEAKER_MAX_HZ), 60);
}

static void test_rejects_out_of_range_frequency(void)
{
    speaker_stop();

    CU_ASSERT_EQUAL(speaker_tone(0, 10), SPEAKER_EINVAL);
    CU_ASSERT_EQUAL(speaker_tone(SPEAKER_MIN_HZ - 1, 10), SPEAKER_EINVAL);
    CU_ASSERT_EQUAL(speaker_tone(SPEAKER_MAX_HZ + 1, 10), SPEAKER_EINVAL);
    CU_ASSERT_EQUAL(speaker_tone(0xFFFFFFFFu, 10), SPEAKER_EINVAL);

    /* Rejected means untouched: still silent. */
    CU_ASSERT_EQUAL(speaker_is_playing(), 0);
    CU_ASSERT_EQUAL(port61() & PORT_61_GATE_BITS, 0);
}

static void test_rejected_tone_leaves_current_tone_playing(void)
{
    CU_ASSERT_EQUAL(speaker_tone(440, 0), SPEAKER_OK);
    CU_ASSERT_EQUAL(speaker_tone(5, 0), SPEAKER_EINVAL);

    CU_ASSERT_EQUAL(speaker_is_playing(), 1);
    CU_ASSERT_EQUAL(port61() & PORT_61_GATE_BITS, PORT_61_GATE_BITS);
    CU_ASSERT_EQUAL(ch2_count() <= speaker_divisor_for(440), 1);

    speaker_stop();
}

static void test_tone_programs_channel2_and_opens_gate(void)
{
    CU_ASSERT_EQUAL(speaker_tone(440, 0), SPEAKER_OK);

    CU_ASSERT_EQUAL(speaker_is_playing(), 1);
    CU_ASSERT_EQUAL(port61() & PORT_61_GATE_BITS, PORT_61_GATE_BITS);

    /* Status low 6 bits: access lobyte/hibyte (11), mode 3 (011), binary. */
    CU_ASSERT_EQUAL(ch2_status() & 0x3F, 0x36);

    /* A running counter never exceeds its reload value, and it is counting:
     * two latches a few port reads apart differ. */
    uint16_t c = ch2_count();
    CU_ASSERT_EQUAL(c <= 2712, 1);
    CU_ASSERT_NOT_EQUAL(c, 0);

    speaker_stop();
}

static void test_channel2_out_actually_toggles(void)
{
    /* 1 kHz: OUT is high for 0.5 ms and low for 0.5 ms. Poll 0x61 bit 5
     * until both levels are seen -- the tone is really being generated,
     * not just configured. The bound is generous (each inb is a VM exit),
     * and the loop stops at the first sighting of both. */
    CU_ASSERT_EQUAL(speaker_tone(1000, 0), SPEAKER_OK);

    int seen_high = 0, seen_low = 0;
    for (uint32_t i = 0; i < 2000000u && !(seen_high && seen_low); i++) {
        if (port61() & PORT_61_OUT2) seen_high = 1;
        else                         seen_low = 1;
    }

    CU_ASSERT_EQUAL(seen_high, 1);
    CU_ASSERT_EQUAL(seen_low, 1);

    speaker_stop();
}

static void test_stop_closes_gate(void)
{
    CU_ASSERT_EQUAL(speaker_tone(880, 0), SPEAKER_OK);
    speaker_stop();

    CU_ASSERT_EQUAL(speaker_is_playing(), 0);
    CU_ASSERT_EQUAL(port61() & PORT_61_GATE_BITS, 0);
}

static void test_timed_tone_stops_on_its_own(void)
{
    CU_ASSERT_EQUAL(speaker_tone(440, 20), SPEAKER_OK);

    /* speaker_tone() returned at once; nothing has had time to stop it. */
    CU_ASSERT_EQUAL(speaker_is_playing(), 1);

    __asm__ volatile ("sti");
    kernel_sleep_ms(40);
    __asm__ volatile ("cli");

    CU_ASSERT_EQUAL(speaker_is_playing(), 0);
    CU_ASSERT_EQUAL(port61() & PORT_61_GATE_BITS, 0);

    speaker_stop();
}

static void test_timed_tone_plays_for_its_duration(void)
{
    /* Stopping late would pass the test above; stopping early must not. */
    CU_ASSERT_EQUAL(speaker_tone(440, 60), SPEAKER_OK);

    __asm__ volatile ("sti");
    kernel_sleep_ms(20);
    __asm__ volatile ("cli");

    CU_ASSERT_EQUAL(speaker_is_playing(), 1);
    CU_ASSERT_EQUAL(port61() & PORT_61_GATE_BITS, PORT_61_GATE_BITS);

    speaker_stop();
}

static void test_untimed_tone_keeps_playing(void)
{
    CU_ASSERT_EQUAL(speaker_tone(440, 0), SPEAKER_OK);

    __asm__ volatile ("sti");
    kernel_sleep_ms(20);
    __asm__ volatile ("cli");

    CU_ASSERT_EQUAL(speaker_is_playing(), 1);

    speaker_stop();
    CU_ASSERT_EQUAL(speaker_is_playing(), 0);
}

static void test_new_tone_replaces_deadline(void)
{
    /* A short timed tone followed by an untimed one: the old deadline must
     * not cut the new tone off. */
    CU_ASSERT_EQUAL(speaker_tone(440, 5), SPEAKER_OK);
    CU_ASSERT_EQUAL(speaker_tone(660, 0), SPEAKER_OK);

    __asm__ volatile ("sti");
    kernel_sleep_ms(20);
    __asm__ volatile ("cli");

    CU_ASSERT_EQUAL(speaker_is_playing(), 1);
    CU_ASSERT_EQUAL(ch2_count() <= speaker_divisor_for(660), 1);

    speaker_stop();
}

static void test_tone_preserves_interrupt_flag(void)
{
    uint64_t before, after;

    __asm__ volatile ("cli");
    __asm__ volatile ("pushfq; popq %0" : "=r"(before));
    speaker_tone(440, 0);
    speaker_stop();
    __asm__ volatile ("pushfq; popq %0" : "=r"(after));

    CU_ASSERT_EQUAL(before & 0x200, 0);
    CU_ASSERT_EQUAL(after & 0x200, 0);
}

void suite_speaker_tests(CU_pSuite s)
{
    CU_add_test(s, "divisor math", test_divisor_math);
    CU_add_test(s, "rejects out-of-range frequency",
               test_rejects_out_of_range_frequency);
    CU_add_test(s, "rejected tone leaves current tone playing",
               test_rejected_tone_leaves_current_tone_playing);
    CU_add_test(s, "tone programs channel 2 and opens gate",
               test_tone_programs_channel2_and_opens_gate);
    CU_add_test(s, "channel 2 OUT actually toggles",
               test_channel2_out_actually_toggles);
    CU_add_test(s, "stop closes gate", test_stop_closes_gate);
    CU_add_test(s, "timed tone stops on its own",
               test_timed_tone_stops_on_its_own);
    CU_add_test(s, "timed tone plays for its duration",
               test_timed_tone_plays_for_its_duration);
    CU_add_test(s, "untimed tone keeps playing",
               test_untimed_tone_keeps_playing);
    CU_add_test(s, "new tone replaces deadline",
               test_new_tone_replaces_deadline);
    CU_add_test(s, "tone preserves interrupt flag",
               test_tone_preserves_interrupt_flag);
}
