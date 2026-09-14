#pragma once

/*
 * Freestanding <fcntl.h> (SCRUM-64).
 *
 * Included by src/doom/i_input.c and src/doom/i_video.c, neither of which
 * calls anything from it: both are doomgeneric's trimmed-down replacements for
 * the SDL input/video backends, and the include is left over from the SDL
 * originals they were cut down from.  Deleting the two `#include <fcntl.h>`
 * lines instead would work equally well for us, but keeping src/doom/ byte-for-
 * byte as vendored (SCRUM-63) keeps a future re-vendor a clean drop-in rather
 * than a merge.
 *
 * So this declares no functions.  `open()` in particular is absent on purpose:
 * the kernel has no file descriptors and no filesystem, so a declaration here
 * would describe an interface that does not exist rather than satisfy one that
 * does.  The O_* flags are defined because they are what a reader opening this
 * header expects to find, and they cost nothing.
 */

#define O_RDONLY   0000000
#define O_WRONLY   0000001
#define O_RDWR     0000002
#define O_CREAT    0000100
#define O_EXCL     0000200
#define O_TRUNC    0001000
#define O_APPEND   0002000
#define O_NONBLOCK 0004000
