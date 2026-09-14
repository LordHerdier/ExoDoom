#pragma once
#include <stddef.h>

/*
 * Freestanding <strings.h> (SCRUM-64).
 *
 * POSIX puts the case-insensitive comparisons here rather than in <string.h>,
 * and doomgeneric's `doomtype.h` includes this header unconditionally on every
 * non-Windows build -- which is every build we do.  That single include is why
 * 66 of the 79 files under src/doom/ used to stop at their first `#include`
 * with a fatal "strings.h: No such file or directory" long before the compiler
 * could report a real error in them.
 *
 * Both functions are genuinely implemented, in src/string.c (SCRUM-11); this
 * header only re-declares them under the name POSIX expects.  src/string.h
 * keeps its own declarations so kernel code that already includes it is
 * unaffected -- the two must stay in sync, which is why the prototypes here
 * are copied verbatim rather than restated.
 */

int strcasecmp(const char *s1, const char *s2);

int strncasecmp(const char *s1, const char *s2, size_t n);
