#pragma once
#include <stdint.h>

/*
 * Freestanding <inttypes.h> (SCRUM-64).
 *
 * src/doom/doomtype.h includes this on every build, and says why in its own
 * comment: "What is really wanted here is stdint.h; however, some old versions
 * of Solaris don't have stdint.h and only have inttypes.h ... inttypes.h is
 * also in the C99 standard and defined to include stdint.h, so include this."
 * That is the entire reason 73 files under src/doom/ reach this header -- they
 * want `uint8_t` and friends, by way of a workaround for a 1990s Solaris bug.
 *
 * <stdint.h> is one of the four headers a freestanding implementation must
 * provide, so GCC ships it and the include above is all the types cost us.
 *
 * The PRIxx and SCNxx format macros are deliberately NOT defined here, even
 * though C99 puts them in this header.  The shim's printf (src/stdio.c) has no
 * length modifiers at all -- see its header comment -- so `PRId64` expanding to
 * the correct "ld" would hand printf a conversion it cannot parse and quietly
 * print the wrong thing.  Leaving them undefined turns that into a compile
 * error at the call site instead, which is the failure anyone would rather
 * have.  Nothing in src/doom/ uses one today.  Define them here when printf
 * grows length modifiers, not before.
 */
