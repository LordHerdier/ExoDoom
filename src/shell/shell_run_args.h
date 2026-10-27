/*
 * shell_run_args.h — turning the text after `run <package>` into the launch
 * params buffer for exo_launch_pkg_params() (SCRUM-301).
 *
 * A package declares how many bytes of launch params it takes (params_len in
 * its header, src/exo_pkg.h); for a program like Doom those bytes are one
 * NUL-terminated argument string. Nothing here touches the console or makes
 * a syscall: the same TU links into the ring-3 shell blob and, under
 * TESTING=1, into the kernel for tests/kernel/test_shell_run_args_k.c
 * (docker/scripts/build.sh).
 */
#ifndef EXODOOM_SHELL_RUN_ARGS_H
#define EXODOOM_SHELL_RUN_ARGS_H

#include <stdint.h>

#define SHELL_RUN_ARGS_OK        0
#define SHELL_RUN_ARGS_NO_PARAMS 1 /* arguments, but the package takes none */
#define SHELL_RUN_ARGS_TOO_LONG  2 /* does not fit params_len with its NUL */

/*
 * Fill buf[0..params_len) from `args`, the text after the package name.
 * Leading and trailing spaces are dropped; the rest is copied as typed and
 * the whole buffer is zero past it, so the program sees one clean string.
 *
 * Returns SHELL_RUN_ARGS_OK, or an error with buf left untouched:
 *   SHELL_RUN_ARGS_NO_PARAMS  params_len is 0 and there is text. (No text and
 *                             no params is fine, and writes nothing.)
 *   SHELL_RUN_ARGS_TOO_LONG   text + NUL exceed params_len. Never truncated:
 *                             the cut-off end of `-iwad doom2.wad` names a
 *                             different file.
 */
int shell_run_args_fill(const char *args, uint32_t params_len, uint8_t *buf);

#endif /* EXODOOM_SHELL_RUN_ARGS_H */
