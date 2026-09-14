#pragma once
#include <stddef.h>

/*
 * Freestanding <unistd.h> (SCRUM-64).
 *
 * Two files under src/doom/ include this, and neither reaches a POSIX call in
 * a build of ours:
 *
 *   - i_system.c includes it unconditionally on non-Windows, but its only use
 *     -- `isatty(fileno(stdout))` in `I_ConsoleStdout()` -- is inside
 *     `#if ORIGCODE`, and src/doom/config.h `#undef`s ORIGCODE.  The live
 *     branch is a plain `return 0`.
 *   - i_timer.c has its include and its `usleep()` call both commented out
 *     already; SCRUM-74 replaces that path with `exo_get_ticks` outright.
 *
 * So nothing here is called today.  The header exists because the `#include`
 * does, and an exokernel has no POSIX layer to point it at -- declaring
 * `read`/`write`/`close`/`fork` against a kernel that has no file descriptors
 * would invent an interface rather than satisfy one.  If a later ticket makes
 * one of those calls reachable, the right move is to add it here with a real
 * implementation behind it, not to widen this header speculatively now.
 *
 * `SEEK_SET`/`SEEK_CUR`/`SEEK_END` live in <stdio.h> where the shim's seek
 * users (w_file_stdc.c, m_misc.c) already get them.
 */

/*
 * The one POSIX declaration worth keeping: ssize_t is <unistd.h>'s to define,
 * and src/doom/ types a few byte counts with it.
 */
#ifndef _EXO_SSIZE_T_DEFINED
#define _EXO_SSIZE_T_DEFINED
typedef long ssize_t;
#endif
