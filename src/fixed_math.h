#pragma once
#include <stdint.h>

/*
 * fixed_math — ExoDoom's fixed-point trigonometry (SCRUM-41).
 *
 * This is the engine behind what the ticket calls "math.h": sine, cosine,
 * tangent and arctangent, computed in integer arithmetic only.  It is
 * deliberately NOT a `double` libm, and that is a build constraint rather
 * than a preference:
 *
 *   The kernel compiles with -mno-sse -mno-sse2 (docker/scripts/build.sh's
 *   CFLAGS), and the x86_64 SysV ABI returns `double` in xmm0.  A
 *   `double sin(double)` in any file under src/ is therefore rejected outright
 *   -- "SSE register return with SSE disabled" -- the same wall
 *   src/doom/m_config.c's M_GetFloatVariable() hit in SCRUM-64, only this
 *   time in a file the kernel's own glob compiles.  Every kernel test TU
 *   is compiled with those same flags, so a double-valued API could not be
 *   called from a unit test either, let alone from ring 0.
 *
 * Integer fixed point sidesteps both: it builds under -mno-sse, it runs in
 * the kernel and in a ring-3 LibOS unchanged, and it is exactly the form
 * Doom's renderer consumes anyway -- the engine is fixed-point throughout
 * (src/doom/m_fixed.c), and the three trig calls that motivated this ticket
 * (src/doom/r_main.c's R_InitTables/R_InitPointToAngle) exist only to fill
 * the fixed-point finesine/finetangent/tantoangle tables in
 * src/doom/tables.c.
 *
 * ── Accuracy ───────────────────────────────────────────────────────────
 *
 * exo_fixed_sin/cos are correctly rounded: every result is the nearest
 * representable 16.16 value to the true sine, so the error never exceeds
 * 0.5 LSB.  That is *better* than Doom's own shipped tables, which were
 * generated with single-precision `float` and a truncating cast and are off
 * by up to 1.01 LSB.  tests/kernel/test_fixed_math_k.c regenerates all
 * three of Doom's tables through this API and checks them entry by entry
 * against src/doom/tables.c -- 24,577 comparisons against the chocolate-doom
 * reference data, which is the acceptance criterion SCRUM-41 asks for.
 *
 * ── Units ──────────────────────────────────────────────────────────────
 *
 * Angles are binary angle measure (BAM): a full turn is 2^32, so the type
 * wraps at exactly 2*pi with no range reduction needed and no accumulated
 * error from repeated addition.  This is Doom's own angle_t convention
 * (src/doom/tables.h), restated here rather than included so this header
 * stays independent of the vendored tree.
 *
 * Magnitudes are 16.16 fixed point, matching Doom's fixed_t/FRACUNIT.  The
 * typedef is deliberately omitted: src/doom/m_fixed.h already defines
 * `fixed_t`, and a second definition here would collide in any translation
 * unit that saw both.
 */

#define EXO_FRACBITS 16
#define EXO_FRACUNIT (1 << EXO_FRACBITS)

/*
 * sin/cos of a BAM angle, returned in 16.16.
 *
 * Range is [-EXO_FRACUNIT, +EXO_FRACUNIT] inclusive -- note that +1.0 is
 * reachable (at a quarter turn exactly), which Doom's own finesine table
 * never represents because its truncating generator produced 65535 there.
 */
int32_t exo_fixed_sin(uint32_t angle);
int32_t exo_fixed_cos(uint32_t angle);

/*
 * tan of a BAM angle, in 16.16.
 *
 * tan has poles at +/- a quarter turn, where no fixed-point answer exists;
 * those two angles (and only those) saturate to INT32_MAX / INT32_MIN
 * rather than dividing by zero.  Saturation is also applied to any angle
 * near enough to a pole that the true tangent overflows 16.16, so this
 * never wraps a huge positive tangent into a negative result.
 *
 * Callers filling a finetangent-shaped table never reach a pole: Doom's
 * indices are offset by half a fine angle precisely so the sampled angles
 * straddle the pole instead of landing on it.
 */
int32_t exo_fixed_tan(uint32_t angle);

/*
 * arctan of a non-negative 16.16 slope, returned as a BAM angle in
 * [0, 2^30] -- i.e. [0, a quarter turn].
 *
 * Implemented with CORDIC in vectoring mode (the ticket's own suggestion),
 * which needs only adds, shifts and a 31-entry table of atan(2^-k).  A
 * negative slope returns 0: the sign belongs to the caller's quadrant
 * logic, not here, and Doom's tantoangle table is built from non-negative
 * slopes only (src/doom/r_main.c's R_InitPointToAngle).
 */
uint32_t exo_fixed_atan(int32_t slope);

/*
 * floor/ceil on a 16.16 value, returning a 16.16 value.
 *
 * These are the "floor/ceil for integers" SCRUM-41 asks for, and they are
 * macros for the same reason <math.h>'s fabs is: the double-valued
 * <math.h> floor()/ceil() cannot exist under -mno-sse (see the header
 * comment above), and nothing under src/doom/ calls them -- the engine
 * rounds fixed-point values with shifts and masks.
 *
 * Both round toward -infinity / +infinity respectively, NOT toward zero, so
 * they match C's floor()/ceil() on negative inputs where a plain shift
 * would not.  The argument is evaluated once.
 */
#define EXO_FIXED_FLOOR(x) ((int32_t)((x) & ~(EXO_FRACUNIT - 1)))
#define EXO_FIXED_CEIL(x) \
    ((int32_t)(((x) + (EXO_FRACUNIT - 1)) & ~(EXO_FRACUNIT - 1)))

/* The same two, but yielding a plain integer rather than a 16.16 value. */
#define EXO_FIXED_FLOOR_INT(x) ((int32_t)((x) >> EXO_FRACBITS))
#define EXO_FIXED_CEIL_INT(x) \
    ((int32_t)(((x) + (EXO_FRACUNIT - 1)) >> EXO_FRACBITS))
