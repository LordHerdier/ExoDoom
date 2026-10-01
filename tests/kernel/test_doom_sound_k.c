/*
 * test_doom_sound_k.c — Doom's PC speaker sound module (SCRUM-101).
 *
 * Acceptance is "sounds play during gameplay without blocking the game
 * loop". This drives src/doom_sound.c's sequencer from ring 0 -- through
 * exo_syscall_dispatch() into the real SCRUM-100 handlers and the real
 * speaker (see that file's EXO_KERNEL note) -- with a hand-advanced clock,
 * so step timing is deterministic. The kernel's own tone deadlines run on
 * real IRQ0 ticks, which cannot land here with IF clear; so a step that the
 * sequencer has moved past is still "sounding" in the driver until the
 * next step replaces it, which is exactly how the two layers compose at
 * runtime too.
 *
 * Which step is sounding is checked on the hardware as well as through
 * doom_sound_current_step(): a PIT channel 2 count never exceeds its
 * reload value, so sampling the latched count distinguishes a 900 Hz step
 * (divisor 1326) from a 150 Hz one (divisor 7955).
 *
 * "Without blocking" is checked the only way that means anything: with
 * interrupts on and real time passing, a thousand start/update calls take
 * less than one tick.
 */

#include "kunit.h"
#include "doom_sound.h"
#include "doom_sfx_tone.h"
#include "doom/sounds.h"
#include "speaker.h"
#include "pit.h"
#include "io.h"

#include <stdint.h>

#define T0 1000000u   /* arbitrary base for the hand clock */

static uint16_t ch2_max_count(int samples)
{
    uint16_t max = 0;
    for (int i = 0; i < samples; i++) {
        outb(0x43, 0x80);
        uint16_t c = inb(0x42);
        c |= (uint16_t)inb(0x42) << 8;
        if (c > max) max = c;
    }
    return max;
}

static void reset(void)
{
    doom_sound_reset();
    speaker_stop();
}

static void test_start_plays_first_step(void)
{
    reset();
    const doom_sfx_tone_t *t = doom_sfx_tone(sfx_shotgn);

    CU_ASSERT_EQUAL(doom_sound_start(sfx_shotgn, 3, 64, 127, T0), 3);
    CU_ASSERT_EQUAL(doom_sound_current_sfx(), sfx_shotgn);
    CU_ASSERT_EQUAL(doom_sound_current_step(), 0);
    CU_ASSERT_EQUAL(doom_sound_is_playing(3, T0), 1);
    CU_ASSERT_EQUAL(speaker_is_playing(), 1);
    CU_ASSERT(ch2_max_count(2000) <= speaker_divisor_for(t->step[0].freq_hz));

    reset();
}

static void test_update_advances_through_steps_then_finishes(void)
{
    reset();
    const doom_sfx_tone_t *t = doom_sfx_tone(sfx_shotgn);
    uint32_t end0 = T0 + t->step[0].dur_ms;
    uint32_t end1 = end0 + t->step[1].dur_ms;
    uint32_t total = doom_sfx_tone_total_ms(t);

    doom_sound_start(sfx_shotgn, 0, 64, 127, T0);

    doom_sound_update(end0 - 1);
    CU_ASSERT_EQUAL(doom_sound_current_step(), 0);

    doom_sound_update(end0);
    CU_ASSERT_EQUAL(doom_sound_current_step(), 1);
    /* 900 Hz -> 300 Hz: the hardware divisor grew past step 0's. */
    CU_ASSERT(ch2_max_count(4000) > speaker_divisor_for(t->step[0].freq_hz));

    doom_sound_update(end1);
    CU_ASSERT_EQUAL(doom_sound_current_step(), 2);

    CU_ASSERT_EQUAL(doom_sound_is_playing(0, T0 + total - 1), 1);
    CU_ASSERT_EQUAL(doom_sound_is_playing(0, T0 + total), 0);
    CU_ASSERT_EQUAL(doom_sound_current_sfx(), 0);

    reset();
}

static void test_late_update_skips_steps_already_over(void)
{
    reset();
    /* Door open: 4 steps x 40 ms, due at +40/+80/+120/+160. One update at
     * +130 -- a long frame -- must land on the last step, not replay step 1
     * a hundred milliseconds late. */
    doom_sound_start(sfx_doropn, 1, 100, 127, T0);
    doom_sound_update(T0 + 130);

    CU_ASSERT_EQUAL(doom_sound_current_sfx(), sfx_doropn);
    CU_ASSERT_EQUAL(doom_sound_current_step(), 3);

    /* An update long after the end just finishes. */
    doom_sound_update(T0 + 10000);
    CU_ASSERT_EQUAL(doom_sound_current_sfx(), 0);

    reset();
}

static void test_more_important_sound_takes_the_voice(void)
{
    reset();
    /* Lower number = more important (s_sound.c's S_GetChannel). */
    doom_sound_start(sfx_itemup, 0, 78, 127, T0);
    doom_sound_start(sfx_pldeth, 1, 32, 127, T0 + 1);

    CU_ASSERT_EQUAL(doom_sound_current_sfx(), sfx_pldeth);
    CU_ASSERT_EQUAL(doom_sound_is_playing(0, T0 + 1), 0);
    CU_ASSERT_EQUAL(doom_sound_is_playing(1, T0 + 1), 1);

    reset();
}

