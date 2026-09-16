#pragma once

/*
 * Freestanding <math.h> (SCRUM-64).
 *
 * Doom is fixed-point throughout (src/doom/m_fixed.c, and the finesine /
 * finetangent tables in src/doom/tables.c), so it needs far less of <math.h>
 * than its five `#include <math.h>` lines suggest.  Four of those five are
 * vestigial: the `atan`/`tan`/`sin` calls in src/doom/r_main.c all sit inside
 * `#if 0` blocks marked "UNUSED - now getting from tables.c" -- the lookup
 * tables replaced the runtime trigonometry decades ago -- and g_game.c,
 * i_input.c and p_setup.c include the header without calling anything from it.
 *
 * That leaves exactly one live call in the whole engine: `fabs()` in
 * `V_DrawMouseSpeedBox()` (src/doom/v_video.c), guarding the mouse-speed
 * widget.  So this header declares what is actually reachable rather than a
 * plausible-looking <math.h>; adding the rest when nothing calls it would be
 * guessing at signatures no test could ever catch being wrong.
 *
 * These are GCC builtins rather than calls into a libm we do not have, so they
 * are real implementations, not stubs -- the compiler emits the instruction
 * inline.  That matters here: the kernel builds with -mno-sse -mno-sse2, so
 * there is no soft-float library to fall back on and an out-of-line call would
 * not link.
 */

#define fabs(x)  __builtin_fabs(x)
#define fabsf(x) __builtin_fabsf(x)

/* Doom's own code spells pi out as a literal where it needs one
 * (src/doom/r_main.c), but m_misc.c and the menu code expect the macro to
 * exist once <math.h> is included. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * ---------------------------------------------------------------------------
 * Trigonometry, floor and ceil: see src/fixed_math.h (SCRUM-41).
 * ---------------------------------------------------------------------------
 *
 * SCRUM-41 asked for sin/cos/tan/atan, abs and floor/ceil.  They exist, and
 * they are deliberately NOT declared here, because a standard <math.h> spells
 * all of them in `double` and this kernel cannot have a double-valued
 * function at all: it compiles -mno-sse -mno-sse2, and the x86_64 SysV ABI
 * returns floating point in xmm0, so `double sin(double)` in any file the
 * kernel's own build globs is rejected outright -- the same wall
 * src/doom/m_config.c's M_GetFloatVariable() hit in SCRUM-64.  Every kernel
 * test TU carries those same flags, so a double API could not be unit-tested
 * either.
 *
 * So the implementations live in src/fixed_math.h as 16.16 fixed point over
 * binary angle measure:
 *
 *     exo_fixed_sin / exo_fixed_cos / exo_fixed_tan   (Maclaurin, Q30 core)
 *     exo_fixed_atan                                  (CORDIC, vectoring)
 *     EXO_FIXED_FLOOR / EXO_FIXED_CEIL (+ _INT forms)  -- the ticket's
 *                                                        "floor/ceil for
 *                                                        integers"
 *
 * and `abs` is where it has been since SCRUM-30, in src/stdlib.c.
 *
 * Nothing under src/doom/ loses anything by this.  The engine is fixed-point
 * throughout, and the only reason it includes <math.h> at all is the single
 * live `fabs()` above -- the sin/tan/atan calls in r_main.c's
 * R_InitTables/R_InitPointToAngle sit inside `#if 0` blocks marked
 * "UNUSED - now getting from tables.c", and those are precisely the tables
 * tests/kernel/test_fixed_math_k.c now regenerates through the fixed-point
 * API to prove it matches chocolate-doom entry for entry.
 *
 * If a future re-vendor ever un-#if-0s that code, the port is a few lines --
 * call exo_fixed_sin/tan/atan on a BAM angle instead of sin/tan/atan on a
 * float -- and it will produce a *more* accurate table than the shipped one,
 * which was generated in single precision with a truncating cast.
 */
