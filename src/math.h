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