static void test_less_important_sound_is_dropped(void)
{
    reset();
    doom_sound_start(sfx_pldeth, 0, 32, 127, T0);

    /* The handle still comes back -- s_sound.c needs one -- but the effect
     * is not playing, so s_sound.c retires that channel. */
    CU_ASSERT_EQUAL(doom_sound_start(sfx_itemup, 1, 78, 127, T0 + 1), 1);
    CU_ASSERT_EQUAL(doom_sound_is_playing(1, T0 + 1), 0);
    CU_ASSERT_EQUAL(doom_sound_current_sfx(), sfx_pldeth);

    reset();
}

static void test_equal_priority_restarts(void)
{
    reset();
    doom_sound_start(sfx_pistol, 0, 64, 127, T0);
    doom_sound_update(T0 + 15);
    CU_ASSERT_EQUAL(doom_sound_current_step(), 1);

    /* Rapid fire: the next shot restarts from step 0. */
    doom_sound_start(sfx_pistol, 2, 64, 127, T0 + 16);
    CU_ASSERT_EQUAL(doom_sound_current_step(), 0);
    CU_ASSERT_EQUAL(doom_sound_is_playing(2, T0 + 16), 1);

    reset();
}

static void test_finished_sound_frees_the_voice(void)
{
    reset();
    const doom_sfx_tone_t *t = doom_sfx_tone(sfx_pldeth);
    doom_sound_start(sfx_pldeth, 0, 32, 127, T0);

    /* Once the important one is over, a lesser one plays. */
    uint32_t after = T0 + doom_sfx_tone_total_ms(t);
    doom_sound_start(sfx_itemup, 1, 78, 127, after);
    CU_ASSERT_EQUAL(doom_sound_current_sfx(), sfx_itemup);

    reset();
}

static void test_zero_volume_and_no_tone_play_nothing(void)
{
    reset();
    doom_sound_start(sfx_shotgn, 0, 64, 0, T0);
    CU_ASSERT_EQUAL(doom_sound_current_sfx(), 0);
    CU_ASSERT_EQUAL(speaker_is_playing(), 0);

    doom_sound_start(sfx_None, 0, 64, 127, T0);
    doom_sound_start(NUMSFX, 0, 64, 127, T0);
    CU_ASSERT_EQUAL(doom_sound_current_sfx(), 0);
    CU_ASSERT_EQUAL(speaker_is_playing(), 0);
}

static void test_stop_only_stops_its_own_channel(void)
{
    reset();
    doom_sound_start(sfx_shotgn, 4, 64, 127, T0);

    doom_sound_stop(5);
    CU_ASSERT_EQUAL(doom_sound_is_playing(4, T0), 1);
    CU_ASSERT_EQUAL(speaker_is_playing(), 1);

    doom_sound_stop(4);
    CU_ASSERT_EQUAL(doom_sound_is_playing(4, T0), 0);
    CU_ASSERT_EQUAL(speaker_is_playing(), 0);
}

static void test_does_not_block_the_game_loop(void)
{
    reset();

    /* Real time this once: interrupts on, so if anything here waited for a
     * tone to finish, ticks would pile up. A thousand frames' worth of
     * start+update+is_playing on the hand clock (every sound re-triggered,
     * every step change a real syscall) must fit well inside one 1 ms tick
     * each -- 5 ms total allows for an IRQ landing mid-loop. */
    __asm__ volatile ("sti");
    uint32_t t0 = kernel_get_ticks_ms();
    for (uint32_t i = 0; i < 1000; i++) {
        uint32_t now = T0 + i * 29;        /* ~one Doom tic apart */
        doom_sound_start(sfx_shotgn, 0, 64, 127, now);
        doom_sound_update(now + 25);
        (void)doom_sound_is_playing(0, now + 25);
    }
    uint32_t elapsed = kernel_get_ticks_ms() - t0;
    __asm__ volatile ("cli");

    CU_ASSERT(elapsed < 5);

    reset();
}

void suite_doom_sound_tests(CU_pSuite s)
{
    CU_add_test(s, "start plays first step", test_start_plays_first_step);
    CU_add_test(s, "update advances through steps then finishes",
               test_update_advances_through_steps_then_finishes);
    CU_add_test(s, "late update skips steps already over",
               test_late_update_skips_steps_already_over);
    CU_add_test(s, "more important sound takes the voice",
               test_more_important_sound_takes_the_voice);
    CU_add_test(s, "less important sound is dropped",
               test_less_important_sound_is_dropped);
    CU_add_test(s, "equal priority restarts", test_equal_priority_restarts);
    CU_add_test(s, "finished sound frees the voice",
               test_finished_sound_frees_the_voice);
    CU_add_test(s, "zero volume and no tone play nothing",
               test_zero_volume_and_no_tone_play_nothing);
    CU_add_test(s, "stop only stops its own channel",
               test_stop_only_stops_its_own_channel);
    CU_add_test(s, "does not block the game loop",
               test_does_not_block_the_game_loop);
}
