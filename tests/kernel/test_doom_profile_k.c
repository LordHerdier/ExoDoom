/*
 * test_doom_profile_k.c — SCRUM-87's tests for src/doom_profile.c.
 *
 * doom_profile_mark() accumulates {frames, total_ms, max_ms} per slot and
 * prints+resets once a slot's window fills (DOOM_PROF_WINDOW_FRAMES, 35 --
 * see src/doom_profile.c). This suite drives the real function from ring 0
 * and asserts on state via doom_profile_test_state(), the same
 * expose-real-state-rather-than-capture-serial approach
 * tests/kernel/test_doom_panic_k.c uses for doom_panic_in_progress().
 */

#include "kunit.h"

#include "doom_profile.h"

static void reset_all(void)
{
    doom_profile_reset_all();
}

static void test_state_starts_clear(void)
{
    uint32_t frames, total_ms, max_ms;

    reset_all();

    doom_profile_test_state(DOOM_PROF_SIM, &frames, &total_ms, &max_ms);
    CU_ASSERT_EQUAL(frames, 0);
    CU_ASSERT_EQUAL(total_ms, 0);
    CU_ASSERT_EQUAL(max_ms, 0);
}

/*
 * A handful of marks, well short of the 35-frame window, must accumulate
 * without resetting -- the report/reset only fires once the window fills.
 */
static void test_accumulates_within_window(void)
{
    uint32_t frames, total_ms, max_ms;

    reset_all();

    doom_profile_mark(DOOM_PROF_RENDER, 3);
    doom_profile_mark(DOOM_PROF_RENDER, 5);
    doom_profile_mark(DOOM_PROF_RENDER, 1);

    doom_profile_test_state(DOOM_PROF_RENDER, &frames, &total_ms, &max_ms);
    CU_ASSERT_EQUAL(frames, 3);
    CU_ASSERT_EQUAL(total_ms, 9);
    CU_ASSERT_EQUAL(max_ms, 5);
}

/*
 * Every slot is independent state -- marking one must not perturb another.
 * The blit/render nesting src/doom_profile.h documents (DOOM_PROF_RENDER
 * includes DOOM_PROF_BLIT's time, because D_Display() calls DG_DrawFrame())
 * is a caller-side relationship, not something the counters themselves
 * enforce, so this checks the counters hold that boundary correctly.
 */
static void test_slots_are_independent(void)
{
    uint32_t sim_frames, sim_total, sim_max;
    uint32_t blit_frames, blit_total, blit_max;

    reset_all();

    doom_profile_mark(DOOM_PROF_SIM, 7);
    doom_profile_mark(DOOM_PROF_BLIT, 2);
    doom_profile_mark(DOOM_PROF_BLIT, 4);

    doom_profile_test_state(DOOM_PROF_SIM, &sim_frames, &sim_total, &sim_max);
    doom_profile_test_state(DOOM_PROF_BLIT, &blit_frames, &blit_total,
                             &blit_max);

    CU_ASSERT_EQUAL(sim_frames, 1);
    CU_ASSERT_EQUAL(sim_total, 7);
    CU_ASSERT_EQUAL(sim_max, 7);

    CU_ASSERT_EQUAL(blit_frames, 2);
    CU_ASSERT_EQUAL(blit_total, 6);
    CU_ASSERT_EQUAL(blit_max, 4);
}

/*
 * DOOM_PROF_TIC_WAIT/_TIC_RUN (SCRUM-87 follow-up) are the two newest slots
 * -- exercised on their own so a slot-table mistake (e.g. the designated
 * initializer in doom_profile.c missing one of them) shows up here rather
 * than only at the printf call site.
 */
static void test_tic_wait_and_run_slots_are_independent(void)
{
    uint32_t wait_frames, wait_total, wait_max;
    uint32_t run_frames, run_total, run_max;

    reset_all();

    doom_profile_mark(DOOM_PROF_TIC_WAIT, 12);
    doom_profile_mark(DOOM_PROF_TIC_WAIT, 15);
    doom_profile_mark(DOOM_PROF_TIC_RUN, 1);

    doom_profile_test_state(DOOM_PROF_TIC_WAIT, &wait_frames, &wait_total,
                             &wait_max);
    doom_profile_test_state(DOOM_PROF_TIC_RUN, &run_frames, &run_total,
                             &run_max);

    CU_ASSERT_EQUAL(wait_frames, 2);
    CU_ASSERT_EQUAL(wait_total, 27);
    CU_ASSERT_EQUAL(wait_max, 15);

    CU_ASSERT_EQUAL(run_frames, 1);
    CU_ASSERT_EQUAL(run_total, 1);
    CU_ASSERT_EQUAL(run_max, 1);
}

/*
 * The window is exactly DOOM_PROF_WINDOW_FRAMES (35) marks wide: the 35th
 * mark must both fold into the report AND reset the counter for the next
 * window, so frame 36 starts from zero again rather than compounding.
 */
static void test_window_closes_and_resets(void)
{
    uint32_t frames, total_ms, max_ms;
    int      i;

    reset_all();

    for (i = 0; i < 34; i++) {
        doom_profile_mark(DOOM_PROF_TOTAL, 2);
    }
    doom_profile_test_state(DOOM_PROF_TOTAL, &frames, &total_ms, &max_ms);
    CU_ASSERT_EQUAL(frames, 34);
    CU_ASSERT_EQUAL(total_ms, 68);

    /* One outlier as the 35th (final) mark of the window. */
    doom_profile_mark(DOOM_PROF_TOTAL, 9);

    doom_profile_test_state(DOOM_PROF_TOTAL, &frames, &total_ms, &max_ms);
    CU_ASSERT_EQUAL(frames, 0);
    CU_ASSERT_EQUAL(total_ms, 0);
    CU_ASSERT_EQUAL(max_ms, 0);

    /* The next window starts clean, not carrying the outlier forward. */
    doom_profile_mark(DOOM_PROF_TOTAL, 1);
    doom_profile_test_state(DOOM_PROF_TOTAL, &frames, &total_ms, &max_ms);
    CU_ASSERT_EQUAL(frames, 1);
    CU_ASSERT_EQUAL(total_ms, 1);
    CU_ASSERT_EQUAL(max_ms, 1);
}

/* An out-of-range slot must be a no-op, not an out-of-bounds write. */
static void test_out_of_range_slot_is_noop(void)
{
    reset_all();

    doom_profile_mark((doom_prof_slot_t)-1, 5);
    doom_profile_mark(DOOM_PROF_COUNT, 5);
    doom_profile_mark((doom_prof_slot_t)(DOOM_PROF_COUNT + 1), 5);

    /* Nothing to assert on directly (there's no counter for an invalid
     * slot) -- the suite passing at all, with no fault, is the assertion. */
    CU_ASSERT_EQUAL(1, 1);
}

void suite_doom_profile_tests(CU_pSuite s)
{
    CU_add_test(s, "state starts clear", test_state_starts_clear);
    CU_add_test(s, "accumulates within window", test_accumulates_within_window);
    CU_add_test(s, "slots are independent", test_slots_are_independent);
    CU_add_test(s, "tic_wait/tic_run slots are independent",
                test_tic_wait_and_run_slots_are_independent);
    CU_add_test(s, "window closes and resets", test_window_closes_and_resets);
    CU_add_test(s, "out-of-range slot is a no-op",
                test_out_of_range_slot_is_noop);
}
