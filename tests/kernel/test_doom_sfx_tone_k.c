/*
 * test_doom_sfx_tone_k.c — Doom SFX -> PC speaker tone table (SCRUM-99).
 *
 * Acceptance is "shotgun blast, door open, imp alert each have distinct
 * sounds". That is checked directly, but the more useful checks are the
 * whole-table ones: every one of sfxenum_t's real ids has a sequence, and
 * every step of every sequence is something src/speaker.c will actually
 * play. A step outside [SPEAKER_MIN_HZ, SPEAKER_MAX_HZ] would come back
 * -EXO_EINVAL from exo_sound_tone and be silently dropped at runtime, so it
 * is caught here instead -- the one place both headers can meet.
 */

#include "kunit.h"
#include "doom_sfx_tone.h"
#include "doom/sounds.h"
#include "speaker.h"

#include <stddef.h>

/* Long enough for Doom's longest effects (deaths, BFG), short enough that
 * a single effect cannot hog the one-voice speaker for most of a second. */
#define MAX_SEQUENCE_MS 600u

static int same_sequence(const doom_sfx_tone_t *a, const doom_sfx_tone_t *b)
{
    if (a->n_steps != b->n_steps) return 0;
    for (uint8_t i = 0; i < a->n_steps; i++) {
        if (a->step[i].freq_hz != b->step[i].freq_hz) return 0;
        if (a->step[i].dur_ms != b->step[i].dur_ms) return 0;
    }
    return 1;
}

static void test_none_and_out_of_range_have_no_tone(void)
{
    CU_ASSERT_PTR_NULL(doom_sfx_tone(sfx_None));
    CU_ASSERT_PTR_NULL(doom_sfx_tone(-1));
    CU_ASSERT_PTR_NULL(doom_sfx_tone(NUMSFX));
    CU_ASSERT_PTR_NULL(doom_sfx_tone(0x7FFFFFFF));
}

static void test_every_sfx_has_a_tone(void)
{
    int missing = 0;

    for (int id = sfx_None + 1; id < NUMSFX; id++) {
        if (doom_sfx_tone(id) == NULL)
            missing++;
    }
    CU_ASSERT_EQUAL(missing, 0);
}

static void test_every_step_is_playable(void)
{
    int bad_count = 0, bad_freq = 0, bad_dur = 0, too_long = 0;

    for (int id = sfx_None + 1; id < NUMSFX; id++) {
        const doom_sfx_tone_t *t = doom_sfx_tone(id);
        if (t == NULL) continue;

        if (t->n_steps < 1 || t->n_steps > DOOM_SFX_MAX_STEPS) {
            bad_count++;
            continue;
        }
        for (uint8_t i = 0; i < t->n_steps; i++) {
            uint32_t f = t->step[i].freq_hz;
            if (f < SPEAKER_MIN_HZ || f > SPEAKER_MAX_HZ) bad_freq++;
            if (t->step[i].dur_ms == 0) bad_dur++;
        }
        if (doom_sfx_tone_total_ms(t) > MAX_SEQUENCE_MS) too_long++;
    }

    CU_ASSERT_EQUAL(bad_count, 0);
    CU_ASSERT_EQUAL(bad_freq, 0);
    CU_ASSERT_EQUAL(bad_dur, 0);
    CU_ASSERT_EQUAL(too_long, 0);
}

static void test_acceptance_sounds_are_distinct(void)
{
    const doom_sfx_tone_t *shotgun = doom_sfx_tone(sfx_shotgn);
    const doom_sfx_tone_t *door    = doom_sfx_tone(sfx_doropn);
    const doom_sfx_tone_t *imp     = doom_sfx_tone(sfx_bgsit1);

    CU_ASSERT_PTR_NOT_NULL(shotgun);
    CU_ASSERT_PTR_NOT_NULL(door);
    CU_ASSERT_PTR_NOT_NULL(imp);
    if (!shotgun || !door || !imp) return;

    CU_ASSERT_EQUAL(same_sequence(shotgun, door), 0);
    CU_ASSERT_EQUAL(same_sequence(shotgun, imp), 0);
    CU_ASSERT_EQUAL(same_sequence(door, imp), 0);

    /* Distinct from the very first step, not just somewhere in the tail:
     * a sound cut short by a higher-priority one still reads as itself. */
    CU_ASSERT_NOT_EQUAL(shotgun->step[0].freq_hz, door->step[0].freq_hz);
    CU_ASSERT_NOT_EQUAL(shotgun->step[0].freq_hz, imp->step[0].freq_hz);
    CU_ASSERT_NOT_EQUAL(door->step[0].freq_hz, imp->step[0].freq_hz);
}

static void test_acceptance_sounds_have_their_character(void)
{
    const doom_sfx_tone_t *shotgun = doom_sfx_tone(sfx_shotgn);
    const doom_sfx_tone_t *door    = doom_sfx_tone(sfx_doropn);
    const doom_sfx_tone_t *imp     = doom_sfx_tone(sfx_bgsit1);
    if (!shotgun || !door || !imp) {
        CU_ASSERT(0 && "acceptance sounds missing");
        return;
    }

    /* Shotgun: a blast falls in pitch step on step. */
    for (uint8_t i = 1; i < shotgun->n_steps; i++)
        CU_ASSERT(shotgun->step[i].freq_hz < shotgun->step[i - 1].freq_hz);

    /* Door open: a rising sweep -- and door close is its mirror. */
    for (uint8_t i = 1; i < door->n_steps; i++)
        CU_ASSERT(door->step[i].freq_hz > door->step[i - 1].freq_hz);
    const doom_sfx_tone_t *close = doom_sfx_tone(sfx_dorcls);
    CU_ASSERT_PTR_NOT_NULL(close);
    if (close) {
        for (uint8_t i = 1; i < close->n_steps; i++)
            CU_ASSERT(close->step[i].freq_hz < close->step[i - 1].freq_hz);
    }

    /* Imp alert: a growl, not a sweep -- it changes direction. */
    CU_ASSERT(imp->n_steps >= 3);
    if (imp->n_steps >= 3) {
        int up_then_down = imp->step[1].freq_hz > imp->step[0].freq_hz &&
                           imp->step[2].freq_hz < imp->step[1].freq_hz;
        int down_then_up = imp->step[1].freq_hz < imp->step[0].freq_hz &&
                           imp->step[2].freq_hz > imp->step[1].freq_hz;
        CU_ASSERT(up_then_down || down_then_up);
    }
}

static void test_total_ms(void)
{
    const doom_sfx_tone_t *shotgun = doom_sfx_tone(sfx_shotgn);

    CU_ASSERT_EQUAL(doom_sfx_tone_total_ms(NULL), 0);
    CU_ASSERT_PTR_NOT_NULL(shotgun);
    if (shotgun) {
        uint32_t sum = 0;
        for (uint8_t i = 0; i < shotgun->n_steps; i++)
            sum += shotgun->step[i].dur_ms;
        CU_ASSERT_EQUAL(doom_sfx_tone_total_ms(shotgun), sum);
    }
}

void suite_doom_sfx_tone_tests(CU_pSuite s)
{
    CU_add_test(s, "sfx_None and out-of-range ids have no tone",
               test_none_and_out_of_range_have_no_tone);
    CU_add_test(s, "every sfx has a tone", test_every_sfx_has_a_tone);
    CU_add_test(s, "every step is playable", test_every_step_is_playable);
    CU_add_test(s, "shotgun, door open, imp alert are distinct",
               test_acceptance_sounds_are_distinct);
    CU_add_test(s, "shotgun, door open, imp alert have their character",
               test_acceptance_sounds_have_their_character);
    CU_add_test(s, "total_ms sums the steps", test_total_ms);
}
